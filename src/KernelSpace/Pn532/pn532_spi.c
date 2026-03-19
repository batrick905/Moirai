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
MODULE_AUTHOR("Patrick");
MODULE_DESCRIPTION("PN532 SPI NFC reader with /proc UID log");
MODULE_VERSION("1.1");

static unsigned int poll_interval_ms = 500;
module_param(poll_interval_ms, uint, 0444);
MODULE_PARM_DESC(poll_interval_ms, "Card poll interval in ms (default 500)");

#define LOG_MAX_ENTRIES   128
#define UID_MAX_LEN       10
#define PROC_FILENAME     "pn532_uids"

#define PN532_SPI_STATREAD   0x40
#define PN532_SPI_DATAWRITE  0x80
#define PN532_SPI_DATAREAD   0xC0
#define PN532_SPI_READY      0x01

#define PN532_HOST_TO_PN532  0xD4
#define PN532_PN532_TO_HOST  0xD5

#define PN532_CMD_SAMCONFIGURATION    0x14
#define PN532_CMD_INLISTPASSIVETARGET 0x4A

static const u8 sam_config_frame[] = {
    0x00, 0x00, 0xFF,
    0x05,
    0xFB,
    0xD4,
    0x14,
    0x01,
    0x14,
    0x01,
    0xE8,
    0x00,
};

static const u8 inlist_frame[] = {
    0x00, 0x00, 0xFF,
    0x04,
    0xFC,
    0xD4,
    0x4A,
    0x01,
    0x00,
    0xE1,
    0x00,
};

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

static inline u8 bitrev8(u8 b)
{
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

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

static int pn532_wait_ready(struct spi_device *spi)
{
    u8 tx[2] = { PN532_SPI_STATREAD, 0x00 };
    u8 rx[2];
    int retries = 100;

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

static int pn532_sam_config(struct spi_device *spi)
{
    u8 resp[16];
    int ret;

    ret = pn532_send_frame(spi, sam_config_frame, sizeof(sam_config_frame));
    if (ret < 0) return ret;

    msleep(10);

    ret = pn532_read_response(spi, resp, 6);
    if (ret < 0) return ret;

    pr_info("pn532: SAM ACK: %02x %02x %02x %02x %02x %02x\n",
            resp[0], resp[1], resp[2], resp[3], resp[4], resp[5]);

    ret = pn532_wait_ready(spi);
    if (ret < 0) return ret;

    ret = pn532_read_response(spi, resp, 9);
    if (ret < 0) return ret;

    pr_info("pn532: SAM resp: %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
            resp[0], resp[1], resp[2], resp[3], resp[4],
            resp[5], resp[6], resp[7], resp[8]);

    return 0;
}

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

    ret = pn532_read_response(spi, resp, 6);
    if (ret < 0) return ret;

    ret = pn532_wait_ready(spi);
    if (ret < 0) return ret;

    ret = pn532_read_response(spi, resp, sizeof(resp));
    if (ret < 0) return ret;

    return pn532_parse_uid(resp, ret, uid, uid_len);
}

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

static int pn532_probe(struct spi_device *spi)
{
    struct pn532_dev *dev;
    int ret;

    spi->mode         = SPI_MODE_0;
    spi->bits_per_word = 8;
    spi->max_speed_hz  = 1000000;
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

    msleep(1000);

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
