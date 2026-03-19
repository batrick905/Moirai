// SPDX-License-Identifier: GPL-2.0
/*
 * pn532_spi.c - PN532 NFC Reader LKM over SPI (Fixed Handshake)
 *
 * This version implements the strict Command -> ACK -> Wait -> Response flow
 * required by the PN532 SPI interface.
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
MODULE_AUTHOR("Gemini AI / Original Author");
MODULE_DESCRIPTION("PN532 SPI NFC reader with strict ACK/Ready handshake");
MODULE_VERSION("1.2");

/* ── Tunables ────────────────────────────────────────────────────── */

static unsigned int poll_interval_ms = 500;
module_param(poll_interval_ms, uint, 0444);

#define LOG_MAX_ENTRIES   128
#define UID_MAX_LEN       10
#define PROC_FILENAME     "pn532_uids"

/* ── PN532 SPI Direction Bytes (Bit-Reversed for RPi MSB) ────────── */

#define PN532_SPI_DATAWRITE  0x80  /* Raw 0x01 */
#define PN532_SPI_STATREAD   0x40  /* Raw 0x02 */
#define PN532_SPI_DATAREAD   0xC0  /* Raw 0x03 */
#define PN532_SPI_READY      0x80  /* Raw 0x01, indicates chip is ready */

/* TFI */
#define PN532_HOST_TO_PN532  0xD4
#define PN532_PN532_TO_HOST  0xD5

/* Commands */
#define PN532_CMD_SAMCONFIGURATION    0x14
#define PN532_CMD_INLISTPASSIVETARGET 0x4A

static const u8 sam_config_frame[] = {
    0x00, 0x00, 0xFF, 0x05, 0xFB, PN532_HOST_TO_PN532, 
    PN532_CMD_SAMCONFIGURATION, 0x01, 0x14, 0x01, 0xE8, 0x00
};

