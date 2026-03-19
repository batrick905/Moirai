/*
 * servo_driver.c - Linux Kernel Module for Micro Servo on Raspberry Pi 4
 * Version 4 — kernel 6.12 compatible
 *
 * Uses chip->pwms[0] directly after finding the BCM2835 PWM platform
 * device on the bus. pwm_request/pwm_request_from_chip are both gone
 * in 6.12; this approach uses only APIs confirmed present in the headers.
 *
 * Requires: dtoverlay=pwm,pin=18,func=2 in /boot/firmware/config.txt
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/pwm.h>
#include <linux/platform_device.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Project Team");
MODULE_DESCRIPTION("Micro Servo Driver for Raspberry Pi 4 via Kernel PWM API");
MODULE_VERSION("4.0");

/* ── Servo PWM parameters ────────────────────────────────────────────── */
#define PWM_PERIOD_NS       20000000u   /* 20ms = 50Hz                   */
#define SERVO_MIN_NS         1000000u   /* 1ms  = 0 degrees              */
#define SERVO_MAX_NS         2000000u   /* 2ms  = 180 degrees            */
#define SERVO_MIN_ANGLE      0
#define SERVO_MAX_ANGLE      180

/* ── IOCTL definitions ───────────────────────────────────────────────── */
#define SERVO_IOC_MAGIC     'S'
#define SERVO_IOCTL_SET_ANGLE   _IOW(SERVO_IOC_MAGIC, 1, int)
#define SERVO_IOCTL_GET_ANGLE   _IOR(SERVO_IOC_MAGIC, 2, int)
#define SERVO_IOCTL_SWEEP       _IOW(SERVO_IOC_MAGIC, 3, struct servo_sweep_cmd)
#define SERVO_IOCTL_CENTER      _IO (SERVO_IOC_MAGIC, 4)

struct servo_sweep_cmd {
    int start_angle;
    int end_angle;
    int step;
    int delay_ms;
};

/* ── Module state ────────────────────────────────────────────────────── */
static dev_t             servo_dev;
static struct cdev       servo_cdev;
static struct class     *servo_class;
static struct device    *servo_device;
static struct pwm_device *servo_pwm;

static DEFINE_MUTEX(servo_mutex);
static int current_angle = 90;

static unsigned long total_writes = 0;
static unsigned long total_reads  = 0;
static unsigned long total_sweeps = 0;
static unsigned long error_count  = 0;

static DECLARE_WAIT_QUEUE_HEAD(servo_wq);
static int angle_changed = 0;

static struct proc_dir_entry *proc_entry;

/* ── PWM helpers ─────────────────────────────────────────────────────── */
static unsigned int angle_to_ns(int angle)
{
    return SERVO_MIN_NS +
           (unsigned int)((angle * (long)(SERVO_MAX_NS - SERVO_MIN_NS))
                          / SERVO_MAX_ANGLE);
}

static int pwm_apply_angle(int angle)
{
    struct pwm_state state;
    pwm_get_state(servo_pwm, &state);
    state.period     = PWM_PERIOD_NS;
    state.duty_cycle = angle_to_ns(angle);
    state.enabled    = true;
    state.polarity   = PWM_POLARITY_NORMAL;
    return pwm_apply_might_sleep(servo_pwm, &state);
}

/* Must be called with servo_mutex held */
static int servo_set_angle_locked(int angle)
{
    int ret;
    if (angle < SERVO_MIN_ANGLE || angle > SERVO_MAX_ANGLE) {
        pr_warn("servo_driver: angle %d out of range [0,180]\n", angle);
        error_count++;
        return -EINVAL;
    }
    ret = pwm_apply_angle(angle);
    if (ret) {
        pr_err("servo_driver: pwm_apply failed (%d)\n", ret);
        error_count++;
        return ret;
    }
    current_angle = angle;
    angle_changed = 1;
    wake_up_interruptible(&servo_wq);
    return 0;
}

/* ── File operations ─────────────────────────────────────────────────── */
static int servo_open(struct inode *inode, struct file *file)
{
    pr_debug("servo_driver: opened\n");
    return 0;
}

static int servo_release(struct inode *inode, struct file *file)
{
    pr_debug("servo_driver: closed\n");
    return 0;
}

/*
 * read() — blocks until the angle changes, then returns the new angle.
 * Demonstrates blocking read via wait queue as required by assignment.
 */
static ssize_t servo_read(struct file *file, char __user *buf,
                           size_t count, loff_t *ppos)
{
    char msg[32];
    int  len;

    if (*ppos > 0)
        return 0;

    if (wait_event_interruptible(servo_wq, angle_changed != 0))
        return -ERESTARTSYS;

    mutex_lock(&servo_mutex);
    angle_changed = 0;
    len = snprintf(msg, sizeof(msg), "%d\n", current_angle);
    mutex_unlock(&servo_mutex);

    if ((size_t)len > count)
        return -EINVAL;
    if (copy_to_user(buf, msg, len))
        return -EFAULT;

    total_reads++;
    *ppos += len;
    return len;
}

