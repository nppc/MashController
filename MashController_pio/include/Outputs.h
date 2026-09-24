#ifndef OUTPUTS_H
#define OUTPUTS_H

void outputsInit();    // call once from setup(), as early as possible
void applyOutputs();   // call every loop() iteration

// True when the heater SSR is actually driven: mash control (respecting
// pause and cool-down) or the calibration test.
bool heaterOutputActive();

#endif
