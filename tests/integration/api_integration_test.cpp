#include <iostream>
#include <stdexcept>
#include <utility>

// Compile the production handlers, setup, loop, and validation helper unchanged.
#include "../../src/main.cpp"

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
ControllerStatus& mutableStatus() {
  return const_cast<ControllerStatus&>(controller.status());
}
void reset() {
  MockString::reset();
  MockSta::reset();
  mockNow = 1000;
  server = WebServer(80);
  configStore = ConfigStore{};
  controller = ThermalController{};
  tmp117 = Tmp117Sensor{};
  WiFi = MockWifi{};
  MockController::reset();
  controller.setTecDirection(TecDirection::Normal);
  MockController::directionApplications.clear();
  mockEvents.clear();
  mockScanStops = 0;
  mockTimerCreateResult = mockTimerStartResult = ESP_OK;
  mockTimerStartCalls = 0;
  mockTimerPeriod = 0;
  mockTimerArgs = {};
  outputGuardTimer = nullptr;
  staSsid.clear(); staPassword.clear(); wifiError.clear(); configurationError.clear();
  staPhase = StaPhase::Idle;
  staDesired = staSaveOnSuccess = staCredentialsSaved = false;
  staRetryWaiting = false;
  staRetryStartedMs = staAttemptStartedMs = 0;
  staRetryDelayMs = 5000;
  wifiScanState = WifiScanState::Idle;
  wifiScanStartedMs = wifiScanLastRequestMs = 0;
  wifiScanHasRequested = false;
  wifiScanResultJson.clear();
  sensorInitialized = sensorRetryWaiting = i2cReady = false;
  lastSensorRetryMs = lastSensorReadMs = lastGoodSensorMs = 0;
  haveGoodSensorSample = false;
  directionSaved = false;
  calibrationResultHandled = true;
  directionError.clear();
  server.setHost("192.168.1.1");
  server.headers["X-PL530-Request"] = "1";
}
void settingsRequest(const String& target = "25", const String& high = "30",
                     const String& low = "20", const String& enabled = "true") {
  server.args = {{"target", target}, {"high", high}, {"low", low}, {"enabled", enabled}};
}
void wifiRequest(const String& ssid = "trial-network", const String& password = "trial-password") {
  server.args = {{"ssid", ssid}, {"password", password}};
}
void response(int expected, const char* label) {
  require(server.code == expected, "unexpected HTTP response code");
  require(server.contentType == "application/json", "response must be JSON");
  require(server.responseHeaders["Cache-Control"] == "no-store", "response must not cache");
  require(server.responseHeaders["X-Content-Type-Options"] == "nosniff", "missing nosniff");
  std::cout << "JSON\t" << label << '\t' << server.body.c_str() << '\n';
}
void snapshot(const char* label) {
  handleStatus();
  response(200, label);
}
void noMutations() {
  require(MockController::setCalls == 0, "rejected request changed controller");
  require(configStore.controllerSaves == 0 && configStore.wifiSaves == 0 &&
              configStore.clears == 0, "rejected request touched persistence");
  require(WiFi.beginCalls == 0 && WiFi.disconnectCalls == 0 && WiFi.scanCalls == 0,
          "rejected request changed WiFi");
  require(configStore.directionSaves.empty() && MockController::directionApplications.empty() &&
              MockController::startCalls == 0 && MockController::cancelCalls == 0,
          "rejected request changed direction/calibration");
}
void strictDecimals() {
  for (const auto& value : std::vector<String>{"NaN", "25junk", "NaN25junk", "inf",
         " 25", "25 ", "2.5e1", "0x19", "", std::string("25\0junk", 7)}) {
    reset(); settingsRequest(value); handleSettings(); response(400, "invalid-decimal");
    noMutations();
  }
}
void strictBand() {
  for (const auto& values : std::vector<std::vector<String>>{
       {"25", "25.1", "20"}, {"25", "30", "24.9"}, {"10.1", "30", "10"},
       {"25", "56", "20"}, {"25", "30", "9"}, {"25", "20", "30"}}) {
    reset(); settingsRequest(values[0], values[1], values[2]);
    handleSettings(); response(400, "invalid-band"); noMutations();
  }
}
void strictEnabledAndRequiredFields() {
  for (const auto& value : std::vector<String>{"yes", "TRUE", "2", " true",
                                              std::string("true\0junk", 9)}) {
    reset(); settingsRequest("25", "30", "20", value);
    handleSettings(); response(400, "invalid-enabled"); noMutations();
  }
  for (const char* key : {"target", "high", "low"}) {
    reset(); settingsRequest(); server.args.erase(key);
    handleSettings(); response(400, "missing-setting"); noMutations();
  }
}
void headerRequiredEverywhere() {
  for (auto handler : {handleSettings, handleStop, handleResetFault, handleWifiConnect,
                       handleWifiDisconnect, handleWifiScan, handleDirectionCalibration}) {
    for (const char* header : {"", "0", "true"}) {
      reset(); settingsRequest();
      if (*header) server.headers["X-PL530-Request"] = header;
      else server.headers.erase("X-PL530-Request");
      handler(); response(403, "request-header"); noMutations();
      require(MockController::resetCalls == 0 && mockScanStops == 0, "header bypass");
    }
  }
}
void settingsSaveFailure() {
  for (const char* enabled : {"true", "false"}) {
    reset();
    controller.setSettings(24, 31, 19, true);
    mutableStatus().driverAwake = true;
    mutableStatus().signedDutyPercent = 40;
    configStore.controllerSaveOk = false;
    settingsRequest("26", "32", "21", enabled);
    handleSettings(); response(503, "settings-save-failure");
    require(!controller.settings().enabled && !controller.status().driverAwake &&
                controller.status().signedDutyPercent == 0, "save failure did not request stop");
    require(controller.settings().targetC == 24 && configStore.controllerSaves == 1,
            "failed save applied new settings or repeated flash");
    require(!configurationError.isEmpty(), "save failure is not visible");
    snapshot("configuration-error");
  }
}
void enableLatched() {
  for (auto state : {ControllerState::ThermalFaultLatched, ControllerState::DriverFaultLatched,
                    ControllerState::SensorTimeoutLatched, ControllerState::SafetyFaultLatched}) {
    reset(); mutableStatus().state = state;
    settingsRequest(); handleSettings(); response(409, "enable-latched"); noMutations();
  }
}
void settingsCommitAndRace() {
  reset(); configurationError = "previous failure";
  settingsRequest("25", "25.2", "24.8"); handleSettings(); response(200, "settings-success");
  require(configStore.controllerSaves == 1 && controller.settings().enabled &&
              configurationError.isEmpty(), "valid boundary settings not committed");
  require(mockEvents == std::vector<std::string>{"save", "enable"}, "enable before durable save");
  reset(); controller.setSettings(25, 30, 20, true); mockEvents.clear();
  settingsRequest("26", "31", "21", "false"); handleSettings(); response(200, "settings-disabled");
  require(mockEvents == std::vector<std::string>{"disable", "save", "disable"},
          "disable must precede flash save");
  reset(); settingsRequest();
  configStore.beforeControllerSave = [] { mutableStatus().state = ControllerState::DriverFaultLatched; };
  handleSettings(); response(409, "settings-race");
  require(configStore.controllerSaves == 1 && !controller.settings().enabled,
          "fault during persistence was ignored");
}
void stopBypassesDirtySettings() {
  controller.setSettings(25, 30, 20, true);
  mutableStatus().driverAwake = true;
  settingsRequest("NaN25junk", "", "", "invalid");
  configStore.controllerSaveOk = false;
  handleStop(); response(200, "stop");
  require(!controller.settings().enabled && !controller.status().driverAwake,
          "stop did not disable");
  require(configStore.controllerSaves == 0, "stop must not write flash");
}
void resetContracts() {
  handleResetFault(); response(409, "reset-no-latch");
  require(MockController::resetCalls == 0, "reset invoked without latch");
  mutableStatus().state = ControllerState::SafetyFaultLatched;
  handleResetFault(); response(409, "reset-safety");
  require(controller.status().state == ControllerState::SafetyFaultLatched, "safety latch cleared");
  mutableStatus().state = ControllerState::DriverFaultLatched;
  handleResetFault(); response(200, "reset-driver");
  require(!controller.settings().enabled && configStore.controllerSaves == 0, "reset enabled or saved");
}
void statusFreshStaleAndWrap() {
  mutableStatus().sensorOk = true;
  mutableStatus().rawTemperatureC = 24.75f;
  mutableStatus().filteredTemperatureC = 24.5f;
  snapshot("no-sample");
  haveGoodSensorSample = true;
  lastGoodSensorMs = UINT32_MAX - 500u;
  mockNow = lastGoodSensorMs + SENSOR_STALE_TIMEOUT_MS - 1u;
  snapshot("fresh-wrap");
  ++mockNow; snapshot("stale-wrap");
  require(MockController::ageCalls == 3 && MockController::pollCalls == 3,
          "status must poll protection before snapshot");
  lastGoodSensorMs = mockNow;
  mutableStatus().rawTemperatureC = NAN;
  mutableStatus().filteredTemperatureC = INFINITY;
  snapshot("nonfinite-temperature");
  staSsid = std::string("quoted\"\\\n\r\t") + char(1);
  wifiError = "line\nerror";
  snapshot("escaped-status");
}
void wifiValidation() {
  const std::vector<std::pair<String, String>> invalid = {
      {"", "password"}, {std::string(33, 's'), "password"}, {"ssid", "short"},
      {"ssid", std::string(64, 'z')}, {"ssid", std::string(65, 'a')},
      {std::string("ss\0id", 5), "password"}, {"ssid", std::string("pass\0word", 9)}};
  for (const auto& pair : invalid) {
    reset(); wifiRequest(pair.first, pair.second); handleWifiConnect();
    response(400, "invalid-wifi"); noMutations();
  }
  reset(); handleWifiConnect(); response(400, "missing-ssid"); noMutations();
}
void wifiAcceptedBoundaries() {
  for (const auto& password : std::vector<String>{"", "12345678",
                                               std::string(63, 'p'), std::string(64, 'A')}) {
    reset(); wifiRequest(std::string(32, 's'), password);
    handleWifiConnect(); response(200, "valid-wifi");
    require(WiFi.beginCalls == 0 && staPhase == StaPhase::Disconnecting,
            "accepted request must wait for the old-association-down boundary");
    pollWifi(mockNow);
    require(WiFi.beginCalls == 1 && WiFi.begunPassword == password &&
                configStore.wifiSaves == 0, "valid WiFi trial was altered or saved early");
  }
}
void trialFailurePreservesCredentials() {
  wifiRequest(); handleWifiConnect(); response(200, "trial-start");
  pollWifi(mockNow);
  require(configStore.wifiSaves == 0, "trial persisted before connection");
  pollWifi(mockNow + WIFI_CONNECT_TIMEOUT_MS - 1);
  require(staConnectRequested(), "trial timeout fired early");
  mockNow += WIFI_CONNECT_TIMEOUT_MS;
  pollWifi(mockNow);
  require(!staDesired && !staConnectRequested() && !staSaveOnSuccess && staPassword.isEmpty(),
          "failed trial still retrying or retaining trial password");
  require(configStore.wifiSaves == 0 && configStore.storedSsid == "previous-network" &&
              configStore.storedPassword == "previous-password", "failed trial overwrote credentials");
  pollWifi(mockNow + 100000);
  require(WiFi.beginCalls == 1, "failed unsaved trial must not retry");
  snapshot("trial-timeout");
}
void trialSuccessSavesOnce() {
  wifiRequest(); handleWifiConnect();
  pollWifi(mockNow);
  WiFi.associate("trial-network");
  pollWifi(mockNow); pollWifi(mockNow + 1);
  require(configStore.wifiSaves == 1 && configStore.storedSsid == "trial-network" &&
              configStore.storedPassword == "trial-password", "success not persisted exactly once");
  require(staCredentialsSaved && !staConnectRequested() && !staRetryWaiting, "success flags incorrect");
  snapshot("wifi-saved");
}
void wifiStorageFailureVisible() {
  configStore.wifiSaveOk = false;
  wifiRequest(); handleWifiConnect(); pollWifi(mockNow); WiFi.associate("trial-network");
  pollWifi(mockNow); pollWifi(mockNow + 1);
  require(configStore.wifiSaves == 1 && !staCredentialsSaved && !wifiError.isEmpty(),
          "save failure hidden or repeatedly written");
  require(configStore.storedSsid == "previous-network", "failed save changed old credentials");
  snapshot("wifi-save-failure");
}
void disconnectFailureAndSuccess() {
  for (bool success : {false, true}) {
    reset(); startStaConnect("trial", "password", true);
    pollWifi(mockNow);
    wifiScanState = WifiScanState::Scanning;
    configStore.clearOk = success;
    handleWifiDisconnect(); response(success ? 200 : 503, success ? "disconnect" : "disconnect-failure");
    require(!staDesired && !staConnectRequested() && !staRetryWaiting && !staCredentialsSaved &&
                staSsid.isEmpty() && staPassword.isEmpty(), "disconnect left active connection intent");
    require(mockScanStops == 1 && wifiScanState == WifiScanState::Idle &&
                configStore.clears == 1, "disconnect did not cancel scan and clear persistence");
    require(success ? configStore.storedSsid.isEmpty() : configStore.storedSsid == "previous-network",
            "clear mock persistence contract broken");
    if (!success) snapshot("wifi-clear-failure");
    pollWifi(mockNow + 100000);
    require(WiFi.beginCalls == 1, "disconnect restarted connection");
  }
}
void retriesAndRollover() {
  for (uint32_t start : {1000u, UINT32_MAX - 10000u}) {
    reset(); mockNow = start;
    startStaConnect("saved", "password", false);
    pollWifi(mockNow);
    pollWifi(start + WIFI_CONNECT_TIMEOUT_MS - 1u);
    require(staConnectRequested(), "saved connection timeout fired early");
    mockNow = start + WIFI_CONNECT_TIMEOUT_MS;
    pollWifi(mockNow);
    require(staRetryWaiting && !staConnectRequested(), "saved connection did not wait for retry");
    pollWifi(mockNow + 4999u);
    require(WiFi.beginCalls == 1, "retry fired early");
    mockNow += 5000u; pollWifi(mockNow);
    require(WiFi.beginCalls == 1 && staPhase == StaPhase::Disconnecting,
            "automatic retry must also establish a new down boundary");
    pollWifi(mockNow);
    require(WiFi.beginCalls == 2 && staConnectRequested() && staRetryDelayMs == 10000u,
            "retry boundary or rollover incorrect");
    for (int i = 0; i < 6; ++i) {
      mockNow += WIFI_CONNECT_TIMEOUT_MS; pollWifi(mockNow);
      const auto delay = staRetryDelayMs;
      mockNow += delay; pollWifi(mockNow);
      pollWifi(mockNow);
      require(staRetryDelayMs <= 60000u, "retry delay exceeded cap");
    }
    require(staRetryDelayMs == 60000u && configStore.wifiSaves == 0, "retry cap/save incorrect");
    WiFi.associate("saved"); pollWifi(mockNow);
    require(staRetryDelayMs == 5000u && !staRetryWaiting && !staConnectRequested(),
            "reconnection did not reset backoff");
  }
}
void scanConnectExclusion() {
  startStaConnect("saved", "password", false);
  handleWifiScan(); response(409, "scan-during-connect");
  require(WiFi.scanCalls == 0, "scan started while connecting");
  reset(); handleWifiScan(); response(200, "scan-start");
  wifiRequest(); handleWifiConnect(); response(409, "connect-during-scan");
  require(WiFi.beginCalls == 0 && !staDesired, "connect started while scanning");
  staDesired = staRetryWaiting = true;
  staRetryStartedMs = mockNow - 10000u;
  pollWifi(mockNow);
  require(WiFi.beginCalls == 0, "automatic retry bypassed scan exclusion");
}
void scanStringDeduplication() {
  handleWifiScan();
  require(WiFi.lastScanAsync && WiFi.lastScanHidden && !WiFi.lastScanPassive &&
              WiFi.lastScanMaxMsPerChan == WIFI_SCAN_MAX_MS_PER_CHANNEL,
          "scan did not use the stable asynchronous active-scan parameters");
  WiFi.networks = {{"a\nb", -30}, {"a", -40}, {"b", -50}, {"a\nb", -60},
                   {"", -70}, {"quote\"\\", -80}};
  WiFi.scanResult = static_cast<int>(WiFi.networks.size());
  pollWifiScan(mockNow);
  handleWifiScan(); response(200, "scan-dedup");
  require(wifiScanState == WifiScanState::Idle && wifiScanResultJson.isEmpty(),
          "completed scan was not consumed");
}
void scanTimeoutThrottleAndWrap() {
  for (uint32_t start : {1000u, UINT32_MAX - 7000u}) {
    reset(); mockNow = start; handleWifiScan();
    pollWifiScan(start + WIFI_SCAN_TIMEOUT_MS - 1u);
    require(wifiScanState == WifiScanState::Scanning, "scan timed out early");
    mockNow = start + WIFI_SCAN_TIMEOUT_MS; pollWifiScan(mockNow);
    require(mockScanStops == 1, "scan timeout did not stop driver");
    handleWifiScan(); response(500, "scan-timeout");
    reset(); mockNow = start; WiFi.scanStart = -2;
    handleWifiScan(); response(500, "scan-start-failure");
    mockNow = start + 3999u; handleWifiScan(); response(429, "scan-throttled");
    mockNow = start + 4000u; WiFi.scanStart = WIFI_SCAN_RUNNING;
    handleWifiScan(); response(200, "scan-retry-boundary");
  }
}
void scanResultBound() {
  handleWifiScan();
  for (int i = 0; i < 30; ++i) WiFi.networks.push_back({String("network-") + String(i), -30 - i});
  WiFi.scanResult = 30; pollWifiScan(mockNow); handleWifiScan(); response(200, "scan-bound");
  reset(); handleWifiScan(); WiFi.scanResult = -2;
  pollWifiScan(mockNow); handleWifiScan(); response(500, "scan-driver-failure");
}
void routesAndNotFound() {
  setupRoutes();
  require(server.routes.size() == (PL530_ENABLE_UNSAFE_DEBUG ? 13u : 9u) && server.collectedHeaders ==
              std::vector<std::string>{"X-PL530-Request", "Host", "Origin"},
          "route/header registration changed");
  const std::vector<std::pair<std::string, HTTPMethod>> expected = {
      {"/", HTTP_GET}, {"/api/status", HTTP_GET}, {"/api/settings", HTTP_POST},
      {"/api/wifi/scan", HTTP_GET}, {"/api/wifi/connect", HTTP_POST},
      {"/api/wifi/disconnect", HTTP_POST}, {"/api/control/reset-fault", HTTP_POST},
      {"/api/control/stop", HTTP_POST},
      {"/api/control/detect-direction", HTTP_POST}};
  for (size_t i = 0; i < expected.size(); ++i) {
    require(server.routes[i].path == expected[i].first &&
                server.routes[i].method == expected[i].second, "route path/method mismatch");
  }
  server.requestUri = "/api/unknown"; server.notFound(); response(404, "api-not-found");
  server.requestUri = "/generate_204"; server.notFound();
  require(server.code == 302 && server.responseHeaders["Location"] == "http://192.168.1.1/",
          "captive portal redirect changed");
  server.resetRequest(); server.setHost("192.168.1.1");
  server.routes[1].handler(); response(200, "public-status");
  server.routes[7].handler(); response(403, "routed-stop-header");
  server.routes[8].handler(); response(403, "routed-direction-header");
}
void startupFailClosed() {
  configStore.storedSettings.enabled = true;
  configStore.controllerLoadOk = false;
  configStore.wifiLoadOk = false;
  mockTimerCreateResult = -1;
  setup();
  require(!controller.settings().enabled &&
              controller.status().state == ControllerState::SafetyFaultLatched,
          "startup enabled persisted output or ignored timer failure");
  require(!configurationError.isEmpty() && !wifiError.isEmpty() && WiFi.beginCalls == 0,
          "startup load failure was hidden or connected");
  require(configStore.controllerSaves == 0 && configStore.wifiSaves == 0, "boot wrote settings");
  snapshot("startup-failure");
}
void validationHelperBoundaries() {
  float parsed = 99;
  for (const char* value : {"+25", "25.", ".5", "-.5"}) {
    require(RequestValidation::decimal(value, std::strlen(value), parsed), "valid decimal rejected");
  }
  require(!RequestValidation::decimal(nullptr, 0, parsed), "null decimal accepted");
  require(!RequestValidation::decimal("9999999999999999999999999", 25, parsed), "long decimal accepted");
  bool enabled = false;
  for (const char* value : {"true", "on", "1"}) {
    require(RequestValidation::enabled(value, enabled) && enabled, "enabled alias rejected");
  }
  for (const char* value : {"false", "off", "0", ""}) {
    require(RequestValidation::enabled(value, enabled) && !enabled, "disabled alias rejected");
  }
  require(!RequestValidation::temperatures(NAN, 30, 20) &&
              !RequestValidation::temperatures(25, INFINITY, 20), "nonfinite band accepted");
}
void sensorAcquisitionTimestamp() {
  for (uint32_t start : {1000u, UINT32_MAX - 500u}) {
    for (uint32_t delay : {25u, 1500u}) {
      reset();
      mockNow = start;
      sensorInitialized = i2cReady = true;
      lastSensorReadMs = start - SENSOR_READ_INTERVAL_MS;
      tmp117.readResult = true;
      tmp117.readDelayMs = delay;
      pollSensor(mockNow);
      require(tmp117.readCalls == 1 && MockController::submitCalls == 1,
              "real polling must perform one acquisition and submission");
      require(MockController::lastValid && MockController::lastSampleStartedMs == start &&
                  lastGoodSensorMs == start && lastSensorReadMs == start + delay,
              "completion time must not replace acquisition start time");
      snapshot(delay < SENSOR_STALE_TIMEOUT_MS ? "sensor-read-fresh" : "sensor-read-delayed");
    }
  }
}
void sensorRetryTiming() {
  for (uint32_t start : {1000u, UINT32_MAX - 500u}) {
    reset();
    mockNow = start;
    pollSensor(mockNow);
    require(tmp117.beginCalls == 2 && !MockController::lastValid && sensorRetryWaiting,
            "initial sensor failure must probe both bus orders");
    tmp117.beginResult = tmp117.readResult = true;
    tmp117.beginDelayMs = 22;
    mockNow = start + SENSOR_RETRY_INTERVAL_MS - 1;
    pollSensor(mockNow);
    require(tmp117.beginCalls == 2, "sensor retried before deadline");
    ++mockNow;
    pollSensor(mockNow);
    require(tmp117.beginCalls == 3 && tmp117.readCalls == 1 && sensorInitialized &&
                !sensorRetryWaiting && MockController::lastValid,
            "sensor must recover at retry deadline");
    require(MockController::lastSampleStartedMs == start + SENSOR_RETRY_INTERVAL_MS + 22,
            "first acquisition starts after initialization completes");
    tmp117.readResult = false;
    mockNow += SENSOR_READ_INTERVAL_MS;
    pollSensor(mockNow);
    require(!sensorInitialized && sensorRetryWaiting && !MockController::lastValid,
            "runtime failure must stop submitting valid values and schedule retry");
  }
}
void timerSetupContracts() {
  reset();
  mockTimerCreateResult = -1;
  setup();
  require(mockTimerStartCalls == 0 && controller.status().state == ControllerState::SafetyFaultLatched,
          "failed creation must not attempt start or enable PWM");
  reset();
  mockTimerStartResult = -1;
  setup();
  require(mockTimerStartCalls == 1 && controller.status().state == ControllerState::SafetyFaultLatched,
          "failed periodic start must latch safety failure");
  reset();
  setup();
  require(mockTimerArgs.callback && mockTimerArgs.dispatch_method == ESP_TIMER_TASK &&
              mockTimerPeriod == OUTPUT_GUARD_INTERVAL_US && !controller.settings().enabled,
          "normal startup must register the periodic guard and stay disabled");
  const int before = MockController::ageCalls;
  mockTimerArgs.callback(nullptr);
  require(MockController::ageCalls == before + 1, "timer must invoke the sample-age guard");
}
void directionRequest() {
  server.args = {{"confirm", "full_power"}};
}
void requireDirectionOff() {
  require(!controller.settings().enabled && !controller.status().driverAwake &&
              controller.status().signedDutyPercent == 0 &&
              controller.status().coolingDutyPercent == 0 &&
              controller.status().heatingDutyPercent == 0,
          "direction operation enabled normal output");
}
void finishDirection(CalibrationPhase phase, TecDirection candidate) {
  MockController::calibration.active = false;
  MockController::calibration.phase = phase;
  MockController::calibration.candidate = candidate;
  MockController::calibration.rawDutyPercent = 0;
  mutableStatus().state = ControllerState::Disabled;
  mutableStatus().driverAwake = false;
}
void directionConfirmation() {
  for (const auto& value : std::vector<String>{"", "true", "FULL_POWER", "full_power ",
                                              std::string("full_power\0junk", 15)}) {
    reset(); server.args["confirm"] = value;
    handleDirectionCalibration(); response(400, "direction-confirm");
    noMutations();
    require(MockController::eligibilityCalls == 0, "confirmation must precede eligibility");
  }
  reset(); handleDirectionCalibration(); response(400, "direction-confirm-missing");
  noMutations();
}
void directionBusyConditions() {
  for (int condition = 0; condition < 6; ++condition) {
    reset(); directionRequest();
    if (condition == 0) wifiScanState = WifiScanState::Scanning;
    if (condition == 1) staPhase = StaPhase::Connecting;
    if (condition == 2) MockController::calibrationEligible = false;
    if (condition == 3) MockController::calibration.active = true;
    if (condition == 4) mutableStatus().state = ControllerState::DriverFaultLatched;
    if (condition == 5) {
      controller.setSettings(25, 30, 20, true);
      MockController::setCalls = 0;
    }
    handleDirectionCalibration(); response(409, "direction-busy");
    noMutations();
    require(controller.tecDirection() == TecDirection::Normal, "busy request invalidated mapping");
  }
}
void directionInvalidationFailure() {
  configStore.storedDirection = TecDirection::Normal;
  directionSaved = true;
  configStore.directionSaveOk = false;
  directionRequest(); handleDirectionCalibration(); response(503, "direction-invalidation-failure");
  require(configStore.directionSaves == std::vector<TecDirection>{TecDirection::Unknown} &&
              configStore.storedDirection == TecDirection::Normal,
          "failed invalidation changed durable direction or retried");
  require(MockController::startCalls == 0 && MockController::pulseStarts == 0 &&
              controller.tecDirection() == TecDirection::Unknown && !directionSaved &&
              !directionError.isEmpty(), "failed invalidation allowed a run or kept runtime mapping");
  serviceDirectionCalibration(); serviceDirectionCalibration();
  require(configStore.directionSaves.size() == 1, "failed invalidation triggered final save");
  requireDirectionOff();
  snapshot("direction-invalidation-error");
}
void directionRechecksAfterInvalidation() {
  for (int race = 0; race < 3; ++race) {
    reset(); directionRequest();
    configStore.beforeDirectionSave = [race] {
      if (race == 0) {
        mockNow += SENSOR_STALE_TIMEOUT_MS;
        MockController::calibrationEligible = false;
      } else if (race == 1) {
        mutableStatus().state = ControllerState::DriverFaultLatched;
      } else {
        MockController::calibrationStartOk = false;
      }
    };
    MockController::beforeStart = [race] {
      require(mockNow == (race == 0 ? 1000u + SENSOR_STALE_TIMEOUT_MS : 1000u),
              "start recheck did not observe post-invalidation time");
      require(configStore.storedDirection == TecDirection::Unknown &&
                  controller.tecDirection() == TecDirection::Unknown,
              "race recheck preceded durable/runtime invalidation");
    };
    handleDirectionCalibration(); response(409, "direction-start-recheck");
    require(MockController::startCalls == 1 && MockController::pulseStarts == 0 &&
                controller.tecDirection() == TecDirection::Unknown &&
                configStore.storedDirection == TecDirection::Unknown && !directionSaved &&
                calibrationResultHandled, "start race did not remain unknown and disarmed");
    require(configStore.directionSaves == std::vector<TecDirection>{TecDirection::Unknown},
            "start recheck failure attempted final save");
    requireDirectionOff();
  }
}
void directionSuccessOnce() {
  for (TecDirection candidate : {TecDirection::Normal, TecDirection::Reversed}) {
    reset(); directionRequest();
    configStore.storedDirection = TecDirection::Normal;
    configStore.beforeDirectionSave = [] {
      require(MockController::startCalls == 0 &&
                  controller.tecDirection() == TecDirection::Normal,
              "runtime invalidation/start preceded durable invalidation");
    };
    MockController::beforeStart = [] {
      require(configStore.storedDirection == TecDirection::Unknown &&
                  controller.tecDirection() == TecDirection::Unknown && !directionSaved,
              "start preceded durable/runtime invalidation");
    };
    handleDirectionCalibration(); response(200, "direction-started");
    require(mockEvents == std::vector<std::string>{
                "calibration-eligible", "save-direction:unknown", "apply-direction:unknown",
                "calibration-start", "calibration-eligible"},
            "direction start ordering changed");
    require(!calibrationResultHandled && MockController::pulseStarts == 1,
            "new run was not armed for result handling");
    serviceDirectionCalibration();
    require(configStore.directionSaves.size() == 1, "active run saved a result");
    configStore.beforeDirectionSave = [] {
      require(controller.tecDirection() == TecDirection::Unknown,
              "candidate applied before durable result save");
    };
    finishDirection(CalibrationPhase::Complete, candidate);
    mockEvents.clear();
    serviceDirectionCalibration(); serviceDirectionCalibration();
    require(configStore.directionSaves == std::vector<TecDirection>{TecDirection::Unknown, candidate} &&
                MockController::directionApplications ==
                    std::vector<TecDirection>{TecDirection::Unknown, candidate},
            "result must be persisted and applied exactly once");
    require(mockEvents == std::vector<std::string>{
                std::string("save-direction:") + tecDirectionKey(candidate),
                std::string("apply-direction:") + tecDirectionKey(candidate)},
            "final mapping must follow durable save without enable");
    require(directionSaved && directionError.isEmpty() && calibrationResultHandled &&
                controller.tecDirection() == candidate, "successful result not exposed");
    requireDirectionOff();
    snapshot(candidate == TecDirection::Normal ? "direction-normal-saved" : "direction-reversed-saved");
    configStore.beforeDirectionSave = nullptr;
    MockController::beforeStart = nullptr;
    directionRequest(); handleDirectionCalibration();
    finishDirection(CalibrationPhase::Complete, candidate);
    serviceDirectionCalibration(); serviceDirectionCalibration();
    require(configStore.directionSaves.size() == 4 &&
                MockController::directionApplications.size() == 4,
            "a new run did not reset the once-only result guard");
  }
}
void directionResultSaveFailure() {
  directionRequest(); handleDirectionCalibration();
  configStore.directionSaveOk = false;
  finishDirection(CalibrationPhase::Complete, TecDirection::Reversed);
  serviceDirectionCalibration(); serviceDirectionCalibration();
  require(configStore.directionSaves ==
              std::vector<TecDirection>{TecDirection::Unknown, TecDirection::Reversed, TecDirection::Unknown} &&
              MockController::directionApplications == std::vector<TecDirection>{TecDirection::Unknown},
          "failed result persistence retried or applied candidate");
  require(controller.tecDirection() == TecDirection::Unknown &&
              configStore.storedDirection == TecDirection::Unknown && !directionSaved &&
              !directionError.isEmpty(), "failed result save was not fail-closed");
  requireDirectionOff();
  snapshot("direction-result-save-failure");
}
void directionFailedSaveRollback() {
  for (const bool rollbackWorks : {false, true}) {
    reset(); directionRequest(); handleDirectionCalibration();
    finishDirection(CalibrationPhase::Complete, TecDirection::Reversed);
    configStore.beforeDirectionSave = [rollbackWorks] {
      const bool candidate = configStore.directionSaves.back() == TecDirection::Reversed;
      configStore.directionSaveOk = !candidate && rollbackWorks;
      configStore.directionCommitBeforeFailure = candidate;
    };
    serviceDirectionCalibration(); serviceDirectionCalibration();
    require(configStore.directionSaves == std::vector<TecDirection>{
                TecDirection::Unknown, TecDirection::Reversed, TecDirection::Unknown},
            "failed verification must attempt one durable invalidation");
    require(controller.tecDirection() == TecDirection::Unknown && !directionSaved,
            "uncertain persistence must not authorize runtime output");
    require(configStore.storedDirection == (rollbackWorks ? TecDirection::Unknown : TecDirection::Reversed),
            "test must model commit followed by failed readback and conditional rollback");
    require(std::string(directionError.c_str()).find(rollbackWorks ? "记录已清除" : "重启可能恢复") !=
                std::string::npos, "rollback certainty must be visible to the operator");
    requireDirectionOff();
    snapshot(rollbackWorks ? "direction-rollback-ok" : "direction-rollback-uncertain");
    configStore.beforeDirectionSave = nullptr;
    setup();
    requireDirectionOff();
    require(controller.tecDirection() == configStore.storedDirection,
            "reboot must match the actual remaining record without enabling output");
  }
}
void directionRuntimeInvalidationRefusal() {
  MockController::directionApplyOk = false;
  directionRequest(); handleDirectionCalibration(); response(409, "direction-runtime-invalidation-refused");
  require(MockController::startCalls == 0 && MockController::pulseStarts == 0 &&
              configStore.storedDirection == TecDirection::Unknown && calibrationResultHandled,
          "runtime invalidation refusal must prevent starting or accepting a result");
  requireDirectionOff();
}
void directionTerminalNoSave() {
  for (auto phase : {CalibrationPhase::Failed, CalibrationPhase::Cancelled, CalibrationPhase::Complete}) {
    reset(); directionRequest(); handleDirectionCalibration();
    finishDirection(phase, phase == CalibrationPhase::Complete
                               ? TecDirection::Unknown : TecDirection::Reversed);
    serviceDirectionCalibration(); serviceDirectionCalibration();
    require(configStore.directionSaves == std::vector<TecDirection>{TecDirection::Unknown} &&
                MockController::directionApplications == std::vector<TecDirection>{TecDirection::Unknown} &&
                controller.tecDirection() == TecDirection::Unknown && !directionSaved,
            "failed/cancelled/unknown result was persisted or applied");
    requireDirectionOff();
  }
}
void directionCancellationStaleComplete() {
  for (bool completeBeforeStop : {false, true}) {
    reset(); directionRequest(); handleDirectionCalibration();
    if (completeBeforeStop) finishDirection(CalibrationPhase::Complete, TecDirection::Reversed);
    else {
      MockController::calibration.phase = CalibrationPhase::Forward;
      MockController::calibration.rawDutyPercent = 100;
      mutableStatus().driverAwake = true;
    }
    handleStop(); response(200, "direction-stop");
    require(MockController::cancelCalls == 1 && !MockController::calibration.active,
            "stop did not cancel calibration");
    requireDirectionOff();
    serviceDirectionCalibration();
    finishDirection(CalibrationPhase::Complete, TecDirection::Reversed);
    serviceDirectionCalibration(); serviceDirectionCalibration();
    require(configStore.directionSaves == std::vector<TecDirection>{TecDirection::Unknown} &&
                MockController::directionApplications == std::vector<TecDirection>{TecDirection::Unknown},
            "stop accepted a stale Complete result and persisted/applied it");
  }
}
void directionActiveBlocksMutations() {
  for (auto handler : {handleSettings, handleWifiConnect, handleWifiScan, handleWifiDisconnect}) {
    reset(); MockController::calibration.active = true;
    mutableStatus().state = ControllerState::DirectionCalibration;
    settingsRequest(); server.args["ssid"] = "trial"; server.args["password"] = "password";
    handler(); response(409, "direction-active-block");
    noMutations();
    require(MockController::calibration.active && mockScanStops == 0,
            "blocked request cancelled calibration or scan");
  }
}
void directionPausesWifiRetry() {
  staSsid = "saved"; staPassword = "password";
  staDesired = staRetryWaiting = true;
  staRetryStartedMs = mockNow - staRetryDelayMs;
  MockController::calibration.active = true;
  pollWifi(mockNow); pollWifi(mockNow + 60000);
  require(WiFi.beginCalls == 0 && WiFi.disconnectCalls == 0 && staRetryWaiting &&
              !staConnectRequested() && staRetryDelayMs == 5000 &&
              configStore.wifiSaves == 0, "active calibration did not pause automatic retry");
  MockController::calibration.active = false;
  pollWifi(mockNow);
  require(WiFi.beginCalls == 0 && staPhase == StaPhase::Disconnecting,
          "retry must first request old-association teardown");
  pollWifi(mockNow);
  require(WiFi.beginCalls == 1 && staConnectRequested() && !staRetryWaiting,
          "retry did not resume after calibration");
}
void directionUnknownSettings() {
  controller = ThermalController{};
  require(controller.tecDirection() == TecDirection::Unknown, "production default must be unknown");
  settingsRequest(); handleSettings(); response(409, "direction-unknown-enable");
  noMutations();
  settingsRequest("26", "31", "21", "false");
  handleSettings(); response(200, "direction-unknown-disabled-settings");
  require(configStore.controllerSaves == 1 && controller.settings().targetC == 26 &&
              controller.tecDirection() == TecDirection::Unknown, "unknown blocked safe settings");
  requireDirectionOff();
}
void directionBootMissingCorrupt() {
  for (bool corrupt : {false, true}) {
    reset();
    configStore.storedSettings.enabled = true;
    configStore.storedDirection = corrupt ? TecDirection::Reversed : TecDirection::Unknown;
    configStore.directionLoadOk = !corrupt;
    if (corrupt) {
      TecDirection poisoned = TecDirection::Normal;
      require(!configStore.loadDirection(poisoned) && poisoned == TecDirection::Unknown,
              "failed load mock must overwrite output with Unknown");
      configStore.directionLoads = 0;
    }
    setup();
    require(configStore.directionLoads == 1 &&
                controller.tecDirection() == TecDirection::Unknown && !directionSaved &&
                configStore.directionSaves.empty() && MockController::startCalls == 0 &&
                MockController::pulseStarts == 0, "missing/corrupt boot restored or energized direction");
    require(corrupt ? !directionError.isEmpty() : directionError.isEmpty(),
            "missing/corrupt direction error distinction lost");
    requireDirectionOff();
    snapshot(corrupt ? "direction-boot-corrupt" : "direction-boot-missing");
  }
}
void directionBootRestoresWithoutPower() {
  for (TecDirection direction : {TecDirection::Normal, TecDirection::Reversed}) {
    reset(); configStore.storedSettings.enabled = true;
    configStore.storedDirection = direction;
    setup();
    require(controller.tecDirection() == direction && directionSaved &&
                configStore.directionLoads == 1 && configStore.directionSaves.empty() &&
                configStore.controllerSaves == 0 && MockController::startCalls == 0 &&
                MockController::pulseStarts == 0, "known boot mapping was not restored without writes/start");
    requireDirectionOff();
    snapshot(direction == TecDirection::Normal ? "direction-boot-normal" : "direction-boot-reversed");
  }
}
void directionStatusRawSemantics() {
  controller = ThermalController{};
  mutableStatus().state = ControllerState::DirectionCalibration;
  MockController::calibration.active = true;
  MockController::calibration.phaseElapsedMs = 1234;
  MockController::calibration.forwardDeltaC = 0.75f;
  MockController::calibration.reverseDeltaC = -0.5f;
  MockController::calibration.error = "probe \"raw\"\n";
  directionError = "store\\error\t";
  for (auto phase : {CalibrationPhase::Idle, CalibrationPhase::BaselineForward, CalibrationPhase::Forward,
                    CalibrationPhase::Settle, CalibrationPhase::BaselineReverse, CalibrationPhase::Reverse,
                    CalibrationPhase::Complete, CalibrationPhase::Failed, CalibrationPhase::Cancelled}) {
    MockController::calibration.phase = phase;
    MockController::calibration.rawDutyPercent = phase == CalibrationPhase::Forward ? 100 :
        phase == CalibrationPhase::Reverse ? -100 : 0;
    const std::string label = std::string("direction-phase-") + calibrationPhaseKey(phase);
    snapshot(label.c_str());
  }
  MockController::calibration.forwardDeltaC = NAN;
  MockController::calibration.reverseDeltaC = INFINITY;
  snapshot("direction-nonfinite-deltas");
  auto copy = controller.directionCalibrationStatus();
  copy.phaseElapsedMs = 999;
  require(controller.directionCalibrationStatus().phaseElapsedMs == 1234 &&
              copy.phaseElapsedMs == 999, "calibration status must be an independent value");
  require(MockController::directionApplications.empty() && configStore.directionSaves.empty(),
          "status endpoint mutated direction");
}
void directionApplyFailureAndUnarmedComplete() {
  finishDirection(CalibrationPhase::Complete, TecDirection::Reversed);
  serviceDirectionCalibration();
  require(configStore.directionSaves.empty() && MockController::directionApplications.empty(),
          "Complete without a new start was accepted");
  directionRequest(); handleDirectionCalibration();
  finishDirection(CalibrationPhase::Complete, TecDirection::Reversed);
  MockController::directionApplyOk = false;
  serviceDirectionCalibration(); serviceDirectionCalibration();
  require(configStore.directionSaves ==
              std::vector<TecDirection>{TecDirection::Unknown, TecDirection::Reversed} &&
              MockController::directionApplications ==
                  std::vector<TecDirection>{TecDirection::Unknown, TecDirection::Reversed} &&
              controller.tecDirection() == TecDirection::Unknown &&
              !directionSaved && !directionError.isEmpty(), "failed apply retried or reported known mapping");
  requireDirectionOff();
  snapshot("direction-apply-failure");
}

