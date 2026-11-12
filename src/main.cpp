#include <Arduino.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <utility>
#include "esp_timer.h"
#include "esp_wifi.h"

#include "tmp117_sensor.h"
#include "config.h"
#include "config_store.h"
#include "request_validation.h"
#include "request_security.h"
#include "thermal_controller.h"
#include "tec_direction.h"
#include "web_ui.h"

namespace {
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000u;
// Arduino-ESP32 derives its asynchronous scan watchdog from this value
// (max_ms_per_chan * 20). The previous 120 ms setting gave the driver only
// about 2.4 s for the whole scan, which is too short in AP+STA mode.
constexpr uint32_t WIFI_SCAN_MAX_MS_PER_CHANNEL = 600u;
constexpr uint32_t WIFI_SCAN_TIMEOUT_MS = 15000u;

WebServer server(80);
DNSServer dnsServer;
ConfigStore configStore;
Tmp117Sensor tmp117;
ThermalController controller;
esp_timer_handle_t outputGuardTimer = nullptr;
bool i2cReady = false;

struct SensorBusPins {
  uint8_t sda;
  uint8_t scl;
};

// The PCB connects module pin 12 to GPIO8 and module pin 17 to GPIO9. Try the
// documented order first, then the swapped order for reversed field wiring.
constexpr SensorBusPins SENSOR_BUS_CANDIDATES[] = {
  {PIN_SDA, PIN_SCL},
  {PIN_SCL, PIN_SDA},
};
SensorBusPins activeSensorBus = SENSOR_BUS_CANDIDATES[0];

String staSsid;
String staPassword;
enum class WifiScanState {
  Idle,
  Scanning,
  Completed,
  Failed
};

enum class StaPhase { Idle, Disconnecting, Connecting, Verified };
StaPhase staPhase = StaPhase::Idle;
bool staDesired = false;
bool staSaveOnSuccess = false;
bool staCredentialsSaved = false;
bool staRetryWaiting = false;
uint32_t staRetryStartedMs = 0;
uint32_t staRetryDelayMs = 5000u;
String wifiError;
String configurationError;
uint32_t staAttemptStartedMs = 0;
WifiScanState wifiScanState = WifiScanState::Idle;
uint32_t wifiScanStartedMs = 0;
uint32_t wifiScanLastRequestMs = 0;
bool wifiScanHasRequested = false;
String wifiScanResultJson;
bool sensorInitialized = false;
uint32_t lastSensorRetryMs = 0;
bool sensorRetryWaiting = false;
uint32_t lastSensorReadMs = 0;
uint32_t lastGoodSensorMs = 0;
bool haveGoodSensorSample = false;
bool directionSaved = false;
bool calibrationResultHandled = true;
String directionError;

String escapeJson(const String& value) {
  String result;
  result.reserve(value.length() + 8);
  for (unsigned int i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    switch (c) {
      case '\"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          char buffer[7];
          snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned int>(static_cast<uint8_t>(c)));
          result += buffer;
        } else {
          result += c;
        }
        break;
    }
  }
  return result;
}

const char* stateKey(ControllerState state) {
  switch (state) {
    case ControllerState::WaitingForSensor:
      return "WaitingForSensor";
    case ControllerState::Disabled:
      return "Disabled";
    case ControllerState::NormalControl:
      return "NormalControl";
    case ControllerState::BalanceControl:
      return "BalanceControl";
    case ControllerState::HardOverTemperature:
      return "HardOverTemperature";
    case ControllerState::HardUnderTemperature:
      return "HardUnderTemperature";
    case ControllerState::ThermalFaultLatched:
      return "ThermalFaultLatched";
    case ControllerState::DriverFaultLatched:
      return "DriverFaultLatched";
    case ControllerState::SensorTimeoutLatched:
      return "SensorTimeoutLatched";
    case ControllerState::SafetyFaultLatched:
      return "SafetyFaultLatched";
    case ControllerState::DirectionCalibration:
      return "DirectionCalibration";
  }
  return "Unknown";
}

bool staConnectRequested() {
  return staPhase == StaPhase::Disconnecting || staPhase == StaPhase::Connecting;
}

bool staAssociationMatches() {
  if (WiFi.status() != WL_CONNECTED || static_cast<uint32_t>(WiFi.localIP()) == 0 ||
      !staSsid.c_str() || staSsid.isEmpty() || staSsid.length() > 32) return false;
  wifi_ap_record_t association = {};
  if (esp_wifi_sta_get_ap_info(&association) != ESP_OK) return false;
  const size_t length = staSsid.length();
  return association.ssid[length] == 0 &&
      memcmp(association.ssid, staSsid.c_str(), length) == 0;
}

void failStaConnect(const char* error, uint32_t now) {
  staPhase = StaPhase::Idle;
  wifiError = error;
  if (staSaveOnSuccess) {
    staDesired = false;
    staSaveOnSuccess = false;
    staPassword.clear();
  } else {
    staRetryWaiting = true;
    staRetryStartedMs = now;
  }
}

void armStaDisconnect(uint32_t now) {
  staRetryWaiting = false;
  staAttemptStartedMs = now;
  staPhase = StaPhase::Disconnecting;
}

bool requestStaDisconnect(uint32_t now) {
  if (!WiFi.disconnect(false, true)) {
    failStaConnect("无法启动 STA 断开，连接未开始", now);
    return false;
  }
  armStaDisconnect(now);
  return true;
}

