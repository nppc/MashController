#include "Heater.h"

#include <math.h>

#include "HeaterModel.h"
#include "MashProfile.h"   // targetTemperature
#include "Outputs.h"
#include "Sensor.h"
#include "Storage.h"

// On/off control that accounts for the heat stored in the element and pot
// base. The heater switches off as soon as the stored heat is enough to carry
// the water to the target, and back on when even the predicted peak would
// fall below target - deadband. Calibrated values come from the Heater
// Calibration page.

bool heaterOn = false;

namespace {
constexpr float WATER_SPECIFIC_HEAT = 4186.0f;
constexpr float GRAIN_SPECIFIC_HEAT = 1900.0f;
constexpr float MIN_AVERAGE_SEC = 4.0f;
constexpr float MAX_AVERAGE_SEC = 120.0f;

HeaterModel model;
float capacityJPerC = 20.0f * WATER_SPECIFIC_HEAT;
bool modelReady = false;
bool outputWasOn = false;
unsigned long previousUpdateAt = 0;
unsigned long lastSwitchAt = 0;
bool hasSwitched = false;
float predictedPeak = NAN;

// Average over one full mixer cycle so the mixing ripple cancels out.
float averageWindowSec(const SettingsEE &s) {
  const float cycle = (float)s.mixerOnSec + (float)s.mixerRestSec;
  return constrain(cycle, MIN_AVERAGE_SEC, MAX_AVERAGE_SEC);
}
}

void heaterResetThermalModel(float /*initialTemperature*/, float waterMassKg) {
  capacityJPerC = max(waterMassKg, 0.1f) * WATER_SPECIFIC_HEAT;
  model.reset();
  previousUpdateAt = millis();
  modelReady = true;
  outputWasOn = false;
  heaterOn = false;
  lastSwitchAt = 0;
  hasSwitched = false;
  predictedPeak = NAN;
}

void heaterIncludeGrain(float grainMassKg) {
  if (!modelReady) return;
  capacityJPerC += max(grainMassKg, 0.0f) * GRAIN_SPECIFIC_HEAT;
}

float heaterPredictedPeak() {
  return predictedPeak;
}

void updateHeater() {
  if (!isRunning || inCoolDown) {
    heaterOn = false;
    modelReady = false;
    hasSwitched = false;
    predictedPeak = NAN;
    return;
  }

  SettingsEE &s = storage.settings();
  const unsigned long now = millis();

  if (!modelReady) {
    heaterResetThermalModel(readTemperature(), activeProfile.waterMassKg);
  }

  // Advance the stored-heat model with what the SSR actually did since the
  // last reading (pause can hold the output off while heaterOn is true).
  model.configure(s.heaterPowerEffW, s.heaterTauSec, s.heaterStoreGain);
  const float dtSec = min(now - previousUpdateAt, 10000UL) / 1000.0f;
  model.update(dtSec, outputWasOn);
  previousUpdateAt = now;

  const float windowSec = averageWindowSec(s);
  const float averageC = averageTemperature(windowSec);
  const float lossW = max(s.heaterLossWPerC, 0.0f) * (averageC - s.heaterAmbientC);

  // The average lags by half its window; bring it forward with the rate the
  // model says the water is changing at right now.
  const float rateCPerSec = (model.flowW() - lossW) / capacityJPerC;
  const float waterNowC = averageC + rateCPerSec * windowSec * 0.5f;
  predictedPeak = model.predictPeak(waterNowC, capacityJPerC, lossW);

  const bool switchLocked =
      hasSwitched &&
      (now - lastSwitchAt < (unsigned long)s.heaterMinSwitchSec * 1000UL);

  if (!switchLocked) {
    if (!heaterOn && predictedPeak < targetTemperature - s.heaterDeadband) {
      heaterOn = true;
      lastSwitchAt = now;
      hasSwitched = true;
    } else if (heaterOn && predictedPeak >= targetTemperature) {
      heaterOn = false;
      lastSwitchAt = now;
      hasSwitched = true;
    }
  }

  outputWasOn = heaterOutputActive();
}
