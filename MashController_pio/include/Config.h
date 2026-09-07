#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Uncomment to use the simulated temperature model instead of a real
// DS18B20 sensor - handy for testing mash logic/UI without hardware.
// #define DEBUG_FAKE_TEMP

#define ONE_WIRE_BUS D2

// Heater SSR: driven directly, active-high (GPIO HIGH -> SSR on).
#define HEATER_PIN D1

// Mixer motor: GPIO drives a small-signal N-MOSFET which, in turn, pulls
// down the gate of the high-power IRFZ30 (pulled up externally to the
// gate-supply rail). That makes this channel ACTIVE-LOW from the GPIO's
// perspective:
//   GPIO HIGH -> small-signal FET ON  -> IRFZ30 gate LOW  -> motor OFF
//   GPIO LOW  -> small-signal FET OFF -> IRFZ30 gate HIGH -> motor ON
// Add an external pull-up (GPIO to 3.3V) on this pin so the motor
// defaults OFF during the boot window before setup() runs.
#define MIXER_PIN D5

#define HISTORY_SIZE 180
#define READ_INTERVAL_MS 2000UL
#define CONVERSION_TIME_MS 750UL   // 12-bit resolution conversion time

#endif
