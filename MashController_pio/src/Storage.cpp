#include "Storage.h"

/* ============================================================================
 * HOW TO USE STORED SETTINGS ACROSS THIS PROJECT
 * ============================================================================
 *
 * `storage` (declared below) is the single global instance. Call
 * `storage.begin()` once from setup() before anything touches it; it loads
 * from EEPROM, migrates older layouts if needed, or writes defaults on first
 * boot / corruption.
 *
 * READING A SETTING (any .cpp file, anywhere in the project):
 *   #include "Storage.h"
 *   float ambient = storage.settings().heaterAmbientC;
 *
 *   `storage.settings()` returns a REFERENCE to the live in-RAM struct
 *   (_data.settings) - not a copy. There is no separate cache to go stale,
 *   so every read reflects the current value, including one changed a
 *   moment ago by a web handler on the same tick. This is how Sensor.cpp's
 *   fakeReadTemperature() picks up heaterAmbientC changes immediately.
 *
 * WRITING A SETTING (e.g. from a web handler):
 *   SettingsEE &s = storage.settings();
 *   s.heaterAmbientC = constrain(newValue, -10.0f, 45.0f);
 *   storage.save();   // <-- REQUIRED to persist to EEPROM
 *
 *   The struct mutation itself takes effect in RAM instantly, for every
 *   other piece of code holding or fetching a reference. `storage.save()`
 *   only controls whether the change SURVIVES A REBOOT - it recomputes the
 *   CRC and writes the whole `_data` blob via EEPROM.commit(). Skipping
 *   save() means the running system behaves correctly but reverts to the
 *   old value on next boot. Always constrain/validate before assigning;
 *   save() does not validate for you (see handleSaveSettings in
 *   WebHandlers.cpp for the pattern of per-field constrain() calls).
 *
 * PROFILES ARE DIFFERENT - NOT A LIVE REFERENCE:
 *   storage.getProfile(index, out)   // copies into `out`
 *   storage.setProfile(index, p)     // copies `p` in (index == count appends)
 *   storage.deleteProfile(index)
 *   storage.clearProfiles()
 *   Profiles are accessed by value, not by reference, so mutate your local
 *   copy and call setProfile() to write it back - then storage.save() to
 *   persist, same as settings.
 *

 * ADDING A NEW SETTING FIELD:
 *   1. Add the field to `SettingsEE` in Storage.h.
 *   2. Give it a sane default in applyHeaterDefaults() (heater-related),
 *      applyMixerDefaults() (mixer/cool-down) or directly in
 *      Storage::loadDefaults() (everything else). Every migration and
 *      loadDefaults() call these helpers first, so a field defaulted there
 *      never ends up as 0 after a memset().
 *   3. If this changes the struct's layout/size, bump STORAGE_VERSION.
 *      The static_assert next to LegacyV12SettingsEE will fail the build
 *      until you:
 *        (a) copy the OLD SettingsEE into a new LegacyV<old>SettingsEE /
 *            LegacyV<old>EepromDataEE pair,
 *        (b) delete the previous Legacy struct pair,
 *        (c) update STORAGE_PREVIOUS_VERSION in Storage.h,
 *        (d) rewrite the migration block in begin() for the new legacy
 *            struct (call the apply*Defaults() helpers, then copy over
 *            every field that existed before).
 *      POLICY: only ONE previous version is migrated. Anything older is
 *      reset to defaults by loadDefaults().
 *      If ProfileEE itself changes, also snapshot the old profile struct
 *      inside the Legacy pair (see how LegacyV10ProfileEE used to be done),
 *      because the Legacy layout embeds ProfileEE.
 * ==========================================================================
 */

Storage storage;

