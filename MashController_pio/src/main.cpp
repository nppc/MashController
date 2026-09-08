#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Storage.h"

#include "Config.h"
#include "Sensor.h"
#include "Heater.h"
#include "Mixer.h"
#include "Outputs.h"
#include "MashProfile.h"
#include "WebHandlers.h"
#include "DoubleReset.h"

/* ---- Timing ---- */
unsigned long lastRead = 0;

/* -------------------------------------------------------------------------- */
/*                                   SETUP                                    */
/* -------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting...");

  const bool doubleReset = detectDoubleReset();

  // Set output pins to their OFF states as early as possible.
  outputsInit();

  // Safe default on startup: mixer manual, off.
  mixerInit();

  sensorInit();

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  }

  // Loads settings + profiles from EEPROM. Writes defaults on first boot
  // or if the stored data fails its CRC check.
  storage.begin();

  if (doubleReset) {
    Serial.println("Double reset detected: using default AP credentials");
    resetApCredentialsToDefaults();
  }

  SettingsEE &s = storage.settings();
  if (strlen(s.wifiPass) == 0) {
    WiFi.softAP(s.wifiSSID);
  } else {
    WiFi.softAP(s.wifiSSID, s.wifiPass);
  }

  webHandlersInit();
  server.begin();
  Serial.println("HTTP server started");
}

/* -------------------------------------------------------------------------- */
/*                                    LOOP                                    */
/* -------------------------------------------------------------------------- */

void loop() {
  server.handleClient();

  // Keep the physical outputs in sync with heaterOn/mixerOn every pass,
  // regardless of which code path last changed them.
  applyOutputs();

#ifndef DEBUG_FAKE_TEMP
  // Real sensor: async conversion state machine, never blocks the web server.
  // Paces itself against CONVERSION_TIME_MS internally, so call every iteration.
  sensorUpdate();
#endif

  // COOL-DOWN COUNTDOWN: Check frequently, independent of READ_INTERVAL
  // This ensures responsive STOP button and precise timing
  mashProfileCheckCoolDown();

  // MASH TIMING: Only runs every READ_INTERVAL_MS
  if (millis() - lastRead > READ_INTERVAL_MS) {
    lastRead = millis();

#ifdef DEBUG_FAKE_TEMP
    // Fake sensor: recompute the simulated model on this same cadence,
    // same as the original blocking code did.
    sensorUpdate();
#endif

    updateMixer();
    mashProfileTick();
  }
}
