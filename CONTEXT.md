PROJECT: Tank Water Triage System
HARDWARE:
  - ESP32 WROOM-32 30-pin
  - GPIO32 = TDS sensor (analog, max 2.3V, direct)
  - GPIO33 = Turbidity RKI-5163 (analog, 4.5V max → 10kΩ+10kΩ divider → 2.25V)
  - GPIO26 = BC547 NPN power switch (HIGH=ON, LOW=OFF)
  - Power: dual 18650 via VIN (7.4V)
OS: Fedora KDE — Linux only commands
FIRMWARE: Arduino C++, Arduino IDE 2.3, ESP32 Dev Module board, /dev/ttyUSB0 115200
DASHBOARD: ESP32 AsyncWebServer + LittleFS. File: data/index.html. Phone browser over hotspot.
NETWORK: STA only. ESP32 joins phone hotspot. Dashboard at ESP32 DHCP IP. MDNS: tanktriage.local
TRIAGE: Four labels — CLEAN(green) / TURBID(yellow) / HIGH-TDS(blue) / BOTH(red)
THRESHOLDS: THETA_TDS=80ppm delta, THETA_TURB=0.4V delta
BASELINE: BASELINE_N=30 readings × 500ms at boot
DEMO: 3 bottles — S1 clean(Bisleri), S2 turbid(tap+2drops milk), S3 high-TDS(tap+1tsp salt)
OUTPUT: tank_triage.ino + data/index.html (uploaded to LittleFS separately before firmware)
ARCHIVE: Legacy app.py, mock_esp32.py, templates/ preserved in _archive/ (historical reference only)
