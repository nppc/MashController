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

constexpr float TARGET_1_C = 50.0f;
constexpr float TARGET_2_C = 70.0f;
constexpr float MIN_RISE_C = 15.0f;

constexpr unsigned long MAX_HEAT_MS = 120UL * 60UL * 1000UL;
constexpr unsigned long MAX_COAST_MS = 25UL * 60UL * 1000UL;
constexpr unsigned long SENSOR_FAIL_MS = 15UL * 1000UL;

// 160 min at one sample per 15-20 s mixer cycle, 4 bytes each.
constexpr uint16_t MAX_SAMPLES = 600;
constexpr uint16_t MAX_SAMPLES_PER_RESPONSE = 100;

enum Phase : uint8_t {
  PHASE_IDLE = 0,
  PHASE_HEATING_1 = 1,
  PHASE_COASTING_1 = 2,
  PHASE_HEATING_2 = 3,
  PHASE_COASTING_2 = 4,
  PHASE_COMPLETE = 5,
  PHASE_TIMEOUT = 6,
  PHASE_SENSOR_FAIL = 7,
  PHASE_STOPPED = 8,
  PHASE_LOG_FULL = 9
};

bool active = false;
bool heaterOn = false;
uint8_t phase = PHASE_IDLE;
uint16_t runId = 0;
const char *lastError = "";

float waterLiters = 20.0f;
float ambientC = NAN;
float currentTemp = NAN;

unsigned long startedAt = 0;
unsigned long lastValidAt = 0;
unsigned long finishedElapsedMs = 0;
int lastHistIndex = 0;
bool lastMixerOn = false;

// Step 1
unsigned long heatEndMs1 = 0;
bool peakSeen1 = false;
float peakTemp1 = 0.0f;
unsigned long peakCoastMs1 = 0;
uint8_t dropBlocks1 = 0;
unsigned long step2StartMs = 0;

// Step 2
unsigned long heatEndMs2 = 0;
bool peakSeen2 = false;
float peakTemp2 = 0.0f;
unsigned long peakCoastMs2 = 0;
uint8_t dropBlocks2 = 0;

float blockSum = 0.0f;
uint8_t blockCount = 0;
unsigned long blockFirstMs = 0;
unsigned long blockLastMs = 0;

uint16_t sampleCount = 0;
uint16_t* sampleTimeSec = nullptr;  // block midpoint, relative to startedAt
int16_t* sampleCentiC = nullptr;    // block mean, 1/100 C

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
  if (phase == PHASE_COASTING_1) {
    if (!peakSeen1 || mean > peakTemp1) {
      peakSeen1 = true;
      peakTemp1 = mean;
      peakCoastMs1 = midMs - heatEndMs1;
      dropBlocks1 = 0;
    } else if (peakSeen1 && mean <= peakTemp1 - 0.06f) {
      dropBlocks1++;
    }
  } else if (phase == PHASE_COASTING_2) {
    if (!peakSeen2 || mean > peakTemp2) {
      peakSeen2 = true;
      peakTemp2 = mean;
      peakCoastMs2 = midMs - heatEndMs2;
      dropBlocks2 = 0;
    } else if (peakSeen2 && mean <= peakTemp2 - 0.06f) {
      dropBlocks2++;
    }
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
  return calibrationStart(newWaterLiters, TARGET_1_C, storage.settings().heaterAmbientC);
}

bool calibrationStart(float newWaterLiters, float newAmbientC) {
  return calibrationStart(newWaterLiters, TARGET_1_C, newAmbientC);
}