bool startStaConnect(const String& ssid, const String& password, bool saveOnSuccess) {
  if (!RequestValidation::wifi(ssid.c_str(), ssid.length(), password.c_str(), password.length())) {
    wifiError = "WiFi 凭据无效，连接未开始";
    return false;
  }
  // Stage both copies before touching globals: setup passes those globals by reference.
  String nextSsid(ssid);
  String nextPassword(password);
  if (!nextSsid.c_str() || !nextPassword.c_str() ||
      nextSsid.length() != ssid.length() || nextPassword.length() != password.length() ||
      memcmp(nextSsid.c_str(), ssid.c_str(), ssid.length()) != 0 ||
      memcmp(nextPassword.c_str(), password.c_str(), password.length()) != 0) {
    wifiError = "WiFi 凭据复制失败，连接未开始";
    return false;
  }
  const uint32_t now = millis();
  if (!WiFi.disconnect(false, true)) {
    wifiError = "无法启动 STA 断开，连接未开始";
    if (!saveOnSuccess) {
      // Saved credentials must keep retrying; a failed replacement trial must
      // leave the currently verified runtime identity untouched.
      staSsid = std::move(nextSsid);
      staPassword = std::move(nextPassword);
      staDesired = true;
      staSaveOnSuccess = false;
      staCredentialsSaved = true;
      staPhase = StaPhase::Idle;
      staRetryWaiting = true;
      staRetryStartedMs = now;
      staRetryDelayMs = 5000u;
    }
    return false;
  }
  // Pinned Arduino String move assignment transfers storage without allocating.
  staSsid = std::move(nextSsid);
  staPassword = std::move(nextPassword);
  staDesired = true;
  staSaveOnSuccess = saveOnSuccess;
  staCredentialsSaved = !saveOnSuccess;
  staRetryWaiting = false;
  staRetryDelayMs = 5000u;
  wifiError.clear();
  armStaDisconnect(now);
  return true;
}

String statusJson() {
  const ControllerStatus& status = controller.status();
  const ControllerSettings& settings = controller.settings();
  const bool wifiConnected = staPhase == StaPhase::Verified && staAssociationMatches();
  const uint32_t sampleAge = millis() - lastGoodSensorMs;
  const bool freshSensor = status.sensorOk && haveGoodSensorSample &&
      sampleAge < SENSOR_STALE_TIMEOUT_MS;
  String wifiMode = "AP";
  if (WiFi.getMode() == WIFI_AP_STA) {
    wifiMode = "AP+STA";
  }

  auto appendTemperature = [](String& json, float temperatureC) {
    if (!isfinite(temperatureC)) {
      json += "null";
    } else {
      json += String(temperatureC, 2);
    }
  };

  String json;
  json += "{\"device\":\"";
  json += DEVICE_NAME;
  json += "\",";
  json += "\"hardware\":\"";
  json += HARDWARE_REVISION;
  json += "\",\"sensor\":\"TMP117\",\"sensorAddress\":";
  json += String(TMP117_ADDRESS);
  json += ",\"sensorBus\":{\"sda\":";
  json += String(activeSensorBus.sda);
  json += ",\"scl\":";
  json += String(activeSensorBus.scl);
  json += "},\"pwmFrequencyHz\":";
  json += String(TEC_PWM_FREQUENCY);
  json += ",\"currentSenseAvailable\":false,\"tecCurrentA\":null,";
  json += "\"configurationError\":\"";
  json += escapeJson(configurationError);
  json += "\",\"sampleAgeMs\":";
  json += haveGoodSensorSample ? String(sampleAge) : String("null");
  json += ",";
  json += "\"temperatureC\":";
  appendTemperature(json, freshSensor ? status.filteredTemperatureC : NAN);
  json += ",\"rawTemperatureC\":";
  appendTemperature(json, freshSensor ? status.rawTemperatureC : NAN);
  json += ",\"sensorOk\":";
  json += freshSensor ? "true" : "false";
  json += ",\"driverAwake\":";
  json += status.driverAwake ? "true" : "false";
  json += ",\"driverFaultLatched\":";
  json += status.state == ControllerState::DriverFaultLatched ? "true" : "false";
  json += ",\"state\":";
  json += String(static_cast<uint8_t>(status.state));
  json += ",\"stateKey\":\"";
  json += stateKey(status.state);
  json += "\",\"stateText\":\"";
  json += escapeJson(controllerStateName(status.state));
  json += "\",\"sensorError\":\"";
  json += escapeJson(haveGoodSensorSample && sampleAge >= SENSOR_STALE_TIMEOUT_MS
                         ? "温度数据已过期" : tmp117.lastError());
  json += "\",\"signedDutyPercent\":";
  json += String(status.signedDutyPercent, 1);
  json += ",\"coolingDutyPercent\":";
  json += String(status.coolingDutyPercent, 1);
  json += ",\"heatingDutyPercent\":";
  json += String(status.heatingDutyPercent, 1);
  json += ",\"slopeCPerSecond\":";
  json += String(status.slopeCPerSecond, 3);
  json += ",\"balanceDrivePercent\":";
  json += String(status.balanceDrivePercent, 1);
  json += ",\"settings\":{\"targetC\":";
  json += String(settings.targetC, 2);
  json += ",\"pwmHighC\":";
  json += String(settings.pwmHighC, 2);
  json += ",\"pwmLowC\":";
  json += String(settings.pwmLowC, 2);
  json += ",\"enabled\":";
  json += settings.enabled ? "true" : "false";
  json += "},\"hardLimits\":{\"minimumC\":";
  json += String(HARD_MIN_TEMPERATURE_C, 1);
  json += ",\"maximumC\":";
  json += String(HARD_MAX_TEMPERATURE_C, 1);
  const auto calibration = controller.directionCalibrationStatus();
  json += "},\"direction\":{\"value\":\"";
  json += tecDirectionKey(controller.tecDirection());
  json += "\",\"saved\":";
  json += directionSaved ? "true" : "false";
  json += ",\"error\":\"";
  json += escapeJson(directionError);
  json += "\",\"calibration\":{\"phase\":\"";
  json += calibrationPhaseKey(calibration.phase);
  json += "\",\"active\":";
  json += calibration.active ? "true" : "false";
  json += ",\"phaseElapsedMs\":";
  json += String(calibration.phaseElapsedMs);
  json += ",\"pulseMs\":";
  json += String(controller.calibrationPulseMs());
  json += ",\"dutyLimitPercent\":";
  json += String(TEC_CAL_OUTPUT_LIMIT_PERCENT);
  json += ",\"rawDutyPercent\":";
  json += String(calibration.rawDutyPercent, 1);
  json += ",\"forwardDeltaC\":";
  appendTemperature(json, calibration.forwardDeltaC);
  json += ",\"reverseDeltaC\":";
  appendTemperature(json, calibration.reverseDeltaC);
  json += ",\"candidate\":\"";
  json += tecDirectionKey(calibration.candidate);
  json += "\",\"error\":\"";
  json += escapeJson(calibration.error);
  json += "\"}},\"wifi\":{\"mode\":\"";
  json += wifiMode;
  json += "\",\"connected\":";
  json += wifiConnected ? "true" : "false";
  json += ",\"connecting\":";
  json += staConnectRequested() ? "true" : "false";
  json += ",\"retrying\":";
  json += staRetryWaiting ? "true" : "false";
  json += ",\"saved\":";
  json += staCredentialsSaved ? "true" : "false";
  json += ",\"error\":\"";
  json += escapeJson(wifiError);
  json += "\"";
  json += ",\"ssid\":\"";
  json += escapeJson(staSsid);
  json += "\",\"ip\":\"";
  json += wifiConnected ? WiFi.localIP().toString() : String("");
  json += "\",\"rssi\":";
  json += String(wifiConnected ? WiFi.RSSI() : 0);
  json += "}}";
  return json;
}

