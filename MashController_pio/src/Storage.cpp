#include "Storage.h"

Storage storage;

namespace {
struct LegacySettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  float    heaterHysteresis;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
};

struct LegacyEepromDataEE {
  uint32_t       magic;
  uint16_t       version;
  LegacySettingsEE settings;
  uint8_t        profileCount;
  ProfileEE      profiles[STORAGE_MAX_PROFILES];
  uint32_t       crc;
};

struct LegacySplitSettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  float    heaterOffHysteresis;
  float    heaterOnHysteresis;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
};

struct LegacySplitEepromDataEE {
  uint32_t             magic;
  uint16_t             version;
  LegacySplitSettingsEE settings;
  uint8_t              profileCount;
  ProfileEE            profiles[STORAGE_MAX_PROFILES];
  uint32_t             crc;
};

struct LegacyV9SettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  uint16_t heaterOffPredictionSec;
  uint16_t heaterOnPredictionSec;
  float    heaterDeadband;
  uint16_t heaterMinSwitchSec;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
};

struct LegacyV9EepromDataEE {
  uint32_t             magic;
  uint16_t             version;
  LegacyV9SettingsEE   settings;
  uint8_t              profileCount;
  ProfileEE            profiles[STORAGE_MAX_PROFILES];
  uint32_t             crc;
};

struct LegacyV10ProfileEE {
  char    name[STORAGE_NAME_LEN];
  uint8_t stepCount;
  StepEE  steps[STORAGE_MAX_STEPS];
};

struct LegacyV10SettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  float    waterMassKg;
  float    grainMassKg;
  float    heaterTransferCoeff;
  float    heaterThermalMass;
  float    heaterDeadband;
  uint16_t heaterMinSwitchSec;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
};

struct LegacyV10EepromDataEE {
  uint32_t             magic;
  uint16_t             version;
  LegacyV10SettingsEE  settings;
  uint8_t              profileCount;
  LegacyV10ProfileEE   profiles[STORAGE_MAX_PROFILES];
  uint32_t             crc;
};

struct LegacyMarginSettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  float    heaterOffMargin;
  float    heaterOnMargin;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
};

struct LegacyMarginEepromDataEE {
  uint32_t             magic;
  uint16_t             version;
  LegacyMarginSettingsEE settings;
  uint8_t              profileCount;
  ProfileEE            profiles[STORAGE_MAX_PROFILES];
  uint32_t             crc;
};

struct LegacyPredictiveSettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  float    heaterPredictionSec;
  float    heaterDeadband;
  uint16_t heaterMinSwitchSec;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
};

struct LegacyPredictiveEepromDataEE {
  uint32_t                 magic;
  uint16_t                 version;
  LegacyPredictiveSettingsEE settings;
  uint8_t                  profileCount;
  ProfileEE                profiles[STORAGE_MAX_PROFILES];
  uint32_t                 crc;
};

struct LegacyAsymmetricSettingsEE {
  char     wifiSSID[STORAGE_SSID_LEN];
  char     wifiPass[STORAGE_PASS_LEN];
  float    heaterOffPredictionSec;
  float    heaterOnPredictionSec;
  float    heaterDeadband;
  uint16_t heaterMinSwitchSec;
  uint8_t  mixerRestSec;
  uint8_t  mixerOnSec;
  uint16_t coolDownSec;
};

struct LegacyAsymmetricEepromDataEE {
  uint32_t                 magic;
  uint16_t                 version;
  LegacyAsymmetricSettingsEE settings;
  uint8_t                  profileCount;
  ProfileEE                profiles[STORAGE_MAX_PROFILES];
  uint32_t                 crc;
};
}

