/*
 * servo_driver.c - Linux Kernel Module for Micro Servo Control on Raspberry Pi 4
 *
 * Uses hardware PWM on GPIO18 (PWM0) via direct BCM2711 register access.
 *
 * Standard servo PWM: 50Hz (20ms period)
 *   - 1ms pulse  = 0 degrees   (duty ~5%)
 *   - 1.5ms pulse = 90 degrees  (duty ~7.5%)
 *   - 2ms pulse  = 180 degrees  (duty ~10%)
 *
 * Usage:
 *   echo "90"  > /dev/servo      -> move to 90 degrees
 *   echo "sweep 0 180 5" > /dev/servo -> sweep from 0 to 180 in steps of 5
 *   cat /dev/servo               -> read current angle
 *   cat /proc/servo_stats        -> view statistics
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <linux/sched.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Project Team");
MODULE_DESCRIPTION("Micro Servo Driver for Raspberry Pi 4 via Hardware PWM");
MODULE_VERSION("1.0");

/* ── BCM2711 Physical Addresses ─────────────────────────────────────── */
#define BCM2711_PERI_BASE   0xFE000000UL

#define GPIO_BASE           (BCM2711_PERI_BASE + 0x200000)
#define PWM_BASE            (BCM2711_PERI_BASE + 0x20C000)
#define CLK_BASE            (BCM2711_PERI_BASE + 0x101000)

#define GPIO_LEN            0xB4
#define PWM_LEN             0x28
#define CLK_LEN             0xA8

/* ── GPIO Registers ──────────────────────────────────────────────────── */
#define GPFSEL1             (0x04 / 4)   /* Function select for GPIO10-19 */
#define GPPUD               (0x94 / 4)
#define GPPUDCLK0           (0x98 / 4)

/* GPIO18 = PWM0_0 alternate function ALT5 (value 0b010) */
#define GPIO18_FSEL_SHIFT   24
#define GPIO18_ALT5         0x2

/* ── PWM Registers ───────────────────────────────────────────────────── */
#define PWM_CTL             (0x00 / 4)
#define PWM_STA             (0x04 / 4)
#define PWM_RNG1            (0x10 / 4)   /* Range (period) channel 1 */
#define PWM_DAT1            (0x14 / 4)   /* Data  (duty)   channel 1 */

#define PWM_CTL_MSEN1       (1 << 7)     /* Mark-space mode (needed for servo) */
#define PWM_CTL_PWEN1       (1 << 0)     /* Enable channel 1 */

/* ── Clock Registers ─────────────────────────────────────────────────── */
#define PWMCLK_CNTL         (0xA0 / 4)
#define PWMCLK_DIV          (0xA4 / 4)
#define CLK_PASSWD          0x5A000000
#define CLK_CTL_ENAB        (1 << 4)
#define CLK_CTL_SRC_OSC     1            /* 19.2 MHz oscillator */

/*
 * Target PWM clock = 1 MHz  →  divisor = 19.2MHz / 1MHz ≈ 19 (integer part)
 * At 1 MHz, a 50 Hz servo needs range = 1,000,000 / 50 = 20,000 ticks
 * 1 ms  pulse = 1000 ticks  (0°)
 * 1.5ms pulse = 1500 ticks  (90°)
 * 2 ms  pulse = 2000 ticks  (180°)
 */
#define PWM_CLOCK_DIV       19
#define PWM_RANGE           20000        /* 20ms period → 50Hz            */
#define SERVO_MIN_TICKS     1000         /* 1ms  → 0°                     */
#define SERVO_MAX_TICKS     2000         /* 2ms  → 180°                   */
#define SERVO_MIN_ANGLE     0
#define SERVO_MAX_ANGLE     180

/* ── IOCTL Definitions ───────────────────────────────────────────────── */
#define SERVO_IOC_MAGIC     'S'
#define SERVO_IOCTL_SET_ANGLE   _IOW(SERVO_IOC_MAGIC, 1, int)
#define SERVO_IOCTL_GET_ANGLE   _IOR(SERVO_IOC_MAGIC, 2, int)
#define SERVO_IOCTL_SWEEP       _IOW(SERVO_IOC_MAGIC, 3, struct servo_sweep_cmd)
#define SERVO_IOCTL_CENTER      _IO (SERVO_IOC_MAGIC, 4)

