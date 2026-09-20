#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <Arduino.h>

bool calibrationIsActive();
bool calibrationHeaterIsOn();
void calibrationUpdate();
bool calibrationStart(float waterLiters);
void calibrationStop();
void calibrationStatusJson(String &out);

#endif
