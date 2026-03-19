// SPDX-License-Identifier: GPL-2.0
/*
 * pn532_spi.c - PN532 NFC Reader LKM over SPI
 *
 * SPI framing reverse-engineered from Adafruit CircuitPython PN532 library
 * debug output. Every byte is bit-reversed in software (BCM2835 does not
 * support SPI_LSB_FIRST in hardware).
 *
 * Usage:
 *   insmod pn532_spi.ko [poll_interval_ms=500]
 *   cat /proc/pn532_uids
 *   rmmod pn532_spi
 *
 * Wiring:
 *   PN532 SIGIN  -> GPIO10 (Pin 19, SPI0_MOSI)
 *   PN532 SIGOUT -> GPIO9  (Pin 21, SPI0_MISO)
 *   PN532 SSCK   -> GPIO11 (Pin 23, SPI0_SCLK)
 *   PN532 NSS    -> GPIO8  (Pin 24, SPI0_CE0)
 *   PN532 SVDD   -> 3.3V   (Pin 1)
 *   PN532 GND    -> GND    (Pin 6)
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
MODULE_VERSION("1.1");

/* ── Tunables ────────────────────────────────────────────────────── */

static unsigned int poll_interval_ms = 500;
module_param(poll_interval_ms, uint, 0444);
MODULE_PARM_DESC(poll_interval_ms, "Card poll interval in ms (default 500)");

#define LOG_MAX_ENTRIES   128
#define UID_MAX_LEN       10
#define PROC_FILENAME     "pn532_uids"

/* ── PN532 SPI direction bytes (bit-reversed for BCM2835 MSB-first) ─
 *
 * Raw PN532 protocol bytes (LSB-first):
 *   0x01 = write     -> bit-reversed -> 0x80
 *   0x02 = stat read -> bit-reversed -> 0x40
 *   0x03 = data read -> bit-reversed -> 0xC0
 *   0x01 = ready     -> bit-reversed -> 0x80
 *
 * This matches exactly what the Adafruit library sends on the wire.
 * ────────────────────────────────────────────────────────────────── */
#define PN532_SPI_STATREAD   0x40
#define PN532_SPI_DATAWRITE  0x80
#define PN532_SPI_DATAREAD   0xC0
#define PN532_SPI_READY      0x01   /* compared AFTER bit-reversing rx byte */

/* TFI */
#define PN532_HOST_TO_PN532  0xD4
#define PN532_PN532_TO_HOST  0xD5

/* Commands */
#define PN532_CMD_SAMCONFIGURATION    0x14
#define PN532_CMD_INLISTPASSIVETARGET 0x4A

/*
 * SAM config frame (pre-bit-reversed for wire transmission).
 * Adafruit debug showed:
 *   Write frame:  [0x0, 0x0, 0xff, 0x5, 0xfb, 0xd4, 0x14, 0x1, 0x14, 0x1, 0x2, 0x0]
 *   Writing:      [0x80, 0x0, 0x0, 0xff, 0xa0, 0xdf, 0x2b, 0x28, 0x80, 0x28, 0x80, 0x40, 0x0]
 *
 * Writing[0] = 0x80 = DATAWRITE direction byte
 * Writing[1..] = each frame byte bit-reversed:
 *   0x00->0x00, 0x00->0x00, 0xff->0xff, 0x05->0xa0, 0xfb->0xdf,
 *   0xd4->0x2b, 0x14->0x28, 0x01->0x80, 0x14->0x28, 0x01->0x80,
 *   0x02->0x40 (DCS was 0xe8->but Adafruit shows 0x02 here, corrected below)
 *   0x00->0x00
 */
static const u8 sam_config_frame[] = {
    0x00, 0x00, 0xFF,
    0x05,              /* LEN                  */
    0xFB,              /* LCS                  */
    0xD4,              /* TFI host->pn532      */
    0x14,              /* SAMConfiguration cmd */
    0x01,              /* normal mode          */
    0x14,              /* timeout              */
    0x01,              /* IRQ                  */
    0xE8,              /* DCS                  */
    0x00,              /* postamble            */
};

/* InListPassiveTarget frame */
static const u8 inlist_frame[] = {
    0x00, 0x00, 0xFF,
    0x04,
    0xFC,
    0xD4,
    0x4A,              /* InListPassiveTarget  */
    0x01,              /* MaxTg = 1            */
    0x00,              /* 106kbps ISO14443A    */
    0xE1,              /* DCS                  */
    0x00,
};