void rebindingMutationProof() {
  server.setHost("rebind.attacker.test");
  server.headers["Origin"] = "http://rebind.attacker.test";
  settingsRequest();
  handleSettings();
  std::cout << "PROOF rebinding settings: HTTP " << server.code
            << ", saves=" << configStore.controllerSaves
            << ", enabled=" << controller.settings().enabled << '\n';
  response(403, "rebinding-settings");
  noMutations();
}
void rebindingReadProof() {
  server.setHost("rebind.attacker.test");
  handleStatus();
  std::cout << "PROOF rebinding status: HTTP " << server.code
            << ", body bytes=" << server.body.length() << '\n';
  response(403, "rebinding-status");
  require(MockController::ageCalls == 0 && MockController::pollCalls == 0,
          "untrusted status reached controller");
}
void untrustedHostsEveryHandler() {
  const std::vector<String> invalid = {
      "", "rebind.attacker.test", "192.168.1.1.attacker.test", "192.168.1.10",
      "x192.168.1.1", "192.168.1.1:8080", "192.168.1.1:080", "192.168.1.1:",
      "192.168.1.1:80:80", "192.168.1.1@attacker.test", "attacker.test@192.168.1.1",
      "http://192.168.1.1", "192.168.1.1/", "192.168.1.1.", "192.168.001.001",
      "3232235777", "0xc0a80101", "[::ffff:192.168.1.1]", "127.0.0.1",
      "0.0.0.0", "255.255.255.255", "192.168.1.1,attacker.test",
      " 192.168.1.1", "192.168.1.1 ", "192.168.1.1\t", "192.168.1.1\n",
      std::string("192.168.1.1") + '\0' + ".attacker.test",
      std::string("192.168.1.1:80") + '\0' + ".attacker.test"};
  for (auto handler : {handleSettings, handleStop, handleResetFault, handleWifiConnect,
                       handleWifiDisconnect, handleWifiScan, handleDirectionCalibration, handleStatus}) {
    for (const auto& host : invalid) {
      reset(); server.setHost(host); settingsRequest();
      server.args["ssid"] = "trial";
      server.args["password"] = "password";
      server.args["confirm"] = "full_power";
      handler(); response(403, "untrusted-host"); noMutations();
      require(MockController::resetCalls == 0 && mockScanStops == 0 &&
                  MockController::ageCalls == 0 && MockController::pollCalls == 0 &&
                  !wifiScanHasRequested, "untrusted host reached handler work");
    }
  }
}
void originsEveryHandler() {
  const std::vector<String> invalid = {
      "null", "http://attacker.test", "http://192.168.1.1.attacker.test",
      "https://192.168.1.1", "http://192.168.1.1:8080", "http://192.168.1.1:080",
      "http://192.168.1.1/", "http://192.168.1.1?x", "http://192.168.1.1#x",
      "http://192.168.1.1@attacker.test", "http://attacker.test@192.168.1.1",
      "http://192.168.1.1 http://attacker.test", "http://192.168.1.1,http://attacker.test",
      " http://192.168.1.1", "http://192.168.1.1 ", "http://192.168.1.1\n",
      "http://192.168.1.42", "HTTP://192.168.1.1",
      std::string("http://192.168.1.1") + '\0' + ".attacker.test"};
  for (auto handler : {handleSettings, handleStop, handleResetFault, handleWifiConnect,
                       handleWifiDisconnect, handleWifiScan, handleDirectionCalibration, handleStatus}) {
    for (const auto& origin : invalid) {
      reset(); WiFi.connection = WL_CONNECTED;
      server.headers["Origin"] = origin;
      settingsRequest(); handler(); response(403, "untrusted-origin"); noMutations();
      require(MockController::ageCalls == 0 && MockController::pollCalls == 0 &&
                  MockController::resetCalls == 0, "untrusted origin reached controller");
    }
  }
}
void legitimateAuthoritiesAndOrigins() {
  for (const char* address : {"192.168.1.1", "192.168.1.42"}) {
    for (const char* port : {"", ":80"}) {
      for (const String& origin : std::vector<String>{
          "", String("http://") + address, String("http://") + address + ":80"}) {
        reset(); WiFi.connection = WL_CONNECTED;
        server.setHost(String(address) + port);
        server.headers["Origin"] = origin;
        settingsRequest(); handleSettings(); response(200, "trusted-settings");
        require(configStore.controllerSaves == 1 && controller.settings().enabled,
                "trusted settings no longer work");
        server.headers.erase("X-PL530-Request");
        handleStatus(); response(200, "trusted-status-no-marker");
        handleWifiScan(); response(403, "trusted-scan-requires-marker");
        require(WiFi.scanCalls == 0, "scan lost marker requirement");
      }
    }
  }
}
void activeStaAddressOnly() {
  server.setHost("192.168.1.42");
  handleStatus(); response(403, "disconnected-sta");
  WiFi.connection = WL_CONNECTED;
  handleStatus(); response(200, "connected-sta");
  WiFi.address = IPAddress(10, 2, 3, 4);
  handleStatus(); response(403, "old-sta-address");
  server.setHost("10.2.3.4:80"); server.headers["Origin"] = "http://10.2.3.4";
  handleStatus(); response(200, "new-sta-address");
  WiFi.connection = WL_DISCONNECTED;
  handleStatus(); response(403, "lost-sta-address");
  WiFi.connection = WL_CONNECTED; WiFi.address = IPAddress(0, 0, 0, 0);
  server.setHost("0.0.0.0"); server.headers.erase("Origin");
  handleStatus(); response(403, "unassigned-sta-address");
  server.setHost("192.168.1.1"); handleStatus(); response(200, "ap-still-available");
}
void parserHeaderBoundary() {
  const char* fields[] = {"Host", "Origin", "X-PL530-Request"};
  server.collectHeaders(fields, 3);
  const auto rawHost = std::string("192.168.1.1") + '\0' + ".attacker.test";
  server.parseHeaders(HTTP_POST, {"hOsT: " + rawHost,
      std::string("Origin: http://192.168.1.1") + '\0' + ".attacker.test",
      std::string("X-PL530-Request: 1") + '\0' + "junk"});
  require(server.hostHeader().length() == rawHost.size() &&
              server.header("HOST").length() == 11 &&
              server.header("origin").length() == 18 &&
              server.header("x-pl530-request").length() == 1,
          "mock no longer matches raw Host versus collected header truncation");
  require(String(std::string("1") + '\0' + "junk") == "1",
          "mock must expose Arduino literal comparison NUL behavior");
  require(server.hasHeader("origin"), "case insensitive hasHeader failed");
  server.parseHeaders(HTTP_GET, {"Origin: "});
  require(server.hostHeader().length() == rawHost.size() &&
              !server.hasHeader("Host") && !server.hasHeader("Origin"),
          "per-request collection or stale host behavior differs from pinned parser");
  server.parseHeaders(HTTP_GET, {"Host:192.168.1.1"});
  require(server.hostHeader() == "92.168.1.1", "pinned GET parser skip changed");
  server.parseHeaders(HTTP_POST, {"Host:192.168.1.1", "X-PL530-Request: 1 \t"});
  require(server.hostHeader() == "192.168.1.1" && server.header("X-PL530-Request") == "1",
          "pinned POST whitespace parsing changed");
}
void rawHostNulAndStale() {
  setupRoutes();
  for (const auto method : {HTTP_GET, HTTP_POST}) {
    server.parseHeaders(method, {
        std::string("Host: 192.168.1.1") + '\0' + ".attacker.test", "X-PL530-Request: 1"});
    server.dispatch(method == HTTP_GET ? "/api/status" : "/api/control/stop", method);
    response(403, "raw-host-nul"); noMutations();
    server.parseHeaders(method, {"Host: 192.168.1.1", "X-PL530-Request: 1"});
    server.dispatch("/api/status", HTTP_GET); response(200, "raw-valid-host");
    server.parseHeaders(method, {"X-PL530-Request: 1"});
    require(server.hostHeader() == "192.168.1.1", "stale-host setup failed");
    server.dispatch(method == HTTP_GET ? "/api/status" : "/api/control/stop", method);
    response(403, "raw-missing-host"); noMutations();
  }
}
void exactMarkerAndConfirmation() {
  server.headers["X-PL530-Request"] = std::string("1") + '\0' + "junk";
  require(server.header("X-PL530-Request") == "1", "legacy marker comparison not reproduced");
  std::cout << "PROOF legacy marker comparison: accepts 6-byte 1\\0junk\n";
  handleStop(); response(403, "nul-marker"); noMutations();
  reset();
  server.args["confirm"] = std::string("full_power") + '\0' + "junk";
  handleDirectionCalibration(); response(400, "nul-confirmation"); noMutations();
}
void encodedNulConfirmation() {
  for (const char* encoded : {"full_power%00junk", "full_power%00",
                              "full_power%00%00", "%00full_power"}) {
    reset(); setupRoutes();
    server.parseHeaders(HTTP_POST, {"Host: 192.168.1.1", "Origin: http://192.168.1.1",
                                   "X-PL530-Request: 1"});
    server.args["confirm"] = WebServer::urlDecode(encoded);
    require(server.arg("confirm").length() > std::strlen(server.arg("confirm").c_str()),
            "percent-encoded NUL was lost before handler validation");
    if (std::strcmp(encoded, "full_power%00junk") == 0) {
      require(server.arg("confirm") == "full_power", "legacy confirmation bypass not reproduced");
      std::cout << "PROOF legacy confirmation comparison: accepts 15-byte full_power%00junk\n";
    }
    server.dispatch("/api/control/detect-direction", HTTP_POST);
    response(400, "percent-nul-confirmation"); noMutations();
    require(MockController::eligibilityCalls == 0, "malformed confirmation reached eligibility");
  }
  reset(); server.args["confirm"] = WebServer::urlDecode("%66ull_power");
  handleDirectionCalibration(); response(200, "encoded-valid-confirmation");
  require(MockController::startCalls == 1, "valid URL decoding stopped working");
}
void rawOriginsAndCollectionLimits() {
  for (const auto method : {HTTP_GET, HTTP_POST}) {
    reset(); setupRoutes();
    server.parseHeaders(method, {"hOsT: 192.168.1.1", "oRiGiN: http://attacker.test",
                                 "x-pl530-request: 1"});
    server.dispatch(method == HTTP_GET ? "/api/wifi/scan" : "/api/control/stop", method);
    response(403, "raw-untrusted-origin"); noMutations();
    server.parseHeaders(method, {"Host: 192.168.1.1", "Origin: http://192.168.1.1:80",
                                 "X-PL530-Request: 1"});
    server.dispatch("/api/status", HTTP_GET); response(200, "raw-trusted-origin");
    server.parseHeaders(method, {"Host: 192.168.1.1", "Origin: null", "X-PL530-Request: 1"});
    server.dispatch("/api/status", HTTP_GET); response(403, "raw-null-origin");
    server.parseHeaders(method, {"Host: 192.168.1.1", "Origin: http://192.168.1.1",
                                 "Origin: http://attacker.test", "X-PL530-Request: 1"});
    server.dispatch("/api/status", HTTP_GET); response(403, "raw-last-origin");
    server.parseHeaders(method, {"Host: 192.168.1.1", "Host: attacker.test",
                                 "X-PL530-Request: 1"});
    server.dispatch("/api/status", HTTP_GET); response(403, "raw-last-host");

    // These bytes are lost upstream. Do not claim the handler can reject them.
    server.parseHeaders(method, {"Host: 192.168.1.1",
        std::string("Origin: http://192.168.1.1") + '\0' + ".attacker.test",
        std::string("X-PL530-Request: 1") + '\0' + "junk"});
    require(server.header("Origin").length() == 18 &&
                server.header("X-PL530-Request").length() == 1, "collection limit changed");
    server.dispatch("/api/status", HTTP_GET); response(200, "raw-truncated-origin-limit");
    require(writeRequestAllowed(), "handler cannot distinguish upstream-truncated marker");
    server.parseHeaders(method, {"Host: 192.168.1.1",
        std::string("Origin: ") + '\0' + "http://attacker.test", "X-PL530-Request: 1"});
    require(!server.hasHeader("Origin"), "empty versus absent Origin limit changed");
    server.dispatch("/api/status", HTTP_GET); response(200, "raw-empty-origin-limit");
  }
}
void framedUiAndCaptiveHosts() {
  setupRoutes();
  server.dispatch("/", HTTP_GET);
  std::cout << "PROOF embedded UI: HTTP " << server.code
            << ", X-Frame-Options=" << server.responseHeaders["X-Frame-Options"].c_str()
            << ", CSP=" << server.responseHeaders["Content-Security-Policy"].c_str() << '\n';
  require(server.code == 200 && server.contentType == "text/html" &&
              server.responseHeaders["X-Frame-Options"] == "DENY" &&
              server.responseHeaders["Content-Security-Policy"] == "frame-ancestors 'none'",
          "UI framing not blocked by HTTP headers");
  for (const char* path : {"/", "/generate_204", "/hotspot-detect.html"}) {
    server.resetRequest(); server.setHost("captive.attacker.test");
    server.dispatch(path, HTTP_GET);
    require(server.code == 302 && server.responseHeaders["Location"] == "http://192.168.1.1/" &&
                server.body.isEmpty(), "captive host must redirect without serving UI");
  }
}
void routedApiTrustAndNoCors() {
  const std::vector<std::pair<const char*, HTTPMethod>> requests = {
      {"/api/status", HTTP_GET}, {"/api/settings", HTTP_POST}, {"/api/wifi/scan", HTTP_GET},
      {"/api/wifi/connect", HTTP_POST}, {"/api/wifi/disconnect", HTTP_POST},
      {"/api/control/reset-fault", HTTP_POST}, {"/api/control/stop", HTTP_POST},
      {"/api/control/detect-direction", HTTP_POST}, {"/api/missing", HTTP_GET},
      {"/api", HTTP_GET}, {"/api/settings", HTTP_GET}, {"/api/settings", HTTP_OPTIONS}};
  for (const auto& request : requests) {
    reset(); setupRoutes(); server.setHost("rebind.attacker.test");
    server.dispatch(request.first, request.second); response(403, "routed-untrusted-api");
    noMutations();
    require(server.responseHeaders.count("Location") == 0 &&
                server.responseHeaders.count("Access-Control-Allow-Origin") == 0,
            "API request redirected or CORS enabled");
  }
  for (const auto& request : std::vector<std::pair<const char*, HTTPMethod>>{
      {"/api/missing", HTTP_GET}, {"/api", HTTP_GET}, {"/api/settings", HTTP_OPTIONS}}) {
    reset(); setupRoutes(); server.dispatch(request.first, request.second);
    response(404, "trusted-api-not-found");
    noMutations();
    require(server.responseHeaders.count("Access-Control-Allow-Origin") == 0, "CORS enabled");
  }
}
void rejectedScanPreservesResultAndThrottle() {
  setupRoutes(); handleWifiScan();
  WiFi.networks = {{"local-result", -42}}; WiFi.scanResult = 1; pollWifiScan(mockNow);
  const String result = wifiScanResultJson;
  const auto scanAt = wifiScanLastRequestMs;
  server.setHost("attacker.test"); handleWifiScan(); response(403, "untrusted-scan-result");
  require(wifiScanState == WifiScanState::Completed &&
              wifiScanResultJson == result && wifiScanLastRequestMs == scanAt &&
              WiFi.scanCalls == 1, "denied scan consumed result or changed throttle");
  server.setHost("192.168.1.1"); handleWifiScan(); response(200, "trusted-scan-result");
  require(wifiScanState == WifiScanState::Idle, "trusted scan no longer consumes result");
}

