# Agents Overview

## Boot Sequence
POST → Gear Up → Admin Gate (SoftAP 192.168.4.1, SSID TankTriage, open) → STA Connect → Baseline → Monitoring

## NVS
- Namespace: `wifi-cfg`
- Keys:
  - `ssid` (String) – Wi‑Fi SSID
  - `password` (String) – Wi‑Fi password

## HTTP Routes
| Method | Path          | Description                              | Availability |
|--------|---------------|------------------------------------------|--------------|
| GET    | `/`           | Serves `index.html` (dashboard)          | Monitoring only |
| GET    | `/api/data`   | Returns sensor JSON (8 fields)           | Monitoring only |
| POST   | `/api/wifi`   | Saves Wi‑Fi credentials, triggers restart| Admin Gate only |
| GET    | `/api/status` | `{ stage, ip, ssid }`                    | Admin Gate only |
| GET    | `/`           | Serves `admin.html` (provisioning)      | Admin Gate only |

## Flash Order
1. LittleFS (data files)
2. Firmware (`tank_triage.ino`)

## Config‑force
Hold GPIO0 (BOOT button) ≥ 2 seconds at power‑on to force Admin Gate.

## Removed References
All Flask, Python, and MQTT references have been removed from the project documentation.