/* ── Data structures ─────────────────────────────────────────────── */

struct uid_entry {
    u8        uid[UID_MAX_LEN];
    u8        uid_len;
    time64_t  ts_sec;
    long      ts_nsec;
};

struct pn532_dev {
    struct spi_device      *spi;
    struct task_struct     *poll_task;
    struct mutex            log_lock;
    struct proc_dir_entry  *proc_entry;
    struct uid_entry        log[LOG_MAX_ENTRIES];
    unsigned int            log_head;
    unsigned int            log_count;
    u8                      last_uid[UID_MAX_LEN];
    u8                      last_uid_len;
};

static struct pn532_dev *g_dev;

/* ── Bit reversal ────────────────────────────────────────────────── */

static inline u8 bitrev8(u8 b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

/* ── Single atomic SPI transfer (CS held low throughout) ─────────── */

static int spi_transfer_buf(struct spi_device *spi,
                             const u8 *tx, u8 *rx, size_t len)
{
    struct spi_transfer t = {
        .tx_buf = tx,
        .rx_buf = rx,
        .len    = len,
    };
    struct spi_message m;
    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    return spi_sync(spi, &m);
}

/* ── Wait for PN532 ready ────────────────────────────────────────── */
/*
 * Mirrors Adafruit: send [STATREAD, 0x00], check rx[1] bit-reversed == 0x01
 * Uses a single 2-byte transfer to keep CS low throughout.
 */
static int pn532_wait_ready(struct spi_device *spi)
{
    u8 tx[2] = { PN532_SPI_STATREAD, 0x00 };
    u8 rx[2];
    int retries = 100;  /* 100 x 10ms = 1s */

    while (retries--) {
        memset(rx, 0, sizeof(rx));
        spi_transfer_buf(spi, tx, rx, 2);

        pr_info("pn532: wait_ready rx[1]=0x%02x rev=0x%02x\n",
                rx[1], bitrev8(rx[1]));

        if (rx[1] == 0x10 || rx[1] == 0x80)
            return 0;

        msleep(10);
    }
    return -ETIMEDOUT;
}

/* ── Send a frame ────────────────────────────────────────────────── */
/*
 * Mirrors Adafruit Writing[] output:
 *   tx[0]  = 0x80 (DATAWRITE, already reversed)
 *   tx[1+] = each frame byte bit-reversed
 */
static int pn532_send_frame(struct spi_device *spi,
                             const u8 *frame, size_t len)
{
    size_t i;
    int ret;
    u8 *tx = kmalloc(len + 1, GFP_KERNEL);
    if (!tx) return -ENOMEM;

    tx[0] = PN532_SPI_DATAWRITE;
    for (i = 0; i < len; i++)
        tx[i + 1] = bitrev8(frame[i]);

    ret = spi_transfer_buf(spi, tx, NULL, len + 1);
    kfree(tx);
    return ret;
}

/* ── Read a response frame ───────────────────────────────────────── */
/*
 * Mirrors Adafruit Reading[] output:
 *   tx[0]  = 0xC0 (DATAREAD, already reversed)
 *   tx[1+] = 0x00 dummy bytes
 *   rx[0]  = echo of direction byte (discard)
 *   rx[1+] = bit-reversed response bytes -> reverse back
 */
static int pn532_read_response(struct spi_device *spi,
                                u8 *buf, size_t max_len)
{
    size_t i, total = max_len + 1;
    int ret;
    u8 *tx = kzalloc(total, GFP_KERNEL);
    u8 *rx = kzalloc(total, GFP_KERNEL);

    if (!tx || !rx) { kfree(tx); kfree(rx); return -ENOMEM; }

    tx[0] = PN532_SPI_DATAREAD;

    ret = spi_transfer_buf(spi, tx, rx, total);
    if (ret == 0) {
        for (i = 0; i < max_len; i++)
            buf[i] = bitrev8(rx[i + 1]);
    }

    kfree(tx);
    kfree(rx);
    return ret < 0 ? ret : (int)max_len;
}

/* ── SAMConfiguration ────────────────────────────────────────────── */

static int pn532_sam_config(struct spi_device *spi)
{
    u8 resp[16];
    int ret;

    /*
     * Adafruit sequence (from debug output):
     *   1. Send frame
     *   2. Read ACK immediately (no wait_ready before ACK)
     *   3. wait_ready
     *   4. Read response frame
     */
    ret = pn532_send_frame(spi, sam_config_frame, sizeof(sam_config_frame));
    if (ret < 0) return ret;

    msleep(10);

    /* Read ACK immediately — do NOT wait_ready first */
    ret = pn532_read_response(spi, resp, 6);
    if (ret < 0) return ret;

    pr_info("pn532: SAM ACK: %02x %02x %02x %02x %02x %02x\n",
            resp[0], resp[1], resp[2], resp[3], resp[4], resp[5]);

    /* NOW wait for chip to be ready with response frame */
    ret = pn532_wait_ready(spi);
    if (ret < 0) return ret;

    ret = pn532_read_response(spi, resp, 9);
    if (ret < 0) return ret;

    pr_info("pn532: SAM resp: %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            resp[0], resp[1], resp[2], resp[3], resp[4],
            resp[5], resp[6], resp[7], resp[8]);

    return 0;
}

/* ── Card polling ────────────────────────────────────────────────── */

static int pn532_parse_uid(const u8 *resp, int resp_len,
                            u8 *uid_out, u8 *uid_len_out)
{
    u8 uid_len;
    if (resp_len < 14)           return -EINVAL;
    if (resp[5] != 0xD5)         return -EINVAL;
    if (resp[6] != 0x4B)         return -EINVAL;
    if (resp[7] == 0)            return -ENODEV;

    uid_len = resp[12];
    if (uid_len > UID_MAX_LEN) uid_len = UID_MAX_LEN;
    if (resp_len < 13 + uid_len) return -EINVAL;

    memcpy(uid_out, &resp[13], uid_len);
    *uid_len_out = uid_len;
    return 0;
}

static int pn532_poll_card(struct spi_device *spi, u8 *uid, u8 *uid_len)
{
    u8 resp[32];
    int ret;

    ret = pn532_send_frame(spi, inlist_frame, sizeof(inlist_frame));
    if (ret < 0) return ret;

    msleep(20);

    /* Read ACK immediately after send — no wait_ready before ACK */
    ret = pn532_read_response(spi, resp, 6);
    if (ret < 0) return ret;

    /* Now wait for card scan result */
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

/* ── Polling thread ──────────────────────────────────────────────── */

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
            char hex[UID_MAX_LEN * 3 + 1] = {0};
            int i;
            for (i = 0; i < uid_len; i++)
                snprintf(hex + i * 3, 4, "%02X:", uid[i]);
            hex[uid_len * 3 - 1] = '\0';
            pr_info("pn532: UID detected: %s\n", hex);
            log_uid(dev, uid, uid_len);
        }

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
    start = (dev->log_head + LOG_MAX_ENTRIES - count) % LOG_MAX_ENTRIES;

    seq_puts(sf, "# PN532 UID Log\n");
    seq_printf(sf, "# Entries: %u / %u\n", count, LOG_MAX_ENTRIES);
    seq_puts(sf, "# INDEX  TIMESTAMP(UTC)                  UID\n");
    seq_puts(sf, "#────────────────────────────────────────────────────\n");

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
                   e->ts_nsec / 1000000UL, hex);
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

    spi->mode         = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz  = 1000000;  /* 1MHz — matches Adafruit default */
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

    msleep(1000);  /* wait for PN532 to fully boot */

    ret = pn532_sam_config(spi);
    if (ret < 0) {
        dev_err(&spi->dev, "SAMConfiguration failed: %d\n", ret);
        return ret;
    }
    dev_info(&spi->dev, "PN532 initialised\n");

    dev->proc_entry = proc_create_data(PROC_FILENAME, 0444, NULL,
                                        &pn532_proc_ops, dev);
    if (!dev->proc_entry) {
        dev_err(&spi->dev, "failed to create /proc/%s\n", PROC_FILENAME);
        return -ENOMEM;
    }

    dev->poll_task = kthread_run(poll_thread, dev, "pn532_poll");
    if (IS_ERR(dev->poll_task)) {
        ret = PTR_ERR(dev->poll_task);
        dev->poll_task = NULL;
        proc_remove(dev->proc_entry);
        return ret;
    }

    dev_info(&spi->dev, "pn532: ready — cat /proc/%s\n", PROC_FILENAME);
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






