#include "Mixer.h"
#include "Outputs.h"
#include "Storage.h"

bool mixerOn = false;
bool mixerManualMode = false;
bool mixerFollowingHeater = false;
unsigned long mixerPhaseStart = 0;

void mixerInit() {
  // Safe default on startup: manual mode, off.
  mixerManualMode = true;
  mixerOn = false;
  mixerFollowingHeater = false;
  mixerPhaseStart = millis();
}

unsigned long mixerRemainingSec() {
  if (mixerFollowingHeater) return 0;

  SettingsEE &s = storage.settings();
  unsigned long phaseLen = mixerOn ? s.mixerOnSec : s.mixerRestSec;
  unsigned long elapsed = (millis() - mixerPhaseStart) / 1000;
  if (elapsed >= phaseLen) return 0;
  return phaseLen - elapsed;
}

void updateMixer() {
  if (mixerManualMode) {
    // Manual mode: relay state is whatever the last /mixerToggle set it to.
    // No automatic phase switching here.
    mixerFollowingHeater = false;
    return;
  }

  SettingsEE &s = storage.settings();
  const bool heaterActive = s.mixerFollowHeater && heaterOutputActive();
  if (heaterActive) {
    mixerOn = true;
    mixerFollowingHeater = true;
    return;
  }

  if (mixerFollowingHeater) {
    // Continue through one normal mixer-on phase after the heater switches off.
    mixerFollowingHeater = false;
    mixerOn = true;
    mixerPhaseStart = millis();
    return;
  }

  unsigned long phaseLen = mixerOn ? s.mixerOnSec : s.mixerRestSec;
  unsigned long elapsed = (millis() - mixerPhaseStart) / 1000;

  if (elapsed >= phaseLen) {
    mixerOn = !mixerOn;
    mixerPhaseStart = millis();
  }
}
