# Moirai

ISE Block 3 Operating Systems Project

NFC Card Reader LDD
Servo Driver LDD

# Moirai — NFC Access Control System

Built for the Operating Systems module assignment. The system uses a Linux kernel device driver to interface with a PN532 NFC reader over SPI, and a servo motor controlled via hardware PWM on a Raspberry Pi 4. Tapping a registered and authorised card unlocks the servo; unauthorised cards are denied.

---

## What's in here

- `KernelSpace/Pn532/` — LKM for the PN532 NFC reader over SPI. Exposes card tap history via `/proc/pn532_uids`.
- `KernelSpace/Servo/` — LKM character device driver for servo control via hardware PWM on GPIO18. Implements open, close, read, write, ioctl and a `/proc` stats file.
- `Nfc_servo.py` — Userspace application that ties both together. Reads card taps from `/proc`, checks authorisation, and drives the servo accordingly.

---

## Hardware

- Raspberry Pi 4
- PN532 NFC module (SPI)
- Micro servo on GPIO18

---

## Setup

**1. Build and load the NFC driver**
```bash
/Moirai/src/KernelSpace/Pn532
make
sudo insmod pn532_spi.ko
```

**2. Build and load the servo driver**
```bash
/Moirai/src/KernelSpace/Servo
make
sudo insmod servo_driver.ko
```

**3. Run the application**
```bash
python3 Nfc_servo.py
```

---

## Usage

- Tap an unknown card → prompted to register a name and set authorised (y/n)
- Tap an authorised card → servo unlocks, holds, locks again
- Tap an unauthorised card → access denied
- All taps logged to `taps.log`

---

## Notes

- Requires `dtoverlay=pwm,pin=18,func=2` and `dtparam=audio=off` in `/boot/firmware/config.txt`
- Run `pinctrl get 18` to confirm GPIO18 is in ALT5 (PWM) mode before loading
