#include <Arduino.h>
#include "Outputs.h"
#include "Config.h"
#include "Heater.h"
#include "Calibration.h"
#include "Mixer.h"
#include "MashProfile.h"

// Current cooler SSR state. Declared with external linkage (extern in
// Outputs.h) so other modules — status display, logging, etc. — can read
// it, the same way heaterOn/mixerOn are exposed. Must stay out of the
// anonymous namespace below, or it would become file-local and no longer
// be visible to those other translation units.
bool coolerOn = LOW;

namespace {
constexpr uint32_t MIXER_PWM_FREQUENCY_HZ = 500;
constexpr uint32_t MIXER_PWM_RANGE = 1023;
// How long the mixer takes to ramp from its starting duty down to 0 (full
// speed) after being switched on, to avoid slamming it to full speed instantly.
constexpr uint32_t MIXER_RAMP_TIME_MS = 500;
// Duty cycle the mixer ramp starts from. analogWrite is inverted here (higher
// duty = slower), so this is a low starting speed rather than a high one.
constexpr uint32_t MIXER_START_DUTY = (1023 - 256); //(MIXER_PWM_RANGE + 1) / 2;

// How long the cooler is kept on after the heater last switches off.
constexpr uint32_t COOLER_OFF_DELAY_MS = 60000;

// Mixer ramp bookkeeping.
uint32_t mixerStartTime = 0; // millis() timestamp when the current ramp began
bool mixerWasOn = false;     // tracks mixerOn's previous state to detect the on-edge

// Cooler off-delay bookkeeping.
uint32_t heaterOffTime = 0; // millis() timestamp of the heater's last on->off transition
bool heaterWasOn = false;   // tracks heaterActive's previous state to detect the off-edge
}

void outputsInit() {

  analogWriteFreq(MIXER_PWM_FREQUENCY_HZ);
  analogWriteRange(MIXER_PWM_RANGE);
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  pinMode(MIXER_PIN, OUTPUT);
  analogWrite(MIXER_PIN, MIXER_PWM_RANGE);

  pinMode(COOLER_PIN, OUTPUT);
  digitalWrite(COOLER_PIN, coolerOn);

}

bool heaterOutputActive() {
  const bool holdTemperatureWhilePaused =
    isPaused && !waitingForTemp; // && !waitingForUser;

  return (heaterOn && isRunning && !inCoolDown &&
          (!isPaused || holdTemperatureWhilePaused)) ||
         (calibrationIsActive() && calibrationHeaterIsOn());
}

// Writes heaterOn/mixerOn out to their actual pins. Called once per loop()
// iteration rather than at every place that sets those booleans, so the
// hardware can never drift out of sync with the state variables.
void applyOutputs() {
  const bool heaterActive = heaterOutputActive();
  digitalWrite(HEATER_PIN, heaterActive ? HIGH : LOW);

  // --- Cooler: on whenever the heater is, and for COOLER_OFF_DELAY_MS
  // after the heater last turned off, so brief heater cycling never
  // toggles the cooler off and on in between. ---
  if (heaterActive) {
    // Heater is on (or back on within the delay window): cooler stays on,
    // and the off-timer is reset for whenever the heater next stops.
    coolerOn = true;
    heaterWasOn = true;
  } else {
    if (heaterWasOn) {
      // Heater just turned off: start the 1-minute cooldown window.
      heaterOffTime = millis();
      heaterWasOn = false;
    }
    // Stays on until COOLER_OFF_DELAY_MS has elapsed since the heater
    // last switched off. If the heater cycles back on before then,
    // the branch above keeps coolerOn true the whole time, so short
    // heater cycling never toggles the cooler off in between.
    coolerOn = (millis() - heaterOffTime) < COOLER_OFF_DELAY_MS;
  }
  digitalWrite(COOLER_PIN, coolerOn);

  // --- Mixer: soft-start ramp. When mixerOn goes true, duty starts at
  // MIXER_START_DUTY (slow) and linearly falls to 0 (full speed) over
  // MIXER_RAMP_TIME_MS, instead of snapping straight to full speed. ---
  if (!mixerOn) {
    analogWrite(MIXER_PIN, MIXER_PWM_RANGE); // fully off
    mixerWasOn = false;
  } else {
    if (!mixerWasOn) {
      // Rising edge: start a new ramp from now.
      mixerStartTime = millis();
      mixerWasOn = true;
    }

    const uint32_t elapsed = millis() - mixerStartTime;
    const uint32_t duty = elapsed >= MIXER_RAMP_TIME_MS
                              ? 0 // ramp finished: full speed
                              : MIXER_START_DUTY -
                                  (MIXER_START_DUTY * elapsed) / MIXER_RAMP_TIME_MS;
    analogWrite(MIXER_PIN, duty);
  }
}