void sendJson(const String& body, int statusCode = 200) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(statusCode, "application/json", body);
}

String requestAddress() {
  const String host = server.hostHeader();
  const String collectedHost = server.header("Host");
  // In Arduino 2.0.17 hostHeader() can be stale if Host is missing, whereas
  // collected fields reset on each request. It also preserves NULs that the
  // collected C-string copy loses. Require both views to match exactly.
  if (collectedHost.isEmpty() || !RequestSecurity::exact(host, collectedHost)) return "";
  if (RequestSecurity::authority(host, "192.168.1.1")) return "192.168.1.1";
  if (WiFi.status() == WL_CONNECTED) {
    const String address = WiFi.localIP().toString();
    if (address != "0.0.0.0" && RequestSecurity::authority(host, address)) return address;
  }
  return "";
}

bool requestAllowed() {
  const String address = requestAddress();
  const String origin = server.header("Origin");
  // Origin is optional for local non-browser clients and same-origin GETs.
  if (address.isEmpty() || (!origin.isEmpty() && !RequestSecurity::origin(origin, address))) {
    sendJson("{\"ok\":false,\"error\":\"请求来源校验失败，请刷新控制页面\"}", 403);
    return false;
  }
  return true;
}

void handleStatus() {
  if (!requestAllowed()) return;
  controller.checkSampleAgeFromTimer(millis());
  controller.pollDriverFault();
  sendJson(statusJson());
}

bool writeRequestAllowed() {
  if (!requestAllowed()) return false;
  // Keep the form/preflight guard as well as the DNS-rebinding host boundary.
  if (!RequestSecurity::exact(server.header("X-PL530-Request"), "1")) {
    sendJson("{\"ok\":false,\"error\":\"请求来源校验失败，请刷新控制页面\"}", 403);
    return false;
  }
  return true;
}

bool latchedState(ControllerState state) {
  return state == ControllerState::ThermalFaultLatched ||
      state == ControllerState::DriverFaultLatched ||
      state == ControllerState::SensorTimeoutLatched ||
      state == ControllerState::SafetyFaultLatched;
}

void handleSettings() {
  if (!writeRequestAllowed()) return;
  if (controller.directionCalibrationStatus().active) {
    sendJson("{\"ok\":false,\"error\":\"方向检测中，请先停止检测再修改温控\"}", 409);
    return;
  }
  if (!server.hasArg("target") || !server.hasArg("high") || !server.hasArg("low")) {
    sendJson("{\"ok\":false,\"error\":\"缺少控制参数\"}", 400);
    return;
  }

  float target = 0, high = 0, low = 0;
  const String targetValue = server.arg("target");
  const String highValue = server.arg("high");
  const String lowValue = server.arg("low");
  const String enabledValue = server.arg("enabled");
  bool enabled = false;

  if (!RequestValidation::decimal(targetValue.c_str(), targetValue.length(), target) ||
      !RequestValidation::decimal(highValue.c_str(), highValue.length(), high) ||
      !RequestValidation::decimal(lowValue.c_str(), lowValue.length(), low) ||
      !enabledValue.c_str() || enabledValue.length() != strlen(enabledValue.c_str()) ||
      !RequestValidation::enabled(enabledValue.c_str(), enabled) ||
      !RequestValidation::temperatures(target, high, low)) {
    sendJson("{\"ok\":false,\"error\":\"温度参数无效\"}", 400);
    return;
  }

  controller.pollDriverFault();
  if (enabled && latchedState(controller.status().state)) {
    sendJson("{\"ok\":false,\"error\":\"故障仍锁定，不能启用 TEC\"}", 409);
    return;
  }
  if (enabled && controller.tecDirection() == TecDirection::Unknown) {
    sendJson("{\"ok\":false,\"error\":\"TEC 方向未确认，请先完成方向检测\"}", 409);
    return;
  }
  if (!enabled) {
    const auto old = controller.settings();
    controller.setSettings(old.targetC, old.pwmHighC, old.pwmLowC, false);
  }
  ControllerSettings requested;
  requested.targetC = target;
  requested.pwmHighC = high;
  requested.pwmLowC = low;
  requested.enabled = enabled;
  if (!configStore.saveController(requested)) {
    const auto old = controller.settings();
    controller.setSettings(old.targetC, old.pwmHighC, old.pwmLowC, false);
    configurationError = "参数保存失败，TEC 已停用";
    sendJson("{\"ok\":false,\"error\":\"参数保存失败，TEC 已停用\"}", 503);
    return;
  }
  configurationError.clear();
  controller.checkSampleAgeFromTimer(millis());
  controller.pollDriverFault();
  controller.setSettings(requested);
  if (enabled && !controller.settings().enabled) {
    sendJson("{\"ok\":false,\"error\":\"参数已保存，但保护已触发，TEC 未启用\"}", 409);
    return;
  }
  sendJson("{\"ok\":true,\"enabled\":" + String(controller.settings().enabled ? "true" : "false") + "}");
}

void handleStop() {
  if (!writeRequestAllowed()) return;
  calibrationResultHandled = true;
  controller.cancelDirectionCalibration();
  const auto settings = controller.settings();
  controller.setSettings(settings.targetC, settings.pwmHighC, settings.pwmLowC, false);
  controller.pollDriverFault();
  sendJson("{\"ok\":true,\"enabled\":false}");
}

struct ControlCommandResult {
  int status;
  const char* error;
};

