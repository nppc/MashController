#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESP8266NetBIOS.h>
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
  s.wifiApSSID[STORAGE_SSID_LEN - 1] = '\0';
  s.wifiApPass[STORAGE_PASS_LEN - 1] = '\0';
  s.wifiStaSSID[STORAGE_SSID_LEN - 1] = '\0';
  s.wifiStaPass[STORAGE_PASS_LEN - 1] = '\0';

  if (strlen(s.wifiApSSID) == 0) {
    strncpy(s.wifiApSSID, "MashController", STORAGE_SSID_LEN - 1);
    s.wifiApSSID[STORAGE_SSID_LEN - 1] = '\0';
  }

  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);

  // Set device hostname so router DHCP assigns "mash" in local DNS
  WiFi.hostname("mash");

  bool connectedSta = false;
  if (s.wifiMode == WIFI_MODE_STA && strlen(s.wifiStaSSID) > 0) {
    // Pure Station mode: connect to external router, keep AP off
    WiFi.softAPdisconnect(true);
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);

    Serial.print("Connecting to external WiFi: ");
    Serial.println(s.wifiStaSSID);

    if (strlen(s.wifiStaPass) > 0) {
      WiFi.begin(s.wifiStaSSID, s.wifiStaPass);
    } else {
      WiFi.begin(s.wifiStaSSID);
    }

    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      connectedSta = true;
      Serial.print("WiFi connected to router! Station IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println("Warning: could not connect to router within 15s. Falling back to Local AP...");
    }
  }

  if (!connectedSta) {
    // Pure Local AP mode (either selected, or fallback if router connection failed)
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    Serial.println("Starting WiFi in Local AP mode...");

    const size_t passwordLength = strlen(s.wifiApPass);
    if (passwordLength == 0) {
      WiFi.softAP(s.wifiApSSID);
    } else if (passwordLength >= 8) {
      WiFi.softAP(s.wifiApSSID, s.wifiApPass);
    } else {
      Serial.println("WiFi AP password is too short; starting open AP");
      WiFi.softAP(s.wifiApSSID);
    }

    Serial.print("AP SSID: ");
    Serial.println(s.wifiApSSID);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // NetBIOS: Allows Windows PC to resolve http://mash directly without mDNS
  NBNS.begin("mash");

  if (MDNS.begin("mash")) {
    Serial.println("mDNS responder started: http://mash.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("ERROR: mDNS responder failed to start");
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
  MDNS.update();
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