struct servo_sweep_cmd {
    int start_angle;
    int end_angle;
    int step;
    int delay_ms;   /* delay between steps */
};

/* ── Module State ────────────────────────────────────────────────────── */
static dev_t            servo_dev;
static struct cdev      servo_cdev;
static struct class    *servo_class;
static struct device   *servo_device;

static volatile void __iomem *gpio_base_ptr;
static volatile void __iomem *pwm_base_ptr;
static volatile void __iomem *clk_base_ptr;

static DEFINE_MUTEX(servo_mutex);
static int current_angle = 90;          /* start centred                  */
static unsigned long total_writes  = 0;
static unsigned long total_reads   = 0;
static unsigned long total_sweeps  = 0;
static unsigned long error_count   = 0;

/* wait queue for blocking reads when angle hasn't changed */
static DECLARE_WAIT_QUEUE_HEAD(servo_read_wq);
static int angle_changed = 0;          /* flag: new angle available       */

static struct proc_dir_entry *proc_entry;

/* ── PWM Helpers ─────────────────────────────────────────────────────── */
static inline int angle_to_ticks(int angle)
{
    /* Linear interpolation: 0°→1000, 180°→2000 */
    return SERVO_MIN_TICKS +
           (angle * (SERVO_MAX_TICKS - SERVO_MIN_TICKS)) / SERVO_MAX_ANGLE;
}

static void pwm_set_duty(int ticks)
{
    iowrite32(ticks, pwm_base_ptr + PWM_DAT1);
}

static int servo_set_angle_locked(int angle)
{
    if (angle < SERVO_MIN_ANGLE || angle > SERVO_MAX_ANGLE) {
        pr_warn("servo_driver: angle %d out of range [0,180]\n", angle);
        error_count++;
        return -EINVAL;
    }
    current_angle = angle;
    pwm_set_duty(angle_to_ticks(angle));

    angle_changed = 1;
    wake_up_interruptible(&servo_read_wq);
    return 0;
}

/* ── Hardware Init / Teardown ────────────────────────────────────────── */
static int pwm_hw_init(void)
{
    u32 val;

    gpio_base_ptr = ioremap(GPIO_BASE, GPIO_LEN);
    if (!gpio_base_ptr) return -ENOMEM;

    pwm_base_ptr = ioremap(PWM_BASE, PWM_LEN);
    if (!pwm_base_ptr) { iounmap(gpio_base_ptr); return -ENOMEM; }

    clk_base_ptr = ioremap(CLK_BASE, CLK_LEN);
    if (!clk_base_ptr) {
        iounmap(pwm_base_ptr);
        iounmap(gpio_base_ptr);
        return -ENOMEM;
    }

    /* 1. Set GPIO18 to ALT5 (PWM0_0) */
    val = ioread32(gpio_base_ptr + GPFSEL1);
    val &= ~(0x7 << GPIO18_FSEL_SHIFT);
    val |=  (GPIO18_ALT5 << GPIO18_FSEL_SHIFT);
    iowrite32(val, gpio_base_ptr + GPFSEL1);

    /* 2. Stop & configure PWM clock */
    iowrite32(CLK_PASSWD | (ioread32(clk_base_ptr + PWMCLK_CNTL) & ~CLK_CTL_ENAB),
              clk_base_ptr + PWMCLK_CNTL);
    udelay(110);

    iowrite32(CLK_PASSWD | (PWM_CLOCK_DIV << 12), clk_base_ptr + PWMCLK_DIV);
    iowrite32(CLK_PASSWD | CLK_CTL_ENAB | CLK_CTL_SRC_OSC,
              clk_base_ptr + PWMCLK_CNTL);
    udelay(110);

    /* 3. Configure PWM channel 1 in mark-space mode at 50Hz */
    iowrite32(0, pwm_base_ptr + PWM_CTL);        /* disable while configuring  */
    iowrite32(PWM_RANGE, pwm_base_ptr + PWM_RNG1);
    pwm_set_duty(angle_to_ticks(current_angle)); /* centre on boot             */
    iowrite32(PWM_CTL_MSEN1 | PWM_CTL_PWEN1, pwm_base_ptr + PWM_CTL);

    pr_info("servo_driver: PWM hardware initialised (GPIO18, 50Hz)\n");
    return 0;
}

