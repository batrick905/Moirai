cp ~/Downloads/nfc_daemon.py ~/Documents/Moirai/src/nfc_daemon.py
cd ~/Documents/Moirai/src
python3 nfc_daemon.py
```

The fix was stripping the `[` and `]` brackets before splitting — `[   1]` has spaces inside the brackets which broke `int(parts[0])`. Now it strips them first so `   1` becomes `1` cleanly.

When you tap a new card:
```
  *** NEW CARD DETECTED ***
  UID: 04:51:4A:B2:61:13:90
  Enter name for this card (or press Enter to skip): John
  Saved: 04:51:4A:B2:61:13:90 → John
```

Next time that card taps:
```
[TAP] John (04:51:4A:B2:61:13:90) at 2026-03-19 17:04:08.143 UTC
