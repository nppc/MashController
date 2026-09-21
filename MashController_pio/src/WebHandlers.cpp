#include "WebHandlers.h"

#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <Updater.h>

#include "Storage.h"
#include "Sensor.h"
#include "Heater.h"
#include "Calibration.h"
#include "Mixer.h"
#include "MashProfile.h"

ESP8266WebServer server(80);
bool otaEnabled = false;

namespace {
bool otaStarted = false;
unsigned long otaActivatedAt = 0;
constexpr unsigned long OTA_TIMEOUT_MS = 5UL * 60UL * 1000UL;
bool updateFailed = false;
bool updateReceived = false;

bool isFilesystemImage(const String &filename) {
  String lowerName = filename;
  lowerName.toLowerCase();
  return lowerName.indexOf("littlefs") >= 0 || lowerName.indexOf("filesystem") >= 0;
}

size_t filesystemSize() {
  FSInfo info;
  return LittleFS.info(info) ? info.totalBytes : 0;
}
}

/* -------------------------------------------------------------------------- */
/*                               FILE HANDLING                                */
/* -------------------------------------------------------------------------- */

static void handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";

  String contentType = "text/html";
  if (path.endsWith(".css")) contentType = "text/css";
  if (path.endsWith(".js")) contentType = "application/javascript";
  if (path.endsWith(".json")) contentType = "application/json";

  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    if (path == "/ota.html") {
      server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
      server.sendHeader("Pragma", "no-cache");
    }
    server.streamFile(file, contentType);
    file.close();
    return;
  }

  server.send(404, "text/plain", "Not found");
}

/* -------------------------------------------------------------------------- */
/*                         PROFILES (REST API, EEPROM)                        */
/* -------------------------------------------------------------------------- */

static void handleGetProfiles() {
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("profiles");

  for (uint8_t i = 0; i < storage.profileCount(); i++) {
    ProfileEE p;
    if (!storage.getProfile(i, p)) continue;

    JsonObject o = arr.createNestedObject();
    o["name"] = p.name;
    o["waterMassKg"] = p.waterMassKg;
    o["grainMassKg"] = p.grainMassKg;

    JsonArray steps = o.createNestedArray("steps");
    for (uint8_t s = 0; s < p.stepCount; s++) {
      JsonObject st = steps.createNestedObject();
      st["temp"] = p.steps[s].temp;
      st["time"] = p.steps[s].timeMin;
    }
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleSaveProfiles() {
  String body = server.arg("plain");
  Serial.println("---- /saveProfiles ----");

  if (body.length() == 0) {
    server.send(400, "text/plain", "No JSON received");
    return;
  }

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "JSON parse error");
    return;
  }

  JsonArray arr = doc["profiles"].as<JsonArray>();
  if (arr.isNull()) {
    server.send(400, "text/plain", "Missing 'profiles' array");
    return;
  }

  if (arr.size() > STORAGE_MAX_PROFILES) {
    server.send(400, "text/plain", "Too many profiles (max " + String(STORAGE_MAX_PROFILES) + ")");
    return;
  }

  // Whole-list replace, mirrors the old file-overwrite behaviour but now on EEPROM.
  storage.clearProfiles();

  for (JsonObject p : arr) {
    ProfileEE pe;
    memset(&pe, 0, sizeof(pe));

    const char *name = p["name"] | "Profile";
    strncpy(pe.name, name, STORAGE_NAME_LEN - 1);
    pe.waterMassKg = max(p["waterMassKg"].as<float>(), 0.1f);
    pe.grainMassKg = max(p["grainMassKg"].as<float>(), 0.0f);

    JsonArray steps = p["steps"].as<JsonArray>();
    pe.stepCount = min((int)steps.size(), (int)STORAGE_MAX_STEPS);

    for (uint8_t i = 0; i < pe.stepCount; i++) {
      pe.steps[i].temp    = steps[i]["temp"].as<float>();
      pe.steps[i].timeMin = steps[i]["time"].as<int>();
    }

    storage.setProfile(storage.profileCount(), pe);
  }

  if (!storage.save()) {
    server.send(500, "text/plain", "EEPROM write error");
    return;
  }

  Serial.println("Profiles saved to EEPROM");
  server.send(200, "text/plain", "Saved");
}