static void pwm_hw_exit(void)
{
    if (pwm_base_ptr) {
        iowrite32(0, pwm_base_ptr + PWM_CTL);   /* disable PWM                */
        iounmap(pwm_base_ptr);
    }
    if (gpio_base_ptr) {
        /* Restore GPIO18 to input */
        u32 val = ioread32(gpio_base_ptr + GPFSEL1);
        val &= ~(0x7 << GPIO18_FSEL_SHIFT);
        iowrite32(val, gpio_base_ptr + GPFSEL1);
        iounmap(gpio_base_ptr);
    }
    if (clk_base_ptr)
        iounmap(clk_base_ptr);
}

/* ── File Operations ─────────────────────────────────────────────────── */

static int servo_open(struct inode *inode, struct file *file)
{
    pr_debug("servo_driver: device opened\n");
    return 0;
}

static int servo_release(struct inode *inode, struct file *file)
{
    pr_debug("servo_driver: device closed\n");
    return 0;
}

/*
 * read() – blocks until the angle changes, then returns the current angle.
 * This demonstrates a blocking read as required by the assignment.
 */
static ssize_t servo_read(struct file *file, char __user *buf,
                           size_t count, loff_t *ppos)
{
    char msg[32];
    int len;

    /* block until a new angle has been set */
    if (wait_event_interruptible(servo_read_wq, angle_changed != 0))
        return -ERESTARTSYS;

    mutex_lock(&servo_mutex);
    angle_changed = 0;
    len = snprintf(msg, sizeof(msg), "%d\n", current_angle);
    mutex_unlock(&servo_mutex);

    if (*ppos > 0)
        return 0;            /* EOF on second read                          */

    if (count < (size_t)len)
        return -EINVAL;

    if (copy_to_user(buf, msg, len))
        return -EFAULT;

    total_reads++;
    *ppos += len;
    return len;
}

/*
 * write() – accepts commands:
 *   "<angle>"                         e.g.  "90"
 *   "sweep <start> <end> <step> [delay_ms]"
 *
 * Blocks if another sweep is already running (mutex).
 */
static ssize_t servo_write(struct file *file, const char __user *buf,
                            size_t count, loff_t *ppos)
{
    char kbuf[64];
    int  angle, start, end, step, delay_ms = 20;
    int  ret = 0;
    size_t len = min(count, sizeof(kbuf) - 1);

    if (copy_from_user(kbuf, buf, len))
        return -EFAULT;
    kbuf[len] = '\0';

    /* Strip trailing newline */
    if (len > 0 && kbuf[len - 1] == '\n')
        kbuf[--len] = '\0';

    /* Blocking lock – a sweep from another process will block here */
    if (mutex_lock_interruptible(&servo_mutex))
        return -ERESTARTSYS;

    if (sscanf(kbuf, "sweep %d %d %d %d", &start, &end, &step, &delay_ms) >= 3) {
        /* ── Sweep command ── */
        int a;
        if (step <= 0) step = 1;
        pr_info("servo_driver: sweep %d→%d step=%d delay=%dms\n",
                start, end, step, delay_ms);

        total_sweeps++;
        if (start <= end) {
            for (a = start; a <= end; a += step) {
                ret = servo_set_angle_locked(a);
                if (ret) break;
                mutex_unlock(&servo_mutex);
                msleep(delay_ms);
                if (mutex_lock_interruptible(&servo_mutex)) {
                    return -ERESTARTSYS;
                }
            }
        } else {
            for (a = start; a >= end; a -= step) {
                ret = servo_set_angle_locked(a);
                if (ret) break;
                mutex_unlock(&servo_mutex);
                msleep(delay_ms);
                if (mutex_lock_interruptible(&servo_mutex)) {
                    return -ERESTARTSYS;
                }
            }
        }
    } else if (sscanf(kbuf, "%d", &angle) == 1) {
        /* ── Simple angle command ── */
        pr_info("servo_driver: set angle → %d°\n", angle);
        ret = servo_set_angle_locked(angle);
    } else {
        pr_warn("servo_driver: unrecognised command '%s'\n", kbuf);
        ret = -EINVAL;
        error_count++;
    }

    if (ret == 0) total_writes++;
    mutex_unlock(&servo_mutex);
    return ret ? ret : (ssize_t)count;
}

