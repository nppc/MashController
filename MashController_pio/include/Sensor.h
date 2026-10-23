#ifndef SENSOR_H
#define SENSOR_H

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "Config.h"

extern OneWire oneWire;
extern DallasTemperature sensors;

extern DeviceAddress sensorAddr;
extern bool sensorFound;
extern bool sensorOk;
extern float lastGoodTemp;
extern bool conversionInProgress;   // exposed so /rediscoverSensor can check it

extern int16_t tempHistory[HISTORY_SIZE];  // holds the last HISTORY_SIZE readings, in t/100 C units
extern int histIndex;

void sensorInit();     // call once from setup(), after Serial.begin()
void sensorUpdate();   // call every loop() iteration (real sensor); call
                        // at the speed-adjusted interval in simulated mode
float readTemperature();
// Mean of the raw readings from roughly the last windowSec seconds.
// Averaging over one full mixer cycle removes the mixing ripple.
float averageTemperature(float windowSec);
bool discoverSensorAddress();
String sensorAddressToString();
bool sensorUsingFakeMode();
bool sensorSetFakeMode(bool enabled);
float sensorFakeSpeedMultiplier();
bool sensorSetFakeSpeedMultiplier(float multiplier);
float sensorSimulationTimeScale();

void sensorPrepareFakeCalibration(float waterLiters, float ambientC, float initialTemperature);
void sensorPrepareFakeMash(float waterLiters, float initialTemperature);
void sensorAddFakeGrain(float grainMassKg);
void sensorEndFakeCalibration();

#endif
