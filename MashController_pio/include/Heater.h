#ifndef HEATER_H
#define HEATER_H

// Predicted peak must fall this far below effective target before heater turns back on.
// Tight value (0.05 °C) suitable for SSRs without mechanical contact wear.
constexpr float HEATER_DEADBAND_C = 0.02f;

extern bool heaterOn;

void heaterResetThermalModel(float initialTemperature, float waterMassKg);
void heaterIncludeGrain(float grainMassKg);

// Call on every new sensor reading.
void updateHeater();

// Where the controller expects the temperature to peak if the heater were
// switched off now (NAN while no mash is running).
float heaterPredictedPeak();

float heaterEstimatedSecondsToTarget();

struct SettingsEE;
float heaterEffectiveTarget(float targetC, const SettingsEE &s);
float heaterCurrentTauSec(float tempC, const SettingsEE &s);
float heaterCurrentLossW(float tempC, const SettingsEE &s);
float heaterCurrentStoreGain(float tempC, const SettingsEE &s);

#endif