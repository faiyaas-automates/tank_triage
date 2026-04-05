1. Complete, runnable code only. No placeholders. No "add your logic here".
2. FIRMWARE: Arduino IDE 2.3 only. NOT ESP-IDF. NOT FreeRTOS. NOT PlatformIO.
   Use Arduino.h, analogRead(), Serial.println() — nothing else.
3. DASHBOARD: Plain HTML + vanilla JS + inline CSS only.
   NO React. NO Vue. NO TypeScript. NO Tailwind. NO Bootstrap. NO CDN JS libs.
   fetch() is allowed. Google Fonts <link> is allowed. That is all.
4. PYTHON: flask + pyserial only. No SQLite. No async. No Celery. No Redis.
5. Firmware = single .ino file. Dashboard = app.py + templates/dashboard.html.
6. No WiFi in firmware. Serial JSON only at 115200 baud.
7. Tamil messages hardcoded per tier. No LLM API call. No external service.
8. Serial port read from env var SERIAL_PORT. Silent fail if port missing — no crash.
9. use_reloader=False in app.run always.
10. Comments explain WHY not WHAT.