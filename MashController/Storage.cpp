#include "Storage.h"

Storage storage;

bool Storage::begin() {
  EEPROM.begin(sizeof(EepromDataEE));
  EEPROM.get(0, _data);

  uint32_t storedCrc = _data.crc;
  uint32_t calc = crc32((uint8_t*)&_data, sizeof(EepromDataEE) - sizeof(_data.crc));

  if (_data.magic != STORAGE_MAGIC || _data.version != STORAGE_VERSION || storedCrc != calc) {
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
  strncpy(_data.settings.wifiPass, "12345678", STORAGE_PASS_LEN - 1);
  _data.settings.heaterHysteresis = 0.5f;
  _data.settings.mixerDurationSec = 60;
  _data.settings.mixerPercent     = 30;

  _data.profileCount = 0;

  // Seed one basic profile so the frontend never has to deal with an empty
  // list on first boot / after a corrupted EEPROM.
  ProfileEE defaultProfile;
  memset(&defaultProfile, 0, sizeof(defaultProfile));
  strncpy(defaultProfile.name, "Default", STORAGE_NAME_LEN - 1);
  defaultProfile.stepCount = 1;
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
