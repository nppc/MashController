#include "Mixer.h"
#include "Storage.h"

bool mixerOn = false;
bool mixerManualMode = false;
unsigned long mixerPhaseStart = 0;

void mixerInit() {
  // Safe default on startup: manual mode, off.
  mixerManualMode = true;
  mixerOn = false;
  mixerPhaseStart = millis();
}

unsigned long mixerRemainingSec() {
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
    return;
  }

  SettingsEE &s = storage.settings();
  unsigned long phaseLen = mixerOn ? s.mixerOnSec : s.mixerRestSec;
  unsigned long elapsed = (millis() - mixerPhaseStart) / 1000;

  if (elapsed >= phaseLen) {
    mixerOn = !mixerOn;
    mixerPhaseStart = millis();
  }
}
