#ifndef MIXER_H
#define MIXER_H

extern bool mixerOn;              // relay/mixer-FET state
extern bool mixerManualMode;      // false = auto (on/rest cycle), true = manual override
extern bool mixerFollowingHeater;  // Auto mixer is being held on by the heater
extern unsigned long mixerPhaseStart; // millis() when current on/rest phase began

void mixerInit();     // call once from setup(): safe default = manual, off
void updateMixer();   // call each loop to follow heater state and update auto phases

// Seconds remaining in the current auto on/rest phase. Used by the status
// endpoint's countdown; returns zero while following the heater.
unsigned long mixerRemainingSec();

#endif
