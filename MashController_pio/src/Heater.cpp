#include "Heater.h"
#include "Storage.h"
#include "MashProfile.h"   // targetTemperature

bool heaterOn = false;

namespace {
float previousTemperature = 0.0f;
float filteredTemperature = 0.0f;
float filteredTemperatureRate = 0.0f;
float previousTarget = 0.0f;
unsigned long previousSampleAt = 0;
unsigned long lastSwitchAt = 0;
bool haveSample = false;
bool haveFilteredTemperature = false;
bool hasSwitched = false;
constexpr float TEMP_FILTER_ALPHA = 0.35f;
constexpr float RATE_FILTER_ALPHA = 0.25f;
}

void updateHeater(float currentTemp) {
  if (!isRunning || inCoolDown) {
    heaterOn = false;
    haveSample = false;
    haveFilteredTemperature = false;
    hasSwitched = false;
    return;
  }

  SettingsEE &settings = storage.settings();
  const unsigned long now = millis();

  if (!haveFilteredTemperature) {
    filteredTemperature = currentTemp;
    haveFilteredTemperature = true;
  } else {
    filteredTemperature += TEMP_FILTER_ALPHA *
                           (currentTemp - filteredTemperature);
  }

  if (!haveSample || previousTarget != targetTemperature) {
    previousTemperature = filteredTemperature;
    previousTarget = targetTemperature;
    previousSampleAt = now;
    filteredTemperatureRate = 0.0f;
    haveSample = true;
  } else {
    const unsigned long elapsedMs = now - previousSampleAt;
    if (elapsedMs > 0) {
      const float elapsedSec = elapsedMs / 1000.0f;
      const float measuredRate =
          (filteredTemperature - previousTemperature) / elapsedSec;
      filteredTemperatureRate += RATE_FILTER_ALPHA *
                                 (measuredRate - filteredTemperatureRate);
      previousTemperature = filteredTemperature;
      previousSampleAt = now;
    }
  }

  const bool switchLocked = hasSwitched &&
                            (now - lastSwitchAt <
                             (unsigned long)settings.heaterMinSwitchSec * 1000UL);

  if (switchLocked) return;

  const float predictedOnTemperature = filteredTemperature +
                                       filteredTemperatureRate *
                                       settings.heaterOnPredictionSec;
  const float predictedOffTemperature = filteredTemperature +
                                        filteredTemperatureRate *
                                        settings.heaterOffPredictionSec;

  if (!heaterOn && predictedOnTemperature <=
                   targetTemperature - settings.heaterDeadband) {
    heaterOn = true;
    lastSwitchAt = now;
    hasSwitched = true;
  } else if (heaterOn && predictedOffTemperature >= targetTemperature) {
    heaterOn = false;
    lastSwitchAt = now;
    hasSwitched = true;
  }
}
