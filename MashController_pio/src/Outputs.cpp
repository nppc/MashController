#include <Arduino.h>
#include "Outputs.h"
#include "Config.h"
#include "Heater.h"
#include "Mixer.h"

namespace {
constexpr uint32_t MIXER_PWM_FREQUENCY_HZ = 500;
constexpr uint32_t MIXER_PWM_RANGE = 1023;
constexpr uint32_t MIXER_RAMP_TIME_MS = 500;
constexpr uint32_t MIXER_START_DUTY = (MIXER_PWM_RANGE + 1) / 2;

uint32_t mixerStartTime = 0;
bool mixerWasOn = false;
}

void outputsInit() {
  analogWriteFreq(MIXER_PWM_FREQUENCY_HZ);
  analogWriteRange(MIXER_PWM_RANGE);
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  pinMode(MIXER_PIN, OUTPUT);
  analogWrite(MIXER_PIN, MIXER_PWM_RANGE);
}

// Writes heaterOn/mixerOn out to their actual pins. Called once per loop()
// iteration rather than at every place that sets those booleans, so the
// hardware can never drift out of sync with the state variables.
void applyOutputs() {
  digitalWrite(HEATER_PIN, heaterOn ? HIGH : LOW);

  if (!mixerOn) {
    analogWrite(MIXER_PIN, MIXER_PWM_RANGE);
    mixerWasOn = false;
  } else {
    if (!mixerWasOn) {
      mixerStartTime = millis();
      mixerWasOn = true;
    }

    const uint32_t elapsed = millis() - mixerStartTime;
    const uint32_t duty = elapsed >= MIXER_RAMP_TIME_MS
                              ? 0
                              : MIXER_START_DUTY -
                                  (MIXER_START_DUTY * elapsed) / MIXER_RAMP_TIME_MS;
    analogWrite(MIXER_PIN, duty);
  }
}
