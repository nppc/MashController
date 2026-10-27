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
unsigned long lastWifiCheck = 0;
unsigned long lastWifiReconnectAttempt = 0;
unsigned long stationConnectStarted = 0;
bool stationModeActive = false;
bool wifiWasConnected = false;
bool mdnsStarted = false;

void setMdnsEnabled(bool enabled) {
  if (enabled) {
    if (!mdnsStarted) {
      NBNS.begin("mash");
      if (MDNS.begin("mash")) {
        mdnsStarted = true;
        MDNS.addService("http", "tcp", 80);
        Serial.println("mDNS and NetBIOS responder started: http://mash.local and http://mash");
      } else {
        Serial.println("ERROR: mDNS responder failed to start");
      }
    }
  } else {
    NBNS.end();
    if (mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
      Serial.println("mDNS and NetBIOS responders stopped");
    }
  }
}

static void startLocalAccessPoint() {
  SettingsEE &s = storage.settings();
  const size_t passwordLength = strlen(s.wifiApPass);
  bool started;
  if (passwordLength == 0 || passwordLength < 8) {
    if (passwordLength > 0) {
      Serial.println("WiFi AP password is too short; starting open AP");
    }
    started = WiFi.softAP(s.wifiApSSID);
  } else {
    started = WiFi.softAP(s.wifiApSSID, s.wifiApPass);
  }

  if (!started) {
    Serial.println("ERROR: failed to start local WiFi AP");
    return;
  }

  Serial.print("AP SSID: ");
  Serial.println(s.wifiApSSID);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());
}

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
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setOutputPower(20.5f);

  // Set device hostname so router DHCP assigns "mash" in local DNS
  WiFi.hostname("mash");

  stationModeActive = s.wifiMode == WIFI_MODE_STA && strlen(s.wifiStaSSID) > 0;
  if (stationModeActive) {
    // Pure Station mode: connect to external router, keep AP off
    WiFi.softAPdisconnect(true);
    WiFi.disconnect();
    WiFi.mode(WIFI_STA);
    WiFi.config(0u, 0u, 0u);

    Serial.print("Connecting to external WiFi: ");
    Serial.println(s.wifiStaSSID);

    if (strlen(s.wifiStaPass) > 0) {
      WiFi.begin(s.wifiStaSSID, s.wifiStaPass);
    } else {
      WiFi.begin(s.wifiStaSSID);
    }
    stationConnectStarted = millis();
    lastWifiReconnectAttempt = millis();

    unsigned long startAttemptTime = millis();
    while ((WiFi.status() != WL_CONNECTED ||
            WiFi.localIP() == IPAddress(0, 0, 0, 0)) &&
           millis() - startAttemptTime < 15000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED &&
        WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      wifiWasConnected = true;
      Serial.print("WiFi connected to router! Station IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("Connected AP BSSID: ");
      Serial.println(WiFi.BSSIDstr());
      Serial.print("Default gateway: ");
      Serial.println(WiFi.gatewayIP());
    } else {
      Serial.println("Warning: could not connect within 15s; continuing STA retries before AP fallback.");
    }
  }

  if (!stationModeActive) {
    // Pure Local AP mode when selected or when station credentials are missing.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    Serial.println("Starting WiFi in Local AP mode...");
    startLocalAccessPoint();
  }

  lastWifiCheck = millis();

  if (s.mdnsEnabled) {
    setMdnsEnabled(true);
  } else {
    Serial.println("mDNS and NetBIOS responders disabled in settings");
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
  if (mdnsStarted) MDNS.update();
  server.handleClient();
  otaUpdate();
  calibrationUpdate();

  if (stationModeActive && millis() - lastWifiCheck >= 3000UL) {
    lastWifiCheck = millis();
    const bool connected =
        WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);

    if (connected && !wifiWasConnected) {
      wifiWasConnected = true;
      Serial.print("WiFi reconnected. Station IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("Connected AP BSSID: ");
      Serial.println(WiFi.BSSIDstr());
      Serial.print("Default gateway: ");
      Serial.println(WiFi.gatewayIP());
      if (storage.settings().mdnsEnabled && !mdnsStarted) {
        setMdnsEnabled(true);
      }
    } else if (!connected && wifiWasConnected) {
      wifiWasConnected = false;
      Serial.println("WiFi connection lost; waiting for reconnect");
    }

    if (!connected && WiFi.getMode() == WIFI_STA &&
        millis() - stationConnectStarted >= 120000UL) {
      Serial.println("No external WiFi after 2 minutes; starting local AP while continuing STA retries");
      WiFi.mode(WIFI_AP_STA);
      startLocalAccessPoint();
      if (storage.settings().mdnsEnabled && !mdnsStarted) {
        setMdnsEnabled(true);
      }
    }

    if (!connected && millis() - lastWifiReconnectAttempt >= 60000UL) {
      lastWifiReconnectAttempt = millis();
      Serial.println("WiFi disconnected: attempting reconnect");
      WiFi.reconnect();
    }
  }

  // Keep the physical outputs in sync with heaterOn/mixerOn every pass,
  // regardless of which code path last changed them.
  applyOutputs();

  if (sensorUsingFakeMode()) {
    // Keep simulated-time samples at the real sensor cadence as the model
    // speeds up by reducing the wall-clock interval between fake readings.
    static unsigned long lastFakeSensorRead = 0;
    const unsigned long fakeReadIntervalMs =
        max(10UL, (unsigned long)(READ_INTERVAL_MS / sensorSimulationTimeScale()));
    if (millis() - lastFakeSensorRead >= fakeReadIntervalMs) {
      lastFakeSensorRead = millis();
      sensorUpdate();
    }
  } else {
    // Real sensor: async conversion state machine, never blocks the web server.
    sensorUpdate();
  }

  // COOL-DOWN COUNTDOWN: Check frequently, independent of READ_INTERVAL
  // This ensures responsive STOP button and precise timing
  mashProfileCheckCoolDown();

  // MASH TIMING: Only runs every READ_INTERVAL_MS
  if (millis() - lastRead > READ_INTERVAL_MS) {
    lastRead = millis();

    updateMixer();
    mashProfileTick();
  }
}
