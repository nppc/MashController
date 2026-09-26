#include "Calibration.h"

#include <math.h>

#include "MashProfile.h"
#include "Mixer.h"
#include "Sensor.h"
#include "Storage.h"

// Heater calibration: heat plain water to the target at full power, switch off
// and log the coast (overshoot, peak and start of cooling). The page fits the
// coast curve to find the heater time constant, effective power, store gain
// and heat loss.

namespace {
// Raw readings (one every READ_INTERVAL_MS) are averaged per mixer cycle: a
// block closes when the mixer starts a new run, which cancels the mixing
// ripple. Blocks shorter than MIN_BLOCK_MS are merged with the next cycle;
// MAX_BLOCK_MS is a fallback when the mixer is not cycling.
constexpr unsigned long MIN_BLOCK_MS = 15UL * 1000UL;
constexpr unsigned long MAX_BLOCK_MS = 60UL * 1000UL;

constexpr float DEFAULT_TARGET_C = 65.0f;
const float DEFAULT_AMBIENT_C = storage.settings().heaterAmbientC;
constexpr float MIN_TARGET_C = 40.0f;
constexpr float MAX_TARGET_C = 80.0f;
constexpr float MIN_RISE_C = 15.0f;

constexpr unsigned long MAX_HEAT_MS = 120UL * 60UL * 1000UL;
// After switch-off, keep logging until the peak is at least PEAK_SETTLE_MS old
// and the coast has lasted MIN_COAST_MS and 2.5x the time to peak.
constexpr unsigned long MIN_COAST_MS = 10UL * 60UL * 1000UL;
constexpr unsigned long PEAK_SETTLE_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long MAX_COAST_MS = 40UL * 60UL * 1000UL;
constexpr unsigned long SENSOR_FAIL_MS = 15UL * 1000UL;

// 160 min at one sample per 15-20 s mixer cycle, 4 bytes each.
constexpr uint16_t MAX_SAMPLES = 600;
constexpr uint16_t MAX_SAMPLES_PER_RESPONSE = 100;

enum Phase : uint8_t {
  PHASE_IDLE = 0,
  PHASE_HEATING = 1,
  PHASE_COASTING = 2,
  PHASE_COMPLETE = 3,
  PHASE_TIMEOUT = 4,
  PHASE_SENSOR_FAIL = 5,
  PHASE_STOPPED = 6,
  PHASE_LOG_FULL = 7
};

bool active = false;
bool heaterOn = false;
uint8_t phase = PHASE_IDLE;
uint16_t runId = 0;
const char *lastError = "";

float waterLiters = 20.0f;
float targetC = DEFAULT_TARGET_C;
float ambientC = DEFAULT_AMBIENT_C;
float currentTemp = NAN;

unsigned long startedAt = 0;
unsigned long lastValidAt = 0;
unsigned long finishedElapsedMs = 0;
int lastHistIndex = 0;
bool lastMixerOn = false;

bool heatEnded = false;
unsigned long heatEndMs = 0;  // relative to startedAt

bool peakSeen = false;
float peakTemp = 0.0f;
unsigned long peakCoastMs = 0;  // relative to heatEndMs

float blockSum = 0.0f;
uint8_t blockCount = 0;
unsigned long blockFirstMs = 0;
unsigned long blockLastMs = 0;

uint16_t sampleCount = 0;
uint16_t sampleTimeSec[MAX_SAMPLES];  // block midpoint, relative to startedAt
int16_t sampleCentiC[MAX_SAMPLES];    // block mean, 1/100 C

bool isValidReading(float t) {
  // 85.0 is the DS18B20 power-on value; -127 means disconnected.
  return isfinite(t) && t > -20.0f && t < 105.0f && t != 85.0f;
}

void closeBlock() {
  if (blockCount == 0) return;
  const float mean = blockSum / blockCount;
  const unsigned long midMs = (blockFirstMs + blockLastMs) / 2UL;
  if (sampleCount < MAX_SAMPLES) {
    sampleTimeSec[sampleCount] = (uint16_t)((midMs + 500UL) / 1000UL);
    sampleCentiC[sampleCount] = (int16_t)lroundf(mean * 100.0f);
    sampleCount++;
  }
  if (phase == PHASE_COASTING && (!peakSeen || mean > peakTemp)) {
    peakSeen = true;
    peakTemp = mean;
    peakCoastMs = midMs - heatEndMs;
  }
  blockSum = 0.0f;
  blockCount = 0;
}

void addReading(float reading, unsigned long elapsed) {
  if (blockCount == 0) blockFirstMs = elapsed;
  blockLastMs = elapsed;
  blockSum += reading;
  blockCount++;
}

void finish(uint8_t endPhase) {
  heaterOn = false;
  closeBlock();
  active = false;
  phase = endPhase;
  mixerManualMode = true;
  mixerOn = false;
  finishedElapsedMs = millis() - startedAt;
}

void appendKey(String &out, const char *key) {
  out += '"';
  out += key;
  out += "\":";
}

void appendNumber(String &out, const char *key, float value, unsigned int decimals) {
  appendKey(out, key);
  if (isfinite(value)) {
    out += String(value, decimals);
  } else {
    out += "null";
  }
  out += ',';
}

void appendInt(String &out, const char *key, long value) {
  appendKey(out, key);
  out += value;
  out += ',';
}

void appendBool(String &out, const char *key, bool value) {
  appendKey(out, key);
  out += value ? "true" : "false";
  out += ',';
}

}  // namespace

