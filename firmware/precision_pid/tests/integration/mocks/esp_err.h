#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_WIFI_BASE = 0x3000;
constexpr esp_err_t ESP_ERR_WIFI_CONN = ESP_ERR_WIFI_BASE + 7;
constexpr esp_err_t ESP_ERR_WIFI_NOT_CONNECT = ESP_ERR_WIFI_BASE + 15;