static const u8 inlist_frame[] = {
    0x00, 0x00, 0xFF, 0x04, 0xFC, PN532_HOST_TO_PN532, 
    PN532_CMD_INLISTPASSIVETARGET, 0x01, 0x00, 0xE1, 0x00
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

/* ── Bit reversal ────────────────────────────────────────────────── */

static inline u8 bitrev8(u8 b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

/* ── SPI Helper ──────────────────────────────────────────────────── */

static int spi_transfer_buf(struct spi_device *spi, const u8 *tx, u8 *rx, size_t len) {
    struct spi_transfer t = { .tx_buf = tx, .rx_buf = rx, .len = len };
    struct spi_message m;
    spi_message_init(&m);
    spi_message_add_tail(&t, &m);
    return spi_sync(spi, &m);
}

/* ── Handshake logic ─────────────────────────────────────────────── */

static int pn532_wait_ready(struct spi_device *spi) {
    u8 tx[2] = { PN532_SPI_STATREAD, 0x00 };
    u8 rx[2];
    int retries = 100;

    while (retries--) {
        spi_transfer_buf(spi, tx, rx, 2);
        /* PN532 ready bit is 0x01 LSB. On MSB-first RPi, that's 0x80 */
        if (rx[1] == PN532_SPI_READY)
            return 0;
        usleep_range(1000, 2000);
    }
    return -ETIMEDOUT;
}

static int pn532_send_frame(struct spi_device *spi, const u8 *frame, size_t len) {
    size_t i;
    u8 *tx = kmalloc(len + 1, GFP_KERNEL);
    int ret;
    if (!tx) return -ENOMEM;

    tx[0] = PN532_SPI_DATAWRITE;
    for (i = 0; i < len; i++)
        tx[i + 1] = bitrev8(frame[i]);

    ret = spi_transfer_buf(spi, tx, NULL, len + 1);
    kfree(tx);
    return ret;
}

static int pn532_read_response(struct spi_device *spi, u8 *buf, size_t max_len) {
    size_t i, total = max_len + 1;
    u8 *tx = kzalloc(total, GFP_KERNEL);
    u8 *rx = kzalloc(total, GFP_KERNEL);
    int ret;

    if (!tx || !rx) { kfree(tx); kfree(rx); return -ENOMEM; }
    tx[0] = PN532_SPI_DATAREAD;

    ret = spi_transfer_buf(spi, tx, rx, total);
    if (ret == 0) {
        for (i = 0; i < max_len; i++)
            buf[i] = bitrev8(rx[i + 1]);
    }

    kfree(tx); kfree(rx);
    return ret < 0 ? ret : (int)max_len;
}

/* ── SAM Config ──────────────────────────────────────────────────── */

static int pn532_sam_config(struct spi_device *spi) {
    u8 ack[6];
    u8 resp[9];
    int ret;

    /* 1. Send Command */
    ret = pn532_send_frame(spi, sam_config_frame, sizeof(sam_config_frame));
    if (ret < 0) return ret;

    /* 2. Read ACK Immediately */
    ret = pn532_read_response(spi, ack, 6);
    if (ret < 0) return ret;

    /* 3. Wait for Chip to Process */
    ret = pn532_wait_ready(spi);
    if (ret < 0) return ret;

    /* 4. Read Actual Response */
    return pn532_read_response(spi, resp, 9);
}

/* ── Card Polling ────────────────────────────────────────────────── */

static int pn532_poll_card(struct spi_device *spi, u8 *uid, u8 *uid_len) {
    u8 ack[6];
    u8 resp[32];
    int ret;

    ret = pn532_send_frame(spi, inlist_frame, sizeof(inlist_frame));
    if (ret < 0) return ret;

    /* Clear ACK */
    pn532_read_response(spi, ack, 6);

    ret = pn532_wait_ready(spi);
    if (ret < 0) return ret;

    ret = pn532_read_response(spi, resp, sizeof(resp));
    if (ret < 0) return ret;

    /* Parse Logic */
    if (resp[5] != PN532_PN532_TO_HOST || resp[7] == 0) return -ENODEV;
    *uid_len = resp[12];
    if (*uid_len > UID_MAX_LEN) *uid_len = UID_MAX_LEN;
    memcpy(uid, &resp[13], *uid_len);
    return 0;
}

/* ── Thread / Proc / Lifecycle ───────────────────────────────────── */

static void log_uid(struct pn532_dev *dev, const u8 *uid, u8 uid_len) {
    struct timespec64 ts;
    struct uid_entry *e;
    ktime_get_real_ts64(&ts);
    mutex_lock(&dev->log_lock);
    e = &dev->log[dev->log_head % LOG_MAX_ENTRIES];
    memcpy(e->uid, uid, uid_len);
    e->uid_len = uid_len;
    e->ts_sec = ts.tv_sec; e->ts_nsec = ts.tv_nsec;
    dev->log_head = (dev->log_head + 1) % LOG_MAX_ENTRIES;
    if (dev->log_count < LOG_MAX_ENTRIES) dev->log_count++;
    mutex_unlock(&dev->log_lock);
}

static int poll_thread(void *data) {
    struct pn532_dev *dev = data;
    u8 uid[UID_MAX_LEN];
    u8 uid_len;
    while (!kthread_should_stop()) {
        if (pn532_poll_card(dev->spi, uid, &uid_len) == 0) {
            if (uid_len != dev->last_uid_len || memcmp(uid, dev->last_uid, uid_len) != 0) {
                log_uid(dev, uid, uid_len);
                memcpy(dev->last_uid, uid, uid_len);
                dev->last_uid_len = uid_len;
            }
        } else {
            dev->last_uid_len = 0;
        }
        msleep(poll_interval_ms);
    }
    return 0;
}

static int proc_show(struct seq_file *sf, void *v) {
    struct pn532_dev *dev = sf->private;
    unsigned int i, count, start;
    mutex_lock(&dev->log_lock);
    count = dev->log_count;
    start = (dev->log_head + LOG_MAX_ENTRIES - count) % LOG_MAX_ENTRIES;
    seq_printf(sf, "# PN532 UID Log (%u entries)\n", count);
    for (i = 0; i < count; i++) {
        struct uid_entry *e = &dev->log[(start + i) % LOG_MAX_ENTRIES];
        int j;
        seq_printf(sf, "[%4u] ", i + 1);
        for (j = 0; j < e->uid_len; j++) seq_printf(sf, "%02X%c", e->uid[j], (j == e->uid_len - 1) ? ' ' : ':');
        seq_putc(sf, '\n');
    }
    mutex_unlock(&dev->log_lock);
    return 0;
}

static int proc_open(struct inode *inode, struct file *file) {
    return single_open(file, proc_show, pde_data(inode));
}

static const struct proc_ops pn532_proc_ops = {
    .proc_open = proc_open, .proc_read = seq_read, .proc_lseek = seq_lseek, .proc_release = single_release,
};

static int pn532_probe(struct spi_device *spi) {
    struct pn532_dev *dev;
    int ret;

    spi->mode = SPI_MODE_0;
    spi->max_speed_hz = 1000000;
    spi_setup(spi);

    dev = devm_kzalloc(&spi->dev, sizeof(*dev), GFP_KERNEL);
    if (!dev) return -ENOMEM;
    dev->spi = spi;
    mutex_init(&dev->log_lock);
    spi_set_drvdata(spi, dev);

    msleep(100); 
    /* Wake-up: Send a dummy read */
    u8 dummy = PN532_SPI_STATREAD;
    spi_write(spi, &dummy, 1);
    msleep(10);

    if (pn532_sam_config(spi) < 0) return -EIO;

    dev->proc_entry = proc_create_data(PROC_FILENAME, 0444, NULL, &pn532_proc_ops, dev);
    dev->poll_task = kthread_run(poll_thread, dev, "pn532_poll");
    
    dev_info(&spi->dev, "PN532 Initialized on SPI\n");
    return 0;
}

static void pn532_remove(struct spi_device *spi) {
    struct pn532_dev *dev = spi_get_drvdata(spi);
    if (dev->poll_task) kthread_stop(dev->poll_task);
    if (dev->proc_entry) proc_remove(dev->proc_entry);
}

static const struct spi_device_id pn532_spi_id[] = { { "pn532", 0 }, { } };
MODULE_DEVICE_TABLE(spi, pn532_spi_id);

static struct spi_driver pn532_spi_driver = {
    .driver = { .name = "pn532" },
    .id_table = pn532_spi_id,
    .probe = pn532_probe,
    .remove = pn532_remove,
};

module_spi_driver(pn532_spi_driver);