void seedVerifiedOldWifi() {
  staSsid = configStore.storedSsid;
  staPassword = configStore.storedPassword;
  staDesired = staCredentialsSaved = true;
  staPhase = StaPhase::Verified;
  WiFi.associate(staSsid);
}

void oldAssociationCannotAuthorizeTrial() {
  for (bool sameSsid : {false, true}) {
    for (bool statusFirst : {false, true}) {
      for (uint32_t start : {1000u, UINT32_MAX - 10000u}) {
        reset(); mockNow = start; seedVerifiedOldWifi();
        const String target = sameSsid ? configStore.storedSsid : String("new-network");
        wifiRequest(target, "wrong-password"); handleWifiConnect();
        response(200, "wifi-replacement-pending");
        require(WiFi.connection == WL_CONNECTED && MockSta::associated &&
                    WiFi.beginCalls == 0, "disconnect must preserve old asynchronous state in the mock");
        pollWifi(start + 1u);
        require(staPhase == StaPhase::Disconnecting && configStore.wifiSaves == 0 &&
                    WiFi.beginCalls == 0, "old connection authorized new trial credentials");
        snapshot("wifi-old-association-pending");
        if (statusFirst) WiFi.deliverDisconnected();
        else WiFi.dropAssociation();
        pollWifi(start + 2u);
        require(WiFi.beginCalls == 0, "both driver association and cached connected status must be down");
        if (statusFirst) WiFi.dropAssociation();
        else WiFi.deliverDisconnected();
        mockNow = start + 3u; pollWifi(mockNow);
        require(WiFi.beginCalls == 1 && staPhase == StaPhase::Connecting &&
                    WiFi.begunSsid == target && WiFi.begunPassword == "wrong-password" &&
                    !WiFi.beganWithOpenPassword, "new begin did not follow the genuine down boundary");
        // A delayed GOT_IP/status update alone is not a new authenticated association.
        WiFi.deliverGotIp();
        pollWifi(start + 4u);
        require(configStore.wifiSaves == 0, "stale cached status/IP was accepted without AP information");
        mockNow = start + WIFI_CONNECT_TIMEOUT_MS; pollWifi(mockNow);
        require(!staDesired && !staConnectRequested() && !staRetryWaiting &&
                    staPassword.isEmpty() && configStore.wifiSaves == 0 &&
                    configStore.storedSsid == "previous-network" &&
                    configStore.storedPassword == "previous-password",
                "wrong-password trial changed NVS or continued retrying");
        require(!WiFi.lastDisconnectWifiOff && WiFi.getMode() == WIFI_AP_STA,
                "STA replacement must preserve AP mode");
      }
    }
  }
}

