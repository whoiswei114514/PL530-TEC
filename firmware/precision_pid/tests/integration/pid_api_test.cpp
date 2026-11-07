#include <iostream>
#include <stdexcept>
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
  MockController::reset();
  mockNow = 1000;
  server = WebServer(80);
  configStore = ConfigStore{};
  controller = ThermalController{};
  tmp117 = Tmp117Sensor{};
  WiFi = MockWifi{};
  controller.setTecDirection(TecDirection::Normal);
  mockEvents.clear();
  staSsid.clear(); staPassword.clear(); wifiError.clear(); configurationError.clear();
  directionError.clear();
  staPhase = StaPhase::Idle;
  staDesired = staSaveOnSuccess = staCredentialsSaved = staRetryWaiting = false;
  staRetryStartedMs = staAttemptStartedMs = 0;
  staRetryDelayMs = 5000;
  wifiScanState = WifiScanState::Idle;
  wifiScanHasRequested = false;
  wifiScanResultJson.clear();
  sensorInitialized = sensorRetryWaiting = i2cReady = haveGoodSensorSample = false;
  lastSensorRetryMs = lastSensorReadMs = lastGoodSensorMs = 0;
  directionSaved = false;
  calibrationResultHandled = true;
  mockTimerCreateResult = mockTimerStartResult = ESP_OK;
  outputGuardTimer = nullptr;
  server.setHost("192.168.1.1");
  server.headers["X-PL530-Request"] = "1";
}
void request() {
  server.args = {{"kp", "24"}, {"ki", "0.3"}, {"kd", "45"},
      {"measurementTau", "0.6"}, {"derivativeTau", "2"},
      {"outputLimit", "30"}, {"slewRate", "15"}, {"integralLimit", "30"}};
}
void response(int code, const char* label) {
  require(server.code == code, "unexpected HTTP code");
  require(server.contentType == "application/json", "not JSON");
  require(server.responseHeaders["Cache-Control"] == "no-store", "response can be cached");
  std::cout << "JSON\t" << label << '\t' << server.body.c_str() << '\n';
}
void unchanged() {
  require(configStore.pidSaves == 0 && MockController::pidSetCalls == 0,
          "rejected request changed PID or flash");
  require(controller.pidSettings().kp == 20, "rejected request changed gain");
}
void routesAndRead() {
  setupRoutes();
  server.dispatch("/api/pid", HTTP_GET);
  response(200, "pid-read");
  require(server.body.value.find("\"hardwareVerified\":false") != std::string::npos,
          "new version claims hardware validation");
  require(server.body.value.find("\"meanC\":null") != std::string::npos,
          "missing samples must be null, not zero or NaN");
  handleStatus();
  response(200, "status-includes-pid");
  require(server.body.value.find("\"pid\":{") != std::string::npos, "status lacks PID");
  request();
  server.dispatch("/api/pid", HTTP_POST);
  response(200, "pid-routed-save");
}
void sourceChecks() {
  for (int condition = 0; condition < 4; ++condition) {
    reset(); setupRoutes(); request();
    if (condition == 0) server.headers.erase("X-PL530-Request");
    if (condition == 1) server.headers["X-PL530-Request"] = "true";
    if (condition == 2) server.setHost("attacker.example");
    if (condition == 3) server.headers["Origin"] = "http://attacker.example";
    server.dispatch("/api/pid", HTTP_POST);
    response(403, "pid-source-rejected");
    unchanged();
  }
  for (const char* header : {"Host", "Origin"}) {
    reset(); setupRoutes();
    server.headers[header] = "http://attacker.example";
    server.dispatch("/api/pid", HTTP_GET);
    response(403, "pid-read-source-rejected");
  }
}
void requiredFields() {
  for (const char* key : {"kp", "ki", "kd", "measurementTau", "derivativeTau",
                          "outputLimit", "slewRate", "integralLimit"}) {
    reset(); request(); server.args.erase(key);
    handlePidSettings(); response(400, "pid-missing-field"); unchanged();
  }
}
void strictDecimals() {
  for (const char* key : {"kp", "ki", "kd", "measurementTau", "derivativeTau",
                          "outputLimit", "slewRate", "integralLimit"}) {
    for (const String& value : std::vector<String>{"NaN", "inf", "1junk", " 1", "1 ",
                                                  "1e0", "", std::string("1\0junk", 6)}) {
      reset(); request(); server.args[key] = value;
      handlePidSettings(); response(400, "pid-invalid-number"); unchanged();
    }
  }
}
void ranges() {
  const std::vector<std::pair<const char*, const char*>> invalid = {
      {"kp", "-1"}, {"kp", "1001"}, {"ki", "-1"}, {"ki", "101"},
      {"kd", "-1"}, {"kd", "10001"}, {"measurementTau", "-1"}, {"measurementTau", "11"},
      {"derivativeTau", "0"}, {"derivativeTau", "31"}, {"outputLimit", "0"},
      {"outputLimit", "101"}, {"slewRate", "0"}, {"slewRate", "101"},
      {"integralLimit", "-1"}, {"integralLimit", "31"}};
  for (const auto& value : invalid) {
    reset(); request(); server.args[value.first] = value.second;
    handlePidSettings(); response(400, "pid-range-rejected"); unchanged();
  }
}
void saveBeforeApply() {
  request(); handlePidSettings(); response(200, "pid-save");
  require(mockEvents == std::vector<std::string>{"save-pid", "apply-pid"},
          "PID changed before durable save");
  require(controller.pidSettings().kp == 24 && configStore.storedPid.kp == 24 &&
              configStore.pidSaves == 1 && !controller.settings().enabled,
          "save did not apply exactly once, or enabled output");
}
void runningRejected() {
  for (int condition = 0; condition < 5; ++condition) {
    reset(); request();
    if (condition == 0) controller.setSettings(25, 30, 20, true);
    if (condition == 1) controller.setManualWakeHold(true);
    if (condition == 2) mutableStatus().driverAwake = true;
    if (condition == 3) MockController::calibration.active = true;
    if (condition == 4) mutableStatus().state = ControllerState::DriverFaultLatched;
    handlePidSettings(); response(409, "pid-busy"); unchanged();
  }
}
void faultPolledBeforeSave() {
  request();
  MockController::pollHook = [] { mutableStatus().state = ControllerState::DriverFaultLatched; };
  handlePidSettings(); response(409, "pid-new-fault"); unchanged();
}
void saveFailure() {
  request(); configStore.pidSaveOk = false;
  handlePidSettings(); response(503, "pid-save-failed");
  require(controller.pidSettings().kp == 20 && configStore.storedPid.kp == 20 &&
              MockController::pidSetCalls == 0 && configStore.pidSaves == 1,
          "failed write changed live settings");
}
void saveFaultRace() {
  request();
  configStore.beforePidSave = [] { mutableStatus().state = ControllerState::DriverFaultLatched; };
  handlePidSettings(); response(409, "pid-save-fault-race");
  require(controller.pidSettings().kp == 20 && configStore.storedPid.kp == 24 &&
              !controller.settings().enabled, "fault during save applied live PID or output");
}
void decisions() {
  auto readDecision = [](const char* expected) {
    setupRoutes(); server.dispatch("/api/pid", HTTP_GET); response(200, expected);
    require(server.body.value.find(std::string("\"decision\":\"") + expected + "\"") !=
                std::string::npos, "wrong PID decision");
  };
  readDecision("disabled");
  controller.setSettings(25, 30, 20, true);
  readDecision("waiting_for_sensor");
  mutableStatus().sensorOk = true;
  auto& pid = const_cast<PrecisionPidResult&>(controller.pidStatus());
  pid.errorC = -0.2; pid.slopeCPerSecond = 0;
  readDecision("correcting_offset");
  pid.saturated = true;
  readDecision("output_limited");
  pid.saturated = false; pid.errorC = 0; pid.slopeCPerSecond = 0.01;
  readDecision("regulating");
  for (uint32_t ms = 1000; ms <= 121000; ms += 200)
    controller.submitTemperature(true, 25, ms);
  readDecision("on_target");
  pid.saturated = true;
  readDecision("output_limited");
}
void startupLoadsPid() {
  configStore.storedPid.kp = 37;
  setup();
  require(configStore.pidLoads == 1 && configStore.pidSaves == 0 &&
              controller.pidSettings().kp == 37 && !controller.settings().enabled,
          "startup did not restore PID without output/flash write");
  reset(); configStore.pidLoadOk = false;
  setup();
  require(controller.pidSettings().kp == 20 && !configurationError.isEmpty() &&
              !controller.settings().enabled, "PID load failure did not use visible defaults");
}
}

int main() {
  const std::vector<std::pair<const char*, void (*)()>> tests = {
      {"routes_and_read", routesAndRead}, {"source_checks", sourceChecks},
      {"required_fields", requiredFields}, {"strict_decimals", strictDecimals},
      {"ranges", ranges}, {"save_before_apply", saveBeforeApply},
      {"running_rejected", runningRejected}, {"fault_polled_before_save", faultPolledBeforeSave},
      {"save_failure", saveFailure}, {"save_fault_race", saveFaultRace},
      {"decisions", decisions}, {"startup_loads_pid", startupLoadsPid}};
  int failed = 0;
  for (const auto& test : tests) {
    try { reset(); test.second(); }
    catch (const std::exception& error) {
      ++failed; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failed << '/' << tests.size() << " PID API tests passed\n";
  return failed ? 1 : 0;
}