ControlCommandResult beginDirectionCalibration() {
  if (wifiScanState == WifiScanState::Scanning || staConnectRequested() ||
      !controller.canStartDirectionCalibration()) {
    return {409, "请先停用 TEC，保持温度 15~45°C、传感器正常且无故障，并等待配网完成"};
  }
  // Invalidate the previous wiring result durably before the first test pulse.
  const bool invalidated = configStore.saveDirection(TecDirection::Unknown);
  const bool runtimeInvalidated = controller.setTecDirection(TecDirection::Unknown);
  directionSaved = false;
  calibrationResultHandled = true;
  if (!invalidated) {
    directionError = "旧方向清除失败，检测未启动；重启可能恢复旧记录，请先重试检测";
    return {503, "旧方向清除失败，检测未启动"};
  }
  if (!runtimeInvalidated) {
    directionError = "运行状态已变化，检测未启动；请先停止并检查故障";
    return {409, "无法清除运行中的方向，检测未启动"};
  }
  if (!controller.startDirectionCalibration()) {
    directionError = "检测未启动，请等待新的有效温度再重试";
    return {409, "检测条件已变化，方向保持未确认，请重试"};
  }
  directionError.clear();
  calibrationResultHandled = false;
  return {200, nullptr};
}

void handleDirectionCalibration() {
  if (!writeRequestAllowed()) return;
  if (!RequestSecurity::exact(server.arg("confirm"), "full_power")) {
    sendJson("{\"ok\":false,\"error\":\"请确认散热器、探头安装及双向全功率测试\"}", 400);
    return;
  }
  const auto result = beginDirectionCalibration();
  if (result.status != 200) {
    sendJson("{\"ok\":false,\"error\":\"" + escapeJson(result.error) + "\"}", result.status);
    return;
  }
  sendJson("{\"ok\":true,\"started\":true}");
}

#if PL530_ENABLE_UNSAFE_DEBUG
ControlCommandResult applyManualDirection(TecDirection direction) {
  controller.pollDriverFault(false);
  if (controller.settings().enabled || controller.manualWakeHold() ||
      controller.status().driverAwake || controller.directionCalibrationStatus().active ||
      latchedState(controller.status().state)) {
    return {409, "请先停止输出、退出强制唤醒并检查故障"};
  }
  if (!configStore.saveDirection(direction)) {
    controller.setTecDirection(TecDirection::Unknown);
    directionSaved = false;
    directionError = "手动方向保存失败，重启可能恢复旧记录";
    return {503, "方向保存失败，运行方向保持未知"};
  }
  directionSaved = direction != TecDirection::Unknown;
  if (!controller.setTecDirection(direction)) {
    directionError = "方向已保存但运行应用失败，请检查后重新确认";
    return {409, "运行状态已变化，方向未应用"};
  }
  directionError.clear();
  return {200, nullptr};
}
#endif

void serviceDirectionCalibration() {
  const auto result = controller.directionCalibrationStatus();
  if (result.active) {
    static uint32_t lastCalPrintMs = 0;
    if (millis() - lastCalPrintMs >= 1000u) {
      lastCalPrintMs = millis();
      const uint32_t targetMs =
          (result.phase == CalibrationPhase::Forward || result.phase == CalibrationPhase::Reverse)
              ? controller.calibrationPulseMs()
              : (result.phase == CalibrationPhase::Settle ? controller.calibrationSettleMs() : 2000u);
      Serial.printf("[CALIBRATION] Phase=%s Elapsed=%lu/%lu ms Temp=%.2f C IN1=%d IN2=%d\n",
                    calibrationPhaseKey(result.phase),
                    static_cast<unsigned long>(result.phaseElapsedMs),
                    static_cast<unsigned long>(targetMs),
                    controller.status().rawTemperatureC,
                    controller.debugIn1Duty(), controller.debugIn2Duty());
    }
    return;
  }
  if (calibrationResultHandled) return;
  calibrationResultHandled = true;
  if (result.phase != CalibrationPhase::Complete || result.candidate == TecDirection::Unknown) return;
  if (!configStore.saveDirection(result.candidate)) {
    // A failed readback does not prove the preceding commit was rolled back.
    const bool cleared = configStore.saveDirection(TecDirection::Unknown);
    directionError = cleared
        ? "方向保存失败，记录已清除；TEC 保持停用，请重新检测"
        : "方向保存及清除校验失败；TEC 保持停用，重启可能恢复该记录，请先重试检测";
    return;
  }
  if (!controller.setTecDirection(result.candidate)) {
    directionError = "方向已保存，但未能应用；TEC 保持停用，请检查故障";
    return;
  }
  directionSaved = true;
  directionError.clear();
  Serial.printf("[CALIBRATION] SUCCESS! Direction detected as %s, saved to NVS and applied!\n",
                tecDirectionKey(result.candidate));
}

void pollWifiScan(uint32_t now) {
  if (wifiScanState != WifiScanState::Scanning) {
    return;
  }

  const int scanState = WiFi.scanComplete();
  if (scanState == WIFI_SCAN_RUNNING) {
    if (now - wifiScanStartedMs >= WIFI_SCAN_TIMEOUT_MS) {
      esp_wifi_scan_stop();
      WiFi.scanDelete();
      wifiScanState = WifiScanState::Failed;
      Serial.printf("WiFi scan timeout after %lu ms\n",
                    static_cast<unsigned long>(now - wifiScanStartedMs));
    }
    return;
  }

  if (scanState >= 0) {
    String json = "{\"ok\":true,\"scanning\":false,\"networks\":[";
    String seen[20];
    size_t seenCount = 0;
    bool first = true;
    for (int i = 0; i < scanState; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) {
        continue;
      }
      bool duplicate = false;
      for (size_t j = 0; j < seenCount; ++j) duplicate |= seen[j] == ssid;
      if (duplicate) continue;
      seen[seenCount++] = ssid;
      if (!first) {
        json += ',';
      }
      first = false;
      json += "{\"ssid\":\"";
      json += escapeJson(ssid);
      json += "\",\"rssi\":";
      json += WiFi.RSSI(i);
      json += "}";
      if (seenCount == 20 || json.length() > 4000) {
        break;
      }
    }
    json += "]}";
    WiFi.scanDelete();
    wifiScanResultJson = json;
    wifiScanState = WifiScanState::Completed;
    Serial.printf("WiFi scan complete: %d APs, %u unique SSIDs\n", scanState,
                  static_cast<unsigned int>(seenCount));
  } else {
    WiFi.scanDelete();
    wifiScanState = WifiScanState::Failed;
    Serial.printf("WiFi scan failed: state=%d, elapsed=%lu ms\n", scanState,
                  static_cast<unsigned long>(now - wifiScanStartedMs));
  }
}

