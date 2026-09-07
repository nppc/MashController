#include "Sensor.h"
#include "Heater.h"   // fakeReadTemperature() reads heaterOn / calls updateHeater()

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

DeviceAddress sensorAddr;
bool sensorFound = false;
bool sensorOk = true;
float lastGoodTemp = 20.0f;
bool conversionInProgress = false;

float tempHistory[HISTORY_SIZE];
int histIndex = 0;

static unsigned long conversionStartTime = 0;
static unsigned long nextConversionAllowedAt = 0;

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

// Cheap getter - safe to call anytime (e.g. from handleStatus()); never blocks.
float readTemperature() {
  return lastGoodTemp;
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
static float fakeReadTemperature() {
  float last = (histIndex > 0)
               ? tempHistory[(histIndex - 1) % HISTORY_SIZE]
               : 20.0f;

  updateHeater(last);

  const float dt = (float)READ_INTERVAL_MS / 1000.0f;

  /* Physical parameters */
  const float heatingRate = 0.35f;  /* °C/s at full power */
  const float coolingRate = 0.20f;  /* °C/s */
  const float heatLoss = 0.10f;     /* stored heat lost per update */

  static float waterTemp = 20.0f;
  static float storedHeat = 0.0f;

  float heatInput;

  /* Heater startup ramp */
  static int heaterRamp = 0;

  if (heaterOn) {
    if (heaterRamp < 3)
      heaterRamp++;
  } else {
    heaterRamp = 0;
  }

  /*
   * Add energy while heater is ON.
   */
  if (heaterOn) {
    float rampFactor = 0.4f + 0.2f * heaterRamp;
    heatInput = heatingRate * rampFactor * dt;
    storedHeat += heatInput;
  }

  /*
   * Stored heat is released gradually.
   *
   * This is what creates thermal inertia:
   * the heater can switch OFF while storedHeat
   * is still positive, so the water keeps warming.
   */
  if (storedHeat > 0.0f) {
    float released = storedHeat * heatLoss;

    /* Don't release more heat than is stored */
    if (released > storedHeat)
      released = storedHeat;

    storedHeat -= released;
    waterTemp += released;
  }

  /*
   * Natural cooling.
   */
  if (waterTemp > 20.0f) {
    waterTemp -= coolingRate * dt;

    if (waterTemp < 20.0f)
      waterTemp = 20.0f;
  }

  /* Small measurement noise */
  waterTemp += ((float)random(-5, 6)) / 100.0f;

  /* Round to 0.1 °C */
  waterTemp = roundf(waterTemp * 10.0f) / 10.0f;

  return waterTemp;
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
      updateHeater(lastGoodTemp);
      addTemp(lastGoodTemp);
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
