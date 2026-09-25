#include "Sensor.h"
#include "Heater.h"    // updateHeater() runs on every new reading
#include "Outputs.h"   // fakeReadTemperature() follows the real heater output

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

DeviceAddress sensorAddr;
bool sensorFound = false;
bool sensorOk = true;
float lastGoodTemp = 20.0f;
bool conversionInProgress = false;

float tempHistory[HISTORY_SIZE];
int histIndex = 0;

#ifndef DEBUG_FAKE_TEMP
static unsigned long conversionStartTime = 0;
static unsigned long nextConversionAllowedAt = 0;
#endif
constexpr uint8_t TEMPERATURE_AVERAGE_SAMPLES = 5;

static void addTemp(float t) {
  tempHistory[histIndex % HISTORY_SIZE] = t;
  histIndex++;
}

/* -------------------------------------------------------------------------- */
/*                        DS18B20 DISCOVERY & NON-BLOCKING READ               */
/* -------------------------------------------------------------------------- */

// Scans the OneWire bus for a DS18B20 (family code 0x28), validates its
// address CRC, and stores it in sensorAddr. Used at boot and on-demand via
// /rediscoverSensor. Uses OneWire::search() directly rather than
// DallasTemperature's own enumeration, which has proven less reliable here.
bool discoverSensorAddress() {
  oneWire.reset_search();
  if (!oneWire.search(sensorAddr)) {
    Serial.println("No DS18B20 found on bus");
    return false;
  }

  if (OneWire::crc8(sensorAddr, 7) != sensorAddr[7]) {
    Serial.println("DS18B20 address CRC mismatch");
    return false;
  }

  if (sensorAddr[0] != 0x28) {
    Serial.println("Device found is not a DS18B20 (wrong family code)");
    return false;
  }

  Serial.print("DS18B20 found at address: ");
  for (int i = 0; i < 8; i++) {
    if (sensorAddr[i] < 16) Serial.print("0");
    Serial.print(sensorAddr[i], HEX);
    Serial.print(" ");
  }
  Serial.println();

  return true;
}

// Formats sensorAddr as a colon-separated hex string for the settings API.
String sensorAddressToString() {
  if (!sensorFound) return "none";

  String out = "";
  for (int i = 0; i < 8; i++) {
    if (sensorAddr[i] < 16) out += "0";
    out += String(sensorAddr[i], HEX);
    if (i < 7) out += ":";
  }
  out.toUpperCase();
  return out;
}

// Shared filtered reading for the heater, mash control, calibration, and UI.
// The raw latest value remains available as lastGoodTemp.
float readTemperature() {
  const int available = min(histIndex, (int)TEMPERATURE_AVERAGE_SAMPLES);
  if (available <= 0) return lastGoodTemp;

  float total = 0.0f;
  const int first = histIndex - available;
  for (int i = 0; i < available; i++) {
    total += tempHistory[(first + i) % HISTORY_SIZE];
  }
  return total / available;
}

float averageTemperature(float windowSec) {
  int count = (int)lroundf(windowSec * 1000.0f / (float)READ_INTERVAL_MS);
  count = constrain(count, 1, HISTORY_SIZE);
  const int available = min(histIndex, count);
  if (available <= 0) return lastGoodTemp;

  float total = 0.0f;
  const int first = histIndex - available;
  for (int i = 0; i < available; i++) {
    total += tempHistory[(first + i) % HISTORY_SIZE];
  }
  return total / available;
}

void sensorInit() {
#ifdef DEBUG_FAKE_TEMP
  Serial.println("DEBUG_FAKE_TEMP active - using simulated temperature, no DS18B20 required");
  sensorFound = true;
  sensorOk = true;
#else
  sensors.begin();
  sensors.setWaitForConversion(false);   // non-blocking conversions

  sensorFound = discoverSensorAddress();
  if (sensorFound) {
    sensors.setResolution(sensorAddr, 12);
  } else {
    Serial.println("WARNING: proceeding without a temperature sensor");
  }
#endif
}

/* -------------------------------------------------------------------------- */
/*                           TEMPERATURE SIMULATION                           */
/*        (kept for debugging - only compiled in when DEBUG_FAKE_TEMP is on)  */
/* -------------------------------------------------------------------------- */