void newAssociationRequiresTargetAndIp() {
  for (const String& target : std::vector<String>{"new-network", std::string(32, 'S')}) {
    reset(); wifiRequest(target, "new-password"); handleWifiConnect(); pollWifi(mockNow);
    WiFi.deliverGotIp(); pollWifi(mockNow);
    require(configStore.wifiSaves == 0, "cached connected status without an association was accepted");
    WiFi.radioAssociate("other-network"); pollWifi(mockNow);
    require(configStore.wifiSaves == 0, "another SSID was accepted as the requested target");
    WiFi.radioAssociate(target);
    WiFi.connection = WL_IDLE_STATUS; pollWifi(mockNow);
    require(configStore.wifiSaves == 0, "association without connected status was accepted");
    WiFi.deliverGotIp(IPAddress(0, 0, 0, 0)); pollWifi(mockNow);
    require(configStore.wifiSaves == 0, "zero IP was accepted");
    WiFi.deliverGotIp();
    MockSta::queryError = ESP_ERR_WIFI_CONN; pollWifi(mockNow);
    require(configStore.wifiSaves == 0, "AP query failure authorized persistence");
    MockSta::queryError = ESP_OK;
    MockSta::association.ssid[target.length()] = 'X';
    pollWifi(mockNow);
    require(configStore.wifiSaves == 0, "nonterminated/prefix-only SSID matched the target");
    WiFi.radioAssociate(target); pollWifi(mockNow); pollWifi(mockNow + 1u);
    require(staPhase == StaPhase::Verified && staCredentialsSaved &&
                configStore.wifiSaves == 1 && configStore.storedSsid == target &&
                configStore.storedPassword == "new-password", "genuine association was not saved once");
    snapshot("wifi-verified-association");
  }
}

