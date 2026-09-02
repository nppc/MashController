#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "Storage.h"

ESP8266WebServer server(80);

/* ---- Temperature history ---- */
#define HISTORY_SIZE 180
float tempHistory[HISTORY_SIZE];
int histIndex = 0;

/* ---- Mash profile execution ---- */
bool isRunning = false;
bool isPaused = false;
int currentStep = 0;
bool waitingForTemp = false;

bool heaterOn = false;

unsigned long stepStartTime = 0;
unsigned long stepDurationSec = 0;
unsigned long pausedElapsedSec = 0;   // elapsed time banked when paused

float targetTemperature = 20.0;   // unified target temperature

struct Step {
  float temp;
  int time; // minutes
};

struct Profile {
  String name;
  Step steps[6];
  int stepCount;
};

Profile activeProfile;

/* ---- Timing ---- */
unsigned long lastRead = 0;
const unsigned long READ_INTERVAL_MS = 2000;

/* -------------------------------------------------------------------------- */
/*                               HEATER HANDLING                              */
/* -------------------------------------------------------------------------- */

void updateHeater(float currentTemp) {
  float hysteresis = storage.settings().heaterHysteresis;

  if (!heaterOn && currentTemp < targetTemperature - hysteresis) {
    heaterOn = true;
  }
  else if (heaterOn && currentTemp > targetTemperature + hysteresis) {
    heaterOn = false;
  }
}


/* -------------------------------------------------------------------------- */
/*                               FILE HANDLING                                */
/* -------------------------------------------------------------------------- */

void handleFileRead(String path) {
  if (path.endsWith("/")) path += "index.html";

  String contentType = "text/html";
  if (path.endsWith(".css")) contentType = "text/css";
  if (path.endsWith(".js")) contentType = "application/javascript";
  if (path.endsWith(".json")) contentType = "application/json";

  if (LittleFS.exists(path)) {
    File file = LittleFS.open(path, "r");
    server.streamFile(file, contentType);
    file.close();
    return;
  }

  server.send(404, "text/plain", "Not found");
}

/* -------------------------------------------------------------------------- */
/*                               PROFILE LOADING                              */
/* -------------------------------------------------------------------------- */

bool loadProfile(int index) {
  ProfileEE p;
  if (!storage.getProfile(index, p)) {
    Serial.println("Profile index out of range");
    return false;
  }

  activeProfile.name = String(p.name);
  activeProfile.stepCount = p.stepCount;

  for (int i = 0; i < activeProfile.stepCount; i++) {
    activeProfile.steps[i].temp = p.steps[i].temp;
    activeProfile.steps[i].time = p.steps[i].timeMin;
  }

  Serial.println("Profile loaded: " + activeProfile.name);
  return true;
}

/* -------------------------------------------------------------------------- */
/*                         PROFILES (REST API, EEPROM)                        */
/* -------------------------------------------------------------------------- */