static void handleCalibrationStart() {
  const float waterLiters = server.arg("liters").toFloat();
  if (waterLiters < 0.1f || waterLiters > 100.0f) {
    server.send(400, "text/plain", "Water volume must be between 0.1 and 100 litres");
    return;
  }
  if (!calibrationStart(waterLiters)) {
    server.send(409, "text/plain", "Controller is busy");
    return;
  }
  server.send(200, "text/plain", "Calibration started");
}

static void handleCalibrationStop() {
  calibrationStop();
  server.send(200, "text/plain", "Calibration stopped");
}

static void handleCalibrationStatus() {
  String out;
  calibrationStatusJson(out);
  server.send(200, "application/json", out);
}

/* -------------------------------------------------------------------------- */
/*                         SETTINGS (REST API, EEPROM)                        */
/* -------------------------------------------------------------------------- */

static void handleGetSettings() {
  SettingsEE &s = storage.settings();
  StaticJsonDocument<384> doc;

  doc["wifiSSID"] = s.wifiSSID;
  // Password is never echoed back - only whether one is currently set.
  doc["wifiPassSet"] = strlen(s.wifiPass) > 0;
  doc["heaterTransferCoeff"] = s.heaterTransferCoeff;
  doc["heaterThermalMass"] = s.heaterThermalMass;
  doc["heaterPowerW"] = s.heaterPowerW;
  doc["heaterDeadband"] = s.heaterDeadband;
  doc["heaterMinSwitchSec"] = s.heaterMinSwitchSec;
  doc["mixerOnSec"] = s.mixerOnSec;
  doc["mixerRestSec"] = s.mixerRestSec;
  doc["coolDownSec"] = s.coolDownSec;

  // Read-only DS18B20 info
  doc["sensorAddress"] = sensorAddressToString();
  doc["sensorFound"] = sensorFound;
  doc["sensorTempAtRead"] = lastGoodTemp;
  doc["sensorOk"] = sensorOk;
#ifdef DEBUG_FAKE_TEMP
  doc["sensorDebugFake"] = true;
#else
  doc["sensorDebugFake"] = false;
#endif

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleSaveSettings() {
  String body = server.arg("plain");
  if (body.length() == 0) {
    server.send(400, "text/plain", "No JSON received");
    return;
  }

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "JSON parse error");
    return;
  }

  SettingsEE &s = storage.settings();

  if (doc.containsKey("wifiSSID")) {
    strncpy(s.wifiSSID, doc["wifiSSID"] | s.wifiSSID, STORAGE_SSID_LEN - 1);
    s.wifiSSID[STORAGE_SSID_LEN - 1] = '\0';
  }
  // Only overwrite the stored password if a non-empty one was actually sent,
  // so the frontend can leave the password field blank to "keep current".
  if (doc.containsKey("wifiPass") && strlen(doc["wifiPass"] | "") > 0) {
    const char *password = doc["wifiPass"] | "";
    if (strlen(password) < 8) {
      server.send(400, "text/plain", "WiFi password must be at least 8 characters");
      return;
    }
    strncpy(s.wifiPass, doc["wifiPass"], STORAGE_PASS_LEN - 1);
    s.wifiPass[STORAGE_PASS_LEN - 1] = '\0';
  }
  if (doc.containsKey("heaterTransferCoeff")) {
    s.heaterTransferCoeff = max(doc["heaterTransferCoeff"].as<float>(), 0.1f);
  }
  if (doc.containsKey("heaterThermalMass")) {
    s.heaterThermalMass = max(doc["heaterThermalMass"].as<float>(), 1.0f);
  }
  if (doc.containsKey("heaterPowerW")) {
    s.heaterPowerW = max(doc["heaterPowerW"].as<float>(), 1.0f);
  }
  if (doc.containsKey("heaterDeadband")) {
    s.heaterDeadband = doc["heaterDeadband"].as<float>();
  }
  if (doc.containsKey("heaterMinSwitchSec")) {
    s.heaterMinSwitchSec = doc["heaterMinSwitchSec"].as<uint16_t>();
  }
  if (doc.containsKey("mixerOnSec")) {
    s.mixerOnSec = doc["mixerOnSec"].as<uint8_t>();
  }
  if (doc.containsKey("mixerRestSec")) {
    s.mixerRestSec = doc["mixerRestSec"].as<uint8_t>();
  }

  if (doc.containsKey("coolDownSec")) {
    s.coolDownSec = doc["coolDownSec"].as<uint16_t>();
  }

  if (!storage.save()) {
    server.send(500, "text/plain", "EEPROM write error");
    return;
  }

  // wifiSSID/wifiPass only take effect on next boot since WiFi.softAP()
  // already ran in setup() by the time this request arrives.
  server.send(200, "text/plain", "Saved (WiFi changes apply after reboot)");
}

