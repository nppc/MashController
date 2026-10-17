#include <Arduino.h>
#include <Ticker.h>

#include "DoubleReset.h"
#include "Storage.h"

namespace {
constexpr uint32_t DOUBLE_RESET_FLAG_SET = 0xD0D01234UL;
constexpr uint32_t DOUBLE_RESET_FLAG_CLEAR = 0xD0D04321UL;
constexpr uint32_t DOUBLE_RESET_TIMEOUT_SEC = 10;
// In ESP8266 Arduino core, offset in rtcUserMemoryRead/Write is in 4-byte blocks (0..127),
// not byte addresses. The core adds 64 internally (system_rtc_mem_read(64 + offset)).
// Offset 0 maps to block 64 (the first user RTC block), exactly matching iSpindel (DRD_ADDRESS 0).
constexpr uint32_t DOUBLE_RESET_RTC_OFFSET = 0;

Ticker doubleResetTicker;

void clearDoubleResetMarker() {
  uint32_t marker = DOUBLE_RESET_FLAG_CLEAR;
  ESP.rtcUserMemoryWrite(DOUBLE_RESET_RTC_OFFSET, &marker, sizeof(marker));
  doubleResetTicker.detach();
}
}

bool detectDoubleReset() {
  uint32_t marker = DOUBLE_RESET_FLAG_CLEAR;
  const bool readOk = ESP.rtcUserMemoryRead(
      DOUBLE_RESET_RTC_OFFSET, &marker, sizeof(marker));

  if (readOk && marker == DOUBLE_RESET_FLAG_SET) {
    clearDoubleResetMarker();
    return true;
  }

  marker = DOUBLE_RESET_FLAG_SET;
  ESP.rtcUserMemoryWrite(DOUBLE_RESET_RTC_OFFSET, &marker, sizeof(marker));
  doubleResetTicker.once(DOUBLE_RESET_TIMEOUT_SEC, clearDoubleResetMarker);
  return false;
}

void resetApCredentialsToDefaults() {
  SettingsEE &settings = storage.settings();
  settings.wifiMode = WIFI_MODE_AP;
  memset(settings.wifiApSSID, 0, sizeof(settings.wifiApSSID));
  memset(settings.wifiApPass, 0, sizeof(settings.wifiApPass));
  strncpy(settings.wifiApSSID, "MashController", sizeof(settings.wifiApSSID) - 1);
}
