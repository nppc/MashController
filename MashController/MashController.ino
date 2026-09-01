#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

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
  float hysteresis = 0.5;  // prevents rapid toggling

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
  File file = LittleFS.open("/profiles_data.json", "r");
  if (!file) {
    Serial.println("Failed to open profiles_data.json");
    return false;
  }

  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();

  if (err) {
    Serial.println("JSON parse error");
    return false;
  }

  JsonArray arr = doc["profiles"].as<JsonArray>();
  if (index < 0 || index >= arr.size()) {
    Serial.println("Profile index out of range");
    return false;
  }

  JsonObject p = arr[index];

  activeProfile.name = p["name"].as<String>();

  JsonArray steps = p["steps"].as<JsonArray>();
  activeProfile.stepCount = steps.size();

  for (int i = 0; i < activeProfile.stepCount; i++) {
    activeProfile.steps[i].temp = steps[i]["temp"].as<float>();
    activeProfile.steps[i].time = steps[i]["time"].as<int>();
  }

  Serial.println("Profile loaded: " + activeProfile.name);
  return true;
}

/* -------------------------------------------------------------------------- */
/*                           SAVE PROFILES (REST API)                         */
/* -------------------------------------------------------------------------- */

void handleSaveProfiles() {
  String body = server.arg("plain");
  Serial.println("---- /saveProfiles ----");

  if (body.length() == 0) {
    server.send(400, "text/plain", "No JSON received");
    return;
  }

  File file = LittleFS.open("/profiles_data.json", "w");
  if (!file) {
    server.send(500, "text/plain", "File write error");
    Serial.println("Failed to open profiles_data.json for writing");
    return;
  }

  file.print(body);
  file.close();

  Serial.println("profiles_data.json updated");
  server.send(200, "text/plain", "Saved");
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

  WiFi.softAP("MashController", "12345678");

  server.on("/", []() { handleFileRead("/index.html"); });
  server.on("/index.html", []() { handleFileRead("/index.html"); });
  server.on("/style.css", []() { handleFileRead("/style.css"); });
  server.on("/script.js", []() { handleFileRead("/script.js"); });
  server.on("/chart.js", []() { handleFileRead("/chart.js"); });
  server.on("/profiles_data.json", []() { handleFileRead("/profiles_data.json"); });

  server.on("/saveProfiles", HTTP_POST, handleSaveProfiles);
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