static void handleRediscoverSensor() {
#ifdef DEBUG_FAKE_TEMP
  server.send(200, "application/json", "{\"sensorFound\":true,\"sensorAddress\":\"DEBUG-FAKE\"}");
  return;
#else
  // Don't rediscover mid-conversion - wait for the current read cycle to finish
  if (conversionInProgress) {
    server.send(409, "text/plain", "Sensor read in progress, try again shortly");
    return;
  }

  sensorFound = discoverSensorAddress();

  if (sensorFound) {
    sensors.setResolution(sensorAddr, 12);
    server.send(200, "application/json",
      "{\"sensorFound\":true,\"sensorAddress\":\"" + sensorAddressToString() + "\"}");
  } else {
    server.send(200, "application/json", "{\"sensorFound\":false}");
  }
#endif
}

/* -------------------------------------------------------------------------- */
/*                               API ENDPOINTS                                */
/* -------------------------------------------------------------------------- */

static void handleData() {
  String json = "{\"temps\":[";
  int count = min(histIndex, HISTORY_SIZE);
  int start = (histIndex > HISTORY_SIZE) ? histIndex % HISTORY_SIZE : 0;

  for (int i = 0; i < count; i++) {
    int idx = (start + i) % HISTORY_SIZE;
    json += String(tempHistory[idx], 1);
    if (i < count - 1) json += ",";
  }

  json += "],\"target\":" + String(targetTemperature, 1) + "}";
  server.send(200, "application/json", json);
}

static void handleStartProfile() {
  if (calibrationIsActive()) {
    server.send(409, "text/plain", "Calibration is running");
    return;
  }

  int idx = server.arg("profile").toInt();

  if (!loadProfile(idx)) {
    server.send(500, "text/plain", "Profile load error");
    return;
  }

  currentStep = 0;
  isRunning = true;
  isPaused = false;
  waitingForUser = false;
  inCoolDown = false;

  targetTemperature = activeProfile.steps[0].temp;
  heaterResetThermalModel(readTemperature(), activeProfile.waterMassKg);

  // Do NOT start timer yet
  waitingForTemp = true;
  stepDurationSec = activeProfile.steps[0].time * 60;
  pausedElapsedSec = 0;

  // Hand the mixer to auto, starting a fresh rest phase, regardless of
  // whatever mode/state it was left in before the mash started.
  mixerManualMode = false;
  mixerOn = false;
  mixerPhaseStart = millis();

  server.send(200, "text/plain", "Started");
}

static void handleStopProfile() {
  isRunning = false;
  isPaused = false;
  waitingForUser = false;
  waitingForTemp = false;
  targetTemperature = 20.0;
  heaterOn = false;

  // Mixer shouldn't keep cycling with nothing being mashed.
  mixerManualMode = true;
  mixerOn = false;

  inCoolDown = false;         // EXIT COOL-DOWN IF ACTIVE

  server.send(200, "text/plain", "Stopped");
}

