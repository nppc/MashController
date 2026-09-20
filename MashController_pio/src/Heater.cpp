#include "Heater.h"
#include "Storage.h"
#include "MashProfile.h"   // targetTemperature

bool heaterOn = false;

namespace {
float filteredTemperature = 0.0f;
float elementTemperature = 20.0f;
float modelWaterTemperature = 20.0f;
float waterMassKg = 0.0f;
float waterThermalMass = 0.0f;
unsigned long previousThermalUpdateAt = 0;
unsigned long lastSwitchAt = 0;
bool haveFilteredTemperature = false;
bool thermalModelReady = false;
bool hasSwitched = false;
constexpr float TEMP_FILTER_ALPHA = 0.35f;
constexpr float WATER_SPECIFIC_HEAT = 4186.0f;
constexpr float GRAIN_SPECIFIC_HEAT = 1900.0f;
}

void heaterResetThermalModel(float initialTemperature, float newWaterMassKg) {
  waterMassKg = max(newWaterMassKg, 0.1f);
  waterThermalMass = waterMassKg * WATER_SPECIFIC_HEAT;
  filteredTemperature = initialTemperature;
  elementTemperature = initialTemperature;
  modelWaterTemperature = initialTemperature;
  previousThermalUpdateAt = millis();
  haveFilteredTemperature = true;
  thermalModelReady = true;
  heaterOn = false;
  lastSwitchAt = 0;
  hasSwitched = false;
}

void heaterIncludeGrain(float grainMassKg) {
  if (!thermalModelReady) return;
  waterThermalMass += max(grainMassKg, 0.0f) * GRAIN_SPECIFIC_HEAT;
}

void updateHeater(float currentTemp) {
  if (!isRunning || inCoolDown) {
    heaterOn = false;
    haveFilteredTemperature = false;
    thermalModelReady = false;
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

  if (!thermalModelReady) {
    heaterResetThermalModel(filteredTemperature, activeProfile.waterMassKg);
  }

  const unsigned long elapsedMs = now - previousThermalUpdateAt;
  if (elapsedMs > 0) {
    const float elapsedSec = min(elapsedMs, 10000UL) / 1000.0f;
    const float transferCoeff = max(settings.heaterTransferCoeff, 0.1f);
    const float elementThermalMass = max(settings.heaterThermalMass, 1.0f);
    const float heatToWater =
      transferCoeff * (elementTemperature - modelWaterTemperature);

    elementTemperature +=
        (settings.heaterPowerW * (heaterOn ? 1.0f : 0.0f) - heatToWater) *
        elapsedSec / elementThermalMass;
    modelWaterTemperature += heatToWater * elapsedSec / waterThermalMass;
    // Correct the estimated water state toward the filtered sensor reading.
    modelWaterTemperature +=
      0.5f * (filteredTemperature - modelWaterTemperature);
    previousThermalUpdateAt = now;
  }

  const bool switchLocked = hasSwitched &&
                            (now - lastSwitchAt <
                             (unsigned long)settings.heaterMinSwitchSec * 1000UL);

  if (switchLocked) return;

  const float predictedEquilibrium =
      (waterThermalMass * filteredTemperature +
       max(settings.heaterThermalMass, 1.0f) * elementTemperature) /
      (waterThermalMass + max(settings.heaterThermalMass, 1.0f));

  if (!heaterOn && filteredTemperature <=
                       targetTemperature - settings.heaterDeadband) {
    heaterOn = true;
    lastSwitchAt = now;
    hasSwitched = true;
  } else if (heaterOn && predictedEquilibrium >= targetTemperature) {
    heaterOn = false;
    lastSwitchAt = now;
    hasSwitched = true;
  }
}
