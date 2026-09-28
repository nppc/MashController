#include "Heater.h"

#include <math.h>

#include "HeaterModel.h"
#include "MashProfile.h"   // targetTemperature
#include "Outputs.h"
#include "Sensor.h"
#include "Storage.h"

// On/off control that accounts for the heat stored in the element and pot
// base. The heater switches off as soon as the stored heat is enough to carry
// the water to the target, and back on when even the predicted peak would
// fall below target - deadband. Calibrated values come from the Heater
// Calibration page.

bool heaterOn = false;

namespace {
constexpr float WATER_SPECIFIC_HEAT = 4186.0f;
constexpr float GRAIN_SPECIFIC_HEAT = 1900.0f;
constexpr float MIN_AVERAGE_SEC = 4.0f;
constexpr float MAX_AVERAGE_SEC = 120.0f;

HeaterModel model;
float capacityJPerC = 20.0f * WATER_SPECIFIC_HEAT;
bool modelReady = false;
bool outputWasOn = false;
unsigned long previousUpdateAt = 0;
unsigned long lastSwitchAt = 0;
bool hasSwitched = false;
float predictedPeak = NAN;
float lastWaterNowC = NAN;

// Average over one full mixer cycle so the mixing ripple cancels out.
float averageWindowSec(const SettingsEE &s) {
  const float cycle = (float)s.mixerOnSec + (float)s.mixerRestSec;
  return constrain(cycle, MIN_AVERAGE_SEC, MAX_AVERAGE_SEC);
}
}

void heaterResetThermalModel(float /*initialTemperature*/, float waterMassKg) {
  capacityJPerC = max(waterMassKg, 0.1f) * WATER_SPECIFIC_HEAT;
  model.reset();
  previousUpdateAt = millis();
  modelReady = true;
  outputWasOn = false;
  heaterOn = false;
  lastSwitchAt = 0;
  hasSwitched = false;
  predictedPeak = NAN;
}

void heaterIncludeGrain(float grainMassKg) {
  if (!modelReady) return;
  capacityJPerC += max(grainMassKg, 0.0f) * GRAIN_SPECIFIC_HEAT;
}

float heaterPredictedPeak() {
  return predictedPeak;
}

void updateHeater() {
  if (!isRunning || inCoolDown) {
    heaterOn = false;
    modelReady = false;
    hasSwitched = false;
    predictedPeak = NAN;
    return;
  }

  SettingsEE &s = storage.settings();
  const unsigned long now = millis();

  if (!modelReady) {
    heaterResetThermalModel(readTemperature(), activeProfile.waterMassKg);
  }

  // Advance the stored-heat model with what the SSR actually did since the
  // last reading (pause can hold the output off while heaterOn is true).
  model.configure(s.heaterPowerEffW, s.heaterTauSec, s.heaterStoreGain);
  const float dtSec = min(now - previousUpdateAt, 10000UL) / 1000.0f;
  model.update(dtSec, outputWasOn);
  previousUpdateAt = now;

  const float windowSec = averageWindowSec(s);
  const float averageC = averageTemperature(windowSec);
  const float lossW = max(s.heaterLossWPerC, 0.0f) * (averageC - s.heaterAmbientC);

  // The average lags by half its window; bring it forward with the rate the
  // model says the water is changing at right now.
  const float rateCPerSec = (model.flowW() - lossW) / capacityJPerC;
  const float waterNowC = averageC + rateCPerSec * windowSec * 0.5f;
  predictedPeak = model.predictPeak(waterNowC, capacityJPerC, lossW);
  lastWaterNowC = waterNowC;

  const bool switchLocked =
      hasSwitched &&
      (now - lastSwitchAt < (unsigned long)s.heaterMinSwitchSec * 1000UL);

  if (!switchLocked) {
    if (!heaterOn && predictedPeak < targetTemperature - s.heaterDeadband) {
      heaterOn = true;
      lastSwitchAt = now;
      hasSwitched = true;
    } else if (heaterOn && predictedPeak >= targetTemperature) {
      heaterOn = false;
      lastSwitchAt = now;
      hasSwitched = true;
    }
  }

  outputWasOn = heaterOutputActive();
}

/*
 * Estimate the remaining time until targetTemperature is reached.
 *
 * The heater model is simulated forward from the current measured water
 * temperature and current heater state.
 *
 * The ETA is normally allowed to count down with real elapsed time.
 * A newly calculated model prediction is used to detect whether the
 * real heating process is consistently faster or slower than expected.
 *
 * Small differences are ignored, while larger differences are applied
 * gradually. This prevents temperature sensor noise, water mixing and
 * other short-term disturbances from making the ETA jump.
 *
 * ETA filtering affects this prediction only. It does not affect heater
 * control or the temperature measurement used by the control algorithm.
 */