static void handlePauseProfile() {
  if (isRunning && !isPaused) {
    if (!waitingForTemp) {
      unsigned long elapsed = (millis() - stepStartTime) / 1000;
      pausedElapsedSec = elapsed;
    }
    isPaused = true;

    if (waitingForTemp || waitingForUser) {
      // Heating and automatic mixing are stopped before the timer starts.
      mixerManualMode = true;
      mixerOn = false;
    } else {
      // During a timed hold, pause only the timer and keep temperature
      // control and automatic mixing active.
      mixerManualMode = false;
      mixerOn = false;
      mixerPhaseStart = millis();
    }
  }
  server.send(200, "text/plain", "Paused");
}

static void handleResumeProfile() {
  if (isRunning && isPaused) {
    if (waitingForUser) {
      if (currentStep == 0) {
        heaterIncludeGrain(activeProfile.grainMassKg);
      }
      waitingForUser = false;
      isPaused = false;
      advanceStep();
    } else {
      if (!waitingForTemp) {
        stepStartTime = millis() - (pausedElapsedSec * 1000);
      }
      isPaused = false;
    }

    if (isRunning) {
      mixerManualMode = false;
      mixerOn = false;
      mixerPhaseStart = millis();
    }
  }
  server.send(200, "text/plain", "Resumed");
}

static void handleSkipStep() {
  if (isRunning && !waitingForUser) {
    advanceStep();
  }
  server.send(200, "text/plain", "Skipped");
}

static void handleMixerMode() {
  String mode = server.arg("mode");

  if (mode == "manual") {
    mixerManualMode = true;
    // Freeze the relay in its current state; the badge becomes a toggle.
  } else if (mode == "auto") {
    mixerManualMode = false;
    // Resume the on/rest cycle from a fresh phase boundary rather than
    // wherever manual mode happened to leave the relay.
    mixerPhaseStart = millis();
  } else {
    server.send(400, "text/plain", "mode must be 'auto' or 'manual'");
    return;
  }

  server.send(200, "text/plain", "OK");
}

static void handleMixerToggle() {
  if (!mixerManualMode) {
    server.send(409, "text/plain", "Mixer is in auto mode");
    return;
  }

  mixerOn = !mixerOn;
  mixerPhaseStart = millis();
  server.send(200, "text/plain", mixerOn ? "On" : "Off");
}

static void handleEnableOta() {
  if (!otaStarted) {
    ArduinoOTA.begin();
    otaStarted = true;
  }

  otaActivatedAt = millis();
  otaEnabled = true;
  handleFileRead("/ota.html");
}

static void handleUpdateUpload() {
  if (!otaEnabled) return;

  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    updateFailed = false;
    updateReceived = false;

    const bool filesystem = server.arg("type") == "filesystem" ||
                isFilesystemImage(upload.filename);
    const int command = filesystem ? U_FS : U_FLASH;
    const size_t maxUpdateSize = filesystem
                                   ? filesystemSize()
                     : (ESP.getFreeSketchSpace() - 0x1000);

    Serial.printf("OTA upload started: %s (%s)\n",
                  upload.filename.c_str(), filesystem ? "LittleFS" : "firmware");

    if (!Update.begin(maxUpdateSize, command)) {
      Update.printError(Serial);
      updateFailed = true;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!updateFailed && Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      updateFailed = true;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!updateFailed && !Update.end(true)) {
      Update.printError(Serial);
      updateFailed = true;
    }
    updateReceived = true;
    Serial.printf("OTA upload finished: %u bytes\n", upload.totalSize);
  }
}

static void handleUpdateResult() {
  if (!otaEnabled) {
    server.send(403, "text/plain", "OTA is not enabled");
    return;
  }

  if (!updateReceived || updateFailed || Update.hasError()) {
    server.send(500, "text/plain", "Update failed");
    return;
  }

  server.send(200, "text/plain", "Update successful. Restarting...");
  delay(100);
  ESP.restart();
}

