#include "Sensor.h"
#include "Heater.h"    // updateHeater() runs on every new reading
#include "Outputs.h"   // fakeReadTemperature() follows the real heater output
#include "Calibration.h"
#include "MashProfile.h"
#include "Storage.h"   // fakeReadTemperature() reads heaterAmbientC from settings

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

namespace {
constexpr float DEFAULT_FAKE_TEMP_SPEED_MULTIPLIER = 2.0f;
constexpr float FAKE_TEMP_WATER_LITERS = 10.0f;

bool fakeMode = false;
float fakeSpeedMultiplier = DEFAULT_FAKE_TEMP_SPEED_MULTIPLIER;
unsigned long conversionStartTime = 0;
unsigned long nextConversionAllowedAt = 0;
unsigned long fakeLastUpdateAt = 0;
float fakeElementTemp = 15.0f;
float fakeWaterTemp = 15.0f;
float fakeSensorTemp = 15.0f;
float fakeWaterLiters = FAKE_TEMP_WATER_LITERS;
float fakeGrainMassKg = 0.0f;
float fakeAmbientOverrideC = NAN;

void resetFakePlant(float waterLiters, float grainMassKg, float ambientC,
                    float initialTemperature) {
  fakeWaterLiters = waterLiters;
  fakeGrainMassKg = grainMassKg;
  fakeAmbientOverrideC = ambientC;
  fakeElementTemp = initialTemperature;
  fakeWaterTemp = initialTemperature;
  fakeSensorTemp = initialTemperature;
  fakeLastUpdateAt = millis();
}
}

DeviceAddress sensorAddr;
bool sensorFound = false;
bool sensorOk = true;
float lastGoodTemp = NAN;
bool conversionInProgress = false;

int16_t tempHistory[HISTORY_SIZE];
int histIndex = 0;

constexpr uint8_t TEMPERATURE_AVERAGE_SAMPLES = 5;

static void addTemp(float t) {
  tempHistory[histIndex % HISTORY_SIZE] = (int16_t)lroundf(t * 100.0f);
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

  int32_t total = 0;   // sum of centi-C; int32_t so it can't overflow across HISTORY_SIZE samples
  const int first = histIndex - available;
  for (int i = 0; i < available; i++) {
    total += tempHistory[(first + i) % HISTORY_SIZE];
  }
  return (total / 100.0f) / available;
}

float averageTemperature(float windowSec) {
  int count = (int)lroundf(windowSec * 1000.0f / (float)READ_INTERVAL_MS);
  count = constrain(count, 1, HISTORY_SIZE);
  const int available = min(histIndex, count);
  if (available <= 0) return lastGoodTemp;

  int32_t total = 0;   // sum of centi-C; int32_t so it can't overflow across HISTORY_SIZE samples
  const int first = histIndex - available;
  for (int i = 0; i < available; i++) {
    total += tempHistory[(first + i) % HISTORY_SIZE];
  }
  return (total / 100.0f) / available;
}

void sensorInit() {
  sensors.begin();
  sensors.setWaitForConversion(false);   // non-blocking conversions

  sensorFound = discoverSensorAddress();
  if (sensorFound) {
    sensors.setResolution(sensorAddr, 12);
  } else {
    sensorOk = false;
    Serial.println("WARNING: proceeding without a temperature sensor");
  }
  fakeLastUpdateAt = millis();
}

/* -------------------------------------------------------------------------- */
/*                           TEMPERATURE SIMULATION                           */
/*          (compiled in, but only used when simulated mode is selected)     */
/* -------------------------------------------------------------------------- */

// Simulated pot on an under-base element. The element and pot base
// store heat and pass it on to the water, so the temperature keeps rising
// after switch-off, just like the real hardware. It starts at 15 C after boot.
// Calibration resets it to its current reading and supplies its own water
// volume and ambient temperature.
// Set this to speed up the simulation; sampling cadence and calibration
// timestamps scale with it so fitted physical parameters remain unchanged.

void sensorPrepareFakeCalibration(float waterLiters, float ambientC, float initialTemperature) {
  resetFakePlant(waterLiters, 0.0f, ambientC, initialTemperature);
}

void sensorPrepareFakeMash(float waterLiters, float initialTemperature) {
  resetFakePlant(waterLiters, 0.0f, storage.settings().heaterAmbientC,
                 initialTemperature);
}

void sensorAddFakeGrain(float grainMassKg) {
  fakeGrainMassKg += max(grainMassKg, 0.0f);
}

void sensorEndFakeCalibration() {
  fakeWaterLiters = FAKE_TEMP_WATER_LITERS;
  fakeGrainMassKg = 0.0f;
  fakeAmbientOverrideC = NAN;
}