void disconnectWaitUsesWholeDeadline() {
  for (uint32_t start : {1000u, UINT32_MAX - 10000u}) {
    reset(); mockNow = start; seedVerifiedOldWifi();
    wifiRequest(); handleWifiConnect();
    pollWifi(start + WIFI_CONNECT_TIMEOUT_MS - 1u);
    require(staPhase == StaPhase::Disconnecting && WiFi.beginCalls == 0,
            "old association should still be awaiting disconnect");
    mockNow = start + WIFI_CONNECT_TIMEOUT_MS; pollWifi(mockNow);
    require(!staDesired && !staConnectRequested() && WiFi.beginCalls == 0 &&
                configStore.wifiSaves == 0, "disconnect stage escaped the overall 20-second deadline");
    WiFi.dropAssociation(); WiFi.deliverDisconnected(); WiFi.associate("trial-network");
    pollWifi(mockNow + 1u);
    require(configStore.wifiSaves == 0 && staPhase == StaPhase::Idle,
            "a late association revived an expired trial");
    snapshot("wifi-expired-late-association");
  }
}

void wifiImmediateFailuresAreVisible() {
  for (int failure = 0; failure < 3; ++failure) {
    reset(); wifiRequest();
    if (failure == 0) WiFi.disconnectOk = false;
    handleWifiConnect();
    response(failure == 0 ? 503 : 200, "wifi-sdk-request");
    if (failure == 1) MockSta::queryError = ESP_ERR_WIFI_CONN;
    if (failure == 2) WiFi.beginOk = false;
    pollWifi(mockNow);
    require(!staDesired && !staConnectRequested() && !staSaveOnSuccess &&
                !wifiError.isEmpty() && configStore.wifiSaves == 0 &&
                configStore.storedPassword == "previous-password",
            "immediate SDK failure was hidden or saved trial credentials");
    require(WiFi.beginCalls == (failure == 2 ? 1 : 0), "SDK error was treated as a down boundary");
    snapshot("wifi-sdk-failure-state");
  }

  reset(); seedVerifiedOldWifi(); WiFi.disconnectOk = false;
  wifiRequest("replacement", "new-password");
  handleWifiConnect(); response(503, "wifi-replacement-disconnect-failure");
  require(staSsid == "previous-network" && staPassword == "previous-password" &&
              staPhase == StaPhase::Verified && staDesired && staCredentialsSaved &&
              !staSaveOnSuccess && WiFi.connection == WL_CONNECTED &&
              configStore.clears == 0 && configStore.wifiSaves == 0,
          "rejected replacement teardown destroyed the verified runtime identity");

  reset(); seedVerifiedOldWifi(); WiFi.disconnectOk = false;
  handleWifiDisconnect(); response(503, "wifi-sdk-disconnect-failure");
  require(WiFi.connection == WL_CONNECTED && MockSta::associated &&
              staPhase == StaPhase::Verified && staSsid == "previous-network" &&
              staPassword == "previous-password" && !staDesired &&
              !staCredentialsSaved && !wifiError.isEmpty() && configStore.clears == 1,
          "failed disconnect must not claim the asynchronous association disappeared");
  snapshot("wifi-disconnect-failure-retains-link");
  require(server.body.value.find("\"connected\":true") != std::string::npos,
          "status hid a verified link after the driver rejected disconnect");

  reset(); seedVerifiedOldWifi(); configStore.clearOk = false;
  handleWifiDisconnect(); response(503, "wifi-clear-failure-after-disconnect");
  require(staPhase == StaPhase::Idle && staSsid.isEmpty() && staPassword.isEmpty() &&
              staCredentialsSaved && configStore.storedSsid == "previous-network",
          "failed persistence clear was reported as unsaved after accepted disconnect");
}

