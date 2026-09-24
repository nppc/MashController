#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <Arduino.h>

bool calibrationIsActive();
bool calibrationHeaterIsOn();
void calibrationUpdate();
bool calibrationStart(float waterLiters);
bool calibrationStart(float waterLiters, float targetC, float ambientC);
void calibrationStop();

// Reason the last calibrationStart() call was refused.
const char *calibrationLastError();

// Status plus logged samples from index firstSample onwards (at most a fixed
// number per call, so the response stays small; the page asks again for more).
void calibrationStatusJson(String &out, uint16_t firstSample);

#endif