bool sensorUsingFakeMode() {
  return fakeMode;
}

float sensorFakeSpeedMultiplier() {
  return fakeSpeedMultiplier;
}

bool sensorSetFakeSpeedMultiplier(float multiplier) {
  if (!isfinite(multiplier) || multiplier < 1.0f || multiplier > 20.0f) return false;
  if (fakeMode && (isRunning || inCoolDown || calibrationIsActive())) return false;
  fakeSpeedMultiplier = multiplier;
  return true;
}

bool sensorSetFakeMode(bool enabled) {
  if (enabled == fakeMode) return true;
  if (isRunning || inCoolDown || calibrationIsActive()) return false;

  conversionInProgress = false;
  nextConversionAllowedAt = 0;
  heaterOn = false;

  if (enabled) {
    const float initialTemperature = isfinite(lastGoodTemp) ? lastGoodTemp : 15.0f;
    fakeMode = true;
    sensorPrepareFakeCalibration(FAKE_TEMP_WATER_LITERS,
                                 storage.settings().heaterAmbientC,
                                 initialTemperature);
    sensorFound = true;
    sensorOk = true;
    lastGoodTemp = initialTemperature;
    histIndex = 0;
    addTemp(initialTemperature);
    Serial.println("Temperature source switched to simulation");
  } else {
    fakeMode = false;
    sensorFound = discoverSensorAddress();
    sensorOk = sensorFound;
    if (sensorFound) sensors.setResolution(sensorAddr, 12);
    histIndex = 0;
    lastGoodTemp = NAN;
    Serial.println(sensorFound
                       ? "Temperature source switched to DS18B20"
                       : "DS18B20 not found after leaving simulation");
  }
  return true;
}

static float fakeReadTemperature() {
  const float POWER_W = storage.settings().heaterPowerW;
  constexpr float TRANSFER_W_PER_C = 30.0f;
  constexpr float ELEMENT_J_PER_C = 2500.0f;
  constexpr float LOSS_W_PER_C = 5.0f;
  constexpr float SENSOR_TAU_SEC = 10.0f;
  constexpr int MIN_SUBSTEPS = 4;

  const float waterCapacityJPerC =
      fakeWaterLiters * 4186.0f + fakeGrainMassKg * 1900.0f;
  const float ambientC = isfinite(fakeAmbientOverrideC)
      ? fakeAmbientOverrideC
      : storage.settings().heaterAmbientC;
  const unsigned long now = millis();
  const float elapsedSimSec =
      (float)(now - fakeLastUpdateAt) / 1000.0f * sensorSimulationTimeScale();
  fakeLastUpdateAt = now;
  const int substeps = max(MIN_SUBSTEPS, (int)ceilf(elapsedSimSec));
  const float dt = elapsedSimSec / substeps;
  const float power = heaterOutputActive() ? POWER_W : 0.0f;

  for (int i = 0; i < substeps; i++) {
    const float toWater = TRANSFER_W_PER_C * (fakeElementTemp - fakeWaterTemp);
    const float loss = LOSS_W_PER_C * (fakeWaterTemp - ambientC);
    fakeElementTemp += (power - toWater) * dt / ELEMENT_J_PER_C;
    fakeWaterTemp += (toWater - loss) * dt / waterCapacityJPerC;
    fakeSensorTemp += (fakeWaterTemp - fakeSensorTemp) * dt / SENSOR_TAU_SEC;
  }

  // DS18B20: small noise, 12-bit (1/16 C) resolution.
  const float noisy = fakeSensorTemp + (float)random(-3, 4) / 100.0f;
  return roundf(noisy * 16.0f) / 16.0f;
}

float sensorSimulationTimeScale() {
  return fakeMode ? fakeSpeedMultiplier : 1.0f;
}

// Drives temperature acquisition without blocking the HTTP server.
// - Real sensor: kicks off a conversion, then reads it back once
//   CONVERSION_TIME_MS has elapsed - call this every loop() iteration.
//   Paced to roughly once per READ_INTERVAL_MS so OneWire bit-banging
//   (which briefly disables interrupts) doesn't run back-to-back and
//   starve the WiFi/HTTP stack.
// - Fake mode: advances the model by elapsed wall time scaled by its speed
//   multiplier; main.cpp paces calls to preserve sample spacing.
void sensorUpdate() {
  if (fakeMode) {
    const float t = fakeReadTemperature();
    sensorFound = true;
    sensorOk = true;
    lastGoodTemp = t;
    addTemp(t);
    updateHeater();
    return;
  }

  if (!sensorFound) {
    sensorOk = false;
    heaterOn = false;
    calibrationStop();
    return;
  }

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
      heaterOn = false;
      calibrationStop();
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
}