bool Storage::begin() {
  EEPROM.begin(sizeof(EepromDataEE));
  EEPROM.get(0, _data);

  uint32_t storedCrc = _data.crc;
  uint32_t calc = crc32((uint8_t*)&_data, sizeof(EepromDataEE) - sizeof(_data.crc));

  if (_data.magic != STORAGE_MAGIC || _data.version != STORAGE_VERSION || storedCrc != calc) {
    {
    LegacyEepromDataEE legacy;
    EEPROM.get(0, legacy);
    uint32_t legacyCalc = crc32((uint8_t*)&legacy,
                                sizeof(LegacyEepromDataEE) - sizeof(legacy.crc));

    if (legacy.magic == STORAGE_MAGIC && legacy.version == 4 &&
        legacy.crc == legacyCalc) {
      memset(&_data, 0, sizeof(_data));
      _data.magic = STORAGE_MAGIC;
      _data.version = STORAGE_VERSION;
      strncpy(_data.settings.wifiSSID, legacy.settings.wifiSSID,
              STORAGE_SSID_LEN - 1);
      strncpy(_data.settings.wifiPass, legacy.settings.wifiPass,
              STORAGE_PASS_LEN - 1);
      _data.settings.heaterTransferCoeff = 20.0f;
      _data.settings.heaterThermalMass = 5000.0f;
      _data.settings.heaterPowerW = 2000.0f;
      _data.settings.heaterDeadband = 0.1f;
      _data.settings.heaterMinSwitchSec = 10;
      _data.settings.mixerRestSec = legacy.settings.mixerRestSec;
      _data.settings.mixerOnSec = legacy.settings.mixerOnSec;
      _data.settings.coolDownSec = legacy.settings.coolDownSec;
      _data.profileCount = min(legacy.profileCount,
                               (uint8_t)STORAGE_MAX_PROFILES);
      memcpy(_data.profiles, legacy.profiles,
             sizeof(ProfileEE) * _data.profileCount);
      Serial.println("Storage: migrated version 4 settings");
      save();
      return true;
    }
    }

    {
    LegacySplitEepromDataEE legacySplit;
    EEPROM.get(0, legacySplit);
    uint32_t legacySplitCalc = crc32(
        (uint8_t*)&legacySplit,
        sizeof(LegacySplitEepromDataEE) - sizeof(legacySplit.crc));

    if (legacySplit.magic == STORAGE_MAGIC && legacySplit.version == 5 &&
        legacySplit.crc == legacySplitCalc) {
      memset(&_data, 0, sizeof(_data));
      _data.magic = STORAGE_MAGIC;
      _data.version = STORAGE_VERSION;
      strncpy(_data.settings.wifiSSID, legacySplit.settings.wifiSSID,
              STORAGE_SSID_LEN - 1);
      strncpy(_data.settings.wifiPass, legacySplit.settings.wifiPass,
              STORAGE_PASS_LEN - 1);
      _data.settings.heaterTransferCoeff = 20.0f;
      _data.settings.heaterThermalMass = 5000.0f;
      _data.settings.heaterPowerW = 2000.0f;
      _data.settings.heaterDeadband = 0.1f;
      _data.settings.heaterMinSwitchSec = 10;
      _data.settings.mixerRestSec = legacySplit.settings.mixerRestSec;
      _data.settings.mixerOnSec = legacySplit.settings.mixerOnSec;
      _data.settings.coolDownSec = legacySplit.settings.coolDownSec;
      _data.profileCount = min(legacySplit.profileCount,
                               (uint8_t)STORAGE_MAX_PROFILES);
      memcpy(_data.profiles, legacySplit.profiles,
             sizeof(ProfileEE) * _data.profileCount);
      Serial.println("Storage: migrated version 5 heater margins");
      save();
      return true;
    }
    }

    {
    LegacyMarginEepromDataEE legacyMargin;
    EEPROM.get(0, legacyMargin);
    uint32_t legacyMarginCalc = crc32(
        (uint8_t*)&legacyMargin,
        sizeof(LegacyMarginEepromDataEE) - sizeof(legacyMargin.crc));

    if (legacyMargin.magic == STORAGE_MAGIC && legacyMargin.version == 6 &&
        legacyMargin.crc == legacyMarginCalc) {
      memset(&_data, 0, sizeof(_data));
      _data.magic = STORAGE_MAGIC;
      _data.version = STORAGE_VERSION;
      strncpy(_data.settings.wifiSSID, legacyMargin.settings.wifiSSID,
              STORAGE_SSID_LEN - 1);
      strncpy(_data.settings.wifiPass, legacyMargin.settings.wifiPass,
              STORAGE_PASS_LEN - 1);
      _data.settings.heaterTransferCoeff = 20.0f;
      _data.settings.heaterThermalMass = 5000.0f;
      _data.settings.heaterPowerW = 2000.0f;
      _data.settings.heaterDeadband = 0.1f;
      _data.settings.heaterMinSwitchSec = 10;
      _data.settings.mixerRestSec = legacyMargin.settings.mixerRestSec;
      _data.settings.mixerOnSec = legacyMargin.settings.mixerOnSec;
      _data.settings.coolDownSec = legacyMargin.settings.coolDownSec;
      _data.profileCount = min(legacyMargin.profileCount,
                               (uint8_t)STORAGE_MAX_PROFILES);
      memcpy(_data.profiles, legacyMargin.profiles,
             sizeof(ProfileEE) * _data.profileCount);
      Serial.println("Storage: migrated version 6 heater margins");
      save();
      return true;
    }
    }

    {
      LegacyPredictiveEepromDataEE legacyPredictive;
      EEPROM.get(0, legacyPredictive);
      uint32_t legacyPredictiveCalc = crc32(
        (uint8_t*)&legacyPredictive,
        sizeof(LegacyPredictiveEepromDataEE) - sizeof(legacyPredictive.crc));

      if (legacyPredictive.magic == STORAGE_MAGIC &&
        legacyPredictive.version == 7 &&
        legacyPredictive.crc == legacyPredictiveCalc) {
        memset(&_data, 0, sizeof(_data));
        _data.magic = STORAGE_MAGIC;
        _data.version = STORAGE_VERSION;
        strncpy(_data.settings.wifiSSID, legacyPredictive.settings.wifiSSID,
            STORAGE_SSID_LEN - 1);
        strncpy(_data.settings.wifiPass, legacyPredictive.settings.wifiPass,
            STORAGE_PASS_LEN - 1);
          _data.settings.heaterTransferCoeff = 20.0f;
          _data.settings.heaterThermalMass = 5000.0f;
          _data.settings.heaterPowerW = 2000.0f;
        _data.settings.heaterDeadband = legacyPredictive.settings.heaterDeadband;
        _data.settings.heaterMinSwitchSec =
          legacyPredictive.settings.heaterMinSwitchSec;
        _data.settings.mixerRestSec = legacyPredictive.settings.mixerRestSec;
        _data.settings.mixerOnSec = legacyPredictive.settings.mixerOnSec;
        _data.settings.coolDownSec = legacyPredictive.settings.coolDownSec;
        _data.profileCount = min(legacyPredictive.profileCount,
                     (uint8_t)STORAGE_MAX_PROFILES);
        memcpy(_data.profiles, legacyPredictive.profiles,
           sizeof(ProfileEE) * _data.profileCount);
        Serial.println("Storage: migrated version 7 predictive settings");
        save();
        return true;
      }
    }

    {
      LegacyAsymmetricEepromDataEE legacyAsymmetric;
      EEPROM.get(0, legacyAsymmetric);
      uint32_t legacyAsymmetricCalc = crc32(
          (uint8_t*)&legacyAsymmetric,
          sizeof(LegacyAsymmetricEepromDataEE) - sizeof(legacyAsymmetric.crc));

      if (legacyAsymmetric.magic == STORAGE_MAGIC &&
          legacyAsymmetric.version == 8 &&
          legacyAsymmetric.crc == legacyAsymmetricCalc) {
        memset(&_data, 0, sizeof(_data));
        _data.magic = STORAGE_MAGIC;
        _data.version = STORAGE_VERSION;
        strncpy(_data.settings.wifiSSID, legacyAsymmetric.settings.wifiSSID,
                STORAGE_SSID_LEN - 1);
        strncpy(_data.settings.wifiPass, legacyAsymmetric.settings.wifiPass,
                STORAGE_PASS_LEN - 1);
        _data.settings.heaterTransferCoeff = 20.0f;
        _data.settings.heaterThermalMass = 5000.0f;
        _data.settings.heaterPowerW = 2000.0f;
        _data.settings.heaterDeadband = legacyAsymmetric.settings.heaterDeadband;
        _data.settings.heaterMinSwitchSec =
            legacyAsymmetric.settings.heaterMinSwitchSec;
        _data.settings.mixerRestSec = legacyAsymmetric.settings.mixerRestSec;
        _data.settings.mixerOnSec = legacyAsymmetric.settings.mixerOnSec;
        _data.settings.coolDownSec = legacyAsymmetric.settings.coolDownSec;
        _data.profileCount = min(legacyAsymmetric.profileCount,
                                 (uint8_t)STORAGE_MAX_PROFILES);
        memcpy(_data.profiles, legacyAsymmetric.profiles,
               sizeof(ProfileEE) * _data.profileCount);
        Serial.println("Storage: migrated version 8 integer horizons");
        save();
        return true;
      }
    }

    {
    LegacyV9EepromDataEE legacyV9;
    EEPROM.get(0, legacyV9);
    uint32_t legacyV9Calc = crc32(
        (uint8_t*)&legacyV9,
        sizeof(LegacyV9EepromDataEE) - sizeof(legacyV9.crc));

    if (legacyV9.magic == STORAGE_MAGIC && legacyV9.version == 9 &&
        legacyV9.crc == legacyV9Calc) {
      memset(&_data, 0, sizeof(_data));
      _data.magic = STORAGE_MAGIC;
      _data.version = STORAGE_VERSION;
      strncpy(_data.settings.wifiSSID, legacyV9.settings.wifiSSID,
              STORAGE_SSID_LEN - 1);
      strncpy(_data.settings.wifiPass, legacyV9.settings.wifiPass,
              STORAGE_PASS_LEN - 1);
      _data.settings.heaterTransferCoeff = 20.0f;
      _data.settings.heaterThermalMass = 5000.0f;
      _data.settings.heaterPowerW = 2000.0f;
      _data.settings.heaterDeadband = legacyV9.settings.heaterDeadband;
      _data.settings.heaterMinSwitchSec = legacyV9.settings.heaterMinSwitchSec;
      _data.settings.mixerRestSec = legacyV9.settings.mixerRestSec;
      _data.settings.mixerOnSec = legacyV9.settings.mixerOnSec;
      _data.settings.coolDownSec = legacyV9.settings.coolDownSec;
      _data.profileCount = min(legacyV9.profileCount,
                               (uint8_t)STORAGE_MAX_PROFILES);
      memcpy(_data.profiles, legacyV9.profiles,
             sizeof(ProfileEE) * _data.profileCount);
      Serial.println("Storage: migrated version 9 predictive settings");
      save();
      return true;
    }
    }

    {
    LegacyV10EepromDataEE legacyV10;
    EEPROM.get(0, legacyV10);
    uint32_t legacyV10Calc = crc32(
        (uint8_t*)&legacyV10,
        sizeof(LegacyV10EepromDataEE) - sizeof(legacyV10.crc));

    if (legacyV10.magic == STORAGE_MAGIC && legacyV10.version == 10 &&
        legacyV10.crc == legacyV10Calc) {
      memset(&_data, 0, sizeof(_data));
      _data.magic = STORAGE_MAGIC;
      _data.version = STORAGE_VERSION;
            strncpy(_data.settings.wifiSSID, legacyV10.settings.wifiSSID,
              STORAGE_SSID_LEN - 1);
            strncpy(_data.settings.wifiPass, legacyV10.settings.wifiPass,
              STORAGE_PASS_LEN - 1);
            _data.settings.heaterTransferCoeff =
          legacyV10.settings.heaterTransferCoeff;
            _data.settings.heaterThermalMass = legacyV10.settings.heaterThermalMass;
      _data.settings.heaterPowerW = 2000.0f;
            _data.settings.heaterDeadband = legacyV10.settings.heaterDeadband;
            _data.settings.heaterMinSwitchSec = legacyV10.settings.heaterMinSwitchSec;
            _data.settings.mixerRestSec = legacyV10.settings.mixerRestSec;
            _data.settings.mixerOnSec = legacyV10.settings.mixerOnSec;
            _data.settings.coolDownSec = legacyV10.settings.coolDownSec;
      _data.profileCount = min(legacyV10.profileCount,
                               (uint8_t)STORAGE_MAX_PROFILES);
      for (uint8_t i = 0; i < _data.profileCount; i++) {
        strncpy(_data.profiles[i].name, legacyV10.profiles[i].name,
                STORAGE_NAME_LEN - 1);
        _data.profiles[i].stepCount = legacyV10.profiles[i].stepCount;
        _data.profiles[i].waterMassKg = legacyV10.settings.waterMassKg;
        _data.profiles[i].grainMassKg = legacyV10.settings.grainMassKg;
        memcpy(_data.profiles[i].steps, legacyV10.profiles[i].steps,
               sizeof(_data.profiles[i].steps));
      }
      Serial.println("Storage: migrated version 10 profile masses");
      save();
      return true;
    }
    }

    Serial.println("Storage: no valid data found (first boot or corrupted) - writing defaults");
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

  strncpy(_data.settings.wifiSSID, "MashController", STORAGE_SSID_LEN - 1);
  _data.settings.heaterTransferCoeff = 20.0f;
  _data.settings.heaterThermalMass = 5000.0f;
  _data.settings.heaterPowerW = 2000.0f;
  _data.settings.heaterDeadband = 0.1f;
  _data.settings.heaterMinSwitchSec = 10;
  _data.settings.mixerRestSec   = 15;
  _data.settings.mixerOnSec     = 5;
  _data.settings.coolDownSec    = 180;   // 3 minutes default

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
