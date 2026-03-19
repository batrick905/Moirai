# Servo Driver LKM – Raspberry Pi 4

Linux Kernel Module that controls a micro servo via hardware PWM on GPIO 18.
Satisfies all assignment requirements: `open`, `close`, `read`, `write`, `ioctl`,
blocking calls, `/proc` stats, and a multi-process/threaded user-space demo.

---

## Hardware Wiring

```
Micro Servo         Raspberry Pi 4 (40-pin header)
───────────         ──────────────────────────────
Orange (signal) ──► Pin 12  (GPIO 18 / PWM0)
Red    (VCC)    ──► Pin  2  (5V) *
Brown  (GND)    ──► Pin  6  (GND)
```

> **⚠️ Power note:** Small servos (SG90 etc.) can usually be powered directly from
> the Pi's 5V pin during testing. For larger servos or multiple servos use an
> external 5V supply with a shared ground.

PWM signal: **50 Hz**, 1–2 ms pulse width (standard servo protocol).

---

## Build & Load

```bash
# On the Raspberry Pi 4 (requires kernel headers):
sudo apt update
sudo apt install raspberrypi-kernel-headers build-essential

git clone <your-repo>
cd servo_driver

make            # builds servo_driver.ko + servo_app
make load       # sudo insmod + shows /dev/servo
```

---

## Usage

### Shell (write/read directly)

```bash
# Set angle
echo "90"  | sudo tee /dev/servo       # centre
echo "0"   | sudo tee /dev/servo       # full left
echo "180" | sudo tee /dev/servo       # full right

# Sweep: sweep <start> <end> <step> [delay_ms]
echo "sweep 0 180 5 20" | sudo tee /dev/servo

# Blocking read – waits until the angle next changes, then prints it
sudo cat /dev/servo

# Statistics
cat /proc/servo_stats
```

### Application

```bash
sudo ./servo_app --angle 45              # move to 45°
sudo ./servo_app --sweep 0 180 10        # sweep 0→180, 10° steps
sudo ./servo_app --interactive           # interactive CLI
sudo ./servo_app --demo                  # full multi-process demo
```

### ioctl (from C)

```c
#include "servo_ioctl.h"
int fd = open("/dev/servo", O_RDWR);

int angle = 120;
ioctl(fd, SERVO_IOCTL_SET_ANGLE, &angle);   // set angle
ioctl(fd, SERVO_IOCTL_GET_ANGLE, &angle);   // get angle
ioctl(fd, SERVO_IOCTL_CENTER);              // go to 90°

struct servo_sweep_cmd sw = {0, 180, 5, 20};
ioctl(fd, SERVO_IOCTL_SWEEP, &sw);          // blocking sweep
```

---

## Assignment Requirements Checklist

| Requirement | Implementation |
|---|---|
| `open` | `servo_open()` – logs, returns 0 |
| `close` | `servo_release()` – logs, returns 0 |
| `read` | `servo_read()` – **blocking** via `wait_event_interruptible` |
| `write` | `servo_write()` – parses angle or sweep command, **blocking** via mutex |
| `ioctl` | `servo_ioctl()` – SET_ANGLE, GET_ANGLE, CENTER, SWEEP |
| `/proc` file | `/proc/servo_stats` – angle, ticks, counters |
| Blocking calls | `read` blocks on wait queue; `write` sweep blocks on mutex |
| Multi-process | Demo forks child reader while parent writes |
| Multi-threaded | Demo uses pthreads for concurrent sweep + blocking reader |
| Actual hardware | Controls real micro servo via BCM2711 hardware PWM |

---

## Unload

```bash
make unload     # sudo rmmod servo_driver
```

GPIO 18 is restored to input and PWM is disabled on unload.
