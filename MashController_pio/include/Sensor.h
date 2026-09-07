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

extern float tempHistory[HISTORY_SIZE];
extern int histIndex;

void sensorInit();     // call once from setup(), after Serial.begin()
void sensorUpdate();   // call every loop() iteration (real sensor); call
                        // once per READ_INTERVAL_MS under DEBUG_FAKE_TEMP
float readTemperature();
bool discoverSensorAddress();
String sensorAddressToString();

#endif
