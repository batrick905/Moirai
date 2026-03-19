// SPDX-License-Identifier: GPL-2.0
/*
 * pn532_spi.c - PN532 NFC Reader LKM over SPI
 *
 * Polls the PN532 for ISO14443A cards via SPI, logs each unique UID
 * with a timestamp to a circular buffer, and exposes it via /proc/pn532_uids.
 *
 * Usage:
 *   insmod pn532_spi.ko [poll_interval_ms=500]
 *   cat /proc/pn532_uids
 *   rmmod pn532_spi
 *
 * Wiring (Raspberry Pi example):
 *   PN532 MOSI -> GPIO10 (SPI0_MOSI)
 *   PN532 MISO -> GPIO9  (SPI0_MISO)
 *   PN532 SCK  -> GPIO11 (SPI0_SCLK)
 *   PN532 NSS  -> GPIO8  (SPI0_CE0)
 *   PN532 VCC  -> 3.3V
 *   PN532 GND  -> GND
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spi/spi.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/time64.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/string.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("PN532 SPI NFC reader with /proc UID log");
MODULE_VERSION("1.0");

/* ── Tunables ────────────────────────────────────────────────────── */

static unsigned int poll_interval_ms = 500;
module_param(poll_interval_ms, uint, 0444);
MODULE_PARM_DESC(poll_interval_ms, "Card poll interval in ms (default 500)");

#define LOG_MAX_ENTRIES   128   /* circular buffer depth              */
#define UID_MAX_LEN       10    /* ISO14443A UIDs are 4, 7, or 10 B  */
#define PROC_FILENAME     "pn532_uids"

/* ── PN532 SPI frame constants ───────────────────────────────────── */

/*
 * PN532 SPI is LSB-first at byte level, but BCM2835 is MSB-first in hardware.
 * Since SPI_LSB_FIRST is unsupported we manually bit-reverse each direction byte:
 *   0x01 write       -> 0x80
 *   0x02 status read -> 0x40
 *   0x03 data read   -> 0xC0
 * The ready status byte 0x01 bit-reversed is also 0x80.
 */
#define PN532_SPI_STATREAD   0x40
#define PN532_SPI_DATAWRITE  0x80
#define PN532_SPI_DATAREAD   0xC0
#define PN532_SPI_READY      0x80

/* TFI (transport frame identifier) */
#define PN532_HOST_TO_PN532  0xD4
#define PN532_PN532_TO_HOST  0xD5

/* Commands */
#define PN532_CMD_SAMCONFIGURATION    0x14
#define PN532_CMD_INLISTPASSIVETARGET 0x4A

/* SAM config: normal mode, no timeout, no IRQ */
static const u8 sam_config_frame[] = {
    0x00, 0x00, 0xFF,  /* preamble + start code          */
    0x05,              /* LEN                             */
    0xFB,              /* LCS  (256 - LEN)                */
    PN532_HOST_TO_PN532,
    PN532_CMD_SAMCONFIGURATION,
    0x01,              /* normal mode                     */
    0x14,              /* timeout = 20 * 50ms = 1s (unused in normal mode) */
    0x01,              /* use IRQ pin = yes (we ignore it here)            */
    0xE8,              /* DCS                             */
    0x00,              /* postamble                       */
};

/* InListPassiveTarget: 1 target, 106kbps ISO14443A */
static const u8 inlist_frame[] = {
    0x00, 0x00, 0xFF,
    0x04,              /* LEN                             */
    0xFC,              /* LCS                             */
    PN532_HOST_TO_PN532,
    PN532_CMD_INLISTPASSIVETARGET,
    0x01,              /* MaxTg = 1                       */
    0x00,              /* BrTy = 106kbps ISO14443A        */
    0xF7,              /* DCS                             */
    0x00,
};

/* ── Data structures ─────────────────────────────────────────────── */

struct uid_entry {
    u8   uid[UID_MAX_LEN];
    u8   uid_len;
    /* stored as seconds + ns since epoch */
    time64_t  ts_sec;
    long      ts_nsec;
};

struct pn532_dev {
    struct spi_device   *spi;
    struct task_struct  *poll_task;
    struct mutex         log_lock;
    struct proc_dir_entry *proc_entry;

    /* circular ring buffer */
    struct uid_entry     log[LOG_MAX_ENTRIES];
    unsigned int         log_head;   /* next write index  */
    unsigned int         log_count;  /* total entries     */

    /* last seen UID (for de-dup across consecutive polls) */
    u8   last_uid[UID_MAX_LEN];
    u8   last_uid_len;
};

