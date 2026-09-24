#pragma once

#include <math.h>

// Energy that the heater has produced but that has not yet shown up at the
// sensor: heat stored in the element and pot base, plus mixing and sensor lag.
// It builds up while the heater is on and levels off at powerEffW * tauSec,
// so a short pulse stores little and a long heat stores the maximum.
//
// Calibrated values come from the Heater Calibration page:
//   powerEffW - power that actually reaches the water (W)
//   tauSec    - time constant of the stored energy (s)
//   storeGain - measured overshoot / (powerEffW * tauSec); above 1 when sensor
//               lag and mixing add to the element's own stored heat
class HeaterModel {
 public:
  void configure(float powerEffW, float tauSec, float storeGain = 1.0f) {
    powerEffW_ = powerEffW > 0.0f ? powerEffW : 0.0f;
    tauSec_ = tauSec > 1.0f ? tauSec : 1.0f;
    storeGain_ = storeGain > 0.0f ? storeGain : 1.0f;
  }

  void reset() { storedJ_ = 0.0f; }

  // Call regularly (e.g. once per second) with the time step and heater state.
  void update(float dtSec, bool heaterOn) {
    if (!(dtSec > 0.0f)) return;
    const float decay = expf(-dtSec / tauSec_);
    const float input = heaterOn ? powerEffW_ * storeGain_ : 0.0f;
    storedJ_ = storedJ_ * decay + input * tauSec_ * (1.0f - decay);
  }

  float storedJ() const { return storedJ_; }

  // Power currently flowing from the stored heat into the water (W).
  float flowW() const { return storedJ_ / tauSec_; }

  // Peak sensor temperature if the heater is switched off now.
  // capacityJPerC: water litres * 4186 (+ grain kg * ~1650 during the mash).
  // lossW: current heat loss, heaterLossWPerC * (waterC - ambientC).
  float predictPeak(float waterC, float capacityJPerC, float lossW) const {
    if (!(capacityJPerC > 0.0f)) return waterC;
    const float rise = storedJ_ / capacityJPerC;
    if (!(lossW > 0.0f)) return waterC + rise;

    const float lossRate = lossW / capacityJPerC;
    const float initialRate = storedJ_ / (tauSec_ * capacityJPerC);
    if (initialRate <= lossRate) return waterC;  // already cooling

    const float tPeak = tauSec_ * logf(initialRate / lossRate);
    return waterC + rise * (1.0f - lossRate / initialRate) - lossRate * tPeak;
  }

 private:
  float powerEffW_ = 0.0f;
  float tauSec_ = 60.0f;
  float storeGain_ = 1.0f;
  float storedJ_ = 0.0f;
};
