#ifndef HEATER_H
#define HEATER_H

extern bool heaterOn;

void heaterResetThermalModel(float initialTemperature, float waterMassKg);
void heaterIncludeGrain(float grainMassKg);

// Call on every new sensor reading.
void updateHeater();

// Where the controller expects the temperature to peak if the heater were
// switched off now (NAN while no mash is running).
float heaterPredictedPeak();

#endif
