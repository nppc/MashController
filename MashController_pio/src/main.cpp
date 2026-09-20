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
#include "Calibration.h"
#include "DoubleReset.h"

/* ---- Timing ---- */
unsigned long lastRead = 0;

/* -------------------------------------------------------------------------- */
/*                                   SETUP                                    */
/* -------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting...");
  Serial.print("Reset reason: ");
  Serial.println(ESP.getResetReason());
  Serial.print("Free heap at boot: ");
  Serial.println(ESP.getFreeHeap());

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
  Serial.print("Free heap after storage: ");
  Serial.println(ESP.getFreeHeap());

  if (doubleReset) {
    Serial.println("Double reset detected: using default AP credentials");
    resetApCredentialsToDefaults();
  }

  SettingsEE &s = storage.settings();
  s.wifiSSID[STORAGE_SSID_LEN - 1] = '\0';
  s.wifiPass[STORAGE_PASS_LEN - 1] = '\0';

  if (strlen(s.wifiSSID) == 0) {
    strncpy(s.wifiSSID, "MashController", STORAGE_SSID_LEN - 1);
    s.wifiSSID[STORAGE_SSID_LEN - 1] = '\0';
  }

  WiFi.mode(WIFI_AP);
  WiFi.persistent(false);
  Serial.println("Starting WiFi AP...");

  bool apStarted = false;
  const size_t passwordLength = strlen(s.wifiPass);
  if (passwordLength == 0) {
    apStarted = WiFi.softAP(s.wifiSSID);
  } else if (passwordLength >= 8) {
    apStarted = WiFi.softAP(s.wifiSSID, s.wifiPass);
  } else {
    Serial.println("WiFi AP password is too short; starting open AP");
    apStarted = WiFi.softAP(s.wifiSSID);
  }

  if (apStarted) {
    Serial.print("WiFi AP started: ");
    Serial.println(s.wifiSSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: WiFi AP failed to start");
  }

  Serial.println("Registering HTTP handlers...");
  webHandlersInit();
  server.begin();
  Serial.println("HTTP server started");
}

/* -------------------------------------------------------------------------- */
/*                                    LOOP                                    */
/* -------------------------------------------------------------------------- */

void loop() {
  server.handleClient();
  otaUpdate();
  calibrationUpdate();

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
