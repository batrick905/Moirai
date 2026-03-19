#ifndef SERVO_IOCTL_H
#define SERVO_IOCTL_H

#include <linux/ioctl.h>

#define SERVO_IOC_MAGIC 'S'

struct servo_sweep_cmd {
    int start_angle;
    int end_angle;
    int step;
    int delay_ms;
};

#define SERVO_IOCTL_SET_ANGLE  _IOW(SERVO_IOC_MAGIC, 1, int)
#define SERVO_IOCTL_GET_ANGLE  _IOR(SERVO_IOC_MAGIC, 2, int)
#define SERVO_IOCTL_SWEEP      _IOW(SERVO_IOC_MAGIC, 3, struct servo_sweep_cmd)
#define SERVO_IOCTL_CENTER     _IO (SERVO_IOC_MAGIC, 4)

#endif