/*
 * write() — accepts:
 *   "<angle>"                         e.g. "90"
 *   "sweep <start> <end> <step> [delay_ms]"
 *
 * A sweep blocks for its full duration via mutex.
 * A second process writing concurrently will block here too.
 */
static ssize_t servo_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos)
{
    char  kbuf[64];
    size_t len = min(count, sizeof(kbuf) - 1);
    int   angle, start, end, step, delay_ms = 20, ret = 0;

    if (copy_from_user(kbuf, buf, len))
        return -EFAULT;
    kbuf[len] = '\0';
    if (len && kbuf[len - 1] == '\n')
        kbuf[--len] = '\0';

    if (mutex_lock_interruptible(&servo_mutex))
        return -ERESTARTSYS;

    if (sscanf(kbuf, "sweep %d %d %d %d", &start, &end, &step, &delay_ms) >= 3) {
        int a;
        if (step    <= 0) step    = 1;
        if (delay_ms <= 0) delay_ms = 20;
        total_sweeps++;
        pr_info("servo_driver: sweep %d->%d step=%d delay=%dms\n",
                start, end, step, delay_ms);
        if (start <= end) {
            for (a = start; a <= end && ret == 0; a += step) {
                ret = servo_set_angle_locked(a);
                mutex_unlock(&servo_mutex);
                msleep(delay_ms);
                if (mutex_lock_interruptible(&servo_mutex))
                    return -ERESTARTSYS;
            }
        } else {
            for (a = start; a >= end && ret == 0; a -= step) {
                ret = servo_set_angle_locked(a);
                mutex_unlock(&servo_mutex);
                msleep(delay_ms);
                if (mutex_lock_interruptible(&servo_mutex))
                    return -ERESTARTSYS;
            }
        }
    } else if (sscanf(kbuf, "%d", &angle) == 1) {
        pr_info("servo_driver: set angle %d degrees\n", angle);
        ret = servo_set_angle_locked(angle);
        if (ret == 0) total_writes++;
    } else {
        pr_warn("servo_driver: unknown command '%s'\n", kbuf);
        error_count++;
        ret = -EINVAL;
    }

    mutex_unlock(&servo_mutex);
    return ret ? ret : (ssize_t)count;
}

static long servo_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int angle, ret = 0;
    struct servo_sweep_cmd sw;

    if (mutex_lock_interruptible(&servo_mutex))
        return -ERESTARTSYS;

    switch (cmd) {
    case SERVO_IOCTL_SET_ANGLE:
        if (copy_from_user(&angle, (int __user *)arg, sizeof(int)))
            { ret = -EFAULT; break; }
        ret = servo_set_angle_locked(angle);
        if (!ret) total_writes++;
        break;

    case SERVO_IOCTL_GET_ANGLE:
        angle = current_angle;
        if (copy_to_user((int __user *)arg, &angle, sizeof(int)))
            ret = -EFAULT;
        total_reads++;
        break;

    case SERVO_IOCTL_CENTER:
        ret = servo_set_angle_locked(90);
        if (!ret) total_writes++;
        break;

    case SERVO_IOCTL_SWEEP: {
        int a, d, s;
        if (copy_from_user(&sw, (struct servo_sweep_cmd __user *)arg, sizeof(sw)))
            { ret = -EFAULT; break; }
        d = sw.delay_ms > 0 ? sw.delay_ms : 20;
        s = sw.step     > 0 ? sw.step     : 1;
        total_sweeps++;
        if (sw.start_angle <= sw.end_angle) {
            for (a = sw.start_angle; a <= sw.end_angle; a += s) {
                servo_set_angle_locked(a);
                mutex_unlock(&servo_mutex);
                msleep(d);
                if (mutex_lock_interruptible(&servo_mutex))
                    return -ERESTARTSYS;
            }
        } else {
            for (a = sw.start_angle; a >= sw.end_angle; a -= s) {
                servo_set_angle_locked(a);
                mutex_unlock(&servo_mutex);
                msleep(d);
                if (mutex_lock_interruptible(&servo_mutex))
                    return -ERESTARTSYS;
            }
        }
        break;
    }

    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&servo_mutex);
    return ret;
}

static const struct file_operations servo_fops = {
    .owner          = THIS_MODULE,
    .open           = servo_open,
    .release        = servo_release,
    .read           = servo_read,
    .write          = servo_write,
    .unlocked_ioctl = servo_ioctl,
};

/* ── /proc/servo_stats ───────────────────────────────────────────────── */
static int proc_show(struct seq_file *m, void *v)
{
    mutex_lock(&servo_mutex);
    seq_printf(m, "=== Servo Driver Statistics ===\n");
    seq_printf(m, "Current Angle  : %d degrees\n",   current_angle);
    seq_printf(m, "Duty Cycle     : %u ns / %u ns\n",
               angle_to_ns(current_angle), PWM_PERIOD_NS);
    seq_printf(m, "Total Writes   : %lu\n", total_writes);
    seq_printf(m, "Total Reads    : %lu\n", total_reads);
    seq_printf(m, "Total Sweeps   : %lu\n", total_sweeps);
    seq_printf(m, "Error Count    : %lu\n", error_count);
    seq_printf(m, "PWM Frequency  : 50 Hz\n");
    seq_printf(m, "GPIO Pin       : 18\n");
    mutex_unlock(&servo_mutex);
    return 0;
}