float heaterEstimatedSecondsToTarget()
{
    static float etaFiltered = NAN;
    static unsigned long etaLastUpdateMs = 0;

    SettingsEE &s = storage.settings();

    if (!modelReady || isnan(lastWaterNowC))
        return NAN;

    /*
     * Target already reached.
     * Clear the stored ETA so that the next heating cycle starts fresh.
     */
    if (lastWaterNowC >= targetTemperature) {
        etaFiltered = NAN;
        etaLastUpdateMs = 0;
        return NAN;
    }

    /*
     * Current time is needed both for the model simulation and for
     * counting down the previously calculated ETA.
     */
    unsigned long nowMs = millis();

    HeaterModel sim = model;

    float T = lastWaterNowC;
    bool on = heaterOn;

    unsigned long simSwitchElapsedMs =
        nowMs - lastSwitchAt;

    const float dt = 2.0f;
    const int maxSteps = (int)(3600.0f / dt);

    float etaRaw = NAN;

    /*
     * Simulate the heater forward until the target temperature is reached.
     */
    for (int i = 0; i < maxSteps; ++i) {

        sim.update(dt, on);

        float heaterLossWPerC =
            s.heaterLossWPerC > 0.0f ?
            s.heaterLossWPerC : 0.0f;

        float lossW =
            heaterLossWPerC *
            (T - s.heaterAmbientC);

        float rate =
            (sim.flowW() - lossW) /
            capacityJPerC;

        T += rate * dt;

        simSwitchElapsedMs +=
            (unsigned long)(dt * 1000.0f);

        /*
         * One simulation step has already been performed when i == 0.
         */
        if (T >= targetTemperature) {
            etaRaw = (float)(i + 1) * dt;
            break;
        }

        bool locked =
            simSwitchElapsedMs <
            (unsigned long)s.heaterMinSwitchSec * 1000UL;

        if (!locked) {

            float peak =
                sim.predictPeak(
                    T,
                    capacityJPerC,
                    lossW);

            if (!on &&
                peak < targetTemperature -
                       s.heaterDeadband) {

                on = true;
                simSwitchElapsedMs = 0;
            }
            else if (on &&
                     peak >= targetTemperature) {

                on = false;
                simSwitchElapsedMs = 0;
            }
        }
    }

    if (isnan(etaRaw))
        return NAN;

    /*
     * First prediction of a heating cycle.
     * Accept the model prediction directly.
     */
    if (isnan(etaFiltered)) {
        etaFiltered = etaRaw;
        etaLastUpdateMs = nowMs;
        return etaFiltered;
    }

    /*
     * Count down the existing ETA according to actual elapsed time.
     *
     * For example:
     *   previous ETA = 300 s
     *   5 s elapsed  = 295 s
     *
     * This prevents the ETA from being repeatedly "restarted" by the
     * model calculation.
     */
    if (etaLastUpdateMs != 0) {

        unsigned long elapsedMs =
            nowMs - etaLastUpdateMs;

        float elapsedSec =
            (float)elapsedMs / 1000.0f;

        if (elapsedSec > 0.0f) {

            etaFiltered -= elapsedSec;

            if (etaFiltered < 0.0f)
                etaFiltered = 0.0f;
        }
    }

    etaLastUpdateMs = nowMs;

    /*
     * Ignore small model corrections.
     *
     * For a heating process lasting roughly 50 minutes, a difference of
     * less than 30 seconds is not considered significant enough to change
     * the displayed ETA.
     */
    const float ETA_DEADBAND_SEC = 30.0f;

    /*
     * Only this fraction of a significant difference is applied during
     * one update.
     *
     * Example:
     *   current ETA = 2500 s
     *   model ETA   = 2700 s
     *   difference  = 200 s
     *
     * With 8%, only 16 seconds of that difference are applied immediately.
     * If the system remains slower than expected, subsequent updates will
     * continue moving the ETA toward the new prediction.
     */
    const float ETA_CORRECTION_ALPHA = 0.08f;

    float error = etaRaw - etaFiltered;

    if (error > ETA_DEADBAND_SEC ||
        error < -ETA_DEADBAND_SEC) {

        etaFiltered +=
            error * ETA_CORRECTION_ALPHA;
    }

    return etaFiltered;
}