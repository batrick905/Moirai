/*
 * servo_ioctl.h - Shared ioctl definitions for servo_driver
 *
 * Include this in both the kernel module AND the user-space app.
 */

#ifndef SERVO_IOCTL_H
#define SERVO_IOCTL_H

#include <linux/ioctl.h>

#define SERVO_IOC_MAGIC 'S'

struct servo_sweep_cmd {
    int start_angle;  /* degrees [0-180] */
    int end_angle;    /* degrees [0-180] */
    int step;         /* step size in degrees */
    int delay_ms;     /* milliseconds between steps */
};

/* Set servo to a specific angle (0-180) */
#define SERVO_IOCTL_SET_ANGLE  _IOW(SERVO_IOC_MAGIC, 1, int)

/* Get current angle */
#define SERVO_IOCTL_GET_ANGLE  _IOR(SERVO_IOC_MAGIC, 2, int)

/* Perform a sweep */
#define SERVO_IOCTL_SWEEP      _IOW(SERVO_IOC_MAGIC, 3, struct servo_sweep_cmd)

/* Centre the servo at 90 degrees */
#define SERVO_IOCTL_CENTER     _IO (SERVO_IOC_MAGIC, 4)

#endif /* SERVO_IOCTL_H */
