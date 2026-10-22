#include "MashProfile.h"
#include "Storage.h"
#include "Sensor.h"
#include "Heater.h"
#include "Mixer.h"

Profile activeProfile;

bool isRunning = false;
bool isPaused = false;
bool waitingForUser = false;
int currentStep = 0;
bool waitingForTemp = false;

float targetTemperature = NAN;

unsigned long stepStartTime = 0;
unsigned long stepDurationSec = 0;
unsigned long pausedElapsedSec = 0;

bool inCoolDown = false;
unsigned long coolDownStart = 0;

namespace {
float estimatedSecondsRemaining = NAN;
unsigned long estimateUpdatedAt = 0;

float estimateRampSeconds(float fromC, float toC, float capacityJPerC) {
  if (!isfinite(fromC) || !isfinite(toC) ||
      !isfinite(capacityJPerC) || !(capacityJPerC > 0.0f)) {
    return NAN;
  }
  if (!(toC > fromC)) return 0.0f;

  const SettingsEE &s = storage.settings();
  const float powerW = max(s.heaterPowerEffW, 1.0f);

  // Use Simpson's rule over [fromC, toC] with dynamic heat loss P_loss(T)
  const float midC = 0.5f * (fromC + toC);
  const float lossFrom = heaterCurrentLossW(fromC, s);
  const float lossMid  = heaterCurrentLossW(midC, s);
  const float lossTo   = heaterCurrentLossW(toC, s);
  const float avgLossW = (lossFrom + 4.0f * lossMid + lossTo) / 6.0f;

  const float netPowerW = powerW - avgLossW;
  if (netPowerW <= 1.0f) return NAN;

  return capacityJPerC * (toC - fromC) / netPowerW;
}

float calculateEstimatedSecondsRemaining() {
  if (!isRunning || inCoolDown || activeProfile.stepCount <= 0 ||
      currentStep < 0 || currentStep >= activeProfile.stepCount) {
    return NAN;
  }

  const float waterCapacityJPerC =
      max(activeProfile.waterMassKg, 0.1f) * 4186.0f;
  const bool grainAdded =
      currentStep > 0 && activeProfile.steps[0].time == 0;
  const bool grainIncludedInFutureRamps =
      grainAdded || activeProfile.steps[0].time == 0;
  const float mashCapacityJPerC = waterCapacityJPerC +
      (grainIncludedInFutureRamps
           ? max(activeProfile.grainMassKg, 0.0f) * 1900.0f
           : 0.0f);

  float totalSec = 0.0f;
  float fromC = readTemperature();

  for (int i = currentStep; i < activeProfile.stepCount; ++i) {
    const float targetC = activeProfile.steps[i].temp;
    const float capacity = (i == 0 && !grainAdded)
                               ? waterCapacityJPerC
                               : mashCapacityJPerC;
    float rampSec = 0.0f;
    if (i == currentStep) {
      if (waitingForTemp) {
        float curEta = heaterEstimatedSecondsToTarget();
        rampSec = isfinite(curEta) ? curEta : estimateRampSeconds(fromC, targetC, capacity);
      }
    } else {
      rampSec = estimateRampSeconds(fromC, targetC, capacity);
    }
    if (!isfinite(rampSec)) return NAN;
    totalSec += rampSec;

    if (i == currentStep) {
      if (waitingForTemp) {
        totalSec += max(activeProfile.steps[i].time, 0) * 60.0f;
      } else if (activeProfile.steps[i].time > 0) {
        const unsigned long elapsedSec = isPaused
            ? pausedElapsedSec
            : (millis() - stepStartTime) / 1000UL;
        const unsigned long durationSec =
            (unsigned long)activeProfile.steps[i].time * 60UL;
        totalSec += elapsedSec >= durationSec
                        ? 0.0f
                        : (float)(durationSec - elapsedSec);
      }
    } else {
      totalSec += max(activeProfile.steps[i].time, 0) * 60.0f;
    }
    fromC = targetC;
  }

  return totalSec;
}

void refreshEstimatedTime() {
  estimatedSecondsRemaining = calculateEstimatedSecondsRemaining();
  estimateUpdatedAt = millis();
}

void updateEstimatedTime() {
  const unsigned long now = millis();
  if (estimateUpdatedAt == 0) {
    estimateUpdatedAt = now;
    return;
  }

  const unsigned long elapsedMs = now - estimateUpdatedAt;
  estimateUpdatedAt = now;
  if (isRunning && !isPaused && !inCoolDown &&
      isfinite(estimatedSecondsRemaining)) {
    const float elapsedSec = (float)elapsedMs / 1000.0f;
    estimatedSecondsRemaining =
        max(0.0f, estimatedSecondsRemaining - elapsedSec);
  }
}
}