/*
 * ioctl() – programmatic control from user-space apps.
 */
static long servo_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int angle, ret = 0;
    struct servo_sweep_cmd sw;

    if (mutex_lock_interruptible(&servo_mutex))
        return -ERESTARTSYS;

    switch (cmd) {
    case SERVO_IOCTL_SET_ANGLE:
        if (copy_from_user(&angle, (int __user *)arg, sizeof(int))) {
            ret = -EFAULT; break;
        }
        ret = servo_set_angle_locked(angle);
        if (ret == 0) total_writes++;
        break;

    case SERVO_IOCTL_GET_ANGLE:
        angle = current_angle;
        if (copy_to_user((int __user *)arg, &angle, sizeof(int)))
            ret = -EFAULT;
        total_reads++;
        break;

    case SERVO_IOCTL_CENTER:
        ret = servo_set_angle_locked(90);
        if (ret == 0) total_writes++;
        break;

    case SERVO_IOCTL_SWEEP:
        if (copy_from_user(&sw, (struct servo_sweep_cmd __user *)arg, sizeof(sw))) {
            ret = -EFAULT; break;
        }
        {
            int a, d = sw.delay_ms > 0 ? sw.delay_ms : 20;
            int s = sw.step > 0 ? sw.step : 1;
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
        }
        break;

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

/* ── /proc Interface ─────────────────────────────────────────────────── */
static int proc_show(struct seq_file *m, void *v)
{
    mutex_lock(&servo_mutex);
    seq_printf(m, "=== Servo Driver Statistics ===\n");
    seq_printf(m, "Current Angle  : %d degrees\n", current_angle);
    seq_printf(m, "PWM Ticks      : %d / %d\n",
               angle_to_ticks(current_angle), PWM_RANGE);
    seq_printf(m, "Total Writes   : %lu\n", total_writes);
    seq_printf(m, "Total Reads    : %lu\n", total_reads);
    seq_printf(m, "Total Sweeps   : %lu\n", total_sweeps);
    seq_printf(m, "Error Count    : %lu\n", error_count);
    seq_printf(m, "GPIO Pin       : 18 (ALT5/PWM0)\n");
    seq_printf(m, "PWM Frequency  : 50 Hz\n");
    seq_printf(m, "PWM Range      : %d ticks\n", PWM_RANGE);
    mutex_unlock(&servo_mutex);
    return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_show, NULL);
}

static const struct proc_ops servo_proc_ops = {
    .proc_open    = proc_open,
    .proc_read    = seq_read,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};

/* ── Module Init / Exit ──────────────────────────────────────────────── */
static int __init servo_init(void)
{
    int ret;

    /* 1. Initialise hardware */
    ret = pwm_hw_init();
    if (ret) {
        pr_err("servo_driver: hardware init failed (%d)\n", ret);
        return ret;
    }

    /* 2. Allocate device number */
    ret = alloc_chrdev_region(&servo_dev, 0, 1, "servo");
    if (ret) {
        pr_err("servo_driver: alloc_chrdev_region failed (%d)\n", ret);
        goto err_hw;
    }

    /* 3. Init & add cdev */
    cdev_init(&servo_cdev, &servo_fops);
    servo_cdev.owner = THIS_MODULE;
    ret = cdev_add(&servo_cdev, servo_dev, 1);
    if (ret) {
        pr_err("servo_driver: cdev_add failed (%d)\n", ret);
        goto err_region;
    }

    /* 4. Create /sys class & device node (/dev/servo) */
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

    /* 5. Create /proc/servo_stats */
    proc_entry = proc_create("servo_stats", 0444, NULL, &servo_proc_ops);
    if (!proc_entry)
        pr_warn("servo_driver: failed to create /proc/servo_stats\n");

    pr_info("servo_driver: loaded  major=%d  /dev/servo ready\n",
            MAJOR(servo_dev));
    return 0;

err_class:
    class_destroy(servo_class);
err_cdev:
    cdev_del(&servo_cdev);
err_region:
    unregister_chrdev_region(servo_dev, 1);
err_hw:
    pwm_hw_exit();
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
    pwm_hw_exit();
    pr_info("servo_driver: unloaded\n");
}

module_init(servo_init);
module_exit(servo_exit);