static struct pn532_dev *g_dev;  /* single-instance global */

/* ── SPI helpers ─────────────────────────────────────────────────── */

/*
 * Bit-reverse a single byte.
 * The PN532 SPI bus is LSB-first at the bit level. The BCM2835 SPI hardware
 * is MSB-first and doesn't support SPI_LSB_FIRST. So we reverse every byte
 * in software before sending, and reverse every received byte after reading.
 */
static inline u8 bitrev8(u8 b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

static void buf_to_lsb(const u8 *in, u8 *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        out[i] = bitrev8(in[i]);
}

static void buf_from_lsb(u8 *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        buf[i] = bitrev8(buf[i]);
}

/*
 * Wait until the PN532 signals it is ready (status byte bit 0).
 * Returns 0 on success, -ETIMEDOUT if the chip doesn't respond.
 */
static int pn532_wait_ready(struct spi_device *spi)
{
    u8 req = PN532_SPI_STATREAD;  /* already bit-reversed: 0x40 */
    u8 status = 0;
    int retries = 20;

    while (retries--) {
        struct spi_transfer t[2] = {
            { .tx_buf = &req,    .len = 1 },
            { .rx_buf = &status, .len = 1 },
        };
        struct spi_message m;
        spi_message_init(&m);
        spi_message_add_tail(&t[0], &m);
        spi_message_add_tail(&t[1], &m);
        spi_sync(spi, &m);

        /* received byte is also LSB-first, reverse before comparing */
        if (bitrev8(status) == 0x01)
            return 0;

        msleep(10);
    }
    return -ETIMEDOUT;
}

/*
 * Send a raw frame to the PN532 over SPI.
 */
static int pn532_send_frame(struct spi_device *spi,
                             const u8 *frame, size_t len)
{
    /* allocate a reversed copy of the frame + direction byte */
    u8 *tx = kmalloc(len + 1, GFP_KERNEL);
    struct spi_transfer t = { 0 };
    struct spi_message m;
    int ret;

    if (!tx)
        return -ENOMEM;

    tx[0] = PN532_SPI_DATAWRITE;  /* already bit-reversed: 0x80 */
    buf_to_lsb(frame, tx + 1, len);

    t.tx_buf = tx;
    t.len    = len + 1;

    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    ret = spi_sync(spi, &m);

    kfree(tx);
    return ret;
}

/*
 * Read a response frame from the PN532 (up to max_len bytes).
 * Returns number of bytes read, or negative on error.
 */
static int pn532_read_response(struct spi_device *spi,
                                u8 *buf, size_t max_len)
{
    u8 dir = PN532_SPI_DATAREAD;  /* already bit-reversed: 0xC0 */
    struct spi_transfer t[2] = {
        { .tx_buf = &dir, .len = 1       },
        { .rx_buf = buf,  .len = max_len },
    };
    struct spi_message m;
    int ret;

    spi_message_init(&m);
    spi_message_add_tail(&t[0], &m);
    spi_message_add_tail(&t[1], &m);
    ret = spi_sync(spi, &m);
    if (ret < 0)
        return ret;

    /* reverse all received bytes from LSB-first to MSB-first */
    buf_from_lsb(buf, max_len);
    return (int)max_len;
}

/* ── PN532 init: SAMConfiguration ────────────────────────────────── */

static int pn532_sam_config(struct spi_device *spi)
{
    u8 resp[16];
    int ret;

    ret = pn532_send_frame(spi, sam_config_frame, sizeof(sam_config_frame));
    if (ret < 0) return ret;

    msleep(100);  /* give chip time to process SAM command */
    ret = pn532_wait_ready(spi);
    if (ret < 0) return ret;

    ret = pn532_read_response(spi, resp, sizeof(resp));
    if (ret < 0) return ret;

    /* ACK is 00 00 FF 00 FF 00 */
    if (resp[0] != 0x00 || resp[1] != 0x00 || resp[2] != 0xFF ||
        resp[3] != 0x00 || resp[4] != 0xFF) {
        pr_warn("pn532: unexpected SAM ACK\n");
    }
    return 0;
}

/* ── Card polling ────────────────────────────────────────────────── */

/*
 * Parse an InListPassiveTarget response and extract the UID.
 * Response layout (after direction byte + preamble):
 *   [0..2]  00 00 FF   (start code)
 *   [3]     LEN
 *   [4]     LCS
 *   [5]     D5         (TFI PN532->host)
 *   [6]     4B         (InListPassiveTarget response code)
 *   [7]     NbTg       (number of targets found)
 *   [8]     Tg         (target number, 01)
 *   [9..10] ATQA (2 B)
 *   [11]    SAK  (1 B)
 *   [12]    NfcIdLength
 *   [13..]  UID bytes
 */
static int pn532_parse_uid(const u8 *resp, int resp_len,
                            u8 *uid_out, u8 *uid_len_out)
{
    /* Minimum valid response: preamble(3)+LEN+LCS+TFI+CMD+NbTg+Tg+ATQA(2)+SAK+NfcIdLen+1UID */
    if (resp_len < 14) return -EINVAL;
    if (resp[5] != PN532_PN532_TO_HOST) return -EINVAL;
    if (resp[6] != (PN532_CMD_INLISTPASSIVETARGET + 1)) return -EINVAL;
    if (resp[7] == 0) return -ENODEV;  /* no target found */

    u8 uid_len = resp[12];
    if (uid_len > UID_MAX_LEN) uid_len = UID_MAX_LEN;
    if (resp_len < 13 + uid_len) return -EINVAL;

    memcpy(uid_out, &resp[13], uid_len);
    *uid_len_out = uid_len;
    return 0;
}

/*
 * Poll for one card.  Returns 0 + fills uid/uid_len if found,
 * -ENODEV if no card, negative errno on comms error.
 */
static int pn532_poll_card(struct spi_device *spi,
                            u8 *uid, u8 *uid_len)
{
    u8 resp[32];
    int ret;

    ret = pn532_send_frame(spi, inlist_frame, sizeof(inlist_frame));
    if (ret < 0) return ret;

    /* PN532 needs ~20ms to scan at 106kbps */
    msleep(20);

    ret = pn532_wait_ready(spi);
    if (ret < 0) return ret;

    ret = pn532_read_response(spi, resp, sizeof(resp));
    if (ret < 0) return ret;

    return pn532_parse_uid(resp, ret, uid, uid_len);
}

/* ── Log helpers ─────────────────────────────────────────────────── */

static void log_uid(struct pn532_dev *dev, const u8 *uid, u8 uid_len)
{
    struct timespec64 ts;
    struct uid_entry *e;

    ktime_get_real_ts64(&ts);

    mutex_lock(&dev->log_lock);
    e = &dev->log[dev->log_head % LOG_MAX_ENTRIES];
    memcpy(e->uid, uid, uid_len);
    e->uid_len  = uid_len;
    e->ts_sec   = ts.tv_sec;
    e->ts_nsec  = ts.tv_nsec;

    dev->log_head = (dev->log_head + 1) % LOG_MAX_ENTRIES;
    if (dev->log_count < LOG_MAX_ENTRIES)
        dev->log_count++;
    mutex_unlock(&dev->log_lock);
}

static bool uid_same(const u8 *a, u8 alen, const u8 *b, u8 blen)
{
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* ── Kernel thread: polling loop ─────────────────────────────────── */

static int poll_thread(void *data)
{
    struct pn532_dev *dev = data;
    u8 uid[UID_MAX_LEN];
    u8 uid_len;

    pr_info("pn532: polling thread started (interval=%ums)\n",
            poll_interval_ms);

    while (!kthread_should_stop()) {
        int ret = pn532_poll_card(dev->spi, uid, &uid_len);

        if (ret == 0) {
            /* card found — only log if it's a new/different UID */
            if (!uid_same(uid, uid_len,
                          dev->last_uid, dev->last_uid_len)) {
                char hex[UID_MAX_LEN * 3 + 1] = {0};
                int i;
                for (i = 0; i < uid_len; i++)
                    snprintf(hex + i * 3, 4, "%02X:", uid[i]);
                hex[uid_len * 3 - 1] = '\0'; /* trim trailing colon */

                pr_info("pn532: UID detected: %s\n", hex);
                log_uid(dev, uid, uid_len);

                memcpy(dev->last_uid, uid, uid_len);
                dev->last_uid_len = uid_len;
            }
        } else if (ret == -ENODEV) {
            /* no card present — reset de-dup so next tap is logged */
            dev->last_uid_len = 0;
        }
        /* other errors (comms): silently retry next cycle */

        msleep(poll_interval_ms);
    }

    pr_info("pn532: polling thread stopped\n");
    return 0;
}

/* ── /proc interface ─────────────────────────────────────────────── */

static int proc_show(struct seq_file *sf, void *v)
{
    struct pn532_dev *dev = sf->private;
    unsigned int i, count, start;

    mutex_lock(&dev->log_lock);
    count = dev->log_count;

    /* walk oldest → newest */
    start = (dev->log_head + LOG_MAX_ENTRIES - count) % LOG_MAX_ENTRIES;

    seq_puts(sf, "# PN532 UID Log\n");
    seq_printf(sf, "# Entries: %u / %u\n", count, LOG_MAX_ENTRIES);
    seq_puts(sf, "# Format: INDEX  TIMESTAMP(UTC)               UID\n");
    seq_puts(sf, "#─────────────────────────────────────────────────────\n");

    for (i = 0; i < count; i++) {
        struct uid_entry *e = &dev->log[(start + i) % LOG_MAX_ENTRIES];
        struct rtc_time tm;
        char hex[UID_MAX_LEN * 3 + 1] = {0};
        int j;

        rtc_time64_to_tm(e->ts_sec, &tm);

        for (j = 0; j < e->uid_len; j++)
            snprintf(hex + j * 3, 4, "%02X:", e->uid[j]);
        if (e->uid_len > 0)
            hex[e->uid_len * 3 - 1] = '\0';

        seq_printf(sf, "[%4u]  %04d-%02d-%02d %02d:%02d:%02d.%03lu UTC  %s\n",
                   i + 1,
                   tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                   tm.tm_hour, tm.tm_min, tm.tm_sec,
                   e->ts_nsec / 1000000UL,
                   hex);
    }

    mutex_unlock(&dev->log_lock);
    return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, pde_data(inode));
}

static const struct proc_ops pn532_proc_ops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ── SPI driver probe / remove ───────────────────────────────────── */

static int pn532_probe(struct spi_device *spi)
{
    struct pn532_dev *dev;
    int ret;

    /* SPI configuration: PN532 uses mode 0, LSB-first, max 5MHz */
    spi->mode      = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz  = 5000000;
    ret = spi_setup(spi);
    if (ret < 0) {
        dev_err(&spi->dev, "spi_setup failed: %d\n", ret);
        return ret;
    }

    dev = devm_kzalloc(&spi->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev) return -ENOMEM;

    dev->spi = spi;
    mutex_init(&dev->log_lock);
    spi_set_drvdata(spi, dev);
    g_dev = dev;

    /* Initialise the PN532 */
    msleep(100);  /* power-on delay */
    ret = pn532_sam_config(spi);
    if (ret < 0) {
        dev_err(&spi->dev, "SAMConfiguration failed: %d\n", ret);
        return ret;
    }
    dev_info(&spi->dev, "PN532 initialised\n");

    /* Create /proc/pn532_uids */
    dev->proc_entry = proc_create_data(PROC_FILENAME, 0444, NULL,
                                        &pn532_proc_ops, dev);
    if (!dev->proc_entry) {
        dev_err(&spi->dev, "Failed to create /proc/%s\n", PROC_FILENAME);
        return -ENOMEM;
    }

    /* Start polling kernel thread */
    dev->poll_task = kthread_run(poll_thread, dev, "pn532_poll");
    if (IS_ERR(dev->poll_task)) {
        ret = PTR_ERR(dev->poll_task);
        dev->poll_task = NULL;
        proc_remove(dev->proc_entry);
        return ret;
    }

    dev_info(&spi->dev, "pn532: ready — read /proc/%s\n", PROC_FILENAME);
    return 0;
}

static void pn532_remove(struct spi_device *spi)
{
    struct pn532_dev *dev = spi_get_drvdata(spi);

    if (dev->poll_task) {
        kthread_stop(dev->poll_task);
        dev->poll_task = NULL;
    }
    if (dev->proc_entry) {
        proc_remove(dev->proc_entry);
        dev->proc_entry = NULL;
    }
    g_dev = NULL;
    dev_info(&spi->dev, "pn532: removed\n");
}

/* ── SPI device table ────────────────────────────────────────────── */

static const struct spi_device_id pn532_spi_id[] = {
    { "pn532", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, pn532_spi_id);

static const struct of_device_id pn532_of_match[] = {
    { .compatible = "nxp,pn532" },
    { }
};
MODULE_DEVICE_TABLE(of, pn532_of_match);

static struct spi_driver pn532_spi_driver = {
    .driver = {
        .name           = "pn532",
        .of_match_table = pn532_of_match,
    },
    .id_table = pn532_spi_id,
    .probe    = pn532_probe,
    .remove   = pn532_remove,
};

module_spi_driver(pn532_spi_driver);