static void handleStatus() {
  StaticJsonDocument<512> doc;

  doc["running"] = isRunning;
  doc["paused"] = isPaused;
  doc["waitingForUser"] = waitingForUser;
  doc["profileName"] = activeProfile.name;
  doc["waterMassKg"] = activeProfile.waterMassKg;
  doc["grainMassKg"] = activeProfile.grainMassKg;
  doc["step"] = currentStep;
  doc["stepTemp"] = targetTemperature;
  doc["currentTemp"] = readTemperature();
  const bool holdTemperatureWhilePaused =
    isPaused && !waitingForTemp && !waitingForUser;
  doc["heaterOn"] = heaterOn && isRunning && !inCoolDown &&
                     (!isPaused || holdTemperatureWhilePaused);

  doc["mixerOn"] = mixerOn;
  doc["mixerMode"] = mixerManualMode ? "manual" : "auto";
  doc["mixerRemaining"] = mixerManualMode ? 0 : mixerRemainingSec();

  if (isRunning) {

      if (waitingForTemp) {
          // Timer has NOT started yet
          doc["remaining"] = stepDurationSec;   // full time
          doc["waiting"] = true;
      } else if (isPaused) {
          // Timer is frozen at the banked elapsed value
          long remaining = stepDurationSec - pausedElapsedSec;
          if (remaining < 0) remaining = 0;

          doc["remaining"] = remaining;
          doc["waiting"] = false;
      } else {
          // Timer is running
          unsigned long elapsed = (millis() - stepStartTime) / 1000;
          long remaining = stepDurationSec - elapsed;
          if (remaining < 0) remaining = 0;

          doc["remaining"] = remaining;
          doc["waiting"] = false;
      }
  }

  // Cool-down status
  doc["coolDownActive"] = inCoolDown;
  if (inCoolDown) {
    SettingsEE &s = storage.settings();
    unsigned long coolDownElapsed = (millis() - coolDownStart) / 1000;
    long remaining = s.coolDownSec - coolDownElapsed;
    if (remaining < 0) remaining = 0;
    doc["coolDownRemaining"] = remaining;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

/* -------------------------------------------------------------------------- */
/*                                  ROUTES                                    */
/* -------------------------------------------------------------------------- */

void webHandlersInit() {
  server.on("/", []() { handleFileRead("/index.html"); });
  server.on("/index.html", []() { handleFileRead("/index.html"); });
  server.on("/style.css", []() { handleFileRead("/style.css"); });
  server.on("/script.js", []() { handleFileRead("/script.js"); });
  server.on("/chart.js", []() { handleFileRead("/chart.js"); });
  server.on("/ota.html", []() { handleFileRead("/ota.html"); });
  server.on("/calibration.html", []() { handleFileRead("/calibration.html"); });
  server.on("/calibration.js", []() { handleFileRead("/calibration.js"); });
  server.on("/enable-ota", HTTP_GET, handleEnableOta);
  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdateUpload);

  server.on("/profiles_data.json", HTTP_GET, handleGetProfiles);
  server.on("/saveProfiles", HTTP_POST, handleSaveProfiles);

  server.on("/settings", HTTP_GET, handleGetSettings);
  server.on("/saveSettings", HTTP_POST, handleSaveSettings);
  server.on("/rediscoverSensor", HTTP_POST, handleRediscoverSensor);

  server.on("/data", handleData);
  server.on("/startProfile", HTTP_POST, handleStartProfile);
  server.on("/stopProfile", HTTP_POST, handleStopProfile);
  server.on("/pauseProfile", HTTP_POST, handlePauseProfile);
  server.on("/resumeProfile", HTTP_POST, handleResumeProfile);
  server.on("/skipStep", HTTP_POST, handleSkipStep);
  server.on("/mixerMode", HTTP_POST, handleMixerMode);
  server.on("/mixerToggle", HTTP_POST, handleMixerToggle);
  server.on("/status", handleStatus);
  server.on("/calibrationStart", HTTP_POST, handleCalibrationStart);
  server.on("/calibrationStop", HTTP_POST, handleCalibrationStop);
  server.on("/calibrationStatus", HTTP_GET, handleCalibrationStatus);
}

void otaUpdate() {
  if (!otaEnabled) return;

  if (millis() - otaActivatedAt >= OTA_TIMEOUT_MS) {
    otaEnabled = false;
    Serial.println("OTA window expired");
    return;
  }

  ArduinoOTA.handle();
}
