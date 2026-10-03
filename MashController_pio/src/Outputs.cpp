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
constexpr uint32_t MIXER_PWM_FREQUENCY_HZ = 1000;
constexpr uint32_t MIXER_PWM_RANGE = 1023;
// How long the mixer takes to ramp from its starting duty up to full speed after
// being switched on, to avoid slamming it to full speed instantly.
constexpr uint32_t MIXER_RAMP_TIME_MS = 500;
// Start the mixer at a low duty cycle, then increase toward full speed. The
// final GPIO duty is inverted when the hardware expects an active-low drive.
constexpr uint32_t MIXER_START_DUTY = 256;

inline uint32_t applyMixerOutputInvert(uint32_t duty) {
#if MIXER_OUTPUT_INVERTED
  return MIXER_PWM_RANGE - duty;
#else
  return duty;
#endif
}

// Cooler post-run follows the heater's last on-time, within these bounds.
constexpr uint32_t COOLER_MIN_RUN_MS = 30UL * 1000UL;
constexpr uint32_t COOLER_MAX_RUN_MS = 5UL * 60UL * 1000UL;

// Mixer ramp bookkeeping.
uint32_t mixerStartTime = 0; // millis() timestamp when the current ramp began
bool mixerWasOn = false;     // tracks mixerOn's previous state to detect the on-edge

// Heater interval and cooler post-run bookkeeping.
uint32_t heaterOnStartTime = 0;
uint32_t heaterOffTime = 0;
uint32_t coolerRunDurationMs = 0;
bool heaterWasOn = false;
}

void outputsInit() {

  heaterOffTime = millis();
  coolerRunDurationMs = 0;
  heaterWasOn = false;
  coolerOn = false;

  analogWriteFreq(MIXER_PWM_FREQUENCY_HZ);
  analogWriteRange(MIXER_PWM_RANGE);
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  pinMode(MIXER_PIN, OUTPUT);
  analogWrite(MIXER_PIN, applyMixerOutputInvert(0));

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
  const uint32_t now = millis();
  const bool heaterActive = heaterOutputActive();
  digitalWrite(HEATER_PIN, heaterActive ? HIGH : LOW);

  // --- Cooler: on with the heater, then for a bounded period matching its
  // last on-time. ---
  if (heaterActive) {
    if (!heaterWasOn) {
      heaterOnStartTime = now;
      heaterWasOn = true;
    }
    coolerOn = true;
  } else {
    if (heaterWasOn) {
      heaterOffTime = now;
      coolerRunDurationMs = now - heaterOnStartTime;
      if (coolerRunDurationMs < COOLER_MIN_RUN_MS) {
        coolerRunDurationMs = COOLER_MIN_RUN_MS;
      } else if (coolerRunDurationMs > COOLER_MAX_RUN_MS) {
        coolerRunDurationMs = COOLER_MAX_RUN_MS;
      }
      heaterWasOn = false;
    }
    coolerOn = coolerRunDurationMs > 0 &&
               (now - heaterOffTime) < coolerRunDurationMs;
  }
  digitalWrite(COOLER_PIN, coolerOn);

  // --- Mixer: soft-start ramp. When mixerOn goes true, the PWM duty ramps up
  // from a low starting value to full speed over MIXER_RAMP_TIME_MS. The final
  // GPIO duty is optionally inverted to support active-low mixer drivers. ---
  if (!mixerOn) {
    analogWrite(MIXER_PIN, applyMixerOutputInvert(0)); // fully off
    mixerWasOn = false;
  } else {
    if (!mixerWasOn) {
      // Rising edge: start a new ramp from now.
      mixerStartTime = millis();
      mixerWasOn = true;
    }

    const uint32_t elapsed = millis() - mixerStartTime;
    uint32_t duty = MIXER_PWM_RANGE; // ramp finished: full speed
    if (elapsed < MIXER_RAMP_TIME_MS) {
      // Quadratic ease-in on speed: start at a low duty and ramp up to full.
      duty = MIXER_START_DUTY +
             ((MIXER_PWM_RANGE - MIXER_START_DUTY) * elapsed * elapsed) /
                 (MIXER_RAMP_TIME_MS * MIXER_RAMP_TIME_MS);
    }
    analogWrite(MIXER_PIN, applyMixerOutputInvert(duty));
  }
}
