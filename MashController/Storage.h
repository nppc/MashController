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
#define STORAGE_SSID_LEN     24
#define STORAGE_PASS_LEN     24

#define STORAGE_MAGIC        0x4D415348UL  // "MASH" - marks EEPROM as initialized
#define STORAGE_VERSION      1             // bump if the struct layout changes

/* -------------------------------------------------------------------------- */
/*                              ON-FLASH LAYOUT                               */
/* -------------------------------------------------------------------------- */

struct StepEE {
  float    temp;
  uint16_t timeMin;
};

struct ProfileEE {
  char    name[STORAGE_NAME_LEN];
  uint8_t stepCount;
  StepEE  steps[STORAGE_MAX_STEPS];
};

struct SettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  float    heaterHysteresis;     // degrees C either side of target
  uint16_t mixerDurationSec;     // length of the mix cycle window
  uint8_t  mixerPercent;         // % of mixerDurationSec the motor runs
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
