#ifndef MASH_PROFILE_H
#define MASH_PROFILE_H

#include <Arduino.h>

struct Step {
  float temp;
  int time; // minutes
};

struct Profile {
  String name;
  Step steps[6];
  int stepCount;
};

extern Profile activeProfile;

/* ---- Mash profile execution ---- */
extern bool isRunning;
extern bool isPaused;
extern int currentStep;
extern bool waitingForTemp;

extern float targetTemperature;   // unified target temperature

extern unsigned long stepStartTime;
extern unsigned long stepDurationSec;
extern unsigned long pausedElapsedSec;   // elapsed time banked when paused

extern bool inCoolDown;                  // Cool-down phase active
extern unsigned long coolDownStart;      // When cool-down began

bool loadProfile(int index);
void advanceStep();

// Cool-down countdown - call every loop() iteration (independent of
// READ_INTERVAL_MS) so STOP stays responsive and timing stays precise.
void mashProfileCheckCoolDown();

// Step-timing state machine (waitingForTemp / elapsed / advanceStep) -
// call once per READ_INTERVAL_MS pass from loop().
void mashProfileTick();

#endif
