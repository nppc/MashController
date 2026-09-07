#ifndef MIXER_H
#define MIXER_H

extern bool mixerOn;              // relay/mixer-FET state
extern bool mixerManualMode;      // false = auto (on/rest cycle), true = manual override
extern unsigned long mixerPhaseStart; // millis() when current on/rest phase began

void mixerInit();     // call once from setup(): safe default = manual, off
void updateMixer();   // call once per READ_INTERVAL_MS pass from loop()

// Seconds remaining in the current phase (on-phase in manual+on, or the
// auto on/rest phase). Used for the status endpoint's countdown.
unsigned long mixerRemainingSec();

#endif
