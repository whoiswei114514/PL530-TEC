#pragma once
#include <Arduino.h>
#include "esp_err.h"

// Only the queried fields are modeled; ssid has the pinned SDK's 33-byte extent.
struct wifi_ap_record_t {
  uint8_t bssid[6]{};
  uint8_t ssid[33]{};
};
namespace MockSta {
inline bool associated = false;
inline wifi_ap_record_t association{};
inline esp_err_t queryError = ESP_OK;
inline int infoCalls = 0;
inline void reset() {
  associated = false;
  association = {};
  queryError = ESP_OK;
  infoCalls = 0;
}
}
inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* info) {
  ++MockSta::infoCalls;
  if (MockSta::queryError != ESP_OK) return MockSta::queryError;
  if (!MockSta::associated) return ESP_ERR_WIFI_NOT_CONNECT;
  *info = MockSta::association;
  return ESP_OK;
}
inline int mockScanStops = 0;
inline void esp_wifi_scan_stop() { ++mockScanStops; }