void savedImmediateDisconnectFailureRetriesAcrossWrap() {
  mockNow = UINT32_MAX - 2000u;
  WiFi.disconnectOk = false;
  require(!startStaConnect("saved", "password", false),
          "saved startup disconnect failure was reported as started");
  require(staDesired && staRetryWaiting && staPhase == StaPhase::Idle &&
              staSsid == "saved" && staPassword == "password" &&
              staCredentialsSaved && staRetryDelayMs == 5000u,
          "saved startup disconnect failure discarded retry intent");
  pollWifi(mockNow + 4999u);
  require(WiFi.disconnectCalls == 1 && WiFi.beginCalls == 0,
          "saved startup retry fired before wrapped backoff");
  WiFi.disconnectOk = true;
  mockNow += 5000u;
  pollWifi(mockNow);
  require(WiFi.disconnectCalls == 2 && WiFi.beginCalls == 0 &&
              staPhase == StaPhase::Disconnecting && staRetryDelayMs == 10000u,
          "saved startup retry did not request a fresh down boundary after wrap");
  pollWifi(mockNow);
  require(WiFi.beginCalls == 1 && staPhase == StaPhase::Connecting &&
              WiFi.begunSsid == "saved" && WiFi.begunPassword == "password",
          "saved startup retry did not launch after the proven down boundary");
}