static int proc_open_fn(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, NULL);
}

static const struct proc_ops servo_proc_ops = {
    .proc_open    = proc_open_fn,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ── PWM acquisition ─────────────────────────────────────────────────
 * In kernel 6.12, pwm_request() and pwm_request_from_chip() are gone.
 * pwm_get() requires a consumer device with a DT "pwms" property.
 *
 * Solution: find the BCM2835 PWM platform device by name on the platform
 * bus, retrieve the pwm_chip from its drvdata, then borrow &chip->pwms[0]
 * directly. No allocation — no put/free needed on teardown.
 */
static struct pwm_device *servo_acquire_pwm(void)
{
    struct device   *dev;
    struct pwm_chip *chip;

    /* Pi 4 BCM2711 PWM controller DT address */
    dev = bus_find_device_by_name(&platform_bus_type, NULL, "fe20c000.pwm");
    if (!dev) {
        pr_err("servo_driver: fe20c000.pwm not found on platform bus\n");
        return ERR_PTR(-ENODEV);
    }

    chip = dev_get_drvdata(dev);
    put_device(dev);

    if (!chip) {
        pr_err("servo_driver: no pwm_chip in drvdata\n");
        return ERR_PTR(-ENODEV);
    }

    pr_info("servo_driver: found PWM chip with %u channel(s)\n", chip->npwm);

    if (chip->npwm < 1) {
        pr_err("servo_driver: PWM chip has no channels\n");
        return ERR_PTR(-ENODEV);
    }

    /* Return a direct pointer into the chip's pwms array — no allocation */
    return &chip->pwms[0];
}

/* ── Module init / exit ──────────────────────────────────────────────── */
static int __init servo_init(void)
{
    int ret;

    /* 1. Get PWM channel */
    servo_pwm = servo_acquire_pwm();
    if (IS_ERR(servo_pwm)) {
        pr_err("servo_driver: failed to acquire PWM (%ld)\n"
               "  Ensure dtoverlay=pwm,pin=18,func=2 is in /boot/firmware/config.txt\n",
               PTR_ERR(servo_pwm));
        return PTR_ERR(servo_pwm);
    }

    /* 2. Centre servo at 90 degrees */
    ret = pwm_apply_angle(current_angle);
    if (ret) {
        pr_err("servo_driver: initial pwm_apply failed (%d)\n", ret);
        return ret;
    }
    pr_info("servo_driver: PWM acquired, servo centred at 90 degrees\n");

    /* 3. Allocate char device number */
    ret = alloc_chrdev_region(&servo_dev, 0, 1, "servo");
    if (ret) {
        pr_err("servo_driver: alloc_chrdev_region failed (%d)\n", ret);
        return ret;
    }

    /* 4. Register cdev */
    cdev_init(&servo_cdev, &servo_fops);
    servo_cdev.owner = THIS_MODULE;
    ret = cdev_add(&servo_cdev, servo_dev, 1);
    if (ret) {
        pr_err("servo_driver: cdev_add failed (%d)\n", ret);
        goto err_region;
    }

    /* 5. Create /dev/servo */
    servo_class = class_create("servo_class");
    if (IS_ERR(servo_class)) {
        ret = PTR_ERR(servo_class);
        goto err_cdev;
    }

    servo_device = device_create(servo_class, NULL, servo_dev, NULL, "servo");
    if (IS_ERR(servo_device)) {
        ret = PTR_ERR(servo_device);
        goto err_class;
    }

    /* 6. Create /proc/servo_stats */
    proc_entry = proc_create("servo_stats", 0444, NULL, &servo_proc_ops);
    if (!proc_entry)
        pr_warn("servo_driver: could not create /proc/servo_stats\n");

    pr_info("servo_driver: loaded — major=%d, /dev/servo ready\n",
            MAJOR(servo_dev));
    return 0;

err_class:
    class_destroy(servo_class);
err_cdev:
    cdev_del(&servo_cdev);
err_region:
    unregister_chrdev_region(servo_dev, 1);
    return ret;
}

static void __exit servo_exit(void)
{
    if (proc_entry)
        remove_proc_entry("servo_stats", NULL);

    device_destroy(servo_class, servo_dev);
    class_destroy(servo_class);
    cdev_del(&servo_cdev);
    unregister_chrdev_region(servo_dev, 1);

    /* Disable PWM output — no pwm_put since we borrowed the pointer */
    if (servo_pwm) {
        struct pwm_state state;
        pwm_get_state(servo_pwm, &state);
        state.enabled = false;
        pwm_apply_might_sleep(servo_pwm, &state);
    }

    pr_info("servo_driver: unloaded\n");
}

module_init(servo_init);
module_exit(servo_exit);
