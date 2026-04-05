#!/usr/bin/env python3
# mock_esp32.py — Simulates ESP32 serial JSON stream for dashboard testing.
# Writes to SERIAL_PORT (env var, default /tmp/esp32_tx) at 115200 baud.
# Mirrors every line to stdout for terminal visibility.
# Only external dep: pyserial

import json
import os
import random
import sys
import time

import serial

SERIAL_PORT = os.environ.get("SERIAL_PORT", "/tmp/esp32_tx")
BAUD_RATE   = 115200
INTERVAL    = 3.0   # seconds — matches firmware and Flask poll cadence

# ── Phase table ───────────────────────────────────────────────────────────────
# Each entry: (label, count, callable → dict)
# BOOT uses random() to mimic live ADC noise during baseline collection.
# All other phases use fixed values as they test known-state water samples.

PHASES = [
    ("BOOT",   10, lambda: {
        "tds":  round(random.uniform(79.0, 85.0), 1),
        "ntu":  round(random.uniform(0.7, 1.0),   1),
        "whi":  100.0,
        "tier": "BOOT",
        "btds": 0.0,
        "bntu": 0.0,
    }),
    ("GREEN",  10, lambda: {"tds": 82.0,  "ntu": 0.9,  "whi": 97.0, "tier": "GREEN",  "btds": 82.0, "bntu": 0.9}),
    ("BLUE",   10, lambda: {"tds": 248.0, "ntu": 6.9,  "whi": 63.0, "tier": "BLUE",   "btds": 82.0, "bntu": 0.9}),
    ("YELLOW", 10, lambda: {"tds": 520.0, "ntu": 11.5, "whi": 44.0, "tier": "YELLOW", "btds": 82.0, "bntu": 0.9}),
    ("RED",    10, lambda: {"tds": 813.0, "ntu": 0.8,  "whi": 21.0, "tier": "RED",    "btds": 82.0, "bntu": 0.9}),
]


# ── Port helper ───────────────────────────────────────────────────────────────
def open_port():
    # Retry until socat is ready — avoids requiring a specific startup order
    while True:
        try:
            s = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=1)
            print(f"[mock] Connected → {SERIAL_PORT} @ {BAUD_RATE}")
            return s
        except serial.SerialException as exc:
            print(f"[mock] {SERIAL_PORT} not ready ({exc}) — retrying in 2 s", file=sys.stderr)
            time.sleep(2)


# ── Main loop ─────────────────────────────────────────────────────────────────
def main():
    ser   = open_port()
    cycle = 0

    try:
        while True:
            cycle += 1
            print(f"── Cycle {cycle} ──────────────────────────────────────")

            for label, count, reading_fn in PHASES:
                for i in range(1, count + 1):
                    data    = reading_fn()
                    line    = json.dumps(data, separators=(",", ":"))
                    payload = (line + "\n").encode("utf-8")

                    try:
                        ser.write(payload)
                        ser.flush()
                    except serial.SerialException as exc:
                        # socat died — reconnect and resend this reading
                        print(f"\n[mock] Write error: {exc} — reconnecting...", file=sys.stderr)
                        try:
                            ser.close()
                        except Exception:
                            pass
                        ser = open_port()
                        try:
                            ser.write(payload)
                            ser.flush()
                        except serial.SerialException:
                            pass   # skip line if still broken; next will retry

                    print(f"  [{label:6s} {i:02d}/{count}] {line}")
                    sys.stdout.flush()
                    time.sleep(INTERVAL)

    except KeyboardInterrupt:
        print("\n[mock] Stopped.")
    finally:
        try:
            ser.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
