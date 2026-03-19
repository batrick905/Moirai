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
MODULE_VERSION("5.0");

#define PWM_PERIOD_NS       20000000
#define SERVO_MIN_NS         1000000
#define SERVO_MAX_NS         2000000
#define SERVO_MIN_ANGLE      0
#define SERVO_MAX_ANGLE      180

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

static int angle_to_ns(int angle)
{
    return SERVO_MIN_NS +
           (angle * (SERVO_MAX_NS - SERVO_MIN_NS)) / SERVO_MAX_ANGLE;
}

static int pwm_set_angle(int angle)
{
    int ret;
    int duty_ns = angle_to_ns(angle);

    ret = pwm_config(servo_pwm, duty_ns, PWM_PERIOD_NS);
    if (ret) {
        pr_err("servo_driver: pwm_config failed (%d)\n", ret);
        return ret;
    }

    ret = pwm_enable(servo_pwm);
    if (ret) {
        pr_err("servo_driver: pwm_enable failed (%d)\n", ret);
        return ret;
    }

    return 0;
}

static int servo_set_angle_locked(int angle)
{
    int ret;

    if (angle < SERVO_MIN_ANGLE || angle > SERVO_MAX_ANGLE) {
        pr_warn("servo_driver: angle %d out of range [0,180]\n", angle);
        error_count++;
        return -EINVAL;
    }

    ret = pwm_set_angle(angle);
    if (ret) {
        error_count++;
        return ret;
    }

    current_angle = angle;
    angle_changed = 1;
    wake_up_interruptible(&servo_wq);
    return 0;
}

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
        if (step     <= 0) step     = 1;
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

static int proc_show(struct seq_file *m, void *v)
{
    mutex_lock(&servo_mutex);
    seq_printf(m, "=== Servo Driver Statistics ===\n");
    seq_printf(m, "Current Angle  : %d degrees\n",   current_angle);
    seq_printf(m, "Duty Cycle     : %d ns / %d ns\n",
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

static struct pwm_device *servo_acquire_pwm(void)
{
    struct device   *dev;
    struct pwm_chip *chip;

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

    return &chip->pwms[0];
}

static int __init servo_init(void)
{
    int ret;

    servo_pwm = servo_acquire_pwm();
    if (IS_ERR(servo_pwm)) {
        pr_err("servo_driver: failed to acquire PWM (%ld)\n",
               PTR_ERR(servo_pwm));
        return PTR_ERR(servo_pwm);
    }

    ret = pwm_config(servo_pwm, angle_to_ns(current_angle), PWM_PERIOD_NS);
    if (ret) {
        pr_err("servo_driver: pwm_config failed (%d)\n", ret);
        return ret;
    }

    ret = pwm_enable(servo_pwm);
    if (ret) {
        pr_err("servo_driver: pwm_enable failed (%d)\n", ret);
        return ret;
    }

    pr_info("servo_driver: PWM enabled, servo centred at 90 degrees\n");

    ret = alloc_chrdev_region(&servo_dev, 0, 1, "servo");
    if (ret) {
        pr_err("servo_driver: alloc_chrdev_region failed (%d)\n", ret);
        goto err_pwm;
    }

    cdev_init(&servo_cdev, &servo_fops);
    servo_cdev.owner = THIS_MODULE;
    ret = cdev_add(&servo_cdev, servo_dev, 1);
    if (ret) {
        pr_err("servo_driver: cdev_add failed (%d)\n", ret);
        goto err_region;
    }

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
err_pwm:
    pwm_disable(servo_pwm);
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

    if (servo_pwm)
        pwm_disable(servo_pwm);

    pr_info("servo_driver: unloaded\n");
}

module_init(servo_init);
module_exit(servo_exit);
