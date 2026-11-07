#pragma once

#include <Arduino.h>
#include "config.h"
#include "tec_direction.h"
#include "precision_pid.h"

enum class ControllerState : uint8_t {
  WaitingForSensor,
  Disabled,
  NormalControl,
  BalanceControl,
  HardOverTemperature,
  HardUnderTemperature,
  ThermalFaultLatched,
  DriverFaultLatched,
  SensorTimeoutLatched,
  SafetyFaultLatched,
  DirectionCalibration,
};

struct ControllerSettings {
  float targetC = DEFAULT_TARGET_C;
  float pwmHighC = DEFAULT_PWM_HIGH_C;
  float pwmLowC = DEFAULT_PWM_LOW_C;
  bool enabled = false;
};

struct ControllerStatus {
  float filteredTemperatureC = NAN;
  float rawTemperatureC = NAN;
  float signedDutyPercent = 0.0f;
  float coolingDutyPercent = 0.0f;
  float heatingDutyPercent = 0.0f;
  float slopeCPerSecond = 0.0f;
  float balanceDrivePercent = 0.0f;
  ControllerState state = ControllerState::WaitingForSensor;
  bool sensorOk = false;
  bool driverAwake = false;
};

const char* controllerStateName(ControllerState state);

class ThermalController {
 public:
  void begin();
  void startPwm(bool outputGuardReady);
  void stopOutput();
  // Main-loop service: reconcile pending faults and expire stale diagnostics.
  void pollDriverFault(bool advanceCalibration = true);
  // Called by the independent timer task. It only cuts nSLEEP and sets a latch.
  void checkSampleAgeFromTimer(uint32_t millisNow);

  void setSettings(float targetC, float pwmHighC, float pwmLowC, bool enabled);
  void setSettings(const ControllerSettings& settings) {
    setSettings(settings.targetC, settings.pwmHighC, settings.pwmLowC, settings.enabled);
  }
  const ControllerSettings& settings() const { return m_settings; }

  // sampleStartedMs is the acquisition start, not its delayed completion time.
  void submitTemperature(bool valid, float rawTemperatureC, uint32_t sampleStartedMs);
  void resetFault();
  const ControllerStatus& status() const { return m_status; }
  const PrecisionPidResult& pidStatus() const { return m_pid.result(); }
  const PrecisionPidSettings& pidSettings() const { return m_pid.settings(); }
  StabilityResult stabilityStatus() const { return m_stability.result(); }
  bool setPidSettings(const PrecisionPidSettings& settings);
  TecDirection tecDirection() const { return m_tecDirection; }
  bool setTecDirection(TecDirection direction);
  bool canStartDirectionCalibration();
  bool startDirectionCalibration();
  void cancelDirectionCalibration();
  DirectionCalibrationStatus directionCalibrationStatus() const;

  void setCalibrationTiming(uint32_t pulseMs, uint32_t baselineMs, uint32_t settleMs,
                            uint32_t totalTimeoutMs, float maxExcursionC = 8.0f,
                            float maxBaselineSlope = 0.15f, float maxBaselineRange = 0.30f) {
    if (!m_calibration.active) {
      m_calPulseMs = pulseMs;
      m_calBaselineMs = baselineMs;
      m_calSettleMs = settleMs;
      m_calTotalTimeoutMs = totalTimeoutMs;
      m_calMaxExcursionC = maxExcursionC;
      m_calMaxBaselineSlope = maxBaselineSlope;
      m_calMaxBaselineRange = maxBaselineRange;
    }
  }
  uint32_t calibrationPulseMs() const { return m_calPulseMs; }
  uint32_t calibrationSettleMs() const { return m_calSettleMs; }

  void setManualWakeHold(bool hold);
  bool manualWakeHold() const { return m_manualWakeHold; }
  void setDebugPwm(int in1Duty, int in2Duty);
  int debugIn1Duty() const { return m_debugIn1Duty; }
  int debugIn2Duty() const { return m_debugIn2Duty; }