#ifdef DEBUG_FAKE_TEMP
// Simulated 20 L pot on a 2 kW under-base element. The element and pot base
// store heat and pass it on to the water, so the temperature keeps rising
// after switch-off, just like the real hardware. Starts at 15 C so the
// calibration test can be run against it.
static float fakeReadTemperature() {
  constexpr float POWER_W = 1900.0f;
  constexpr float TRANSFER_W_PER_C = 30.0f;
  constexpr float ELEMENT_J_PER_C = 2500.0f;   // tau = 2500 / 30 = 83 s
  constexpr float WATER_J_PER_C = 20.0f * 4186.0f;
  constexpr float LOSS_W_PER_C = 5.0f;
  constexpr float AMBIENT_C = 20.0f;
  constexpr float SENSOR_TAU_SEC = 10.0f;
  constexpr int SUBSTEPS = 4;

  static float elementTemp = 15.0f;
  static float waterTemp = 15.0f;
  static float sensorTemp = 15.0f;

  const float dt = (float)READ_INTERVAL_MS / 1000.0f / SUBSTEPS;
  const float power = heaterOutputActive() ? POWER_W : 0.0f;

  for (int i = 0; i < SUBSTEPS; i++) {
    const float toWater = TRANSFER_W_PER_C * (elementTemp - waterTemp);
    const float loss = LOSS_W_PER_C * (waterTemp - AMBIENT_C);
    elementTemp += (power - toWater) * dt / ELEMENT_J_PER_C;
    waterTemp += (toWater - loss) * dt / WATER_J_PER_C;
    sensorTemp += (waterTemp - sensorTemp) * dt / SENSOR_TAU_SEC;
  }

  // DS18B20: small noise, 12-bit (1/16 C) resolution.
  const float noisy = sensorTemp + (float)random(-3, 4) / 100.0f;
  return roundf(noisy * 16.0f) / 16.0f;
}
#endif // DEBUG_FAKE_TEMP

// Drives temperature acquisition without blocking the HTTP server.
// - Real sensor: kicks off a conversion, then reads it back once
//   CONVERSION_TIME_MS has elapsed - call this every loop() iteration.
//   Paced to roughly once per READ_INTERVAL_MS so OneWire bit-banging
//   (which briefly disables interrupts) doesn't run back-to-back and
//   starve the WiFi/HTTP stack.
// - DEBUG_FAKE_TEMP: recomputes the simulated model - call this once per
//   READ_INTERVAL_MS, same cadence as the original code.
void sensorUpdate() {
#ifdef DEBUG_FAKE_TEMP
  float t = fakeReadTemperature();
  sensorFound = true;
  sensorOk = true;
  lastGoodTemp = t;
  addTemp(t);
  updateHeater();
#else
  if (!sensorFound) return;

  unsigned long now = millis();

  if (!conversionInProgress) {
    if (now < nextConversionAllowedAt) return;   // resting between cycles - don't hammer the bus

    sensors.requestTemperatures();      // returns immediately, doesn't block
    conversionStartTime = now;
    conversionInProgress = true;
    return;
  }

  if (now - conversionStartTime >= CONVERSION_TIME_MS) {
    float t = sensors.getTempC(sensorAddr);

    if (t == DEVICE_DISCONNECTED_C) {
      sensorOk = false;
      Serial.println("DS18B20 read error");
      // keep lastGoodTemp as-is; don't feed a garbage value to updateHeater()
    } else {
      sensorOk = true;
      lastGoodTemp = t;
      addTemp(lastGoodTemp);
      updateHeater();
    }

    conversionInProgress = false;   // next call starts a fresh conversion

    // Rest until the remainder of READ_INTERVAL_MS has elapsed before
    // starting the next conversion, rather than firing immediately again.
    if (READ_INTERVAL_MS > CONVERSION_TIME_MS) {
      nextConversionAllowedAt = now + (READ_INTERVAL_MS - CONVERSION_TIME_MS);
    } else {
      nextConversionAllowedAt = now;
    }
  }
#endif
}
