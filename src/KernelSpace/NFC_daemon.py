#!/usr/bin/env python3
"""
nfc_daemon.py - PN532 UID registration and tap logging daemon

Reads /proc/pn532_uids, matches UIDs to names in uids.json,
logs all taps to taps.log, and prompts for registration of new UIDs.

De-dup logic: same card tapping again is only logged if it was
absent for at least RETAP_COOLDOWN seconds (i.e. card was lifted).

Usage:
    python3 nfc_daemon.py
"""

import json
import os
import time
import sys
import signal

PROC_FILE       = "/proc/pn532_uids"
DB_FILE         = "uids.json"
LOG_FILE        = "taps.log"
RETAP_COOLDOWN  = 2.0  # seconds before same card counts as a new tap

# ── Load / save UID database ──────────────────────────────────────────

def load_db():
    if os.path.exists(DB_FILE):
        with open(DB_FILE, "r") as f:
            return json.load(f)
    return {}

def save_db(db):
    with open(DB_FILE, "w") as f:
        json.dump(db, f, indent=2)

# ── Log a tap event ───────────────────────────────────────────────────

def log_tap(uid, name, timestamp):
    with open(LOG_FILE, "a") as f:
        f.write(f"{timestamp}  {uid}  {name}\n")
    print(f"  → Logged: [{name}] at {timestamp}")

# ── Parse /proc/pn532_uids ────────────────────────────────────────────

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
                    # parts: [idx, date, time, UTC, uid]
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

# ── Register a new UID ────────────────────────────────────────────────

def register_uid(uid, db):
    print(f"\n  *** NEW CARD DETECTED ***")
    print(f"  UID: {uid}")
    try:
        name = input("  Enter name for this card (or press Enter to skip): ").strip()
    except (EOFError, KeyboardInterrupt):
        return db
    if name:
        db[uid] = name
        save_db(db)
        print(f"  Saved: {uid} → {name}")
    else:
        print(f"  Skipped — card will show as UNKNOWN in log.")
    return db

# ── Main loop ─────────────────────────────────────────────────────────

def main(register_callback):
    print("=" * 55)
    print("  PN532 NFC Daemon")
    print(f"  DB:      {DB_FILE}")
    print(f"  Log:     {LOG_FILE}")
    print(f"  Proc:    {PROC_FILE}")
    print(f"  Cooldown: {RETAP_COOLDOWN}s between same-card taps")
    print("  Ctrl+C to exit")
    print("=" * 55)

    db = load_db()
    print(f"  Loaded {len(db)} registered UIDs.")

    # prime with current entries so old taps aren't re-processed on startup
    existing = read_proc()
    seen_idx = max((e[0] for e in existing), default=0)
    if seen_idx:
        print(f"  Skipping {seen_idx} existing log entries.")

    print("\n  Waiting for card taps...\n")

    # de-dup tracking: uid -> last logged time
    last_tap_times = {}

    def handle_exit(sig, frame):
        print("\n  Daemon stopped.")
        sys.exit(0)
    signal.signal(signal.SIGINT, handle_exit)

    while True:
        entries     = read_proc()
        new_entries = [e for e in entries if e[0] > seen_idx]

        for idx, timestamp, uid in new_entries:
            seen_idx = max(seen_idx, idx)

            # de-dup: ignore if same card tapped within cooldown window
            now = time.time()
            if uid in last_tap_times and (now - last_tap_times[uid]) < RETAP_COOLDOWN:
                continue

            last_tap_times[uid] = now

            if uid in db:
                name = db[uid]
                print(f"[TAP] {name} ({uid}) at {timestamp}")
                log_tap(uid, name, timestamp)
            else:
                db   = GUI.register_uid(uid, db)
                name = db.get(uid, "UNKNOWN")
                log_tap(uid, name, timestamp)
                print("\n  Waiting for card taps...\n")

        time.sleep(0.5)

if __name__ == "__main__":
    main()