void cachedBeginFailureCannotPoisonRetry() {
  for (bool sdkSucceeds : {false, true}) {
    reset(); WiFi.radioAssociate("old-network");
    WiFi.connection = WL_CONNECT_FAILED;
    startStaConnect("saved", "password", false);
    pollWifi(mockNow);
    require(WiFi.beginCalls == 0 && WiFi.statusRebases == 0,
            "cached failure must not substitute for a driver-proven down boundary");
    WiFi.dropAssociation(); WiFi.beginOk = sdkSucceeds;
    pollWifi(mockNow);
    require(WiFi.beginCalls == 1 && WiFi.statusRebases == 1 &&
                configStore.wifiSaves == 0, "old failure was not rebased after the proven boundary");
    if (!sdkSucceeds) {
      require(staRetryWaiting && !staConnectRequested() && !wifiError.isEmpty(),
              "rebasing the old cache must not hide a new immediate SDK failure");
      const uint32_t retry = mockNow + staRetryDelayMs;
      pollWifi(retry - 1u);
      require(WiFi.beginCalls == 1, "immediate failure bypassed retry backoff");
      mockNow = retry; pollWifi(mockNow);
      WiFi.beginOk = true; pollWifi(mockNow);
      require(WiFi.beginCalls == 2, "saved-credential retry was not retained");
    }
    require(staPhase == StaPhase::Connecting, "cached old failure poisoned a new SDK launch");
    WiFi.associate("saved"); pollWifi(mockNow);
    require(staPhase == StaPhase::Verified && staRetryDelayMs == 5000u,
            "a subsequently verified connection did not reset retry delay");
  }
}

void wifiCalibrationExclusionAcrossPhases() {
  for (bool beginIssued : {false, true}) {
    reset(); wifiRequest(); handleWifiConnect();
    if (beginIssued) pollWifi(mockNow);
    directionRequest(); handleDirectionCalibration(); response(409, "wifi-phase-blocks-direction");
    require(configStore.directionSaves.empty(), "pending disconnect/connect wrote calibration metadata");
    MockController::calibration.active = true;
    WiFi.associate("trial-network");
    const int begins = WiFi.beginCalls, disconnects = WiFi.disconnectCalls, queries = MockSta::infoCalls;
    pollWifi(mockNow + WIFI_CONNECT_TIMEOUT_MS);
    require(WiFi.beginCalls == begins && WiFi.disconnectCalls == disconnects &&
                MockSta::infoCalls == queries && configStore.wifiSaves == 0,
            "active calibration must exclude network progress and WiFi flash writes");
    MockController::calibration.active = false;
    mockNow += WIFI_CONNECT_TIMEOUT_MS; pollWifi(mockNow);
    require(configStore.wifiSaves == 0 && !staDesired, "resuming service extended the trial deadline");
  }
}

void wifiCredentialAllocationFailureIsAtomic() {
  const String longSsid(std::string(32, 's'));
  const String longPassword(std::string(63, 'p'));
  for (bool failSsid : {false, true}) {
    reset(); seedVerifiedOldWifi();
    wifiRequest(longSsid, longPassword);
    MockString::failValue = failSsid ? longSsid.value : longPassword.value;
    // server.arg first copies into the validated handler local; fail the staged copy.
    MockString::matchingCopiesBeforeFailure = 1;
    handleWifiConnect(); response(503, "wifi-copy-allocation-failure");
    require(MockString::failedCopies == 1 && WiFi.beginCalls == 0 && WiFi.disconnectCalls == 0 &&
                configStore.wifiSaves == 0 && configStore.clears == 0,
            "credential-copy failure changed network or persistence");
    require(staSsid == "previous-network" && staPassword == "previous-password" &&
                staPhase == StaPhase::Verified && staDesired && staCredentialsSaved &&
                !staSaveOnSuccess && WiFi.connection == WL_CONNECTED,
            "failed staging partially replaced the current runtime credentials/intent");
    snapshot("wifi-copy-old-link-retained");

    reset(); staSsid = longSsid; staPassword = longPassword;
    MockString::failValue = failSsid ? longSsid.value : longPassword.value;
    MockString::matchingCopiesBeforeFailure = 0;
    require(!startStaConnect(staSsid, staPassword, false),
            "failed startup-style aliased copy was accepted");
    require(MockString::failedCopies == 1 && staSsid == longSsid && staPassword == longPassword &&
                !staDesired && WiFi.disconnectCalls == 0 && WiFi.beginCalls == 0,
            "aliased arguments were mutated before both copies succeeded");
  }
}

