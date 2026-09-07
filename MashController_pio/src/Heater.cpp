#include "Heater.h"
#include "Storage.h"
#include "MashProfile.h"   // targetTemperature

bool heaterOn = false;

void updateHeater(float currentTemp) {
  float hysteresis = storage.settings().heaterHysteresis;

  if (!heaterOn && currentTemp < targetTemperature - hysteresis) {
    heaterOn = true;
  }
  else if (heaterOn && currentTemp > targetTemperature + hysteresis) {
    heaterOn = false;
  }
}
