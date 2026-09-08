#include <Arduino.h>
#include <Ticker.h>

#include "DoubleReset.h"
#include "Storage.h"

namespace {
constexpr uint32_t DOUBLE_RESET_MAGIC = 0x4D415348UL;
constexpr uint32_t DOUBLE_RESET_TIMEOUT_SEC = 5;
// RTC user memory is separate from EEPROM. Keep this aligned and above the
// low RTC area reserved by the ESP8266 SDK.
constexpr uint32_t DOUBLE_RESET_RTC_OFFSET = 256;

Ticker doubleResetTicker;

void clearDoubleResetMarker() {
  uint32_t marker = 0;
  ESP.rtcUserMemoryWrite(DOUBLE_RESET_RTC_OFFSET, &marker, sizeof(marker));
}
}

bool detectDoubleReset() {
  uint32_t marker = 0;
  const bool readOk = ESP.rtcUserMemoryRead(
      DOUBLE_RESET_RTC_OFFSET, &marker, sizeof(marker));

  if (readOk && marker == DOUBLE_RESET_MAGIC) {
    clearDoubleResetMarker();
    return true;
  }

  marker = DOUBLE_RESET_MAGIC;
  ESP.rtcUserMemoryWrite(DOUBLE_RESET_RTC_OFFSET, &marker, sizeof(marker));
  doubleResetTicker.attach(DOUBLE_RESET_TIMEOUT_SEC, clearDoubleResetMarker);
  return false;
}

void resetApCredentialsToDefaults() {
  SettingsEE &settings = storage.settings();
  memset(settings.wifiSSID, 0, sizeof(settings.wifiSSID));
  memset(settings.wifiPass, 0, sizeof(settings.wifiPass));
  strncpy(settings.wifiSSID, "MashController", sizeof(settings.wifiSSID) - 1);
}