void handleGetProfiles() {
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("profiles");

  for (uint8_t i = 0; i < storage.profileCount(); i++) {
    ProfileEE p;
    if (!storage.getProfile(i, p)) continue;

    JsonObject o = arr.createNestedObject();
    o["name"] = p.name;

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

void handleSaveProfiles() {
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

/* -------------------------------------------------------------------------- */
/*                         SETTINGS (REST API, EEPROM)                        */
/* -------------------------------------------------------------------------- */

void handleGetSettings() {
  SettingsEE &s = storage.settings();
  StaticJsonDocument<256> doc;

  doc["wifiSSID"] = s.wifiSSID;
  // Password is never echoed back - only whether one is currently set.
  doc["wifiPassSet"] = strlen(s.wifiPass) > 0;
  doc["heaterHysteresis"] = s.heaterHysteresis;
  doc["mixerDurationSec"] = s.mixerDurationSec;
  doc["mixerOnSec"] = s.mixerOnSec;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSaveSettings() {
  String body = server.arg("plain");
  if (body.length() == 0) {
    server.send(400, "text/plain", "No JSON received");
    return;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "text/plain", "JSON parse error");
    return;
  }

  SettingsEE &s = storage.settings();

  if (doc.containsKey("wifiSSID")) {
    strncpy(s.wifiSSID, doc["wifiSSID"] | s.wifiSSID, STORAGE_SSID_LEN - 1);
  }
  // Only overwrite the stored password if a non-empty one was actually sent,
  // so the frontend can leave the password field blank to "keep current".
  if (doc.containsKey("wifiPass") && strlen(doc["wifiPass"] | "") > 0) {
    strncpy(s.wifiPass, doc["wifiPass"], STORAGE_PASS_LEN - 1);
  }
  if (doc.containsKey("heaterHysteresis")) {
    s.heaterHysteresis = doc["heaterHysteresis"].as<float>();
  }
  if (doc.containsKey("mixerDurationSec")) {
    s.mixerDurationSec = doc["mixerDurationSec"].as<uint16_t>();
  }
  if (doc.containsKey("mixerPercent")) {
    s.mixerOnSec = doc["mixerOnSec"].as<uint8_t>();
  }

  if (!storage.save()) {
    server.send(500, "text/plain", "EEPROM write error");
    return;
  }

  // wifiSSID/wifiPass only take effect on next boot since WiFi.softAP()
  // already ran in setup() by the time this request arrives.
  server.send(200, "text/plain", "Saved (WiFi changes apply after reboot)");
}

/* -------------------------------------------------------------------------- */
/*                           TEMPERATURE SIMULATION                           */
/* -------------------------------------------------------------------------- */

float readTemperature(void)
{
    float last = (histIndex > 0)
                 ? tempHistory[(histIndex - 1) % HISTORY_SIZE]
                 : 20.0f;

    updateHeater(last);

    const float dt = (float)READ_INTERVAL_MS / 1000.0f;

    /* Physical parameters */
    const float heatingRate = 0.35f;  /* °C/s at full power */
    const float coolingRate = 0.20f;  /* °C/s */
    const float heatLoss = 0.10f;     /* stored heat lost per update */

    static float waterTemp = 20.0f;
    static float storedHeat = 0.0f;

    float heatInput;

    /* Heater startup ramp */
    static int heaterRamp = 0;

    if (heaterOn) {
        if (heaterRamp < 3)
            heaterRamp++;
    } else {
        heaterRamp = 0;
    }

    /*
     * Add energy while heater is ON.
     */
    if (heaterOn) {
        float rampFactor = 0.4f + 0.2f * heaterRamp;

        heatInput = heatingRate * rampFactor * dt;

        storedHeat += heatInput;
    }

    /*
     * Stored heat is released gradually.
     *
     * This is what creates thermal inertia:
     * the heater can switch OFF while storedHeat
     * is still positive, so the water keeps warming.
     */
    if (storedHeat > 0.0f) {
        float released;

        released = storedHeat * heatLoss;

        /* Don't release more heat than is stored */
        if (released > storedHeat)
            released = storedHeat;

        storedHeat -= released;
        waterTemp += released;
    }

    /*
     * Natural cooling.
     */
    if (waterTemp > 20.0f) {
        waterTemp -= coolingRate * dt;

        if (waterTemp < 20.0f)
            waterTemp = 20.0f;
    }

    /* Small measurement noise */
    waterTemp += ((float)random(-5, 6)) / 100.0f;

    /* Round to 0.1 °C */
    waterTemp = roundf(waterTemp * 10.0f) / 10.0f;

    return waterTemp;
}


void addTemp(float t) {
  tempHistory[histIndex % HISTORY_SIZE] = t;
  histIndex++;
}

/* -------------------------------------------------------------------------- */
/*                            STEP ADVANCE HELPER                             */
/* -------------------------------------------------------------------------- */

void advanceStep() {
  currentStep++;

  if (currentStep >= activeProfile.stepCount) {
    isRunning = false;
    isPaused = false;
    waitingForTemp = false;
    targetTemperature = 20.0;
    Serial.println("Profile finished, reset to default");
  } else {
    targetTemperature = activeProfile.steps[currentStep].temp;
    stepDurationSec = activeProfile.steps[currentStep].time * 60;
    waitingForTemp = true;
    pausedElapsedSec = 0;
  }
}

/* -------------------------------------------------------------------------- */
/*                               API ENDPOINTS                                */
/* -------------------------------------------------------------------------- */

void handleData() {
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

void handleStartProfile() {
  int idx = server.arg("profile").toInt();

  if (!loadProfile(idx)) {
    server.send(500, "text/plain", "Profile load error");
    return;
  }

  currentStep = 0;
  isRunning = true;
  isPaused = false;

  targetTemperature = activeProfile.steps[0].temp;

  // Do NOT start timer yet
  waitingForTemp = true;
  stepDurationSec = activeProfile.steps[0].time * 60;
  pausedElapsedSec = 0;

  server.send(200, "text/plain", "Started");
}

void handleStopProfile() {
  isRunning = false;
  isPaused = false;
  waitingForTemp = false;
  targetTemperature = 20.0;   // <<< RESET
  server.send(200, "text/plain", "Stopped");
}

void handlePauseProfile() {
  if (isRunning && !isPaused) {
    if (!waitingForTemp) {
      unsigned long elapsed = (millis() - stepStartTime) / 1000;
      pausedElapsedSec = elapsed;
    }
    isPaused = true;
  }
  server.send(200, "text/plain", "Paused");
}

void handleResumeProfile() {
  if (isRunning && isPaused) {
    if (!waitingForTemp) {
      stepStartTime = millis() - (pausedElapsedSec * 1000);
    }
    isPaused = false;
  }
  server.send(200, "text/plain", "Resumed");
}

void handleSkipStep() {
  if (isRunning) {
    advanceStep();
  }
  server.send(200, "text/plain", "Skipped");
}

void handleStatus() {
  StaticJsonDocument<256> doc;

  doc["running"] = isRunning;
  doc["paused"] = isPaused;
  doc["profileName"] = activeProfile.name;
  doc["step"] = currentStep;
  doc["stepTemp"] = targetTemperature;
  doc["currentTemp"] = readTemperature();
  doc["heaterOn"] = heaterOn;

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

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

/* -------------------------------------------------------------------------- */
/*                                   SETUP                                    */
/* -------------------------------------------------------------------------- */

void setup() {
  Serial.begin(115200);
  Serial.println("\nBooting...");

  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
  }

  // Loads settings + profiles from EEPROM. Writes defaults on first boot
  // or if the stored data fails its CRC check.
  storage.begin();

  SettingsEE &s = storage.settings();
  WiFi.softAP(s.wifiSSID, s.wifiPass);

  server.on("/", []() { handleFileRead("/index.html"); });
  server.on("/index.html", []() { handleFileRead("/index.html"); });
  server.on("/style.css", []() { handleFileRead("/style.css"); });
  server.on("/script.js", []() { handleFileRead("/script.js"); });
  server.on("/chart.js", []() { handleFileRead("/chart.js"); });

  server.on("/profiles_data.json", HTTP_GET, handleGetProfiles);
  server.on("/saveProfiles", HTTP_POST, handleSaveProfiles);

  server.on("/settings", HTTP_GET, handleGetSettings);
  server.on("/saveSettings", HTTP_POST, handleSaveSettings);

  server.on("/data", handleData);
  server.on("/startProfile", HTTP_POST, handleStartProfile);
  server.on("/stopProfile", HTTP_POST, handleStopProfile);
  server.on("/pauseProfile", HTTP_POST, handlePauseProfile);
  server.on("/resumeProfile", HTTP_POST, handleResumeProfile);
  server.on("/skipStep", HTTP_POST, handleSkipStep);
  server.on("/status", handleStatus);

  server.begin();
  Serial.println("HTTP server started");
}

/* -------------------------------------------------------------------------- */
/*                                    LOOP                                    */
/* -------------------------------------------------------------------------- */

void loop() {
  server.handleClient();

  if (millis() - lastRead > READ_INTERVAL_MS) {
    lastRead = millis();

    float t = readTemperature();
    addTemp(t);

    if (isRunning && !isPaused) {
      float currentTemp = t;

      // If waiting for temperature → check if we reached it
      if (waitingForTemp) {
          if (currentTemp >= targetTemperature - 0.5) {   // tolerance
              waitingForTemp = false;
              stepStartTime = millis();                   // NOW start timer
              Serial.println("Step timer started");
          }
      }
      else {
          // Timer is running normally
          unsigned long elapsed = (millis() - stepStartTime) / 1000;

          if (elapsed >= stepDurationSec) {
              advanceStep();
          }
      }

    }
  }
}