namespace {

// ---- Previous layout (version STORAGE_PREVIOUS_VERSION) -------------------
// Layout of version 14 (before dual-anchor heater calibration was added).
struct LegacyV14SettingsEE {
  uint8_t  wifiMode;
  char     wifiApSSID[STORAGE_SSID_LEN];
  char     wifiApPass[STORAGE_PASS_LEN];
  char     wifiStaSSID[STORAGE_SSID_LEN];
  char     wifiStaPass[STORAGE_PASS_LEN];
  float    heaterPowerW;
  float    heaterPowerEffW;
  float    heaterTauSec;
  float    heaterStoreGain;
  float    heaterLossWPerC;
  float    heaterAmbientC;
  float    heaterDeadband;
  uint16_t heaterMinSwitchSec;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
  float    targetReachedHystC;
  bool     alertSoundEnabled;
};

struct LegacyV14EepromDataEE {
  uint32_t             magic;
  uint16_t             version;
  LegacyV14SettingsEE  settings;
  uint8_t              profileCount;
  ProfileEE            profiles[STORAGE_MAX_PROFILES];
  uint32_t             crc;
};

// Build breaks here when STORAGE_VERSION is bumped without updating the
// migration. See "ADDING A NEW SETTING FIELD" at the top of this file.
static_assert(STORAGE_VERSION == STORAGE_PREVIOUS_VERSION + 1,
              "STORAGE_VERSION was bumped: snapshot the old SettingsEE as "
              "LegacyV<old>SettingsEE, update STORAGE_PREVIOUS_VERSION, "
              "migrate it in begin(), and delete the older Legacy struct.");
static_assert(STORAGE_PREVIOUS_VERSION == 14,
              "STORAGE_PREVIOUS_VERSION no longer matches LegacyV14* - "
              "rename/replace the Legacy struct and migration block.");

// Uncalibrated starting point for a 2 kW under-base element; run the
// Heater Calibration page to replace these with measured values.
void applyHeaterDefaults(SettingsEE &s) {
  s.heaterPowerW = 2000.0f;
  s.heaterPowerEffW = 1900.0f;
  s.heaterTauSec = 90.0f;
  s.heaterStoreGain = 1.0f;
  s.heaterLossWPerC = 4.0f;
  s.heaterAmbientC = 20.0f;
  s.heaterDeadband = 0.05f;
  s.heaterMinSwitchSec = 5;

  s.heaterT1 = 50.0f;
  s.heaterTau1 = 115.0f;
  s.heaterLossW1 = 65.0f;
  s.heaterStoreGain1 = 0.85f;
  s.heaterT2 = 70.0f;
  s.heaterTau2 = 95.0f;
  s.heaterLossW2 = 150.0f;
  s.heaterStoreGain2 = 0.88f;

  s.heaterTrimLowC = 0.0f;
  s.heaterTrimHighC = 0.0f;
  s.heaterLossScalePct = 100.0f;

  // Not heater values, but seeded here because every migration path and
  // loadDefaults() already call this helper.
  s.targetReachedHystC = 0.5f;   // was hardcoded in mashProfileTick()
  s.alertSoundEnabled = true;
}

void applyMixerDefaults(SettingsEE &s) {
  s.mixerRestSec = 15;
  s.mixerOnSec   = 5;
  s.coolDownSec  = 180;   // 3 minutes default
}
}

bool Storage::begin() {
  EEPROM.begin(sizeof(EepromDataEE));
  EEPROM.get(0, _data);

  uint32_t storedCrc = _data.crc;
  uint32_t calc = crc32((uint8_t*)&_data, sizeof(EepromDataEE) - sizeof(_data.crc));

  if (_data.magic != STORAGE_MAGIC || _data.version != STORAGE_VERSION || storedCrc != calc) {
    LegacyV14EepromDataEE legacyV14;
    EEPROM.get(0, legacyV14);
    uint32_t legacyV14Calc = crc32(
        (uint8_t*)&legacyV14,
        sizeof(LegacyV14EepromDataEE) - sizeof(legacyV14.crc));

    if (legacyV14.magic == STORAGE_MAGIC &&
        legacyV14.version == STORAGE_PREVIOUS_VERSION &&
        legacyV14.crc == legacyV14Calc) {
      memset(&_data, 0, sizeof(_data));
      _data.magic = STORAGE_MAGIC;
      _data.version = STORAGE_VERSION;

      applyHeaterDefaults(_data.settings);
      applyMixerDefaults(_data.settings);

      _data.settings.wifiMode = legacyV14.settings.wifiMode;
      memcpy(_data.settings.wifiApSSID, legacyV14.settings.wifiApSSID, STORAGE_SSID_LEN);
      memcpy(_data.settings.wifiApPass, legacyV14.settings.wifiApPass, STORAGE_PASS_LEN);
      memcpy(_data.settings.wifiStaSSID, legacyV14.settings.wifiStaSSID, STORAGE_SSID_LEN);
      memcpy(_data.settings.wifiStaPass, legacyV14.settings.wifiStaPass, STORAGE_PASS_LEN);

      _data.settings.heaterPowerW = legacyV14.settings.heaterPowerW;
      _data.settings.heaterPowerEffW = legacyV14.settings.heaterPowerEffW;
      _data.settings.heaterTauSec = legacyV14.settings.heaterTauSec;
      _data.settings.heaterStoreGain = legacyV14.settings.heaterStoreGain;
      _data.settings.heaterLossWPerC = legacyV14.settings.heaterLossWPerC;
      _data.settings.heaterAmbientC = legacyV14.settings.heaterAmbientC;
      _data.settings.heaterDeadband = legacyV14.settings.heaterDeadband;
      _data.settings.heaterMinSwitchSec = legacyV14.settings.heaterMinSwitchSec;
      _data.settings.mixerRestSec = legacyV14.settings.mixerRestSec;
      _data.settings.mixerOnSec = legacyV14.settings.mixerOnSec;
      _data.settings.coolDownSec = legacyV14.settings.coolDownSec;
      _data.settings.targetReachedHystC = legacyV14.settings.targetReachedHystC;
      _data.settings.alertSoundEnabled = legacyV14.settings.alertSoundEnabled;

      _data.profileCount = min(legacyV14.profileCount,
                               (uint8_t)STORAGE_MAX_PROFILES);
      memcpy(_data.profiles, legacyV14.profiles,
             sizeof(ProfileEE) * _data.profileCount);
      Serial.println("Storage: migrated version 14 settings");
      save();
      return true;
    }

    Serial.println("Storage: no valid data found (first boot, corrupted or too old) - writing defaults");
    loadDefaults();
    save();
    return false;
  }

  Serial.print("Storage: loaded OK, ");
  Serial.print(_data.profileCount);
  Serial.println(" profile(s)");
  return true;
}