bool calibrationIsActive() {
  return active;
}

bool calibrationHeaterIsOn() {
  return heaterOn;
}

const char *calibrationLastError() {
  return lastError;
}

bool calibrationStart(float newWaterLiters) {
  return calibrationStart(newWaterLiters, DEFAULT_TARGET_C, DEFAULT_AMBIENT_C);
}

bool calibrationStart(float newWaterLiters, float newTargetC, float newAmbientC) {
  lastError = "";
  if (active || isRunning || inCoolDown) {
    lastError = "Controller is busy";
    return false;
  }
  if (!sensorFound || !sensorOk || histIndex == 0) {
    lastError = "No temperature sensor reading";
    Serial.println("Calibration refused: no temperature sensor reading");
    return false;
  }
  if (!(newWaterLiters >= 0.1f && newWaterLiters <= 100.0f)) {
    lastError = "Water volume must be 0.1-100 litres";
    return false;
  }
  if (!(newTargetC >= MIN_TARGET_C && newTargetC <= MAX_TARGET_C)) {
    lastError = "Target must be 40-80 C";
    return false;
  }
  if (!(newAmbientC >= -10.0f && newAmbientC <= 45.0f)) {
    lastError = "Room temperature must be -10-45 C";
    return false;
  }
  const float startTemp = lastGoodTemp;
  if (!isValidReading(startTemp)) {
    lastError = "Temperature reading is invalid";
    return false;
  }
  if (newTargetC - startTemp < MIN_RISE_C) {
    lastError = "Water must start at least 15 C below the target";
    return false;
  }

  waterLiters = newWaterLiters;
  targetC = newTargetC;
  ambientC = newAmbientC;
  currentTemp = startTemp;

  startedAt = millis();
  lastValidAt = startedAt;
  finishedElapsedMs = 0;
  lastHistIndex = histIndex;
  heatEnded = false;
  heatEndMs = 0;
  peakSeen = false;
  peakTemp = 0.0f;
  peakCoastMs = 0;
  blockSum = 0.0f;
  blockCount = 0;
  sampleCount = 0;
  // Random, so a page left open across a reboot never mixes two runs.
  const uint16_t previousRunId = runId;
  do {
    runId = (uint16_t)ESP.random();
  } while (runId == 0 || runId == previousRunId);

  // Mixer runs its normal automatic cycle, the same as during brewing.
  mixerManualMode = false;
  mixerOn = false;
  mixerPhaseStart = startedAt;
  lastMixerOn = false;

  // The first reading is the one already taken; log it as the start point.
  addReading(startTemp, 0);

  phase = PHASE_HEATING;
  heaterOn = true;
  active = true;
  return true;
}

void calibrationStop() {
  if (!active) return;
  finish(PHASE_STOPPED);
}

