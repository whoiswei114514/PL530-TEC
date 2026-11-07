#pragma once
#include <Arduino.h>
#include "esp_err.h"
using esp_timer_handle_t = void*;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
  void (*callback)(void*) = nullptr;
  const char* name = nullptr;
  int dispatch_method = 0;
};
inline int mockTimerCreateResult = ESP_OK, mockTimerStartResult = ESP_OK;
inline int mockTimerStartCalls = 0;
inline uint32_t mockTimerPeriod = 0;
inline esp_timer_create_args_t mockTimerArgs;
inline int esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* handle) {
  mockTimerArgs = *args;
  if (mockTimerCreateResult == ESP_OK) *handle = reinterpret_cast<void*>(1);
  return mockTimerCreateResult;
}
inline int esp_timer_start_periodic(esp_timer_handle_t, uint32_t period) {
  ++mockTimerStartCalls; mockTimerPeriod = period; return mockTimerStartResult;
}
inline uint64_t esp_timer_get_time() { return uint64_t{mockNow} * 1000; }
