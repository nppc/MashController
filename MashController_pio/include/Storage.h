#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <EEPROM.h>

/* -------------------------------------------------------------------------- */
/*                                   LIMITS                                   */
/* -------------------------------------------------------------------------- */

#define STORAGE_MAX_PROFILES 15    // raise if you need more, plenty of EEPROM headroom
#define STORAGE_MAX_STEPS    6     // matches Step steps[6] used elsewhere in the sketch
#define STORAGE_NAME_LEN     24
#define STORAGE_SSID_LEN     33
#define STORAGE_PASS_LEN     64

#define STORAGE_MAGIC        0x4D415348UL  // "MASH" - marks EEPROM as initialized
#define STORAGE_VERSION      16            // bump if the struct layout changes
#define STORAGE_PREVIOUS_VERSION 15        // the ONLY older layout migrated (see Storage.cpp);
                                           // bumping STORAGE_VERSION without updating this
                                           // and the Legacy struct breaks the build on purpose

/* -------------------------------------------------------------------------- */
/*                              ON-FLASH LAYOUT                               */
/* -------------------------------------------------------------------------- */

enum WifiModeEE : uint8_t {
  WIFI_MODE_AP = 0,
  WIFI_MODE_STA = 1
};

struct StepEE {
  float    temp;
  uint16_t timeMin;
};

struct ProfileEE {
  char    name[STORAGE_NAME_LEN];
  uint8_t stepCount;
  float   waterMassKg;
  float   grainMassKg;
  StepEE  steps[STORAGE_MAX_STEPS];
};

struct SettingsEE {
  uint8_t  wifiMode;
  char     wifiApSSID[STORAGE_SSID_LEN];
  char     wifiApPass[STORAGE_PASS_LEN];
  char     wifiStaSSID[STORAGE_SSID_LEN];
  char     wifiStaPass[STORAGE_PASS_LEN];
  float    heaterPowerW;         // installed (rated) heater electrical power
  float    heaterPowerEffW;      // calibrated power that reaches the water
  float    heaterTauSec;         // calibrated time constant of the stored heat
  float    heaterStoreGain;      // calibrated overshoot / (heaterPowerEffW * heaterTauSec)
  float    heaterLossWPerC;      // calibrated heat loss, watts per degree C above room
  float    heaterAmbientC;       // room temperature used for the heat loss
  float    heaterDeadband;       // predicted peak must fall this far below target to switch on
  uint16_t heaterMinSwitchSec;   // minimum time between heater switches

  // Dual-point calibration learned parameters (internal)
  float    heaterT1;             // Cal point 1 temp (e.g. 50.0 C)
  float    heaterTau1;           // tau at T1 (s)
  float    heaterLossW1;         // loss at T1 (W)
  float    heaterStoreGain1;     // store gain at T1
  float    heaterT2;             // Cal point 2 temp (e.g. 70.0 C)
  float    heaterTau2;           // tau at T2 (s)
  float    heaterLossW2;         // loss at T2 (W)
  float    heaterStoreGain2;     // store gain at T2

  // User fine-tune knobs
  float    heaterTrimLowC;       // Low rest trim at 50 C (e.g. -0.5 to +0.5 C, default 0.0 C)
  float    heaterTrimHighC;      // High rest trim at 70 C (e.g. -0.5 to +0.5 C, default 0.0 C)
  float    heaterLossScalePct;   // Insulation / loss scale % (70 to 130 %, default 100 %)

  uint8_t  mixerRestSec;         // length of the rest period
  uint8_t  mixerOnSec;           // seconds of mixerDurationSec the motor runs
  uint16_t coolDownSec;          // seconds mixer runs after profile completes
  float    targetReachedHystC;   // a step counts as reached at target minus this many degrees C
  bool     alertSoundEnabled;    // browser plays the alert chime when a manual Pause step is ready
  bool     mdnsEnabled;          // true = start mDNS responder (http://mash.local)
};

struct EepromDataEE {
  uint32_t   magic;
  uint16_t   version;
  SettingsEE settings;
  uint8_t    profileCount;
  ProfileEE  profiles[STORAGE_MAX_PROFILES];
  uint32_t   crc;                // CRC32 over everything above, computed on save
};

// ESP8266 EEPROM emulation lives in a single 4KB flash sector - keep us honest.
static_assert(sizeof(EepromDataEE) <= 4096,
              "EepromDataEE too large for the ESP8266 EEPROM sector (4096 bytes max)");

/* -------------------------------------------------------------------------- */
/*                                   STORAGE                                  */
/* -------------------------------------------------------------------------- */

class Storage {
  public:
    // Mounts EEPROM and loads data. Returns true if valid data was found,
    // false if defaults had to be written (e.g. first boot / corrupted data).
    bool begin();

    // Persists the current in-RAM copy (settings + profiles) to flash.
    bool save();

    // Mutable reference to the in-RAM settings. Modify in place, then call save().
    SettingsEE& settings();

    uint8_t profileCount() const;

    // Copies profile [index] into 'out'. Returns false if index is out of range.
    bool getProfile(uint8_t index, ProfileEE &out) const;

    // Overwrites profile [index] if it exists, or appends it if index == profileCount().
    // Returns false if index is invalid or storage is full.
    bool setProfile(uint8_t index, const ProfileEE &p);

    bool deleteProfile(uint8_t index);
    void clearProfiles();

  private:
    EepromDataEE _data;

    void     loadDefaults();
    uint32_t crc32(const uint8_t *data, size_t length) const;
};

extern Storage storage;

#endif