void Storage::loadDefaults() {
  memset(&_data, 0, sizeof(_data));
  _data.magic   = STORAGE_MAGIC;
  _data.version = STORAGE_VERSION;

  _data.settings.wifiMode = WIFI_MODE_AP;
  strncpy(_data.settings.wifiApSSID, "MashController", STORAGE_SSID_LEN - 1);
  _data.settings.wifiApSSID[STORAGE_SSID_LEN - 1] = '\0';
  _data.settings.wifiApPass[0] = '\0';
  _data.settings.wifiStaSSID[0] = '\0';
  _data.settings.wifiStaPass[0] = '\0';

  applyHeaterDefaults(_data.settings);
  applyMixerDefaults(_data.settings);

  _data.profileCount = 0;

  // Seed one basic profile so the frontend never has to deal with an empty
  // list on first boot / after a corrupted EEPROM.
  ProfileEE defaultProfile;
  memset(&defaultProfile, 0, sizeof(defaultProfile));
  strncpy(defaultProfile.name, "Default", STORAGE_NAME_LEN - 1);
  defaultProfile.stepCount = 1;
  defaultProfile.waterMassKg = 20.0f;
  defaultProfile.grainMassKg = 5.0f;
  defaultProfile.steps[0].temp    = 67.0f;
  defaultProfile.steps[0].timeMin = 60;

  _data.profiles[_data.profileCount] = defaultProfile;
  _data.profileCount++;
}

bool Storage::save() {
  _data.crc = crc32((uint8_t*)&_data, sizeof(EepromDataEE) - sizeof(_data.crc));
  EEPROM.put(0, _data);
  bool ok = EEPROM.commit();
  if (!ok) Serial.println("Storage: EEPROM commit failed");
  return ok;
}

SettingsEE& Storage::settings() {
  return _data.settings;
}

uint8_t Storage::profileCount() const {
  return _data.profileCount;
}

bool Storage::getProfile(uint8_t index, ProfileEE &out) const {
  if (index >= _data.profileCount) return false;
  out = _data.profiles[index];
  return true;
}

bool Storage::setProfile(uint8_t index, const ProfileEE &p) {
  if (index < _data.profileCount) {
    _data.profiles[index] = p;
    return true;
  }
  if (index == _data.profileCount && _data.profileCount < STORAGE_MAX_PROFILES) {
    _data.profiles[_data.profileCount] = p;
    _data.profileCount++;
    return true;
  }
  return false; // out of range, or storage full
}

bool Storage::deleteProfile(uint8_t index) {
  if (index >= _data.profileCount) return false;
  for (uint8_t i = index; i < _data.profileCount - 1; i++) {
    _data.profiles[i] = _data.profiles[i + 1];
  }
  _data.profileCount--;
  return true;
}

void Storage::clearProfiles() {
  _data.profileCount = 0;
}

// Standard CRC32 (poly 0xEDB88320), bitwise - no table needed, only runs
// a handful of times (on boot and on save), so speed doesn't matter here.
uint32_t Storage::crc32(const uint8_t *data, size_t length) const {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
    }
  }
  return ~crc;
}