 private:
  struct CalibrationSample {
    uint32_t ms = 0;
    float temperatureC = 0;
  };
  void finishCalibration(CalibrationPhase phase, const char* error);
  void enterCalibrationPhase(CalibrationPhase phase, uint32_t now);
  void serviceCalibration();
  void recordCalibrationSample(float temperatureC, uint32_t ms);
  bool calibrationBaseline(float& slope) const;
  bool finishCalibrationPulse();
  void writeRawHBridge(float rawDutyPercent);
  bool outputDeadlineSafe();
  void invalidateSample();
  void initializeSample(float rawTemperatureC, uint32_t millisNow);
  void applySignedDuty(float signedDutyPercent, bool immediate = false);
  void writeHBridge(float signedDutyPercent);
  bool armOutputIfFresh();
  void disableDriver();
  void latchDriverFault();
  bool faultLatched() const;
  bool hardRecoveryActive() const;
  static void onDriverFault(void* argument);
  void checkHardLimits(float rawTemperatureC, uint32_t millisNow);
  static float clampFloat(float value, float lowest, float highest);

  ControllerSettings m_settings;
  ControllerStatus m_status;
  TecDirection m_tecDirection = TecDirection::Unknown;
  DirectionCalibrationStatus m_calibration;
  bool m_calServiceBusy = false;
  uint32_t m_calPulseMs = TEC_CAL_PULSE_MS;
  uint32_t m_calBaselineMs = TEC_CAL_BASELINE_MS;
  uint32_t m_calSettleMs = TEC_CAL_SETTLE_MS;
  uint32_t m_calTotalTimeoutMs = TEC_CAL_TOTAL_TIMEOUT_MS;
  float m_calMaxExcursionC = TEC_CAL_MAX_EXCURSION_C;
  float m_calMaxBaselineSlope = TEC_CAL_MAX_BASELINE_SLOPE_C_S;
  float m_calMaxBaselineRange = TEC_CAL_MAX_BASELINE_RANGE_C;
  CalibrationSample m_calSamples[200]{};
  uint16_t m_calSampleCount = 0;
  uint32_t m_calPhaseMs = 0;
  uint32_t m_calFinishedElapsedMs = 0;
  float m_calStartTemperatureC = NAN;
  float m_calBaselineSlope = 0;
  CalibrationSample m_calPulseBaseline;
  float m_calForwardCorrectedC = NAN;
  volatile bool m_guardCalibrationActive = false;
  volatile bool m_guardCalibrationPulse = false;
  volatile bool m_calPulseExpired = false;
  volatile bool m_calTotalExpired = false;
  volatile uint32_t m_guardCalibrationStartMs = 0;
  volatile uint32_t m_guardPulseStartMs = 0;
  uint32_t m_lastValidSampleMs = 0;
  uint32_t m_hardLimitEnteredMs = 0;
  bool m_inBalance = false;
  bool m_havePreviousSample = false;
  bool m_haveSlopeSample = false;
  bool m_outputStarted = false;
  bool m_outputGuardReady = false;
  volatile bool m_driverFaultPending = false;
  volatile bool m_monitorDriverFault = false;
  volatile bool m_sensorTimeoutPending = false;
  volatile bool m_outputWatchdogArmed = false;
  volatile bool m_guardHaveValidSample = false;
  volatile uint32_t m_guardLastSampleMs = 0;
  bool m_enabledBeforeTimeout = false;
#if defined(ARDUINO) && defined(ESP32)
  portMUX_TYPE m_guardMux = portMUX_INITIALIZER_UNLOCKED;
#endif
  volatile bool m_manualWakeHold = false;
  int m_debugIn1Duty = 0;
  int m_debugIn2Duty = 0;
  int8_t m_lastDirection = 0;
  float m_previousFilteredTemperatureC = NAN;
  PrecisionPid m_pid;
  TemperatureStability m_stability;
};