void calibrationUpdate() {
  if (!active) return;

  const unsigned long now = millis();
  const unsigned long elapsed = now - startedAt;

  // A new mixer run starts a new block, once the block covers a full cycle.
  if (mixerOn && !lastMixerOn && blockCount > 0 &&
      elapsed - blockFirstMs >= MIN_BLOCK_MS) {
    closeBlock();
  }
  lastMixerOn = mixerOn;

  if (histIndex == lastHistIndex) {
    if (now - lastValidAt >= SENSOR_FAIL_MS) {
      Serial.println("Calibration aborted: temperature sensor unavailable");
      finish(PHASE_SENSOR_FAIL);
      return;
    }
    if (phase == PHASE_HEATING && elapsed >= MAX_HEAT_MS) finish(PHASE_TIMEOUT);
    return;
  }
  lastHistIndex = histIndex;

  const float reading = lastGoodTemp;
  if (!sensorOk || !isValidReading(reading)) return;
  lastValidAt = now;
  currentTemp = reading;

  if (phase == PHASE_HEATING && reading >= targetC) {
    heaterOn = false;
    closeBlock();  // the partial block belongs to the heating phase
    phase = PHASE_COASTING;
    heatEnded = true;
    heatEndMs = elapsed;
  }

  addReading(reading, elapsed);
  if (elapsed - blockFirstMs >= MAX_BLOCK_MS) closeBlock();

  if (sampleCount >= MAX_SAMPLES) {
    finish(PHASE_LOG_FULL);
    return;
  }

  if (phase == PHASE_HEATING && elapsed >= MAX_HEAT_MS) {
    finish(PHASE_TIMEOUT);
  } else if (phase == PHASE_COASTING) {
    const unsigned long coastMs = elapsed - heatEndMs;
    const bool settled = peakSeen &&
                         coastMs - peakCoastMs >= PEAK_SETTLE_MS &&
                         coastMs >= MIN_COAST_MS &&
                         coastMs >= peakCoastMs * 5UL / 2UL;
    if (settled || coastMs >= MAX_COAST_MS) finish(PHASE_COMPLETE);
  }
}

void calibrationStatusJson(String &out, uint16_t firstSample) {
  const SettingsEE &s = storage.settings();
  const unsigned long elapsedMs = active ? millis() - startedAt : finishedElapsedMs;
  const float temp = active ? currentTemp : readTemperature();

  if (firstSample > sampleCount) firstSample = sampleCount;
  uint16_t lastSample = firstSample + MAX_SAMPLES_PER_RESPONSE;
  if (lastSample > sampleCount) lastSample = sampleCount;

  out = "";
  out.reserve(640 + (size_t)(lastSample - firstSample) * 12);
  out += '{';
  appendInt(out, "runId", runId);
  appendBool(out, "active", active);
  appendBool(out, "completed", phase == PHASE_COMPLETE);
  appendInt(out, "phase", phase);
  appendBool(out, "heaterOn", heaterOn);
  appendInt(out, "elapsedSec", (long)(elapsedMs / 1000UL));
  appendNumber(out, "currentTemp", isValidReading(temp) ? temp : NAN, 2);
  appendNumber(out, "targetC", targetC, 1);
  appendNumber(out, "ambientC", ambientC, 1);
  appendNumber(out, "waterLiters", waterLiters, 2);
  appendNumber(out, "heaterPowerW", s.heaterPowerW, 0);
  appendInt(out, "mixerCycleSec", (long)s.mixerOnSec + (long)s.mixerRestSec);
  appendNumber(out, "heatEndSec", heatEnded ? heatEndMs / 1000.0f : NAN, 1);
  appendNumber(out, "peakTemp", peakSeen ? peakTemp : NAN, 3);
  appendNumber(out, "peakCoastSec", peakSeen ? peakCoastMs / 1000.0f : NAN, 0);
  appendInt(out, "maxHeatSec", (long)(MAX_HEAT_MS / 1000UL));
  appendInt(out, "minCoastSec", (long)(MIN_COAST_MS / 1000UL));
  appendInt(out, "peakSettleSec", (long)(PEAK_SETTLE_MS / 1000UL));
  appendInt(out, "maxCoastSec", (long)(MAX_COAST_MS / 1000UL));
  appendInt(out, "maxSamples", MAX_SAMPLES);
  appendInt(out, "sampleCount", sampleCount);
  appendInt(out, "samplesFrom", firstSample);

  out += "\"t\":[";
  for (uint16_t i = firstSample; i < lastSample; i++) {
    if (i != firstSample) out += ',';
    out += sampleTimeSec[i];
  }
  out += "],\"T\":[";
  for (uint16_t i = firstSample; i < lastSample; i++) {
    if (i != firstSample) out += ',';
    out += String(sampleCentiC[i] / 100.0f, 2);
  }
  out += "]}";
}
