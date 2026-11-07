#pragma once
// Reuse the production data types/API, replacing only method implementations.
#include "../../../include/thermal_controller.h"
namespace MockController {
inline int setCalls = 0, resetCalls = 0, ageCalls = 0, pollCalls = 0;
inline int submitCalls = 0;
inline int pidSetCalls = 0;
inline bool lastValid = false;
inline uint32_t lastSampleStartedMs = 0;
inline std::function<void()> ageHook, pollHook;
inline DirectionCalibrationStatus calibration;
inline bool calibrationEligible = true, calibrationStartOk = true, directionApplyOk = true;
inline int eligibilityCalls = 0, startCalls = 0, cancelCalls = 0, pulseStarts = 0;
inline std::vector<TecDirection> directionApplications;
inline std::function<void()> beforeStart;
inline void reset() {
  setCalls = resetCalls = ageCalls = pollCalls = 0;
  submitCalls = 0;
  pidSetCalls = 0;
  lastValid = false;
  lastSampleStartedMs = 0;
  ageHook = pollHook = nullptr;
  calibration = {};
  calibrationEligible = calibrationStartOk = directionApplyOk = true;
  eligibilityCalls = startCalls = cancelCalls = pulseStarts = 0;
  directionApplications.clear();
  beforeStart = nullptr;
}
inline bool latch(ControllerState state) {
  return state == ControllerState::ThermalFaultLatched ||
      state == ControllerState::DriverFaultLatched ||
      state == ControllerState::SensorTimeoutLatched ||
      state == ControllerState::SafetyFaultLatched;
}
}
inline const char* controllerStateName(ControllerState) { return "mock controller"; }
inline void ThermalController::begin() {
  m_tecDirection = TecDirection::Unknown;
  m_settings.enabled = false;
}
inline void ThermalController::startPwm(bool ready) {
  if (!ready) m_status.state = ControllerState::SafetyFaultLatched;
}
inline void ThermalController::pollDriverFault(bool) {
  ++MockController::pollCalls;
  if (MockController::pollHook) MockController::pollHook();
}
inline void ThermalController::checkSampleAgeFromTimer(uint32_t) {
  ++MockController::ageCalls;
  if (MockController::ageHook) MockController::ageHook();
}
inline void ThermalController::setSettings(float target, float high, float low, bool enabled) {
  ++MockController::setCalls;
  mockEvents.push_back(enabled ? "enable" : "disable");
  m_settings = {target, high, low, enabled && !MockController::latch(m_status.state) &&
      m_tecDirection != TecDirection::Unknown && !MockController::calibration.active};
  if (!m_settings.enabled) {
    m_status.driverAwake = false;
    m_status.signedDutyPercent = m_status.coolingDutyPercent = m_status.heatingDutyPercent = 0;
  }
}
inline void ThermalController::resetFault() {
  ++MockController::resetCalls;
  if (m_status.state != ControllerState::SafetyFaultLatched) m_status.state = ControllerState::Disabled;
  m_settings.enabled = false;
}
inline void ThermalController::submitTemperature(bool valid, float temperature, uint32_t startedMs) {
  ++MockController::submitCalls;
  MockController::lastValid = valid;
  MockController::lastSampleStartedMs = startedMs;
  m_status.sensorOk = valid;
  m_status.rawTemperatureC = m_status.filteredTemperatureC = valid ? temperature : NAN;
  if (valid && m_settings.enabled) m_stability.add(temperature, m_settings.targetC, startedMs);
}
inline bool ThermalController::setPidSettings(const PrecisionPidSettings& settings) {
  ++MockController::pidSetCalls;
  mockEvents.push_back("apply-pid");
  pollDriverFault(false);
  if (m_settings.enabled || MockController::calibration.active || m_manualWakeHold ||
      m_status.driverAwake || MockController::latch(m_status.state) || !settings.valid()) return false;
  m_stability.reset();
  return m_pid.configure(settings);
}
inline bool ThermalController::setTecDirection(TecDirection direction) {
  MockController::directionApplications.push_back(direction);
  mockEvents.push_back(std::string("apply-direction:") + tecDirectionKey(direction));
  if (!MockController::directionApplyOk) return false;
  m_tecDirection = direction;
  return true;
}
inline bool ThermalController::canStartDirectionCalibration() {
  ++MockController::eligibilityCalls;
  mockEvents.push_back("calibration-eligible");
  return MockController::calibrationEligible && !m_settings.enabled &&
      !MockController::calibration.active && !MockController::latch(m_status.state);
}
inline bool ThermalController::startDirectionCalibration() {
  ++MockController::startCalls;
  mockEvents.push_back("calibration-start");
  if (MockController::beforeStart) MockController::beforeStart();
  if (!MockController::calibrationStartOk || !canStartDirectionCalibration()) return false;
  MockController::calibration = {};
  MockController::calibration.phase = CalibrationPhase::BaselineForward;
  MockController::calibration.active = true;
  m_status.state = ControllerState::DirectionCalibration;
  // Sentinel for accepting a run that can subsequently pulse, not a thermal model.
  ++MockController::pulseStarts;
  return true;
}
inline void ThermalController::cancelDirectionCalibration() {
  ++MockController::cancelCalls;
  mockEvents.push_back("calibration-cancel");
  if (!MockController::calibration.active) return;
  MockController::calibration.active = false;
  MockController::calibration.phase = CalibrationPhase::Cancelled;
  MockController::calibration.rawDutyPercent = 0;
  m_status.driverAwake = false;
  m_settings.enabled = false;
  m_status.state = ControllerState::Disabled;
}
inline DirectionCalibrationStatus ThermalController::directionCalibrationStatus() const {
  return MockController::calibration;
}
inline void ThermalController::setManualWakeHold(bool hold) {
  m_manualWakeHold = hold;
  m_status.driverAwake = hold;
  if (!hold) m_debugIn1Duty = m_debugIn2Duty = 0;
}
inline void ThermalController::setDebugPwm(int in1, int in2) {
  m_debugIn1Duty = in1;
  m_debugIn2Duty = in2;
}
