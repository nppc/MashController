#ifndef HEATER_H
#define HEATER_H

extern bool heaterOn;

void heaterResetThermalModel(float initialTemperature, float waterMassKg);
void heaterIncludeGrain(float grainMassKg);
void updateHeater(float currentTemp);

#endif
