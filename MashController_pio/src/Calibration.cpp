#include "Calibration.h"

#include <ArduinoJson.h>

#include "MashProfile.h"
#include "Mixer.h"
#include "Sensor.h"
#include "Storage.h"

namespace {
constexpr unsigned long SAMPLE_MS = 60UL * 1000UL;
constexpr float HEAT_TARGET_C = 50.0f;
constexpr float COOLDOWN_DROP_C = 2.0f;
constexpr unsigned long MIN_COOLDOWN_MS = 60UL * 1000UL;
constexpr unsigned long MAX_RUN_MS = 30UL * 60UL * 1000UL;
constexpr uint8_t MAX_SAMPLES = 61;

bool active = false;
bool heaterOn = false;
bool completed = false;
uint8_t phase = 0;
uint8_t sampleCount = 0;
unsigned long startedAt = 0;
unsigned long finishedElapsedSec = 0;
unsigned long nextSampleAt = 0;
unsigned long heatEndSec = 0;
unsigned long coolingStartedAt = 0;
float peakTemperature = 0.0f;
float samples[MAX_SAMPLES] = {};
float waterLiters = 10.0f;

}

bool calibrationIsActive() {
  return active;
}

bool calibrationHeaterIsOn() {
  return heaterOn;
}

bool calibrationStart(float newWaterLiters) {
  if (active || isRunning || inCoolDown || !sensorFound) {
    if (!sensorFound) {
      Serial.println("Calibration refused: no temperature sensor");
    }
    return false;
  }
  if (newWaterLiters < 0.1f || newWaterLiters > 100.0f) return false;

  active = true;
  waterLiters = newWaterLiters;
  heaterOn = true;
  mixerManualMode = false;
  mixerOn = false;
  mixerPhaseStart = millis();
  completed = false;
  phase = 1;
  sampleCount = 0;
  startedAt = millis();
  finishedElapsedSec = 0;
  nextSampleAt = startedAt;
  heatEndSec = 0;
  coolingStartedAt = 0;
  peakTemperature = readTemperature();
  memset(samples, 0, sizeof(samples));
  return true;
}

void calibrationStop() {
  heaterOn = false;
  active = false;
  completed = false;
  mixerManualMode = true;
  mixerOn = false;
  finishedElapsedSec = (millis() - startedAt) / 1000UL;
}

void calibrationUpdate() {
  if (!active) return;

  if (!sensorFound) {
    heaterOn = false;
    active = false;
    completed = false;
    phase = 5;
    mixerManualMode = true;
    mixerOn = false;
    finishedElapsedSec = (millis() - startedAt) / 1000UL;
    Serial.println("Calibration aborted: temperature sensor unavailable");
    return;
  }

  const unsigned long now = millis();
  const unsigned long elapsed = now - startedAt;
  const float currentTemperature = readTemperature();

  if (phase == 1 && currentTemperature >= HEAT_TARGET_C) {
    heaterOn = false;
    phase = 2;
    heatEndSec = elapsed / 1000UL;
    coolingStartedAt = now;
    peakTemperature = currentTemperature;
  } else if (phase == 2) {
    if (currentTemperature > peakTemperature) peakTemperature = currentTemperature;
    if (now - coolingStartedAt >= MIN_COOLDOWN_MS &&
        currentTemperature <= peakTemperature - COOLDOWN_DROP_C) {
      // The threshold can be reached between the regular one-minute samples.
      // Keep the triggering reading so the UI reports the actual stop value.
      if (sampleCount < MAX_SAMPLES) {
        samples[sampleCount++] = currentTemperature;
      }
      phase = 3;
      active = false;
      completed = true;
      mixerManualMode = true;
      mixerOn = false;
      finishedElapsedSec = elapsed / 1000UL;
    }
  }

  if (now >= nextSampleAt && sampleCount < MAX_SAMPLES) {
    samples[sampleCount++] = currentTemperature;
    nextSampleAt += SAMPLE_MS;
  }

  if (elapsed >= MAX_RUN_MS) {
    heaterOn = false;
    active = false;
    phase = 4;
    completed = false;
    mixerManualMode = true;
    mixerOn = false;
    finishedElapsedSec = elapsed / 1000UL;
  }
}

void calibrationStatusJson(String &out) {
  DynamicJsonDocument doc(4096);
  const unsigned long elapsed = active
                                    ? millis() - startedAt
                                    : finishedElapsedSec * 1000UL;

  doc["active"] = active;
  doc["heaterOn"] = heaterOn;
  doc["elapsedSec"] = elapsed / 1000UL;
  doc["maxDurationSec"] = MAX_RUN_MS / 1000UL;
  doc["sampleIntervalSec"] = SAMPLE_MS / 1000UL;
  doc["maxSamples"] = MAX_SAMPLES;
  doc["heatTargetC"] = HEAT_TARGET_C;
  doc["cooldownDropC"] = COOLDOWN_DROP_C;
  doc["minCooldownSec"] = MIN_COOLDOWN_MS / 1000UL;
  doc["heatEndSec"] = heatEndSec;
  doc["peakTemp"] = peakTemperature;
  doc["cooldownTarget"] = peakTemperature - COOLDOWN_DROP_C;
  doc["phase"] = phase;
  doc["completed"] = completed;
  doc["currentTemp"] = readTemperature();
  doc["heaterPowerW"] = storage.settings().heaterPowerW;
  doc["waterLiters"] = waterLiters;

  JsonArray sampleArray = doc.createNestedArray("samples");
  for (uint8_t i = 0; i < sampleCount; i++) {
    JsonObject sample = sampleArray.createNestedObject();
    sample["timeSec"] = (unsigned long)i * SAMPLE_MS / 1000UL;
    sample["temp"] = samples[i];
  }

  serializeJson(doc, out);
}