void handleWifiScan() {
  if (!writeRequestAllowed()) return;
  if (controller.directionCalibrationStatus().active) {
    sendJson("{\"ok\":false,\"error\":\"方向检测中，请稍后配网\"}", 409);
    return;
  }
  if (staConnectRequested()) {
    sendJson("{\"ok\":false,\"error\":\"正在连接 WiFi，请稍后扫描\"}", 409);
    return;
  }
  if (wifiScanState == WifiScanState::Scanning) {
    sendJson("{\"ok\":true,\"scanning\":true,\"networks\":[]}");
    return;
  }
  if (wifiScanState == WifiScanState::Completed) {
    sendJson(wifiScanResultJson);
    wifiScanResultJson = "";
    wifiScanState = WifiScanState::Idle;
    return;
  }
  if (wifiScanState == WifiScanState::Failed) {
    sendJson("{\"ok\":false,\"scanning\":false,\"networks\":[],\"error\":\"WiFi 扫描失败或超时\"}", 500);
    wifiScanState = WifiScanState::Idle;
    return;
  }

  const uint32_t now = millis();
  if (wifiScanHasRequested && now - wifiScanLastRequestMs < 4000) {
    sendJson("{\"ok\":false,\"scanning\":false,\"networks\":[],\"error\":\"扫描过于频繁\"}", 429);
    return;
  }

  wifiScanHasRequested = true;
  wifiScanLastRequestMs = now;
  Serial.printf("WiFi scan start: max %lu ms/channel\n",
                static_cast<unsigned long>(WIFI_SCAN_MAX_MS_PER_CHANNEL));
  const int started = WiFi.scanNetworks(true, true, false, WIFI_SCAN_MAX_MS_PER_CHANNEL);
  if (started == WIFI_SCAN_RUNNING || started >= 0) {
    wifiScanStartedMs = now;
    wifiScanState = WifiScanState::Scanning;
    sendJson("{\"ok\":true,\"scanning\":true,\"networks\":[]}");
  } else {
    WiFi.scanDelete();
    wifiScanLastRequestMs = now;
    Serial.printf("WiFi scan start failed: state=%d\n", started);
    sendJson("{\"ok\":false,\"scanning\":false,\"networks\":[],\"error\":\"无法启动 WiFi 扫描\"}", 500);
  }
}

void handleWifiConnect() {
  if (!writeRequestAllowed()) return;
  if (controller.directionCalibrationStatus().active) {
    sendJson("{\"ok\":false,\"error\":\"方向检测中，请稍后配网\"}", 409);
    return;
  }
  if (wifiScanState == WifiScanState::Scanning) {
    sendJson("{\"ok\":false,\"error\":\"正在扫描 WiFi，请稍后连接\"}", 409);
    return;
  }
  if (!server.hasArg("ssid")) {
    sendJson("{\"ok\":false,\"error\":\"缺少 SSID\"}", 400);
    return;
  }
  const String ssid = server.arg("ssid");
  const String password = server.arg("password");
  if (!RequestValidation::wifi(ssid.c_str(), ssid.length(), password.c_str(), password.length())) {
    sendJson("{\"ok\":false,\"error\":\"SSID 或密码格式无效\"}", 400);
    return;
  }
  if (!startStaConnect(ssid, password, true)) {
    sendJson("{\"ok\":false,\"error\":\"" + escapeJson(wifiError) + "\"}", 503);
    return;
  }
  sendJson("{\"ok\":true}");
}

void handleWifiDisconnect() {
  if (!writeRequestAllowed()) return;
  if (controller.directionCalibrationStatus().active) {
    sendJson("{\"ok\":false,\"error\":\"方向检测中，请先停止检测再断开网络\"}", 409);
    return;
  }
  staDesired = false;
  staSaveOnSuccess = false;
  staRetryWaiting = false;
  if (wifiScanState == WifiScanState::Scanning) esp_wifi_scan_stop();
  WiFi.scanDelete();
  wifiScanState = WifiScanState::Idle;
  const bool disconnected = WiFi.disconnect(false, true);
  const bool credentialsCleared = configStore.clearWifi();
  if (credentialsCleared) staCredentialsSaved = false;
  if (disconnected) {
    staPhase = StaPhase::Idle;
    staSsid.clear();
    staPassword.clear();
  }
  if (!credentialsCleared) {
    wifiError = disconnected ? "已请求断开，但凭据清除失败，重启后可能重新连接"
                             : "STA 断开和凭据清除均失败，请检查网络状态";
    sendJson("{\"ok\":false,\"error\":\"" + escapeJson(wifiError) + "\"}", 503);
    return;
  }
  if (!disconnected) {
    wifiError = "凭据已清除，但 STA 断开请求失败，请检查网络状态";
    sendJson("{\"ok\":false,\"error\":\"凭据已清除，但 STA 断开请求失败\"}", 503);
    return;
  }
  wifiError.clear();
  sendJson("{\"ok\":true}");
}

void handleResetFault() {
  if (!writeRequestAllowed()) return;
  controller.pollDriverFault();
  if (!latchedState(controller.status().state)) {
    sendJson("{\"ok\":false,\"error\":\"当前没有可解除的故障锁定\"}", 409);
    return;
  }
  controller.resetFault();
  if (controller.status().state == ControllerState::SafetyFaultLatched) {
    sendJson("{\"ok\":false,\"error\":\"输出保护未就绪，请检查后重启\"}", 409);
    return;
  }
  sendJson("{\"ok\":true}");
}

