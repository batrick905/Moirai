#!/usr/bin/env python3
"""
servo_test.py - Auto sweep servo 0-180 and back via sysfs PWM
Servo on GPIO18, hardware PWM via pwmchip0
"""

import time
import sys

PWM_PATH = "/sys/class/pwm/pwmchip0"
CHIP      = f"{PWM_PATH}"
PWM0      = f"{PWM_PATH}/pwm0"

PERIOD_NS  = 20_000_000   # 20ms = 50Hz

# Adjust these to match your servo's actual range
MIN_NS     =   500_000    # ~0 degrees
MAX_NS     = 2_500_000    # ~180 degrees

STEP_DELAY = 0.02         # seconds between steps


def write(path, value):
    with open(path, "w") as f:
        f.write(str(value))


def setup():
    try:
        write(f"{CHIP}/export", 0)
        time.sleep(0.1)
    except OSError:
        pass  # already exported

    write(f"{PWM0}/period",     PERIOD_NS)
    write(f"{PWM0}/duty_cycle", MIN_NS)
    write(f"{PWM0}/enable",     1)
    print("PWM initialised")


def teardown():
    try:
        write(f"{PWM0}/enable", 0)
        write(f"{CHIP}/unexport", 0)
    except OSError:
        pass
    print("PWM cleaned up")


def angle_to_ns(angle):
    return int(MIN_NS + (angle / 180.0) * (MAX_NS - MIN_NS))


def sweep(start, end, step=1):
    angles = range(start, end + 1, step) if start <= end else range(start, end - 1, -step)
    for angle in angles:
        write(f"{PWM0}/duty_cycle", angle_to_ns(angle))
        time.sleep(STEP_DELAY)


def main():
    print("Servo sweep demo — Ctrl+C to stop")
    setup()

    try:
        while True:
            print("Sweeping 0 → 180")
            sweep(0, 180, step=2)
            time.sleep(0.5)

            print("Sweeping 180 → 0")
            sweep(180, 0, step=2)
            time.sleep(0.5)

    except KeyboardInterrupt:
        print("\nStopped")
    finally:
        teardown()


if __name__ == "__main__":
    main()
