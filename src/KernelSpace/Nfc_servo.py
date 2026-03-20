#!/usr/bin/env python3

import json
import os
import time
import sys
import signal

if not sys.stdin.isatty():
    sys.stdin = open("/dev/tty", "r")

PROC_FILE      = "/proc/pn532_uids"
DB_FILE        = "uids.json"
LOG_FILE       = "taps.log"
RETAP_COOLDOWN = 2.0
SERVO_DEV      = "/dev/servo"
UNLOCK_HOLD    = 2.0

def servo_set(angle):
    with open(SERVO_DEV, "w") as f:
        f.write(f"{angle}\n")

def servo_unlock():
    print("  Servo: UNLOCKING")
    with open(SERVO_DEV, "w") as f:
        f.write("sweep 0 180 2 20\n")
    time.sleep(UNLOCK_HOLD)
    print("  Servo: LOCKING")
    with open(SERVO_DEV, "w") as f:
        f.write("sweep 180 0 2 20\n")

def servo_deny():
    print("  Servo: ACCESS DENIED")
    with open(SERVO_DEV, "w") as f:
        f.write("sweep 0 20 2 20\n")
    time.sleep(0.2)
    with open(SERVO_DEV, "w") as f:
        f.write("sweep 20 0 2 20\n")

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
    db[uid] = {"name": name, "authorised": authorised}
    save_db(db)

    status = "AUTHORISED" if authorised else "UNAUTHORISED"
    print(f"  Saved: {uid} -> {name} [{status}]")
    return db

def main():
    print("=" * 55)
    print("  NFC Servo Controller")
    print(f"  DB:       {DB_FILE}")
    print(f"  Log:      {LOG_FILE}")
    print(f"  Proc:     {PROC_FILE}")
    print(f"  Cooldown: {RETAP_COOLDOWN}s")
    print("  Ctrl+C to exit")
    print("=" * 55)

    db = load_db()

    migrated = False
    for uid, val in db.items():
        if isinstance(val, str):
            db[uid] = {"name": val, "authorised": True}
            migrated = True
    if migrated:
        save_db(db)
        print("  Migrated old DB entries to new format.")

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
                    print(f"\n[TAP] AUTHORISED — {name} ({uid})")
                    log_tap(uid, name, True, timestamp)
                    servo_unlock()
                else:
                    print(f"\n[TAP] DENIED — {name} ({uid})")
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
