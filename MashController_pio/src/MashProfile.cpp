#include "MashProfile.h"
#include "Storage.h"
#include "Sensor.h"
#include "Heater.h"
#include "Mixer.h"

Profile activeProfile;

bool isRunning = false;
bool isPaused = false;
int currentStep = 0;
bool waitingForTemp = false;

float targetTemperature = 20.0;

unsigned long stepStartTime = 0;
unsigned long stepDurationSec = 0;
unsigned long pausedElapsedSec = 0;

bool inCoolDown = false;
unsigned long coolDownStart = 0;

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
    heaterOn = false;           // Turn off heater immediately
    targetTemperature = 20.0;

    // Start cool-down: mixer runs in AUTO mode to circulate while cooling
    mixerManualMode = false;    // Switch to AUTO for cool-down circulation
    mixerOn = false;            // Will cycle on/rest per settings
    mixerPhaseStart = millis();

    inCoolDown = true;          // Mark we're in cool-down phase
    coolDownStart = millis();

    Serial.println("Profile finished, entering cool-down phase...");
  } else {
    targetTemperature = activeProfile.steps[currentStep].temp;
    stepDurationSec = activeProfile.steps[currentStep].time * 60;
    waitingForTemp = true;
    pausedElapsedSec = 0;
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
  if (!(isRunning && !isPaused && !inCoolDown)) return;

  float currentTemp = readTemperature();   // cached value - never blocks

  // If waiting for temperature -> check if we reached it
  if (waitingForTemp) {
    if (currentTemp >= targetTemperature - 0.5) {   // tolerance
      waitingForTemp = false;
      stepStartTime = millis();                     // NOW start timer
      Serial.println("Step timer started");
    }
  } else {
    // Timer is running normally
    unsigned long elapsed = (millis() - stepStartTime) / 1000;

    if (elapsed >= stepDurationSec) {
      advanceStep();
    }
  }
}
