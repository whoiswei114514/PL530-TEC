#pragma once
#include "thermal_controller.h"
class ConfigStore {
 public:
  bool controllerLoadOk = true, controllerSaveOk = true;
  bool wifiLoadOk = true, wifiSaveOk = true, clearOk = true;
  int controllerSaves = 0, wifiSaves = 0, clears = 0;
  ControllerSettings storedSettings;
  String storedSsid = "previous-network", storedPassword = "previous-password";
  std::function<void()> beforeControllerSave;
  bool directionLoadOk = true, directionSaveOk = true;
  bool directionCommitBeforeFailure = false;
  TecDirection storedDirection = TecDirection::Unknown;
  int directionLoads = 0;
  std::vector<TecDirection> directionSaves;
  std::function<void()> beforeDirectionSave;
  bool loadDirection(TecDirection& direction) {
    ++directionLoads;
    direction = directionLoadOk ? storedDirection : TecDirection::Unknown;
    return directionLoadOk;
  }
  bool saveDirection(TecDirection direction) {
    directionSaves.push_back(direction);
    mockEvents.push_back(std::string("save-direction:") + tecDirectionKey(direction));
    if (beforeDirectionSave) beforeDirectionSave();
    if (directionSaveOk || directionCommitBeforeFailure) storedDirection = direction;
    return directionSaveOk;
  }
  bool loadController(ControllerSettings& settings) {
    settings = storedSettings; return controllerLoadOk;
  }
  bool saveController(const ControllerSettings& settings) {
    ++controllerSaves; mockEvents.push_back("save");
    if (beforeControllerSave) beforeControllerSave();
    if (controllerSaveOk) storedSettings = settings;
    return controllerSaveOk;
  }
  bool loadWifi(String& ssid, String& password) {
    if (wifiLoadOk) { ssid = storedSsid; password = storedPassword; }
    return wifiLoadOk;
  }
  bool saveWifi(const String& ssid, const String& password) {
    ++wifiSaves;
    if (wifiSaveOk) { storedSsid = ssid; storedPassword = password; }
    return wifiSaveOk;
  }
  bool clearWifi() {
    ++clears;
    if (clearOk) { storedSsid.clear(); storedPassword.clear(); }
    return clearOk;
  }
};
