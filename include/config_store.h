#pragma once

#include <Arduino.h>
#include "thermal_controller.h"
#include "tec_direction.h"

// Call from one owner/task. A failed load leaves safe defaults/empty credentials.
class ConfigStore {
 public:
  bool loadController(ControllerSettings& settings);
  bool saveController(const ControllerSettings& settings);
  bool loadWifi(String& ssid, String& password);
  bool saveWifi(const String& ssid, const String& password);
  bool clearWifi();
  bool loadDirection(TecDirection& direction);
  bool saveDirection(TecDirection direction);
  const char* lastError() const { return error_; }

 private:
  bool fail(const char* message) { error_ = message; return false; }
  const char* error_ = "";
};
