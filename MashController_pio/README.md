# Migrating MashController to PlatformIO

## What's already done here
- `MashController.ino` → `src/main.cpp` (added an explicit `#include <Arduino.h>`
  — PlatformIO doesn't auto-add it or auto-generate function prototypes the
  way the Arduino IDE does for `.ino` files, but since everything is already
  broken into headers, that one include is the only change needed).
- All other `.cpp` files copied as-is into `src/`.
- All `.h` files copied as-is into `include/`.
- `platformio.ini` written for a Wemos D1 mini (`espressif8266` / `d1_mini`),
  with `lib_deps` for ArduinoJson, OneWire, and DallasTemperature. Everything
  else (ESP8266WiFi, ESP8266WebServer, LittleFS) ships with the framework.

## What you still need to add
1. **`Storage.h` / `Storage.cpp`** — copy your existing versions into
   `include/Storage.h` and `src/Storage.cpp`. Nothing about them needs to
   change; they were never touched in this project.
2. **Your web assets** (`index.html`, `style.css`, `script.js`, `chart.js`)
   — put these in the `data/` folder at the project root (already created,
   currently empty). PlatformIO uploads that folder to LittleFS with:
   ```
   pio run --target uploadfs
   ```
   This replaces the Arduino IDE's "ESP8266 LittleFS Data Upload" tool.

## Building and uploading
- Build: `pio run`
- Upload firmware: `pio run --target upload`
- Upload filesystem (web assets): `pio run --target uploadfs`
- Serial monitor: `pio device monitor` (or `pio run --target monitor`)

If you're using VS Code with the PlatformIO extension, all of these are also
buttons in the PlatformIO sidebar (checkmark = build, arrow = upload, etc.).

## One thing to double check
I couldn't actually run a build in my sandbox (no network access to the
PlatformIO package registry from here), so please do a first `pio run`
yourself before flashing. If it errors, send me the exact message — most
likely candidates would be a library version mismatch or a missing
`Storage.h`/`Storage.cpp` (see step 1 above).