void bootCredentialsMoveWithoutAnotherCopy() {
  configStore.storedSsid = std::string(32, 's');
  configStore.storedPassword = std::string(63, 'p');
  MockString::failValue = configStore.storedPassword.value;
  // Permit loadWifi's global copy and startStaConnect's staged copy, but no third copy.
  MockString::matchingCopiesBeforeFailure = 2;
  setup();
  require(MockString::failedCopies == 0 && MockString::matchingCopiesBeforeFailure == 0 &&
              staPhase == StaPhase::Disconnecting && WiFi.beginCalls == 0 &&
              !WiFi.persistentEnabled && !WiFi.autoReconnectEnabled &&
              WiFi.getMode() == WIFI_AP_STA, "boot did not safely stage/move aliased credentials");
  pollWifi(mockNow);
  require(WiFi.begunSsid.value == std::string(32, 's') &&
              WiFi.begunPassword.value == std::string(63, 'p') &&
              !WiFi.beganWithOpenPassword && configStore.wifiSaves == 0,
          "checked password was lost or downgraded to open WiFi at begin");
}

void enabledArgumentAllocationFailureRejects() {
  const std::string hugeEnabled(4096, 'x');
  settingsRequest("25", "30", "20", String(hugeEnabled));
  MockString::failValue = hugeEnabled;
  MockString::matchingCopiesBeforeFailure = 0;
  handleSettings(); response(400, "enabled-allocation-failure");
  require(MockString::failedCopies == 1, "enabled argument copy failure was not injected");
  noMutations();
}
void calibrationLoopServicesStopRequest() {
  setupRoutes();
  MockController::calibration.active = true;
  MockController::calibration.phase = CalibrationPhase::Forward;
  server.clientHook = [] { server.dispatch("/api/control/stop", HTTP_POST); };
  loop();
  require(server.clientCalls == 1, "calibration must still service the HTTP client");
  require(!MockController::calibration.active && !controller.settings().enabled,
          "HTTP stop during calibration must cancel output");
  response(200, "calibration-loop-stop");
}
void debugRouteIsolationAndValidation() {
  setupRoutes();
  if (!PL530_ENABLE_UNSAFE_DEBUG) {
    server.dispatch("/api/debug/pwm", HTTP_GET);
    response(404, "debug-disabled");
    require(!controller.manualWakeHold(), "normal firmware exposed manual PWM");
    return;
  }
  for (const char* endpoint : {"/api/debug/wake", "/api/debug/pwm", "/api/control/set-direction"}) {
    server.headers.erase("X-PL530-Request");
    server.dispatch(endpoint, endpoint == std::string("/api/control/set-direction") ? HTTP_POST : HTTP_GET);
    response(403, "debug-marker-required");
    require(!controller.manualWakeHold() && configStore.directionSaves.empty(),
            "untrusted debug request changed hardware or NVS");
  }
  server.headers["X-PL530-Request"] = "1";
  server.setHost("attacker.example");
  server.dispatch("/api/debug/status", HTTP_GET);
  response(403, "debug-status-host");
  server.setHost("192.168.1.1");
  server.args = {{"state", "1"}};
  server.dispatch("/api/debug/wake", HTTP_GET);
  response(200, "debug-wake");
  server.args = {{"direction", "normal"}};
  server.dispatch("/api/control/set-direction", HTTP_POST);
  response(409, "debug-direction-while-awake");
  require(configStore.directionSaves.empty(), "awake direction override wrote NVS");
  controller.setManualWakeHold(false);
  server.args = {{"direction", "invalid"}};
  server.dispatch("/api/control/set-direction", HTTP_POST);
  response(400, "debug-invalid-direction");
  server.args = {{"direction", "reversed"}};
  configStore.directionSaveOk = false;
  server.dispatch("/api/control/set-direction", HTTP_POST);
  response(503, "debug-direction-save-failure");
}
}

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"strict_decimals", strictDecimals}, {"strict_band", strictBand},
      {"strict_enabled_and_required_fields", strictEnabledAndRequiredFields},
      {"header_required_all_writes", headerRequiredEverywhere},
      {"settings_save_failure_disables", settingsSaveFailure},
      {"enable_latched_conflict", enableLatched},
      {"settings_commit_order_and_fault_race", settingsCommitAndRace},
      {"stop_dirty_without_flash", stopBypassesDirtySettings}, {"reset_contracts", resetContracts},
      {"snapshot_fresh_stale_wrap_json", statusFreshStaleAndWrap},
      {"wifi_invalid_credentials", wifiValidation}, {"wifi_accepted_boundaries", wifiAcceptedBoundaries},
      {"wifi_failed_trial_preserves_credentials", trialFailurePreservesCredentials},
      {"wifi_success_saves_once", trialSuccessSavesOnce},
      {"wifi_save_failure_visible", wifiStorageFailureVisible},
      {"disconnect_tombstone_failure_and_success", disconnectFailureAndSuccess},
      {"saved_wifi_backoff_rollover", retriesAndRollover},
      {"scan_connect_mutual_exclusion", scanConnectExclusion},
      {"scan_actual_string_dedup", scanStringDeduplication},
      {"scan_timeout_throttle_rollover", scanTimeoutThrottleAndWrap},
      {"scan_result_bound_and_driver_failure", scanResultBound},
      {"routes_headers_and_not_found", routesAndNotFound},
      {"startup_fail_closed", startupFailClosed},
      {"real_validation_helper_boundaries", validationHelperBoundaries},
      {"sensor_acquisition_start_not_completion", sensorAcquisitionTimestamp},
      {"sensor_retry_and_rollover", sensorRetryTiming},
      {"timer_create_start_callback_contracts", timerSetupContracts},
      {"direction_explicit_confirmation", directionConfirmation},
      {"direction_busy_conditions", directionBusyConditions},
      {"direction_failed_invalidation_no_start", directionInvalidationFailure},
      {"direction_invalidation_time_and_fault_recheck", directionRechecksAfterInvalidation},
      {"direction_success_persists_then_applies_once_per_start", directionSuccessOnce},
      {"direction_result_save_failure_unknown_off", directionResultSaveFailure},
      {"direction_failed_save_rollback_and_reboot", directionFailedSaveRollback},
      {"direction_runtime_invalidation_refusal", directionRuntimeInvalidationRefusal},
      {"direction_terminal_failure_cancel_unknown_no_save", directionTerminalNoSave},
      {"direction_stop_rejects_stale_complete", directionCancellationStaleComplete},
      {"direction_active_blocks_settings_and_wifi", directionActiveBlocksMutations},
      {"direction_pauses_wifi_retry", directionPausesWifiRetry},
      {"direction_unknown_blocks_enable", directionUnknownSettings},
      {"direction_boot_missing_corrupt_unknown", directionBootMissingCorrupt},
      {"direction_boot_known_restored_without_power", directionBootRestoresWithoutPower},
      {"direction_status_raw_phase_delta_semantics", directionStatusRawSemantics},
      {"direction_apply_failure_and_unarmed_complete", directionApplyFailureAndUnarmedComplete},
      {"rebinding_mutation_proof", rebindingMutationProof},
      {"rebinding_read_proof", rebindingReadProof},
      {"untrusted_hosts_every_handler", untrustedHostsEveryHandler},
      {"origins_every_handler", originsEveryHandler},
      {"legitimate_authorities_and_origins", legitimateAuthoritiesAndOrigins},
      {"active_sta_address_only", activeStaAddressOnly},
      {"pinned_parser_header_boundary", parserHeaderBoundary},
      {"raw_host_nul_and_stale", rawHostNulAndStale},
      {"exact_marker_and_confirmation", exactMarkerAndConfirmation},
      {"percent_encoded_nul_confirmation", encodedNulConfirmation},
      {"raw_origins_and_collection_limits", rawOriginsAndCollectionLimits},
      {"framed_ui_and_captive_hosts", framedUiAndCaptiveHosts},
      {"routed_api_trust_and_no_cors", routedApiTrustAndNoCors},
      {"rejected_scan_preserves_result_and_throttle", rejectedScanPreservesResultAndThrottle},
      {"wifi_old_association_cannot_authorize_trial", oldAssociationCannotAuthorizeTrial},
      {"wifi_new_association_requires_target_and_ip", newAssociationRequiresTargetAndIp},
      {"wifi_disconnect_wait_whole_deadline", disconnectWaitUsesWholeDeadline},
      {"wifi_immediate_sdk_failures_visible", wifiImmediateFailuresAreVisible},
      {"wifi_saved_immediate_disconnect_failure_wrap_retry",
       savedImmediateDisconnectFailureRetriesAcrossWrap},
      {"wifi_cached_begin_failure_not_poison_retry", cachedBeginFailureCannotPoisonRetry},
      {"wifi_calibration_exclusion_across_phases", wifiCalibrationExclusionAcrossPhases},
      {"wifi_credential_copy_failure_atomic", wifiCredentialAllocationFailureIsAtomic},
      {"wifi_boot_aliases_move_without_recopy", bootCredentialsMoveWithoutAnotherCopy},
      {"enabled_argument_copy_failure_rejects", enabledArgumentAllocationFailureRejects},
      {"calibration_loop_services_stop", calibrationLoopServicesStopRequest},
      {"debug_routes_isolation_validation", debugRouteIsolationAndValidation},
  };
  int failed = 0;
  for (const auto& test : tests) {
    try {
      reset(); test.second(); std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception& error) {
      ++failed; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failed << '/' << tests.size() << " integration tests passed\n";
  return failed ? 1 : 0;
}