bool calibrationStart(float newWaterLiters, float /*targetC*/, float newAmbientC) {
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
  if (!(newAmbientC >= -10.0f && newAmbientC <= 45.0f)) {
    lastError = "Room temperature must be -10-45 C";
    return false;
  }
  const float startTemp = lastGoodTemp;
  if (!isValidReading(startTemp)) {
    lastError = "Temperature reading is invalid";
    return false;
  }
  if (TARGET_1_C - startTemp < MIN_RISE_C) {
    lastError = "Water must start at least 15 C below 50 C (<= 35 C)";
    return false;
  }

  if (!sampleTimeSec)
      sampleTimeSec = new uint16_t[MAX_SAMPLES];

  if (!sampleCentiC)
      sampleCentiC = new int16_t[MAX_SAMPLES];

  if (!sampleTimeSec || !sampleCentiC) {
      delete[] sampleTimeSec;
      delete[] sampleCentiC;

      sampleTimeSec = nullptr;
      sampleCentiC = nullptr;

      lastError = "Out of memory";
      return false;
  }  

  waterLiters = newWaterLiters;
  ambientC = newAmbientC;
  currentTemp = startTemp;

  startedAt = millis();
  lastValidAt = startedAt;
  finishedElapsedMs = 0;
  lastHistIndex = histIndex;

  heatEndMs1 = 0;
  peakSeen1 = false;
  peakTemp1 = 0.0f;
  peakCoastMs1 = 0;
  dropBlocks1 = 0;
  step2StartMs = 0;

  heatEndMs2 = 0;
  peakSeen2 = false;
  peakTemp2 = 0.0f;
  peakCoastMs2 = 0;
  dropBlocks2 = 0;

  blockSum = 0.0f;
  blockCount = 0;
  sampleCount = 0;

  const uint16_t previousRunId = runId;
  do {
    runId = (uint16_t)ESP.random();
  } while (runId == 0 || runId == previousRunId);

  mixerManualMode = false;
  mixerOn = false;
  mixerPhaseStart = startedAt;
  lastMixerOn = false;

  addReading(startTemp, 0);

  phase = PHASE_HEATING_1;
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
    if ((phase == PHASE_HEATING_1 || phase == PHASE_HEATING_2) && elapsed >= MAX_HEAT_MS) {
      finish(PHASE_TIMEOUT);
    }
    return;
  }
  lastHistIndex = histIndex;

  const float reading = lastGoodTemp;
  if (!sensorOk || !isValidReading(reading)) return;
  lastValidAt = now;
  currentTemp = reading;

  // Step 1: heating reaches 50 C
  if (phase == PHASE_HEATING_1 && reading >= TARGET_1_C) {
    heaterOn = false;
    closeBlock();
    phase = PHASE_COASTING_1;
    heatEndMs1 = elapsed;
    peakSeen1 = false;
    peakTemp1 = reading;
    peakCoastMs1 = 0;
    dropBlocks1 = 0;
  }
  // Step 2: heating reaches 70 C
  else if (phase == PHASE_HEATING_2 && reading >= TARGET_2_C) {
    heaterOn = false;
    closeBlock();
    phase = PHASE_COASTING_2;
    heatEndMs2 = elapsed;
    peakSeen2 = false;
    peakTemp2 = reading;
    peakCoastMs2 = 0;
    dropBlocks2 = 0;
  }

  addReading(reading, elapsed);
  if (elapsed - blockFirstMs >= MAX_BLOCK_MS) closeBlock();

  if (sampleCount >= MAX_SAMPLES) {
    finish(PHASE_LOG_FULL);
    return;
  }

  if (phase == PHASE_COASTING_1) {
    const unsigned long coast1Ms = elapsed - heatEndMs1;
    // Advance to Step 2 when peak has been passed and temperature drop is confirmed (>= 3 drop blocks or 3 min after peak)
    const bool dropConfirmed1 = peakSeen1 && coast1Ms >= 180000UL &&
                               (dropBlocks1 >= 3 || (coast1Ms >= peakCoastMs1 + 180000UL) || coast1Ms >= 600000UL);
    if (dropConfirmed1) {
      closeBlock();
      step2StartMs = elapsed;
      phase = PHASE_HEATING_2;
      heaterOn = true;
      lastMixerOn = mixerOn;
      Serial.println("Calibration: Step 1 coast complete, starting Step 2 heating to 70 C");
    }
  } else if (phase == PHASE_COASTING_2) {
    const unsigned long coast2Ms = elapsed - heatEndMs2;
    const bool dropConfirmed2 = peakSeen2 && coast2Ms >= 180000UL &&
                               (dropBlocks2 >= 3 || (coast2Ms >= peakCoastMs2 + 180000UL) || coast2Ms >= 600000UL);
    if (dropConfirmed2 || coast2Ms >= MAX_COAST_MS) {
      finish(PHASE_COMPLETE);
      Serial.println("Calibration: Step 2 coast complete, calibration finished!");
    }
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
  out.reserve(700 + (size_t)(lastSample - firstSample) * 12);
  out += '{';
  appendInt(out, "runId", runId);
  appendBool(out, "active", active);
  appendBool(out, "completed", phase == PHASE_COMPLETE);
  appendInt(out, "phase", phase);
  appendBool(out, "heaterOn", heaterOn);
  appendInt(out, "elapsedSec", (long)(elapsedMs / 1000UL));
  appendNumber(out, "currentTemp", isValidReading(temp) ? temp : NAN, 2);
  appendNumber(out, "target1C", TARGET_1_C, 1);
  appendNumber(out, "target2C", TARGET_2_C, 1);
  appendNumber(out, "ambientC", isfinite(ambientC) ? ambientC : storage.settings().heaterAmbientC, 1);
  appendNumber(out, "waterLiters", waterLiters, 2);
  appendNumber(out, "heaterPowerW", s.heaterPowerW, 0);
  appendInt(out, "mixerCycleSec", (long)s.mixerOnSec + (long)s.mixerRestSec);

  // Step 1
  appendNumber(out, "heatEnd1Sec", heatEndMs1 > 0 ? heatEndMs1 / 1000.0f : NAN, 1);
  appendNumber(out, "peakTemp1", peakSeen1 ? peakTemp1 : NAN, 2);
  appendNumber(out, "peakCoast1Sec", peakSeen1 ? peakCoastMs1 / 1000.0f : NAN, 0);
  appendNumber(out, "step2StartSec", step2StartMs > 0 ? step2StartMs / 1000.0f : NAN, 1);

  // Step 2
  appendNumber(out, "heatEnd2Sec", heatEndMs2 > 0 ? heatEndMs2 / 1000.0f : NAN, 1);
  appendNumber(out, "peakTemp2", peakSeen2 ? peakTemp2 : NAN, 2);
  appendNumber(out, "peakCoast2Sec", peakSeen2 ? peakCoastMs2 / 1000.0f : NAN, 0);

  // Legacy fields for backward compatibility
  const float currentTarget = phase <= PHASE_COASTING_1 ? TARGET_1_C : TARGET_2_C;
  appendNumber(out, "targetC", currentTarget, 1);
  appendNumber(out, "heatEndSec", (phase <= PHASE_COASTING_1 ? heatEndMs1 : heatEndMs2) > 0 ? (phase <= PHASE_COASTING_1 ? heatEndMs1 : heatEndMs2) / 1000.0f : NAN, 1);
  appendNumber(out, "peakTemp", (phase <= PHASE_COASTING_1 ? peakSeen1 : peakSeen2) ? (phase <= PHASE_COASTING_1 ? peakTemp1 : peakTemp2) : NAN, 2);
  appendNumber(out, "peakCoastSec", (phase <= PHASE_COASTING_1 ? peakCoastMs1 : peakCoastMs2) ? (phase <= PHASE_COASTING_1 ? peakCoastMs1 : peakCoastMs2) / 1000.0f : NAN, 0);

  appendInt(out, "maxHeatSec", (long)(MAX_HEAT_MS / 1000UL));
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
