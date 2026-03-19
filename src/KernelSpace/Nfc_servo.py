#!/usr/bin/env python3
"""
nfc_servo.py - PN532 NFC card reader + servo control with authorisation
Tap an authorised card -> servo unlocks.
Tap an unauthorised card -> servo stays locked, access denied.
Unknown card -> prompt to register + set authorisation.

Usage: sudo python3 nfc_servo.py
"""

import json
import os
import time
import sys
import signal

# ── NFC config ────────────────────────────────────────────────────────
PROC_FILE      = "/proc/pn532_uids"
DB_FILE        = "uids.json"
LOG_FILE       = "taps.log"
RETAP_COOLDOWN = 2.0

# ── Servo / PWM config ────────────────────────────────────────────────
PWM_PATH    = "/sys/class/pwm/pwmchip0"
PWM0        = f"{PWM_PATH}/pwm0"
PERIOD_NS   = 20_000_000
MIN_NS      =    500_000
MAX_NS      =  2_500_000
STEP_DELAY  = 0.02
UNLOCK_HOLD = 2.0

# ── PWM helpers ───────────────────────────────────────────────────────
def pwm_write(path, value):
    with open(path, "w") as f:
        f.write(str(value))

def pwm_setup():
    try:
        pwm_write(f"{PWM_PATH}/export", 0)
        time.sleep(0.1)
    except OSError:
        pass
    pwm_write(f"{PWM0}/period",     PERIOD_NS)
    pwm_write(f"{PWM0}/duty_cycle", angle_to_ns(0))
    pwm_write(f"{PWM0}/enable",     1)
    print("  Servo initialised at 0 degrees")

def pwm_teardown():
    try:
        pwm_write(f"{PWM0}/enable", 0)
        pwm_write(f"{PWM_PATH}/unexport", 0)
    except OSError:
        pass
    print("  Servo PWM cleaned up")

def angle_to_ns(angle):
    return int(MIN_NS + (angle / 180.0) * (MAX_NS - MIN_NS))

def servo_set(angle):
    pwm_write(f"{PWM0}/duty_cycle", angle_to_ns(angle))

def servo_sweep(start, end, step=2):
    angles = range(start, end + 1, step) if start <= end \
             else range(start, end - 1, -step)
    for angle in angles:
        servo_set(angle)
        time.sleep(STEP_DELAY)

def servo_unlock():
    print("  Servo: UNLOCKING")
    servo_sweep(0, 180, step=2)
    time.sleep(UNLOCK_HOLD)
    print("  Servo: LOCKING")
    servo_sweep(180, 0, step=2)

def servo_deny():
    """Small jiggle to indicate denial."""
    print("  Servo: ACCESS DENIED")
    servo_sweep(0, 20, step=2)
    time.sleep(0.2)
    servo_sweep(20, 0, step=2)

# ── NFC / DB helpers ──────────────────────────────────────────────────
def load_db():
    if os.path.exists(DB_FILE):
        with open(DB_FILE, "r") as f:
            return json.load(f)
    return {}

def save_db(db):
    with open(DB_FILE, "w") as f:
        json.dump(db, f, indent=2)

def log_tap(uid, name, authorised, timestamp):
    status = "AUTHORISED" if authorised else "DENIED"
    with open(LOG_FILE, "a") as f:
        f.write(f"{timestamp}  {uid}  {name}  [{status}]\n")
    print(f"  Logged: [{name}] {status} at {timestamp}")

def read_proc():
    entries = []
    try:
        with open(PROC_FILE, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                try:
                    line  = line.replace("[", "").replace("]", "")
                    parts = line.split()
                    if len(parts) < 5:
                        continue
                    idx       = int(parts[0])
                    timestamp = f"{parts[1]} {parts[2]} {parts[3]}"
                    uid       = parts[4]
                    entries.append((idx, timestamp, uid))
                except (ValueError, IndexError):
                    continue
    except FileNotFoundError:
        print(f"ERROR: {PROC_FILE} not found — is the LKM loaded?")
        print("  Run: sudo insmod ~/Documents/Moirai/src/KernelSpace/Pn532/pn532_spi.ko")
        sys.exit(1)
    return entries

def register_uid(uid, db):
    print(f"\n  *** NEW CARD DETECTED ***")
    print(f"  UID: {uid}")
    try:
        name = input("  Enter name for this card (or press Enter to skip): ").strip()
    except (EOFError, KeyboardInterrupt):
        return db

    if not name:
        print("  Skipped — card will show as UNKNOWN.")
        return db

    try:
        auth_input = input("  Authorise this card? (y/n): ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        return db

    authorised = auth_input == "y"

    db[uid] = {
        "name":       name,
        "authorised": authorised
    }
    save_db(db)

    status = "AUTHORISED" if authorised else "UNAUTHORISED"
    print(f"  Saved: {uid} -> {name} [{status}]")
    return db

# ── Main ──────────────────────────────────────────────────────────────
def main():
    print("=" * 55)
    print("  NFC Servo Controller")
    print(f"  DB:       {DB_FILE}")
    print(f"  Log:      {LOG_FILE}")
    print(f"  Proc:     {PROC_FILE}")
    print(f"  Cooldown: {RETAP_COOLDOWN}s")
    print("  Ctrl+C to exit")
    print("=" * 55)

    pwm_setup()

    db = load_db()

    # Handle old-format DB entries (just strings, no authorised field)
    migrated = False
    for uid, val in db.items():
        if isinstance(val, str):
            db[uid] = {"name": val, "authorised": True}
            migrated = True
    if migrated:
        save_db(db)
        print("  Migrated old DB entries to new format.")

    # Print current registered cards
    print(f"\n  Registered cards ({len(db)}):")
    for uid, info in db.items():
        status = "AUTHORISED" if info["authorised"] else "UNAUTHORISED"
        print(f"    {uid}  {info['name']}  [{status}]")

    existing = read_proc()
    seen_idx = max((e[0] for e in existing), default=0)
    if seen_idx:
        print(f"\n  Skipping {seen_idx} existing log entries.")

    print("\n  Waiting for card taps...\n")

    last_tap_times = {}

    def handle_exit(sig, frame):
        print("\n  Shutting down...")
        pwm_teardown()
        sys.exit(0)
    signal.signal(signal.SIGINT, handle_exit)

    while True:
        entries     = read_proc()
        new_entries = [e for e in entries if e[0] > seen_idx]

        for idx, timestamp, uid in new_entries:
            seen_idx = max(seen_idx, idx)

            now = time.time()
            if uid in last_tap_times and (now - last_tap_times[uid]) < RETAP_COOLDOWN:
                continue
            last_tap_times[uid] = now

            if uid in db:
                info       = db[uid]
                name       = info["name"]
                authorised = info["authorised"]

                if authorised:
                    print(f"\n[TAP] ✓ AUTHORISED — {name} ({uid})")
                    log_tap(uid, name, True, timestamp)
                    servo_unlock()
                else:
                    print(f"\n[TAP] ✗ DENIED — {name} ({uid})")
                    log_tap(uid, name, False, timestamp)
                    servo_deny()

            else:
                db   = register_uid(uid, db)
                info = db.get(uid)
                if info:
                    log_tap(uid, info["name"], info["authorised"], timestamp)
                    if info["authorised"]:
                        print("  Card registered as AUTHORISED — tap again to unlock!")
                    else:
                        print("  Card registered as UNAUTHORISED.")
                print("\n  Waiting for card taps...\n")

        time.sleep(0.5)

if __name__ == "__main__":
    main()