void setupRoutes() {
  const char* headers[] = {"X-PL530-Request", "Host", "Origin"};
  server.collectHeaders(headers, 3);
  server.on("/", HTTP_GET, []() {
    if (requestAddress().isEmpty()) {
      server.sendHeader("Location", "http://192.168.1.1/", true);
      server.send(302, "text/plain", "");
      return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.sendHeader("X-Content-Type-Options", "nosniff");
    server.sendHeader("X-Frame-Options", "DENY");
    server.sendHeader("Content-Security-Policy", "frame-ancestors 'none'");
    server.send_P(200, "text/html", WEB_UI_PAGE);
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/disconnect", HTTP_POST, handleWifiDisconnect);
  server.on("/api/control/reset-fault", HTTP_POST, handleResetFault);
  server.on("/api/control/stop", HTTP_POST, handleStop);
  server.on("/api/control/detect-direction", HTTP_POST, handleDirectionCalibration);
#if PL530_ENABLE_UNSAFE_DEBUG
  server.on("/api/control/set-direction", HTTP_POST, []() {
    if (!writeRequestAllowed()) return;
    const String dir = server.arg("direction");
    TecDirection d = TecDirection::Unknown;
    if (RequestSecurity::exact(dir, "reversed")) d = TecDirection::Reversed;
    else if (RequestSecurity::exact(dir, "normal")) d = TecDirection::Normal;
    else if (!RequestSecurity::exact(dir, "unknown")) {
      sendJson("{\"ok\":false,\"error\":\"方向参数无效\"}", 400);
      return;
    }
    const auto result = applyManualDirection(d);
    if (result.status != 200) {
      sendJson("{\"ok\":false,\"error\":\"" + escapeJson(result.error) + "\"}", result.status);
      return;
    }
    sendJson("{\"ok\":true,\"direction\":\"" + String(tecDirectionKey(d)) + "\"}");
  });
  server.on("/api/debug/wake", HTTP_GET, []() {
    if (!writeRequestAllowed()) return;
    const String state = server.arg("state");
    if (!RequestSecurity::exact(state, "0") && !RequestSecurity::exact(state, "1")) {
      sendJson("{\"ok\":false,\"error\":\"state 必须为 0 或 1\"}", 400);
      return;
    }
    const bool wake = RequestSecurity::exact(state, "1");
    controller.setManualWakeHold(wake);
    const int sleepLevel = digitalRead(PIN_DRV_SLEEP);
    const int faultLevel = digitalRead(PIN_DRV_FAULT);
    sendJson("{\"ok\":true,\"manualWakeHold\":" + String(controller.manualWakeHold() ? "true" : "false") +
             ",\"nSLEEP\":" + String(sleepLevel) + ",\"nFAULT\":" + String(faultLevel) + "}");
  });

  server.on("/api/debug/pwm", HTTP_GET, []() {
    if (!writeRequestAllowed()) return;
    const String in1Value = server.arg("in1");
    const String in2Value = server.arg("in2");
    uint32_t in1 = 0, in2 = 0;
    if ((!server.hasArg("in1") && !server.hasArg("in2")) ||
        (server.hasArg("in1") && !RequestValidation::unsignedInteger(
            in1Value.c_str(), in1Value.length(), TEC_PWM_MAX_COUNT, in1)) ||
        (server.hasArg("in2") && !RequestValidation::unsignedInteger(
            in2Value.c_str(), in2Value.length(), TEC_PWM_MAX_COUNT, in2))) {
      sendJson("{\"ok\":false,\"error\":\"PWM 计数必须为范围内的非负整数\"}", 400);
      return;
    }
    if (!controller.manualWakeHold()) {
      controller.setManualWakeHold(true);
    }
    controller.setDebugPwm(in1, in2);
    sendJson("{\"ok\":true,\"in1\":" + String(controller.debugIn1Duty()) +
             ",\"in2\":" + String(controller.debugIn2Duty()) +
             ",\"nSLEEP\":" + String(digitalRead(PIN_DRV_SLEEP)) +
             ",\"nFAULT\":" + String(digitalRead(PIN_DRV_FAULT)) + "}");
  });

  server.on("/api/debug/status", HTTP_GET, []() {
    if (!requestAllowed()) return;
    String json = "{";
    json += "\"ok\":true,";
    json += "\"manualWakeHold\":" + String(controller.manualWakeHold() ? "true" : "false") + ",";
    json += "\"nSLEEP\":" + String(digitalRead(PIN_DRV_SLEEP)) + ",";
    json += "\"nFAULT\":" + String(digitalRead(PIN_DRV_FAULT)) + ",";
    json += "\"in1\":" + String(controller.debugIn1Duty()) + ",";
    json += "\"in2\":" + String(controller.debugIn2Duty()) + ",";
    json += "\"temperature\":" + (isfinite(controller.status().rawTemperatureC) ? String(controller.status().rawTemperatureC, 2) : "null") + ",";
    json += "\"state\":\"" + String(controllerStateName(controller.status().state)) + "\"";
    json += "}";
    sendJson(json);
  });
#endif

  server.onNotFound([]() {
    if (server.uri() == "/api" || server.uri().startsWith("/api/")) {
      if (!requestAllowed()) return;
      sendJson("{\"ok\":false,\"error\":\"未找到接口\"}", 404);
      return;
    }
    server.sendHeader("Location", "http://192.168.1.1/", true);
    server.send(302, "text/plain", "");
  });
}

void setupAp() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(apIp(), apGateway(), apSubnet());
  const bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, 6, false, 4);
  Serial.printf("AP %s: %s\n", apStarted ? "已启动" : "启动失败", AP_SSID);
  dnsServer.start(53, "*", apIp());
}

void stopSensorBus() {
  // The active implementation is software I2C; always release its pins even
  // when probing failed before i2cReady was set.
  tmp117.end();
  i2cReady = false;
}

bool startSensorBus(const SensorBusPins& pins) {
  stopSensorBus();
  activeSensorBus = pins;
  Serial.printf("TMP117 probe: SDA=GPIO%u, SCL=GPIO%u\n",
                static_cast<unsigned int>(pins.sda), static_cast<unsigned int>(pins.scl));
  if (!tmp117.begin(pins.sda, pins.scl)) {
    Serial.printf("TMP117 probe failed: %s\n", tmp117.lastError());
    stopSensorBus();
    return false;
  }
  i2cReady = true;

  Serial.printf("TMP117 detected: addr=0x%02X, SDA=GPIO%u, SCL=GPIO%u\n",
                TMP117_ADDRESS, static_cast<unsigned int>(pins.sda),
                static_cast<unsigned int>(pins.scl));
  return true;
}

bool initializeSensor() {
  for (const SensorBusPins& candidate : SENSOR_BUS_CANDIDATES) {
    if (startSensorBus(candidate)) return true;
  }
  stopSensorBus();
  Serial.println("TMP117 not found on GPIO8/GPIO9 in either order");
  return false;
}

void pollSensor(uint32_t now) {
  if (!sensorInitialized) {
    if (sensorRetryWaiting && now - lastSensorRetryMs < SENSOR_RETRY_INTERVAL_MS) {
      return;
    }
    lastSensorRetryMs = now;
    sensorRetryWaiting = true;
    sensorInitialized = initializeSensor();
    if (!sensorInitialized) {
      controller.submitTemperature(false, NAN, millis());
      return;
    }
    sensorRetryWaiting = false;
    now = millis();
  }

  if (sensorInitialized && now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    float temperatureC = NAN;
    const uint32_t sampleStartedMs = millis();
    const bool valid = tmp117.readTemperature(temperatureC);
    lastSensorReadMs = millis();
    if (valid) {
      haveGoodSensorSample = true;
      lastGoodSensorMs = sampleStartedMs;
      controller.submitTemperature(true, temperatureC, sampleStartedMs);
    } else {
      sensorInitialized = false;
      sensorRetryWaiting = true;
      lastSensorRetryMs = lastSensorReadMs;
      stopSensorBus();
      controller.submitTemperature(false, NAN, lastSensorReadMs);
    }
  }
}

void pollWifi(uint32_t now) {
  if (!staDesired || wifiScanState == WifiScanState::Scanning ||
      controller.directionCalibrationStatus().active) {
    return;
  }
  // One deadline covers old-association teardown and the entire new attempt.
  if (staConnectRequested() && now - staAttemptStartedMs >= WIFI_CONNECT_TIMEOUT_MS) {
    const bool cancelled = WiFi.disconnect(false, true);
    failStaConnect(cancelled ? "WiFi 连接超时，请检查 SSID 和密码"
                             : "WiFi 连接超时，且 STA 断开请求失败", now);
    return;
  }
  if (staPhase == StaPhase::Disconnecting) {
    wifi_ap_record_t association = {};
    const esp_err_t result = esp_wifi_sta_get_ap_info(&association);
    if (result == ESP_ERR_WIFI_NOT_CONNECT && WiFi.status() != WL_CONNECTED) {
      // Do not let an old same-SSID connection authorize a new password.
      staPhase = StaPhase::Connecting;
      // begin() returns cached status on success; discard only a proven-down old failure.
      if (WiFi.status() == WL_CONNECT_FAILED) WiFi._setStatus(WL_DISCONNECTED);
      if (WiFi.begin(staSsid.c_str(), staPassword.c_str(), 0, nullptr, true) == WL_CONNECT_FAILED) {
        const bool cancelled = WiFi.disconnect(false, true);
        failStaConnect(cancelled ? "WiFi 连接启动失败"
                                 : "WiFi 连接启动失败，且 STA 断开请求失败", now);
      }
    } else if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_CONNECT) {
      const bool cancelled = WiFi.disconnect(false, true);
      failStaConnect(cancelled ? "无法确认 STA 关联状态，连接已取消"
                               : "无法确认 STA 关联状态，且断开请求失败", now);
    }
    return;
  }
  if ((staPhase == StaPhase::Connecting || staPhase == StaPhase::Verified) &&
      staAssociationMatches()) {
    staPhase = StaPhase::Verified;
    if (staSaveOnSuccess) {
      staSaveOnSuccess = false;
      staCredentialsSaved = configStore.saveWifi(staSsid, staPassword);
      wifiError = staCredentialsSaved ? "" : "WiFi 已连接，但凭据保存失败";
    }
    if (staCredentialsSaved) wifiError.clear();
    staRetryWaiting = false;
    staRetryDelayMs = 5000u;
    return;
  }
  if (staPhase == StaPhase::Connecting) return;
  staPhase = StaPhase::Idle;
  if (!staRetryWaiting) {
    staRetryWaiting = true;
    staRetryStartedMs = now;
  } else if (now - staRetryStartedMs >= staRetryDelayMs) {
    staRetryDelayMs = min(staRetryDelayMs * 2u, uint32_t{60000});
    requestStaDisconnect(now);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  Serial.println("PL530 boot: serial ready");
  controller.begin();
  controller.setCalibrationTiming(4000u, 2000u, 18000u, 60000u, 8.0f, 0.15f, 0.30f);
  esp_timer_create_args_t guardArgs = {};
  guardArgs.callback = [](void*) {
    controller.checkSampleAgeFromTimer(static_cast<uint32_t>(esp_timer_get_time() / 1000ULL));
  };
  guardArgs.name = "tec_sample_guard";
  guardArgs.dispatch_method = ESP_TIMER_TASK;
  const esp_err_t guardCreate = esp_timer_create(&guardArgs, &outputGuardTimer);
  const esp_err_t guardStart = guardCreate == ESP_OK
      ? esp_timer_start_periodic(outputGuardTimer, OUTPUT_GUARD_INTERVAL_US)
      : static_cast<esp_err_t>(-1);
  const bool guardReady = guardCreate == ESP_OK && guardStart == ESP_OK;
  Serial.printf("Output guard: create=%d start=%d period=%lu us ready=%d\n",
                static_cast<int>(guardCreate), static_cast<int>(guardStart),
                static_cast<unsigned long>(OUTPUT_GUARD_INTERVAL_US),
                guardReady ? 1 : 0);

  ControllerSettings settings;
  if (!configStore.loadController(settings)) {
    settings = ControllerSettings{};
    configurationError = "配置读取或迁移失败，请检查并重新保存参数";
  }
  settings.enabled = false;
  controller.setSettings(settings);
  TecDirection storedDirection = TecDirection::Unknown;
  if (!configStore.loadDirection(storedDirection)) {
    directionError = "方向记录无效或读取失败，请重新检测";
  }
  controller.setTecDirection(storedDirection);
  directionSaved = storedDirection != TecDirection::Unknown;
  controller.startPwm(guardReady);
  Serial.printf("Hardware: %s; TMP117=0x%02X; PWM=%lu Hz; PSRAM=%u bytes\n",
                HARDWARE_REVISION, TMP117_ADDRESS,
                static_cast<unsigned long>(TEC_PWM_FREQUENCY), ESP.getPsramSize());
  Serial.printf("TMP117 bus: internal pull-ups, about %lu Hz; candidates: SDA=GPIO%u/SCL=GPIO%u, SDA=GPIO%u/SCL=GPIO%u\n",
                static_cast<unsigned long>(SENSOR_I2C_FREQUENCY),
                static_cast<unsigned int>(SENSOR_BUS_CANDIDATES[0].sda),
                static_cast<unsigned int>(SENSOR_BUS_CANDIDATES[0].scl),
                static_cast<unsigned int>(SENSOR_BUS_CANDIDATES[1].sda),
                static_cast<unsigned int>(SENSOR_BUS_CANDIDATES[1].scl));

  setupAp();
  if (!configStore.loadWifi(staSsid, staPassword)) {
    wifiError = "WiFi 配置读取失败，请重新配网";
    staSsid.clear();
    staPassword.clear();
  }
  if (!staSsid.isEmpty()) {
    startStaConnect(staSsid, staPassword, false);
  }

  setupRoutes();
  server.begin();
#if PL530_ENABLE_UNSAFE_DEBUG
  Serial.println("警告：不安全调试入口已启用，强制唤醒可绕过故障与采样保护。");
#endif
  Serial.printf("%s ready\n", DEVICE_NAME);
}

void loop() {
#if PL530_ENABLE_UNSAFE_DEBUG && defined(ARDUINO) && defined(ESP32)
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c == 'w' || c == 'W') {
      controller.setManualWakeHold(true);
    } else if (c == 's' || c == 'S') {
      controller.setManualWakeHold(false);
    } else if (c == '1') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(307, 0);  // 30% PWM Forward
    } else if (c == '2') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 307);  // 30% PWM Reverse
    } else if (c == '5') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(512, 0);  // 50% PWM Forward
    } else if (c == '6') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 512);  // 50% PWM Reverse
    } else if (c == '0') {
      controller.setDebugPwm(0, 0);
    } else if (c == '3') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(102, 0);  // 10% PWM Forward
    } else if (c == '4') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 102);  // 10% PWM Reverse
    } else if (c == '7') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(51, 0);   // 5% PWM Forward
    } else if (c == '8') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 51);   // 5% PWM Reverse
    } else if (c == 'a') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 153);  // 15% PWM Reverse
    } else if (c == 'A') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(153, 0);  // 15% PWM Forward
    } else if (c == 'b') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 204);  // 20% PWM Reverse
    } else if (c == 'B') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(204, 0);  // 20% PWM Forward
    } else if (c == 'e') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 256);  // 25% PWM Reverse
    } else if (c == '9' || c == 'f' || c == 'F') {
      controller.setManualWakeHold(true);
      controller.setDebugPwm(0, 1023); // 100% Reverse PWM (Pure DC 3.3V)
      Serial.println("[SERIAL] 100% Reverse PWM set (Pure DC 3.3V on TEC)!");
    } else if (c == 'r' || c == 'R' || c == 'n' || c == 'N' || c == 'u' || c == 'U') {
      const TecDirection direction = (c == 'r' || c == 'R') ? TecDirection::Reversed
          : (c == 'n' || c == 'N') ? TecDirection::Normal : TecDirection::Unknown;
      const auto result = applyManualDirection(direction);
      Serial.printf("[SERIAL] Direction %s: %s%s%s\n", tecDirectionKey(direction),
                    result.status == 200 ? "SAVED" : "REJECTED",
                    result.error ? ": " : "", result.error ? result.error : "");
    } else if (c == 'c' || c == 'C') {
      controller.resetFault();
      Serial.printf("[SERIAL] Fault reset requested. Current state: %s, nFAULT=%d\n",
                    controllerStateName(controller.status().state), digitalRead(PIN_DRV_FAULT));
    } else if (c == 'd' || c == 'D') {
      Serial.println("[SERIAL] Triggering Direction Calibration...");
      controller.setManualWakeHold(false);
      const auto result = beginDirectionCalibration();
      Serial.printf("[SERIAL] startDirectionCalibration result: %s (state: %s)%s%s\n",
                    result.status == 200 ? "SUCCESS" : "REJECTED",
                    controllerStateName(controller.status().state),
                    result.error ? ": " : "", result.error ? result.error : "");
    } else if (c == '?' || c == 'h' || c == 'H') {
      Serial.println("\n=== PL530 SERIAL DEBUG COMMANDS ===");
      Serial.println("  w: Hold DRV8701 nSLEEP HIGH (Wake Hold ON)");
      Serial.println("  s: Put DRV8701 nSLEEP LOW (Sleep Mode)");
      Serial.println("  7: 5% Forward PWM (IN1=51, IN2=0)");
      Serial.println("  8: 5% Reverse PWM (IN1=0, IN2=51)");
      Serial.println("  3: 10% Forward PWM (IN1=102, IN2=0)");
      Serial.println("  4: 10% Reverse PWM (IN1=0, IN2=102)");
      Serial.println("  1: 30% Forward PWM (IN1=307, IN2=0)");
      Serial.println("  2: 30% Reverse PWM (IN1=0, IN2=307)");
      Serial.println("  5: 50% Forward PWM (IN1=512, IN2=0)");
      Serial.println("  6: 50% Reverse PWM (IN1=0, IN2=512)");
      Serial.println("  9 / f: 100% Reverse PWM (Pure DC 3.3V, IN1=0, IN2=1023)");
      Serial.println("  0: PWM OFF (IN1=0, IN2=0)");
      Serial.println("  r: Set direction REVERSED & save to NVS (Direct Cooling Mode)");
      Serial.println("  n: Set direction NORMAL & save to NVS");
      Serial.println("  u: Set direction UNKNOWN (clear NVS)");
      Serial.println("  c: Reset latched faults");
      Serial.println("  d: Start Direction Calibration");
      Serial.println("===================================\n");
    }
  }

  static uint32_t lastWakePrintMs = 0;
  if (controller.manualWakeHold() && millis() - lastWakePrintMs >= 1000u) {
    lastWakePrintMs = millis();
    Serial.printf("[WAKE HELD] nSLEEP=%d, nFAULT=%d, IN1=%d, IN2=%d, Temp=%.2f C\n",
                  digitalRead(PIN_DRV_SLEEP), digitalRead(PIN_DRV_FAULT),
                  controller.debugIn1Duty(), controller.debugIn2Duty(),
                  controller.status().rawTemperatureC);
  }
#endif
  controller.pollDriverFault();
  pollSensor(millis());
  serviceDirectionCalibration();
  dnsServer.processNextRequest();
  server.handleClient();
  controller.pollDriverFault();
  pollSensor(millis());
  if (!controller.directionCalibrationStatus().active) {
    pollWifiScan(millis());
    pollWifi(millis());
  }
  controller.pollDriverFault();
  serviceDirectionCalibration();
}