/* -------------------------------------------------------------------------- */
/*                         PROFILE LOADING                                    */
/* -------------------------------------------------------------------------- */

bool loadProfile(int index) {
  ProfileEE p;
  if (!storage.getProfile(index, p)) {
    Serial.println("Profile index out of range");
    return false;
  }

  activeProfile.name = String(p.name);
  activeProfile.waterMassKg = p.waterMassKg;
  activeProfile.grainMassKg = p.grainMassKg;
  activeProfile.stepCount = p.stepCount;

  for (int i = 0; i < activeProfile.stepCount; i++) {
    activeProfile.steps[i].temp = p.steps[i].temp;
    activeProfile.steps[i].time = p.steps[i].timeMin;
  }

  Serial.println("Profile loaded: " + activeProfile.name);
  return true;
}

/* -------------------------------------------------------------------------- */
/*                            STEP ADVANCE HELPER                             */
/* -------------------------------------------------------------------------- */

void advanceStep() {
  currentStep++;

  if (currentStep >= activeProfile.stepCount) {
    isRunning = true;
    heaterOn = false;           // Turn off heater immediately
    targetTemperature = NAN;
    waitingForUser = false;
    waitingForTemp = false;
    isPaused = false;
    stepDurationSec = 0;
    pausedElapsedSec = 0;

    // Start cool-down: mixer runs in AUTO mode to circulate while cooling.
    // Keep this transition independent of how the final step was completed.
    inCoolDown = true;
    coolDownStart = millis();
    mixerManualMode = false;    // Switch to AUTO for cool-down circulation
    mixerOn = false;            // Will cycle on/rest per settings
    mixerPhaseStart = millis();
    estimatedSecondsRemaining = NAN;
    estimateUpdatedAt = millis();

    Serial.println("Profile finished, entering cool-down phase...");
  } else {
    targetTemperature = activeProfile.steps[currentStep].temp;
    stepDurationSec = activeProfile.steps[currentStep].time * 60;
    waitingForTemp = true;
    pausedElapsedSec = 0;
    refreshEstimatedTime();
  }
}

/* -------------------------------------------------------------------------- */
/*                    COOL-DOWN + STEP-TIMING STATE MACHINE                   */
/* -------------------------------------------------------------------------- */

void mashProfileCheckCoolDown() {
  if (!inCoolDown) return;

  SettingsEE &s = storage.settings();
  unsigned long coolDownElapsed = (millis() - coolDownStart) / 1000;

  if (coolDownElapsed >= s.coolDownSec) {
    // Cool-down finished
    inCoolDown = false;
    isRunning = false;
    mixerManualMode = true;   // Back to manual mode
    mixerOn = false;          // Stop mixer
    Serial.println("Cool-down complete");
  }
}

void mashProfileTick() {
  updateEstimatedTime();
  if (!(isRunning && !isPaused && !inCoolDown)) return;

  float currentTemp = readTemperature();   // cached value - never blocks

  // If waiting for temperature -> check if we reached it
  if (waitingForTemp) {
    static unsigned long lastRampRefreshMs = 0;
    if (millis() - lastRampRefreshMs >= 10000UL) {
      lastRampRefreshMs = millis();
      refreshEstimatedTime();
    }

    // A step counts as reached this many degrees below target (Settings)
    const float toleranceC = storage.settings().targetReachedHystC;
    if (currentTemp >= targetTemperature - toleranceC) {
      waitingForTemp = false;
      if (activeProfile.steps[currentStep].time == 0) {
        isPaused = true;
        waitingForUser = true;
        pausedElapsedSec = 0;
        Serial.println("Manual step temperature reached; waiting for user");
      } else {
        stepStartTime = millis();                   // NOW start timer
        Serial.println("Step timer started");
      }
      refreshEstimatedTime();
    }
  } else {
    // Timer is running normally
    unsigned long elapsed = (millis() - stepStartTime) / 1000;

    if (elapsed >= stepDurationSec) {
      advanceStep();
    }
  }
}

float mashProfileEstimatedSecondsRemaining() {
  return estimatedSecondsRemaining;
}

void mashProfileResetEstimatedTime() {
  refreshEstimatedTime();
}
