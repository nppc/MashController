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

float heaterEffectiveTarget(float targetC, const SettingsEE &s) {
  if (s.heaterT2 > s.heaterT1 + 1.0f) {
    const float ratio = constrain((targetC - s.heaterT1) / (s.heaterT2 - s.heaterT1), 0.0f, 1.0f);
    const float trim = s.heaterTrimLowC + (s.heaterTrimHighC - s.heaterTrimLowC) * ratio;
    return targetC + constrain(trim, -1.0f, 1.0f);
  }
  return targetC + constrain(s.heaterTrimLowC, -1.0f, 1.0f);
}

float heaterCurrentTauSec(float tempC, const SettingsEE &s) {
  if (s.heaterT2 > s.heaterT1 + 1.0f && s.heaterTau1 > 1.0f && s.heaterTau2 > 1.0f) {
    const float slope = (s.heaterTau2 - s.heaterTau1) / (s.heaterT2 - s.heaterT1);
    const float tau = s.heaterTau1 + slope * (tempC - s.heaterT1);
    return constrain(tau, 15.0f, 300.0f);
  }
  return s.heaterTauSec > 1.0f ? s.heaterTauSec : 90.0f;
}

float heaterCurrentLossW(float tempC, const SettingsEE &s) {
  const float deltaT = max(tempC - s.heaterAmbientC, 0.0f);
  if (deltaT <= 0.01f) return 0.0f;

  const float scale = constrain(s.heaterLossScalePct, 50.0f, 200.0f) / 100.0f;

  if (s.heaterT2 > s.heaterT1 + 1.0f && s.heaterLossW1 > 0.1f && s.heaterLossW2 > 0.1f) {
    const float dt1 = max(s.heaterT1 - s.heaterAmbientC, 1.0f);
    const float dt2 = max(s.heaterT2 - s.heaterAmbientC, dt1 + 1.0f);
    const float alpha = constrain(logf(s.heaterLossW2 / s.heaterLossW1) / logf(dt2 / dt1), 1.0f, 2.0f);
    const float lossW = s.heaterLossW1 * powf(deltaT / dt1, alpha);
    return max(lossW * scale, 0.0f);
  }

  // Fallback: single-point linear loss with sqrt(sqrt(dt)) scaling
  const float refLossWPerC = s.heaterLossWPerC > 0.0f ? s.heaterLossWPerC : 3.0f;
  const float dtRef = max(65.0f - s.heaterAmbientC, 10.0f);
  const float lossW = refLossWPerC * deltaT * sqrtf(sqrtf(deltaT / dtRef));
  return max(lossW * scale, 0.0f);
}

float heaterCurrentStoreGain(float tempC, const SettingsEE &s) {
  if (s.heaterT2 > s.heaterT1 + 1.0f && s.heaterStoreGain1 > 0.1f && s.heaterStoreGain2 > 0.1f) {
    const float ratio = constrain((tempC - s.heaterT1) / (s.heaterT2 - s.heaterT1), 0.0f, 1.0f);
    const float gain = s.heaterStoreGain1 + (s.heaterStoreGain2 - s.heaterStoreGain1) * ratio;
    return constrain(gain, 0.5f, 2.0f);
  }
  return s.heaterStoreGain > 0.1f ? s.heaterStoreGain : 1.0f;
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

  const float windowSec = averageWindowSec(s);
  const float averageC = averageTemperature(windowSec);
  const float lossW = heaterCurrentLossW(averageC, s);
  const float tauSec = heaterCurrentTauSec(averageC, s);
  const float storeGain = heaterCurrentStoreGain(averageC, s);

  // Advance the stored-heat model with dynamic parameters and SSR history
  model.configure(s.heaterPowerEffW, tauSec, storeGain);
  const float simulationScale = sensorSimulationTimeScale();
  const float elapsedWallSec = (float)(now - previousUpdateAt) / 1000.0f;
  const float dtSec = min(elapsedWallSec * simulationScale, 10.0f);
  model.update(dtSec, outputWasOn);
  previousUpdateAt = now;

  // The average lags by half its window; bring it forward with the rate the
  // model says the water is changing at right now.
  const float rateCPerSec = (model.flowW() - lossW) / capacityJPerC;
  const float waterNowC = averageC + rateCPerSec * windowSec * 0.5f;
  predictedPeak = model.predictPeak(waterNowC, capacityJPerC, lossW);
  lastWaterNowC = waterNowC;

  const float effTarget = heaterEffectiveTarget(targetTemperature, s);

  const bool switchLocked =
      hasSwitched &&
      ((float)(now - lastSwitchAt) * simulationScale <
       (float)s.heaterMinSwitchSec * 1000.0f);

  if (!switchLocked) {
    if (!heaterOn && predictedPeak < effTarget - HEATER_DEADBAND_C) {
      heaterOn = true;
      lastSwitchAt = now;
      hasSwitched = true;
    } else if (heaterOn && predictedPeak >= effTarget) {
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

    if (!modelReady || isnan(lastWaterNowC) || !waitingForTemp) {
        etaFiltered = NAN;
        etaLastUpdateMs = 0;
        return NAN;
    }

    /*
     * Target already reached (or within target tolerance band where step timer starts).
     * Clear the stored ETA so that the next heating cycle starts fresh.
     */
    const float effTarget = heaterEffectiveTarget(targetTemperature, s);
    const float toleranceC = s.targetReachedHystC;
    if (lastWaterNowC >= effTarget - toleranceC) {
        etaFiltered = NAN;
        etaLastUpdateMs = 0;
        return NAN;
    }

    /*
     * Current time is needed both for the model simulation and for
     * counting down the previously calculated ETA.
     */
    unsigned long nowMs = millis();
    const float simulationScale = sensorSimulationTimeScale();

    HeaterModel sim = model;

    float T = lastWaterNowC;
    bool on = heaterOn;

    float simSwitchElapsedMs =
        (float)(nowMs - lastSwitchAt) * simulationScale;

    const float dt = 2.0f;
    const int maxSteps = (int)(3600.0f / dt);

    float etaRaw = NAN;

    /*
     * Simulate the heater forward until the effective target temperature is reached.
     */
    for (int i = 0; i < maxSteps; ++i) {
        sim.configure(s.heaterPowerEffW, heaterCurrentTauSec(T, s), heaterCurrentStoreGain(T, s));
        sim.update(dt, on);

        float lossW = heaterCurrentLossW(T, s);
        float rate = (sim.flowW() - lossW) / capacityJPerC;
        T += rate * dt;

        simSwitchElapsedMs += dt * 1000.0f;

        float peak = sim.predictPeak(T, capacityJPerC, lossW);

        /*
         * Ramp target is reached when the predictive peak reaches effective target
         * (the heater turns off because stored heat will carry it to the rest temp),
         * or when actual water temperature enters the step target tolerance band.
         */
        if (peak >= effTarget || T >= effTarget - toleranceC) {
            etaRaw = (float)(i + 1) * dt / simulationScale;
            break;
        }

        bool locked = simSwitchElapsedMs < (float)s.heaterMinSwitchSec * 1000.0f;

        if (!locked) {
            if (!on && peak < effTarget - HEATER_DEADBAND_C) {
                on = true;
                simSwitchElapsedMs = 0;
            } else if (on && peak >= effTarget) {
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