#include "thermal_controller.h"

#include "config.h"

#if defined(ARDUINO) && defined(ESP32)
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "soc/gpio_struct.h"
#endif

static_assert(PIN_DRV_FAULT < 32, "Fault handoff uses the low GPIO interrupt status register");

#if defined(ARDUINO) && defined(ESP32)
namespace {
bool configurePwmPin(uint8_t pin, uint8_t channel) {
  ledc_channel_config_t config = {};
  config.gpio_num = pin;
  config.speed_mode = LEDC_LOW_SPEED_MODE;
  config.channel = static_cast<ledc_channel_t>(channel);
  config.intr_type = LEDC_INTR_DISABLE;
  config.timer_sel = static_cast<ledc_timer_t>((channel / 2u) % 4u);
  config.duty = 0;
  config.hpoint = 0;
  return ledc_channel_config(&config) == ESP_OK;
}
}  // namespace
#endif

const char* controllerStateName(ControllerState state) {
  switch (state) {
    case ControllerState::WaitingForSensor:
      return "等待传感器";
    case ControllerState::Disabled:
      return "已停用";
    case ControllerState::NormalControl:
      return "温度调节";
    case ControllerState::BalanceControl:
      return "热平衡控制";
    case ControllerState::HardOverTemperature:
      return "超温保护：全力制冷";
    case ControllerState::HardUnderTemperature:
      return "低温保护：全力加热";
    case ControllerState::ThermalFaultLatched:
      return "热失控锁定";
    case ControllerState::DriverFaultLatched:
      return "驱动器故障锁定";
    case ControllerState::SensorTimeoutLatched:
      return "温度采样超时锁定";
    case ControllerState::SafetyFaultLatched:
      return "输出保护初始化失败";
    case ControllerState::DirectionCalibration:
      return "TEC 方向检测";
  }
  return "未知状态";
}

void ThermalController::begin() {
  m_settings = ControllerSettings{};
  m_status = ControllerStatus{};
  m_status.state = ControllerState::Disabled;
  m_tecDirection = TecDirection::Unknown;
  m_calibration = DirectionCalibrationStatus{};
  m_calServiceBusy = false;
  m_calSampleCount = 0;
  m_calPhaseMs = 0;
  m_calFinishedElapsedMs = 0;
  m_calStartTemperatureC = NAN;
  m_calBaselineSlope = 0;
  m_calForwardCorrectedC = NAN;
  m_guardCalibrationActive = false;
  m_guardCalibrationPulse = false;
  m_calPulseExpired = false;
  m_calTotalExpired = false;
  m_guardCalibrationStartMs = 0;
  m_guardPulseStartMs = 0;
  m_lastValidSampleMs = 0;
  m_hardLimitEnteredMs = 0;
  m_inBalance = false;
  m_havePreviousSample = false;
  m_haveSlopeSample = false;
  m_outputStarted = false;
  m_outputGuardReady = false;
  m_driverFaultPending = false;
  m_monitorDriverFault = false;
  m_sensorTimeoutPending = false;
  m_outputWatchdogArmed = false;
  m_guardHaveValidSample = false;
  m_guardLastSampleMs = 0;
  m_lastDirection = 0;
  m_previousFilteredTemperatureC = NAN;
#if defined(ARDUINO) && defined(ESP32)
  pinMode(PIN_DRV_SLEEP, OUTPUT);
  pinMode(PIN_TEC_IN1, OUTPUT);
  pinMode(PIN_TEC_IN2, OUTPUT);
  // Establish the inactive bridge state before exposing the driver control
  // pins. R15 is the board pulldown; the internal pull-up complements R14 so
  // an unpopulated or high-impedance nFAULT line cannot look like a fault.
  digitalWrite(PIN_DRV_SLEEP, LOW);
  digitalWrite(PIN_TEC_IN1, LOW);
  digitalWrite(PIN_TEC_IN2, LOW);
  pinMode(PIN_DRV_FAULT, INPUT_PULLUP);
  const auto faultPin = static_cast<gpio_num_t>(PIN_DRV_FAULT);
  // Install from setup/loop's core. An existing service has unknown affinity
  // and cache-safety flags, so it cannot satisfy the output safety contract.
  // Do not remove a handler here: on a clean boot this pin has no registered
  // handler, and ESP-IDF correctly reports that as an error.
  const esp_err_t isrInstall = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
  const esp_err_t interruptType = isrInstall == ESP_OK
      ? gpio_set_intr_type(faultPin, GPIO_INTR_NEGEDGE)
      : ESP_ERR_INVALID_STATE;
  const esp_err_t handlerAdd = interruptType == ESP_OK
      ? gpio_isr_handler_add(faultPin, onDriverFault, this)
      : ESP_ERR_INVALID_STATE;
#if defined(ARDUINO) && defined(ESP32)
  Serial.printf("DRV nFAULT init: install=%d type=%d add=%d pin=%u level=%d\n",
                static_cast<int>(isrInstall), static_cast<int>(interruptType),
                static_cast<int>(handlerAdd), static_cast<unsigned int>(PIN_DRV_FAULT),
                digitalRead(PIN_DRV_FAULT));
#endif
  if (isrInstall != ESP_OK || interruptType != ESP_OK || handlerAdd != ESP_OK) {
    m_status.state = ControllerState::SafetyFaultLatched;
  }
  Serial.printf("DRV wake probe starting: initial nFAULT=%d (nSLEEP=0)\n", digitalRead(PIN_DRV_FAULT));
  digitalWrite(PIN_DRV_SLEEP, HIGH);
  uint32_t wakeTimeMs = 0;
  for (uint32_t t = 5; t <= 400; t += 5) {
    delay(5);
    const int level = digitalRead(PIN_DRV_FAULT);
    if (level == HIGH && wakeTimeMs == 0) {
      wakeTimeMs = t;
    }
  }
  digitalWrite(PIN_DRV_SLEEP, LOW);
  Serial.printf("DRV wake probe result: wakeTimeMs=%lu, end nFAULT=%d (nSLEEP returned to 0)\n",
                static_cast<unsigned long>(wakeTimeMs), digitalRead(PIN_DRV_FAULT));
#endif
  stopOutput();
}

void ThermalController::startPwm(bool outputGuardReady) {
  m_outputGuardReady = outputGuardReady;
  if (!m_outputGuardReady || m_status.state == ControllerState::SafetyFaultLatched) {
    m_settings.enabled = false;
    m_status.state = ControllerState::SafetyFaultLatched;
    stopOutput();
    return;
  }
  if (m_outputStarted) {
    return;
  }
#if defined(ARDUINO) && defined(ESP32)
  const double frequency1 = ledcSetup(LEDC_CHANNEL_IN1, TEC_PWM_FREQUENCY, TEC_PWM_RESOLUTION);
  const double frequency2 = ledcSetup(LEDC_CHANNEL_IN2, TEC_PWM_FREQUENCY, TEC_PWM_RESOLUTION);
  Serial.printf("DRV PWM setup: guard=%d IN1=%.2fHz IN2=%.2fHz target=%luHz res=%u\n",
                m_outputGuardReady ? 1 : 0, frequency1, frequency2,
                static_cast<unsigned long>(TEC_PWM_FREQUENCY),
                static_cast<unsigned int>(TEC_PWM_RESOLUTION));
  if (frequency1 <= 0.0 || frequency2 <= 0.0 ||
      fabs(frequency1 - TEC_PWM_FREQUENCY) > TEC_PWM_FREQUENCY * 0.01 ||
      fabs(frequency2 - TEC_PWM_FREQUENCY) > TEC_PWM_FREQUENCY * 0.01) {
    m_settings.enabled = false;
    m_status.state = ControllerState::SafetyFaultLatched;
    stopOutput();
    return;
  }
  // Arduino's ledcAttachPin() discards ledc_channel_config()'s error. Do the
  // binding explicitly so a failed GPIO matrix/LEDC reservation cannot leave
  // the controller believing that a non-existent PWM output is active.
  const bool pwmIn1Ready = configurePwmPin(PIN_TEC_IN1, LEDC_CHANNEL_IN1);
  const bool pwmIn2Ready = configurePwmPin(PIN_TEC_IN2, LEDC_CHANNEL_IN2);
  Serial.printf("DRV PWM pins: IN1 GPIO%u=%d IN2 GPIO%u=%d\n",
                static_cast<unsigned int>(PIN_TEC_IN1), pwmIn1Ready ? 1 : 0,
                static_cast<unsigned int>(PIN_TEC_IN2), pwmIn2Ready ? 1 : 0);
  if (!pwmIn1Ready || !pwmIn2Ready) {
    m_settings.enabled = false;
    m_status.state = ControllerState::SafetyFaultLatched;
    stopOutput();
    return;
  }
#endif
  m_outputStarted = true;
 #if defined(ARDUINO) && defined(ESP32)
  Serial.println("DRV PWM ready; bridge outputs remain disabled until a fresh sensor sample");
 #endif
  writeHBridge(0.0f);
}

void ThermalController::stopOutput() {
  if (m_calibration.active) {
    finishCalibration(CalibrationPhase::Cancelled, "cancelled");
  }
  // Aborting a hard-limit recovery requires an explicit fault reset.
  if (hardRecoveryActive()) {
    m_settings.enabled = false;
    m_inBalance = false;
    m_status.balanceDrivePercent = 0.0f;
    m_status.state = ControllerState::ThermalFaultLatched;
  }
  m_status.signedDutyPercent = 0.0f;
  m_status.coolingDutyPercent = 0.0f;
  m_status.heatingDutyPercent = 0.0f;
  if (!faultLatched() &&
      m_status.state != ControllerState::HardOverTemperature &&
      m_status.state != ControllerState::HardUnderTemperature) {
    m_status.state = m_settings.enabled ? ControllerState::WaitingForSensor : ControllerState::Disabled;
  }
  writeHBridge(0.0f);
}

void ThermalController::setSettings(float targetC, float pwmHighC, float pwmLowC, bool enabled) {
  pollDriverFault(false);
  if (m_calibration.active) {
    if (enabled) return;
    cancelDirectionCalibration();
  }
  if (!isfinite(targetC) || !isfinite(pwmHighC) || !isfinite(pwmLowC) ||
      pwmLowC >= targetC || targetC >= pwmHighC) {
    m_settings.enabled = false;
    m_inBalance = false;
    stopOutput();
    pollDriverFault();
    return;
  }
  targetC = clampFloat(targetC, HARD_MIN_TEMPERATURE_C + 0.2f, HARD_MAX_TEMPERATURE_C - 0.2f);
  pwmHighC = clampFloat(pwmHighC, targetC + 0.2f, HARD_MAX_TEMPERATURE_C);
  pwmLowC = clampFloat(pwmLowC, HARD_MIN_TEMPERATURE_C, targetC - 0.2f);

  const bool targetChanged = fabs(targetC - m_settings.targetC) > 0.001f;
  const bool bandChanged = fabs(pwmHighC - m_settings.pwmHighC) > 0.001f ||
                           fabs(pwmLowC - m_settings.pwmLowC) > 0.001f;
  m_settings.targetC = targetC;
  m_settings.pwmHighC = pwmHighC;
  m_settings.pwmLowC = pwmLowC;

  if (targetChanged || bandChanged) {
    m_inBalance = false;
  }

  if (!enabled) {
    m_enabledBeforeTimeout = false;
    m_settings.enabled = false;
    m_inBalance = false;
    if (!faultLatched() && !hardRecoveryActive()) {
      m_status.state = ControllerState::Disabled;
    }
    stopOutput();
    pollDriverFault();
    return;
  }

  if (faultLatched() || m_tecDirection == TecDirection::Unknown) {
    m_settings.enabled = false;
    stopOutput();
    return;
  }

  m_settings.enabled = true;
  if (m_status.state == ControllerState::Disabled) {
    m_status.state = ControllerState::WaitingForSensor;
  }
}

void ThermalController::submitTemperature(bool valid, float rawTemperatureC, uint32_t sampleStartedMs) {
  pollDriverFault(false);
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  const bool finiteSample = valid && isfinite(rawTemperatureC);
  const bool fresh = finiteSample &&
      static_cast<uint32_t>(millis() - sampleStartedMs) < SENSOR_STALE_TIMEOUT_MS;
  m_guardHaveValidSample = fresh;
  if (fresh) {
    m_guardLastSampleMs = sampleStartedMs;
    m_sensorTimeoutPending = false;
  } else if (finiteSample) {
    m_sensorTimeoutPending = true;
  }
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif
  if (!fresh) {
    if (m_calibration.active) {
      finishCalibration(CalibrationPhase::Failed, finiteSample ? "sensor_stale" : "sensor_invalid");
    }
    pollDriverFault();
    invalidateSample();
    if (!faultLatched() && !hardRecoveryActive()) {
      m_status.state = m_settings.enabled ? ControllerState::WaitingForSensor : ControllerState::Disabled;
    }
    stopOutput();
    return;
  }

  // Probe recovered: automatically unlock sensor timeout fault lock
  if (m_status.state == ControllerState::SensorTimeoutLatched) {
    if (m_enabledBeforeTimeout) {
      m_settings.enabled = true;
    }
    m_status.state = m_settings.enabled ? ControllerState::WaitingForSensor : ControllerState::Disabled;
    m_enabledBeforeTimeout = false;
  }

  const uint32_t millisNow = sampleStartedMs;
  const float dtSeconds = m_havePreviousSample
                              ? clampFloat(static_cast<float>(millisNow - m_lastValidSampleMs) / 1000.0f,
                                           0.001f, 2.0f)
                              : CONTROL_LOOP_INTERVAL_MS / 1000.0f;

  if (!m_havePreviousSample) {
    initializeSample(rawTemperatureC, millisNow);
    if ((!m_settings.enabled && !m_calibration.active) || !m_outputStarted) {
      stopOutput();
      return;
    }
  }

  m_status.rawTemperatureC = rawTemperatureC;
  const float previousFilteredC = m_status.filteredTemperatureC;
  m_status.filteredTemperatureC +=
      TEMPERATURE_FILTER_ALPHA * (rawTemperatureC - previousFilteredC);

  if (m_haveSlopeSample) {
    const float measuredSlope =
        (m_status.filteredTemperatureC - previousFilteredC) / dtSeconds;
    m_status.slopeCPerSecond +=
        0.15f * (measuredSlope - m_status.slopeCPerSecond);
  } else {
    m_haveSlopeSample = true;
  }
  m_previousFilteredTemperatureC = m_status.filteredTemperatureC;
  m_lastValidSampleMs = millisNow;
  m_status.sensorOk = true;

  if (faultLatched()) {
    stopOutput();
    return;
  }

  if (m_calibration.active) {
    if (rawTemperatureC <= TEC_CAL_ABORT_MIN_C || rawTemperatureC >= TEC_CAL_ABORT_MAX_C) {
      finishCalibration(CalibrationPhase::Failed, "temperature_bounds");
    } else if (fabs(rawTemperatureC - m_calStartTemperatureC) >= m_calMaxExcursionC) {
      finishCalibration(CalibrationPhase::Failed, "temperature_excursion");
    } else {
      recordCalibrationSample(rawTemperatureC, millisNow);
      serviceCalibration();
    }
    return;
  }

  if (!m_settings.enabled) {
    m_status.state = ControllerState::Disabled;
    stopOutput();
    return;
  }
  if (!m_outputStarted) {
    stopOutput();
    return;
  }

  checkHardLimits(rawTemperatureC, millisNow);
  if (faultLatched() ||
      m_status.state == ControllerState::HardOverTemperature ||
      m_status.state == ControllerState::HardUnderTemperature) {
    return;
  }

  const float error = m_status.filteredTemperatureC - m_settings.targetC;
  // Keep the learned equilibrium through ordinary drift inside the soft band.
  const bool retainBalance = m_inBalance &&
      m_status.filteredTemperatureC > m_settings.pwmLowC &&
      m_status.filteredTemperatureC < m_settings.pwmHighC;
  if (fabs(error) <= BALANCE_DEADBAND_C || retainBalance) {
    if (!m_inBalance) {
      enterBalance(m_status.filteredTemperatureC);
    }
    m_status.state = ControllerState::BalanceControl;
    updateBalance(m_status.filteredTemperatureC, dtSeconds);
    return;
  }

  m_inBalance = false;
  m_status.state = ControllerState::NormalControl;
  applySignedDuty(normalControlDuty(m_status.filteredTemperatureC), false);
}

void ThermalController::invalidateSample() {
  m_status.sensorOk = false;
  m_status.rawTemperatureC = NAN;
  m_status.filteredTemperatureC = NAN;
  m_status.slopeCPerSecond = 0.0f;
  m_status.balanceDrivePercent = 0.0f;
  m_inBalance = false;
  m_havePreviousSample = false;
  m_haveSlopeSample = false;
  m_previousFilteredTemperatureC = NAN;
  m_lastValidSampleMs = 0;
}

void ThermalController::initializeSample(float rawTemperatureC, uint32_t millisNow) {
  m_status.rawTemperatureC = rawTemperatureC;
  m_status.filteredTemperatureC = rawTemperatureC;
  m_previousFilteredTemperatureC = rawTemperatureC;
  m_lastValidSampleMs = millisNow;
  m_status.sensorOk = true;
  m_havePreviousSample = true;
  m_haveSlopeSample = false;
}

float ThermalController::normalControlDuty(float temperatureC) {
  const float error = temperatureC - m_settings.targetC;
  if (error > NORMAL_CONTROL_DEADBAND_C) {
    const float span = max(m_settings.pwmHighC - m_settings.targetC, 0.2f);
    const float progress = clampFloat((temperatureC - m_settings.targetC) / span, 0.0f, 1.0f);
    const float magnitude = BALANCE_BASE_DUTY_PERCENT +
                            (100.0f - BALANCE_BASE_DUTY_PERCENT) * powf(progress, 0.45f);
    return -clampFloat(magnitude, BALANCE_BASE_DUTY_PERCENT, 100.0f);
  }
  if (error < -NORMAL_CONTROL_DEADBAND_C) {
    const float span = max(m_settings.targetC - m_settings.pwmLowC, 0.2f);
    const float progress = clampFloat((m_settings.targetC - temperatureC) / span, 0.0f, 1.0f);
    const float magnitude = BALANCE_BASE_DUTY_PERCENT +
                            (100.0f - BALANCE_BASE_DUTY_PERCENT) * powf(progress, 0.45f);
    return clampFloat(magnitude, BALANCE_BASE_DUTY_PERCENT, 100.0f);
  }
  return 0.0f;
}

void ThermalController::enterBalance(float rawTemperatureC) {
  m_inBalance = true;
  // Smoothly inherit active approach duty to prevent setpoint thermal rebounds.
  if (fabs(m_status.signedDutyPercent) > 0.01f) {
    m_status.balanceDrivePercent = m_status.signedDutyPercent;
  } else {
    const bool cooling = rawTemperatureC > m_settings.targetC;
    m_status.balanceDrivePercent = cooling ? -BALANCE_BASE_DUTY_PERCENT : BALANCE_BASE_DUTY_PERCENT;
  }
  m_status.state = ControllerState::BalanceControl;
}

void ThermalController::updateBalance(float rawTemperatureC, float dtSeconds) {
  const float error = rawTemperatureC - m_settings.targetC;
  // Positive drive heats. Rising temperature therefore requires a more negative correction.
  float correction = -(m_status.slopeCPerSecond * BALANCE_SLOPE_GAIN +
                       error * BALANCE_ERROR_GAIN);
  // While meaningfully off-target, do not permit derivative damping to reverse
  // the correction direction (which causes off-target balance settling).
  if (error > BALANCE_DEADBAND_C) {
    if (correction > 0.0f) correction = 0.0f;
  } else if (error < -BALANCE_DEADBAND_C) {
    if (correction < 0.0f) correction = 0.0f;
  }
  const float proposed = m_status.balanceDrivePercent + correction * dtSeconds;
  const float limited = clampFloat(proposed,
                                   m_status.balanceDrivePercent - MAX_BALANCE_DRIVE_STEP_PER_TICK,
                                   m_status.balanceDrivePercent + MAX_BALANCE_DRIVE_STEP_PER_TICK);
  m_status.balanceDrivePercent = clampFloat(limited, -100.0f, 100.0f);
  applySignedDuty(m_status.balanceDrivePercent, false);
}

void ThermalController::checkHardLimits(float rawTemperatureC, uint32_t millisNow) {
  // Keep the recovery timer running until the hysteresis threshold is crossed.
  // Jumping straight to the opposite hard limit is still an unsafe episode.
  if (rawTemperatureC >= HARD_MAX_TEMPERATURE_C ||
      (m_status.state == ControllerState::HardOverTemperature &&
       rawTemperatureC > HARD_RECOVER_MAX_C)) {
    if (m_status.state != ControllerState::HardOverTemperature) {
      if (!hardRecoveryActive()) m_hardLimitEnteredMs = millisNow;
      m_inBalance = false;
      m_status.balanceDrivePercent = 0.0f;
    }
    m_status.state = ControllerState::HardOverTemperature;
    const uint32_t elapsed = millisNow - m_hardLimitEnteredMs;
    const bool insufficientCooling =
        elapsed >= RUNAWAY_NO_PROGRESS_MS &&
        m_status.slopeCPerSecond >= RUNAWAY_MIN_COOLING_RATE_C_PER_S;
    if (insufficientCooling || elapsed >= RUNAWAY_LIMIT_MS) {
      m_settings.enabled = false;
      m_status.state = ControllerState::ThermalFaultLatched;
      stopOutput();
    } else {
      applySignedDuty(-100.0f, true);
    }
    return;
  }

  if (rawTemperatureC <= HARD_MIN_TEMPERATURE_C ||
      (m_status.state == ControllerState::HardUnderTemperature &&
       rawTemperatureC < HARD_RECOVER_MIN_C)) {
    if (m_status.state != ControllerState::HardUnderTemperature) {
      if (!hardRecoveryActive()) m_hardLimitEnteredMs = millisNow;
      m_inBalance = false;
      m_status.balanceDrivePercent = 0.0f;
    }
    m_status.state = ControllerState::HardUnderTemperature;
    const uint32_t elapsed = millisNow - m_hardLimitEnteredMs;
    const bool insufficientHeating =
        elapsed >= RUNAWAY_NO_PROGRESS_MS &&
        m_status.slopeCPerSecond <= RUNAWAY_MIN_HEATING_RATE_C_PER_S;
    if (insufficientHeating || elapsed >= RUNAWAY_LIMIT_MS) {
      m_settings.enabled = false;
      m_status.state = ControllerState::ThermalFaultLatched;
      stopOutput();
    } else {
      applySignedDuty(100.0f, true);
    }
    return;
  }

  const bool recoveredFromHot =
      rawTemperatureC <= HARD_RECOVER_MAX_C && m_status.state == ControllerState::HardOverTemperature;
  const bool recoveredFromCold =
      rawTemperatureC >= HARD_RECOVER_MIN_C && m_status.state == ControllerState::HardUnderTemperature;
  if (recoveredFromHot || recoveredFromCold) {
    m_hardLimitEnteredMs = 0;
    m_status.state = ControllerState::NormalControl;
  }
}

void ThermalController::resetFault() {
  pollDriverFault(false);
  cancelDirectionCalibration();
  if (!faultLatched() || !m_outputGuardReady ||
      m_status.state == ControllerState::SafetyFaultLatched) {
    return;
  }
  disableDriver();
  m_enabledBeforeTimeout = false;
  m_settings.enabled = false;
  m_status.state = ControllerState::Disabled;
  invalidateSample();
  m_hardLimitEnteredMs = 0;
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  m_driverFaultPending = false;
  m_sensorTimeoutPending = false;
  m_guardHaveValidSample = false;
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif
  stopOutput();
}

void ThermalController::applySignedDuty(float signedDutyPercent, bool immediate) {
  signedDutyPercent = clampFloat(signedDutyPercent, -100.0f, 100.0f);
  if (!immediate) {
    const float delta = signedDutyPercent - m_status.signedDutyPercent;
    // When starting from rest (duty ~0), enforce a gentle initial ramp limit
    // to prevent DCDC peak-current tripping.
    const float maxStep = (fabs(m_status.signedDutyPercent) < 0.5f)
                              ? min(MAX_NORMAL_DUTY_STEP_PER_TICK, 8.0f)
                              : MAX_NORMAL_DUTY_STEP_PER_TICK;
    signedDutyPercent = m_status.signedDutyPercent +
                        clampFloat(delta, -maxStep, maxStep);
  }

  m_status.signedDutyPercent = signedDutyPercent;
  if (signedDutyPercent < -0.01f) {
    m_status.coolingDutyPercent = -signedDutyPercent;
    m_status.heatingDutyPercent = 0.0f;
  } else {
    m_status.heatingDutyPercent = signedDutyPercent;
    m_status.coolingDutyPercent = 0.0f;
  }
  writeHBridge(signedDutyPercent);
}

void ThermalController::writeHBridge(float signedDutyPercent) {
  if (m_calibration.active || m_tecDirection == TecDirection::Unknown) {
    if (fabs(signedDutyPercent) > 0.01f) {
      m_status.signedDutyPercent = 0;
      m_status.heatingDutyPercent = 0;
      m_status.coolingDutyPercent = 0;
    }
    disableDriver();
    return;
  }
  const float rawDuty = m_tecDirection == TecDirection::Normal ? -signedDutyPercent : signedDutyPercent;
  writeRawHBridge(rawDuty);
}

void ThermalController::writeRawHBridge(float rawDutyPercent) {
  const bool calibrationWrite = m_calibration.active;

  if (!m_outputStarted || !isfinite(rawDutyPercent) || fabs(rawDutyPercent) <= 0.01f) {
    disableDriver();
    return;
  }
  pollDriverFault(false);
  if (faultLatched() || !outputDeadlineSafe()) {
    disableDriver();
    return;
  }
  const int8_t direction = rawDutyPercent > 0.0f ? 1 : -1;
#if defined(ARDUINO) && defined(ESP32)
  if (m_lastDirection != 0 && m_lastDirection != direction) {
    disableDriver();
    delayMicroseconds(DRV_SLEEP_DELAY_US);
  }
#if defined(ARDUINO) && defined(ESP32)
  if (digitalRead(PIN_DRV_SLEEP) == LOW) {
    m_status.driverAwake = false;
  }
#endif
  if (!m_status.driverAwake) {
    ledcWrite(LEDC_CHANNEL_IN1, 0);
    ledcWrite(LEDC_CHANNEL_IN2, 0);
    if (!armOutputIfFresh()) {
#if defined(ARDUINO) && defined(ESP32)
      Serial.printf("DRV wake rejected: sensorFresh=%d faultPending=%d timeoutPending=%d nFAULT=%d phase=%s\n",
                    m_guardHaveValidSample ? 1 : 0, m_driverFaultPending ? 1 : 0,
                    m_sensorTimeoutPending ? 1 : 0, digitalRead(PIN_DRV_FAULT),
                    calibrationPhaseKey(m_calibration.phase));
#endif
      pollDriverFault(false);
      return;
    }
    delayMicroseconds(DRV_WAKE_DELAY_US);
#if defined(ARDUINO) && defined(ESP32)
    // DRV8701 charge pump takes up to ~150-250 ms with 47 uF VCP cap to exceed VCPUV.
    // While VCP < VCPUV, DRV8701 actively asserts nFAULT LOW.
    // Wait until nFAULT rises HIGH before arming fault monitoring.
    uint32_t waitUs = 0;
    while (digitalRead(PIN_DRV_FAULT) == LOW && waitUs < DRV_WAKE_TIMEOUT_US) {
      if (digitalRead(PIN_DRV_SLEEP) == LOW || !outputDeadlineSafe()) break;
      delayMicroseconds(1000);
      waitUs += 1000;
    }
    if (digitalRead(PIN_DRV_FAULT) == LOW) {
      Serial.printf("DRV wake failed: nFAULT remained LOW after %lu us (nSLEEP=%d phase=%s)\n",
                    static_cast<unsigned long>(DRV_WAKE_DELAY_US + waitUs),
                    digitalRead(PIN_DRV_SLEEP), calibrationPhaseKey(m_calibration.phase));
      disableDriver();
      if (m_calibration.active) finishCalibration(CalibrationPhase::Failed, "driver_fault");
      latchDriverFault();
      return;
    }
    Serial.printf("DRV wake success: nFAULT rose HIGH after %lu us\n",
                  static_cast<unsigned long>(DRV_WAKE_DELAY_US + waitUs));
#endif
    m_status.driverAwake = true;
    portENTER_CRITICAL(&m_guardMux);
    GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
    m_driverFaultPending = false;
    m_monitorDriverFault = false;  // Keep false until PWM is applied and 2ms activation blanking passes
    portEXIT_CRITICAL(&m_guardMux);
    pollDriverFault(false);
    if (faultLatched() || !outputDeadlineSafe()) {
      disableDriver();
      return;
    }
#if defined(ARDUINO) && defined(ESP32)
    Serial.printf("DRV awake: nSLEEP=%d nFAULT=%d rawDuty=%.1f phase=%s\n",
                  digitalRead(PIN_DRV_SLEEP), digitalRead(PIN_DRV_FAULT),
                  rawDutyPercent, calibrationPhaseKey(m_calibration.phase));
#endif
  }
  if (!outputDeadlineSafe()) {
    disableDriver();
    return;
  }
  if (calibrationWrite) {
    portENTER_CRITICAL(&m_guardMux);
    const bool mayPulse = m_guardCalibrationActive && !m_calTotalExpired && !m_calPulseExpired &&
        !m_driverFaultPending && !m_sensorTimeoutPending;
    if (mayPulse && !m_guardCalibrationPulse) {
      m_guardPulseStartMs = millis();
      m_calPhaseMs = m_guardPulseStartMs;
      m_guardCalibrationPulse = true;
    }
    portEXIT_CRITICAL(&m_guardMux);
    if (!mayPulse) {
      disableDriver();
      return;
    }
  }
  const uint32_t count = static_cast<uint32_t>(lroundf(
      fabs(rawDutyPercent) / 100.0f * static_cast<float>(TEC_PWM_MAX_COUNT)));
#if defined(ESP_PLATFORM)
  const uint32_t hwCount = calibrationWrite
      ? min(count, static_cast<uint32_t>(TEC_PWM_MAX_COUNT * TEC_CAL_OUTPUT_LIMIT_PERCENT / 100))
      : count;
  if (rawDutyPercent > 0.01f) {
    ledcWrite(LEDC_CHANNEL_IN2, 0);
    ledcWrite(LEDC_CHANNEL_IN1, hwCount);
  } else if (rawDutyPercent < -0.01f) {
    ledcWrite(LEDC_CHANNEL_IN1, 0);
    ledcWrite(LEDC_CHANNEL_IN2, hwCount);
  } else {
    ledcWrite(LEDC_CHANNEL_IN1, 0);
    ledcWrite(LEDC_CHANNEL_IN2, 0);
  }
  if (calibrationWrite) {
    if (!m_monitorDriverFault) {
      // Blank the ~1ms bridge activation transient of DRV8701 during calibration
      delayMicroseconds(5000);
      Serial.printf("DRV PWM active: rawDuty=%.1f count=%lu IN1=%lu IN2=%lu nSLEEP=%d nFAULT=%d\n",
                    rawDutyPercent, static_cast<unsigned long>(hwCount),
                    static_cast<unsigned long>(rawDutyPercent > 0.01f ? hwCount : 0),
                    static_cast<unsigned long>(rawDutyPercent < -0.01f ? hwCount : 0),
                    digitalRead(PIN_DRV_SLEEP), digitalRead(PIN_DRV_FAULT));
      portENTER_CRITICAL(&m_guardMux);
      GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
      m_driverFaultPending = false;
      m_monitorDriverFault = true;
      portEXIT_CRITICAL(&m_guardMux);
    }
  } else if (!m_monitorDriverFault) {
    portENTER_CRITICAL(&m_guardMux);
    GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
    m_driverFaultPending = false;
    m_monitorDriverFault = true;
    portEXIT_CRITICAL(&m_guardMux);
  }
#else
  if (rawDutyPercent > 0.01f) {
    ledcWrite(LEDC_CHANNEL_IN2, 0);
    ledcWrite(LEDC_CHANNEL_IN1, count);
  } else if (rawDutyPercent < -0.01f) {
    ledcWrite(LEDC_CHANNEL_IN1, 0);
    ledcWrite(LEDC_CHANNEL_IN2, count);
  } else {
    ledcWrite(LEDC_CHANNEL_IN1, 0);
    ledcWrite(LEDC_CHANNEL_IN2, 0);
  }
  if (!m_monitorDriverFault) {
    portENTER_CRITICAL(&m_guardMux);
    GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
    m_driverFaultPending = false;
    m_monitorDriverFault = true;
    portEXIT_CRITICAL(&m_guardMux);
  }
#endif
#else
  if (!armOutputIfFresh()) {
    pollDriverFault(false);
    return;
  }
  m_status.driverAwake = true;
  if (calibrationWrite && !m_guardCalibrationPulse) {
    m_guardPulseStartMs = millis();
    m_calPhaseMs = m_guardPulseStartMs;
    m_guardCalibrationPulse = true;
  }
#endif
  m_lastDirection = direction;
  pollDriverFault(false);
  if (!outputDeadlineSafe()) disableDriver();
}

bool ThermalController::outputDeadlineSafe() {
  checkSampleAgeFromTimer(millis());
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  const bool safe = !m_driverFaultPending && !m_sensorTimeoutPending &&
      (m_calibration.active
           ? m_guardCalibrationActive && !m_calPulseExpired && !m_calTotalExpired &&
                 (m_calibration.phase == CalibrationPhase::Forward || m_calibration.phase == CalibrationPhase::Reverse)
           : m_settings.enabled && m_tecDirection != TecDirection::Unknown);
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif
  return safe;
}

bool ThermalController::armOutputIfFresh() {
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  const bool fresh = m_guardHaveValidSample &&
      static_cast<uint32_t>(millis() - m_guardLastSampleMs) < SENSOR_STALE_TIMEOUT_MS;
  if (!fresh) {
    m_sensorTimeoutPending = true;
  }
  const bool allowedMode = m_calibration.active
      ? m_guardCalibrationActive && !m_calPulseExpired && !m_calTotalExpired &&
            (m_calibration.phase == CalibrationPhase::Forward || m_calibration.phase == CalibrationPhase::Reverse)
      : m_settings.enabled && m_tecDirection != TecDirection::Unknown;
  const bool mayWake = m_outputGuardReady && fresh && allowedMode &&
      !m_driverFaultPending && !m_sensorTimeoutPending;
  if (mayWake) {
    m_outputWatchdogArmed = true;
#if defined(ARDUINO) && defined(ESP32)
    digitalWrite(PIN_DRV_SLEEP, HIGH);
#endif
  }
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif
  return mayWake;
}

void ThermalController::disableDriver() {
  if (m_manualWakeHold) {
#if defined(ARDUINO) && defined(ESP32)
    if (m_outputStarted) {
      ledcWrite(LEDC_CHANNEL_IN1, 0);
      ledcWrite(LEDC_CHANNEL_IN2, 0);
    }
#endif
    return;
  }

  const bool wasActive = m_status.driverAwake || m_lastDirection != 0 || m_outputWatchdogArmed;

#if defined(ESP_PLATFORM)
  if (m_outputStarted) {
    // 1. Immediately zero PWM output while nSLEEP is still HIGH.
    // IN1=IN2=0 selects coast, not low-side brake, on DRV8701P.
    ledcWrite(LEDC_CHANNEL_IN1, 0);
    ledcWrite(LEDC_CHANNEL_IN2, 0);
    // Freewheel current to ground for 2000us (2 full 1kHz PWM cycles) so the PWM cycle
    // finishes and inductive current completely decays, eliminating back-EMF kickback into 5V_TEC
    esp_rom_delay_us(2000);
  }
#endif

  portENTER_CRITICAL(&m_guardMux);
  m_outputWatchdogArmed = false;
  digitalWrite(PIN_DRV_SLEEP, LOW);
  portEXIT_CRITICAL(&m_guardMux);

#if defined(ARDUINO) && defined(ESP32)
  if (m_outputStarted) {
    ledcWrite(LEDC_CHANNEL_IN1, 0);
    ledcWrite(LEDC_CHANNEL_IN2, 0);
  }
#else
  m_outputWatchdogArmed = false;
#endif
  portENTER_CRITICAL(&m_guardMux);
  m_monitorDriverFault = false;
  portEXIT_CRITICAL(&m_guardMux);

  m_status.driverAwake = false;
  m_lastDirection = 0;
#if defined(ARDUINO) && defined(ESP32)
  if (wasActive) {
    Serial.printf("DRV output disabled: nSLEEP=%d nFAULT=%d pendingFault=%d timeout=%d\n",
                  digitalRead(PIN_DRV_SLEEP), digitalRead(PIN_DRV_FAULT),
                  m_driverFaultPending ? 1 : 0, m_sensorTimeoutPending ? 1 : 0);
  }
#endif
}

void ThermalController::latchDriverFault() {
  m_settings.enabled = false;
  m_inBalance = false;
  m_status.balanceDrivePercent = 0.0f;
  m_status.state = ControllerState::DriverFaultLatched;
  stopOutput();
}

void ThermalController::pollDriverFault(bool advanceCalibration) {
  checkSampleAgeFromTimer(millis());
#if defined(ARDUINO) && defined(ESP32)
  if (digitalRead(PIN_DRV_SLEEP) == LOW) {
    m_status.driverAwake = false;
  }
  portENTER_CRITICAL(&m_guardMux);
#if defined(ESP_PLATFORM)
  if (m_monitorDriverFault && digitalRead(PIN_DRV_FAULT) == HIGH && digitalRead(PIN_DRV_SLEEP) == HIGH) {
    m_driverFaultPending = false;
    GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
  }
  const bool driverFault = m_driverFaultPending ||
      (m_monitorDriverFault && digitalRead(PIN_DRV_FAULT) == LOW && digitalRead(PIN_DRV_SLEEP) == HIGH);
#else
  const bool driverFault = m_driverFaultPending ||
      (m_monitorDriverFault && (digitalRead(PIN_DRV_FAULT) == LOW ||
                                (GPIO.status & (uint32_t{1} << PIN_DRV_FAULT)) != 0));
#endif
  const bool timedOut = m_sensorTimeoutPending;
  portEXIT_CRITICAL(&m_guardMux);
#else
  const bool driverFault = m_driverFaultPending;
  const bool timedOut = m_sensorTimeoutPending;
#endif
#if defined(ARDUINO) && defined(ESP32)
  if (driverFault && m_status.state != ControllerState::DriverFaultLatched &&
      m_status.state != ControllerState::SafetyFaultLatched) {
    Serial.printf("DRV nFAULT asserted: pin=%d pending=%d phase=%s\n",
                  digitalRead(PIN_DRV_FAULT), m_driverFaultPending ? 1 : 0,
                  calibrationPhaseKey(m_calibration.phase));
  }
  if (timedOut && m_status.state != ControllerState::SensorTimeoutLatched &&
      m_status.state != ControllerState::ThermalFaultLatched &&
      m_status.state != ControllerState::DriverFaultLatched &&
      m_status.state != ControllerState::SafetyFaultLatched) {
    Serial.printf("DRV output watchdog expired: sampleAge=%lu ms phase=%s\n",
                  static_cast<unsigned long>(millis() - m_guardLastSampleMs),
                  calibrationPhaseKey(m_calibration.phase));
  }
#endif
  if (driverFault && m_status.state != ControllerState::SafetyFaultLatched) {
    if (m_calibration.active) finishCalibration(CalibrationPhase::Failed, "driver_fault");
    latchDriverFault();
  }
  if (m_calibration.active && m_status.state == ControllerState::SafetyFaultLatched) {
    finishCalibration(CalibrationPhase::Failed, "safety_fault");
  }
  if (timedOut && m_status.state != ControllerState::ThermalFaultLatched &&
      m_status.state != ControllerState::DriverFaultLatched &&
      m_status.state != ControllerState::SensorTimeoutLatched &&
      m_status.state != ControllerState::SafetyFaultLatched) {
    m_enabledBeforeTimeout = m_settings.enabled;
    m_settings.enabled = false;
    if (m_calibration.active) finishCalibration(CalibrationPhase::Failed, "sensor_stale");
    invalidateSample();
    m_status.state = hardRecoveryActive() ? ControllerState::ThermalFaultLatched
                                         : ControllerState::SensorTimeoutLatched;
    stopOutput();
  }
  // Diagnostic freshness still expires while output is off. A latched timeout
  // must not repeatedly erase fresh samples collected for fault diagnosis.
  if (m_havePreviousSample &&
      static_cast<uint32_t>(millis() - m_lastValidSampleMs) >= SENSOR_STALE_TIMEOUT_MS) {
    if (m_calibration.active) finishCalibration(CalibrationPhase::Failed, "sensor_stale");
    invalidateSample();
    stopOutput();
  }
  if (advanceCalibration) serviceCalibration();
}

bool ThermalController::faultLatched() const {
  return m_status.state == ControllerState::ThermalFaultLatched ||
      m_status.state == ControllerState::DriverFaultLatched ||
      m_status.state == ControllerState::SensorTimeoutLatched ||
      m_status.state == ControllerState::SafetyFaultLatched;
}

bool ThermalController::hardRecoveryActive() const {
  return m_status.state == ControllerState::HardOverTemperature ||
      m_status.state == ControllerState::HardUnderTemperature;
}

void ThermalController::checkSampleAgeFromTimer(uint32_t millisNow) {
  bool cut = false;
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  if ((m_outputWatchdogArmed || m_guardCalibrationActive) &&
      static_cast<int32_t>(millisNow - m_guardLastSampleMs) >=
          static_cast<int32_t>(SENSOR_STALE_TIMEOUT_MS)) {
    m_sensorTimeoutPending = true;
    cut = true;
  }
  if (m_guardCalibrationActive &&
      static_cast<int32_t>(millisNow - m_guardCalibrationStartMs) >=
          static_cast<int32_t>(m_calTotalTimeoutMs)) {
    m_calTotalExpired = true;
    cut = true;
  }
  if (m_guardCalibrationPulse &&
      static_cast<int32_t>(millisNow - m_guardPulseStartMs) >=
          static_cast<int32_t>(m_calPulseMs)) {
    m_calPulseExpired = true;
    cut = true;
  }
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif

  if (cut) {
#if defined(ESP_PLATFORM)
    if (!m_manualWakeHold) {
      // 1. Immediately zero PWM output while nSLEEP is still HIGH.
      // IN1=IN2=0 selects coast; current decay depends on the physical circuit.
      ledcWrite(LEDC_CHANNEL_IN1, 0);
      ledcWrite(LEDC_CHANNEL_IN2, 0);
      // Freewheel current to ground for 2000us so current dissipates before gate drive drops
      esp_rom_delay_us(2000);
      portENTER_CRITICAL(&m_guardMux);
      if (!m_manualWakeHold) {
        GPIO.out_w1tc = uint32_t{1} << PIN_DRV_SLEEP;
      }
      portEXIT_CRITICAL(&m_guardMux);
    }
#else
    if (!m_manualWakeHold) {
      GPIO.out_w1tc = uint32_t{1} << PIN_DRV_SLEEP;
    }
#endif
  }
}

bool ThermalController::setTecDirection(TecDirection direction) {
  if (direction != TecDirection::Unknown && direction != TecDirection::Normal &&
      direction != TecDirection::Reversed) return false;
  if (m_calibration.active || m_settings.enabled || m_status.driverAwake ||
      fabs(m_status.signedDutyPercent) > 0.01f) return false;
  pollDriverFault(false);
  if (direction != TecDirection::Unknown && faultLatched()) return false;
  m_tecDirection = direction;
  m_inBalance = false;
  m_status.balanceDrivePercent = 0;
  return true;
}

bool ThermalController::canStartDirectionCalibration() {
  pollDriverFault(false);
  return !m_calibration.active && !m_settings.enabled && !m_status.driverAwake &&
      !faultLatched() && m_outputStarted && m_outputGuardReady &&
      m_status.sensorOk && m_havePreviousSample && isfinite(m_status.rawTemperatureC) &&
      static_cast<uint32_t>(millis() - m_lastValidSampleMs) <= TEC_CAL_MAX_SAMPLE_GAP_MS &&
      m_status.rawTemperatureC >= TEC_CAL_START_MIN_C &&
      m_status.rawTemperatureC <= TEC_CAL_START_MAX_C;
}

bool ThermalController::startDirectionCalibration() {
  if (!canStartDirectionCalibration()) return false;
  m_settings.enabled = false;
  m_tecDirection = TecDirection::Unknown;
  m_inBalance = false;
  m_status.balanceDrivePercent = 0;
  m_calibration = DirectionCalibrationStatus{};
  m_calibration.active = true;
  m_status.state = ControllerState::DirectionCalibration;
  m_calStartTemperatureC = m_status.rawTemperatureC;
  m_calForwardCorrectedC = NAN;
  m_calFinishedElapsedMs = 0;
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  m_guardCalibrationActive = true;
  m_guardCalibrationStartMs = millis();
  m_guardCalibrationPulse = false;
  m_guardPulseStartMs = 0;
  m_calTotalExpired = false;
  m_calPulseExpired = false;
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif
  enterCalibrationPhase(CalibrationPhase::BaselineForward, millis());
  return true;
}

void ThermalController::cancelDirectionCalibration() {
  if (m_calibration.active ||
      (m_calibration.phase == CalibrationPhase::Complete && m_tecDirection == TecDirection::Unknown)) {
    finishCalibration(CalibrationPhase::Cancelled, "cancelled");
  }
}

DirectionCalibrationStatus ThermalController::directionCalibrationStatus() const {
  DirectionCalibrationStatus result = m_calibration;
  result.phaseElapsedMs = result.active ? millis() - m_calPhaseMs : m_calFinishedElapsedMs;
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&const_cast<ThermalController*>(this)->m_guardMux);
#endif
  const bool cutoff = m_calPulseExpired || m_calTotalExpired ||
      m_sensorTimeoutPending || m_driverFaultPending;
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&const_cast<ThermalController*>(this)->m_guardMux);
#endif
  if (cutoff) result.rawDutyPercent = 0;
  return result;
}

void ThermalController::finishCalibration(CalibrationPhase phase, const char* error) {
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  m_guardCalibrationActive = false;
  m_guardCalibrationPulse = false;
  m_calPulseExpired = false;
  m_calTotalExpired = false;
  const bool driverFault = m_driverFaultPending;
  const bool sensorTimeout = m_sensorTimeoutPending;
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif

  m_calFinishedElapsedMs = millis() - m_calPhaseMs;
  m_calibration.active = false;
  m_calibration.phase = phase;
  m_calibration.rawDutyPercent = 0;
  m_calibration.error = error;

  disableDriver();

  if (driverFault && m_status.state != ControllerState::SafetyFaultLatched) {
    phase = CalibrationPhase::Failed;
    error = "driver_fault";
    m_status.state = ControllerState::DriverFaultLatched;
    m_calibration.phase = phase;
    m_calibration.error = error;
  } else if (sensorTimeout && !faultLatched()) {
    phase = CalibrationPhase::Failed;
    error = "sensor_stale";
    m_status.state = ControllerState::SensorTimeoutLatched;
    m_calibration.phase = phase;
    m_calibration.error = error;
  }
  if (phase != CalibrationPhase::Complete) m_calibration.candidate = TecDirection::Unknown;
  m_tecDirection = TecDirection::Unknown;
  m_settings.enabled = false;
  m_inBalance = false;
  m_status.signedDutyPercent = 0;
  m_status.coolingDutyPercent = 0;
  m_status.heatingDutyPercent = 0;
  m_status.balanceDrivePercent = 0;
  if (!faultLatched()) m_status.state = ControllerState::Disabled;
#if defined(ARDUINO) && defined(ESP32)
  Serial.printf("TEC calibration finished: phase=%s error=%s candidate=%s forward=%.3f reverse=%.3f\n",
                calibrationPhaseKey(phase), error ? error : "",
                tecDirectionKey(m_calibration.candidate),
                m_calibration.forwardDeltaC, m_calibration.reverseDeltaC);
#endif
}

void ThermalController::enterCalibrationPhase(CalibrationPhase phase, uint32_t now) {
  m_calibration.phase = phase;
  m_calPhaseMs = now;
  m_calSampleCount = 0;
  m_calibration.rawDutyPercent = 0;
  m_status.signedDutyPercent = 0;
  m_status.coolingDutyPercent = 0;
  m_status.heatingDutyPercent = 0;
#if defined(ARDUINO) && defined(ESP32)
  Serial.printf("TEC calibration phase: %s rawDuty=%.1f elapsed=%lu ms nFAULT=%d\n",
                calibrationPhaseKey(phase), phase == CalibrationPhase::Forward ? 100.0f
                                                                                : phase == CalibrationPhase::Reverse ? -100.0f : 0.0f,
                static_cast<unsigned long>(now - m_guardCalibrationStartMs),
                digitalRead(PIN_DRV_FAULT));
#endif
  if (phase == CalibrationPhase::BaselineForward || phase == CalibrationPhase::BaselineReverse) {
    // The sample that made the explicit detection request eligible is already
    // valid. Keep it as t=0 of the baseline so ordinary loop jitter cannot
    // make the first powered pulse fail before it ever reaches the bridge.
    if (m_status.sensorOk && m_havePreviousSample && isfinite(m_status.rawTemperatureC)) {
      recordCalibrationSample(m_status.rawTemperatureC, now);
    }
  }
  if (phase == CalibrationPhase::Forward || phase == CalibrationPhase::Reverse) {
#if defined(ARDUINO) && defined(ESP32)
    portENTER_CRITICAL(&m_guardMux);
#endif
    m_guardCalibrationPulse = true;
    m_guardPulseStartMs = now;
    m_calPhaseMs = now;
    m_calPulseExpired = false;
#if defined(ARDUINO) && defined(ESP32)
    portEXIT_CRITICAL(&m_guardMux);
#endif
    m_calibration.rawDutyPercent = phase == CalibrationPhase::Forward ? 100.0f : -100.0f;
    writeRawHBridge(m_calibration.rawDutyPercent);
  } else {
    disableDriver();
#if defined(ARDUINO) && defined(ESP32)
    portENTER_CRITICAL(&m_guardMux);
#endif
    m_guardCalibrationPulse = false;
    m_calPulseExpired = false;
#if defined(ARDUINO) && defined(ESP32)
    portEXIT_CRITICAL(&m_guardMux);
#endif
  }
}

void ThermalController::recordCalibrationSample(float temperatureC, uint32_t ms) {
  if (m_calibration.phase == CalibrationPhase::Settle) return;
  // Never count samples acquired before the phase, duplicates, or late pulse data.
  if (static_cast<int32_t>(ms - m_calPhaseMs) < 0) return;
  if (m_calSampleCount && static_cast<int32_t>(ms - m_calSamples[m_calSampleCount - 1].ms) <= 0) return;
  if ((m_calibration.phase == CalibrationPhase::Forward || m_calibration.phase == CalibrationPhase::Reverse) &&
      ms - m_calPhaseMs > m_calPulseMs) return;
  if (m_calSampleCount == 200) return;
  m_calSamples[m_calSampleCount].ms = ms;
  m_calSamples[m_calSampleCount].temperatureC = temperatureC;
  ++m_calSampleCount;
}

bool ThermalController::calibrationBaseline(float& slope) const {
  if (m_calSampleCount < 8) return false;
  const auto& first = m_calSamples[0];
  const auto& last = m_calSamples[m_calSampleCount - 1];
  if (last.ms - first.ms < 1600u || first.ms - m_calPhaseMs > TEC_CAL_MAX_SAMPLE_GAP_MS ||
      millis() - last.ms > TEC_CAL_MAX_SAMPLE_GAP_MS) return false;
  float minC = first.temperatureC, maxC = first.temperatureC;
  float sumT = 0, sumY = 0, sumTT = 0, sumTY = 0;
  for (uint16_t i = 0; i < m_calSampleCount; ++i) {
    if (i && m_calSamples[i].ms - m_calSamples[i - 1].ms > TEC_CAL_MAX_SAMPLE_GAP_MS) return false;
    const float t = (m_calSamples[i].ms - first.ms) / 1000.0f;
    const float y = m_calSamples[i].temperatureC - first.temperatureC;
    sumT += t; sumY += y; sumTT += t * t; sumTY += t * y;
    minC = min(minC, m_calSamples[i].temperatureC);
    maxC = max(maxC, m_calSamples[i].temperatureC);
  }
  const float denominator = m_calSampleCount * sumTT - sumT * sumT;
  if (denominator <= 0) return false;
  slope = (m_calSampleCount * sumTY - sumT * sumY) / denominator;
#if defined(ARDUINO) && defined(ESP32)
  Serial.printf("TEC baseline: phase=%s count=%u slope=%.4f range=%.4f (limit: slope<=%.4f range<=%.4f)\n",
                calibrationPhaseKey(m_calibration.phase), m_calSampleCount, slope, maxC - minC,
                m_calMaxBaselineSlope, m_calMaxBaselineRange);
#endif
  return isfinite(slope) && fabs(slope) <= m_calMaxBaselineSlope &&
      maxC - minC <= m_calMaxBaselineRange;
}

bool ThermalController::finishCalibrationPulse() {
  if (m_calSampleCount < 10) return false;
  const auto& last = m_calSamples[m_calSampleCount - 1];
  if (m_calSamples[0].ms - m_calPhaseMs > TEC_CAL_MAX_SAMPLE_GAP_MS ||
      last.ms - m_calPhaseMs < m_calPulseMs - TEC_CAL_MAX_SAMPLE_GAP_MS ||
      millis() - last.ms > TEC_CAL_MAX_SAMPLE_GAP_MS) return false;
  for (uint16_t i = 1; i < m_calSampleCount; ++i) {
    if (m_calSamples[i].ms - m_calSamples[i - 1].ms > TEC_CAL_MAX_SAMPLE_GAP_MS) return false;
  }
  float endC = 0;
  float endSeconds = 0;
  for (uint16_t i = m_calSampleCount - 3; i < m_calSampleCount; ++i) {
    endC += m_calSamples[i].temperatureC / 3.0f;
    endSeconds += (m_calSamples[i].ms - m_calPulseBaseline.ms) / 3000.0f;
  }
  const float delta = endC - m_calPulseBaseline.temperatureC;
  const float corrected = delta - m_calBaselineSlope * endSeconds;
  if (m_calibration.phase == CalibrationPhase::Forward) {
    m_calibration.forwardDeltaC = delta;
    m_calForwardCorrectedC = corrected;
  } else {
    m_calibration.reverseDeltaC = delta;
    const float threshold = TEC_CAL_MIN_RESPONSE_C;
    if (m_calibration.forwardDeltaC <= -threshold && m_calForwardCorrectedC <= -threshold &&
        delta >= threshold && corrected >= threshold) {
      m_calibration.candidate = TecDirection::Normal;
    } else if (m_calibration.forwardDeltaC >= threshold && m_calForwardCorrectedC >= threshold &&
               delta <= -threshold && corrected <= -threshold) {
      m_calibration.candidate = TecDirection::Reversed;
    }
  }
  return isfinite(delta) && isfinite(corrected);
}

void ThermalController::serviceCalibration() {
  if (!m_calibration.active || m_calServiceBusy) return;
  struct ServiceGuard {
    bool& busy;
    explicit ServiceGuard(bool& value) : busy(value) { busy = true; }
    ~ServiceGuard() { busy = false; }
  } guard(m_calServiceBusy);
  const uint32_t now = millis();
#if defined(ARDUINO) && defined(ESP32)
  portENTER_CRITICAL(&m_guardMux);
#endif
  const bool pulseExpired = m_calPulseExpired;
  const bool totalExpired = m_calTotalExpired;
#if defined(ARDUINO) && defined(ESP32)
  portEXIT_CRITICAL(&m_guardMux);
#endif
  if (totalExpired) {
    finishCalibration(CalibrationPhase::Failed, "total_timeout");
    return;
  }
  if (!m_status.sensorOk || !isfinite(m_status.rawTemperatureC)) {
    finishCalibration(CalibrationPhase::Failed, "sensor_invalid");
    return;
  }
  if (m_status.rawTemperatureC <= TEC_CAL_ABORT_MIN_C || m_status.rawTemperatureC >= TEC_CAL_ABORT_MAX_C) {
    finishCalibration(CalibrationPhase::Failed, "temperature_bounds");
    return;
  }
  if (fabs(m_status.rawTemperatureC - m_calStartTemperatureC) >= m_calMaxExcursionC) {
    finishCalibration(CalibrationPhase::Failed, "temperature_excursion");
    return;
  }
  switch (m_calibration.phase) {
    case CalibrationPhase::BaselineForward:
    case CalibrationPhase::BaselineReverse:
      if (now - m_calPhaseMs >= m_calBaselineMs) {
        if (!calibrationBaseline(m_calBaselineSlope)) {
          finishCalibration(CalibrationPhase::Failed, "baseline_unstable");
          return;
        }
        m_calPulseBaseline = m_calSamples[m_calSampleCount - 1];
        enterCalibrationPhase(m_calibration.phase == CalibrationPhase::BaselineForward
                                  ? CalibrationPhase::Forward : CalibrationPhase::Reverse, now);
      }
      return;
    case CalibrationPhase::Forward:
    case CalibrationPhase::Reverse:
      if (pulseExpired) {
        disableDriver();
        m_calibration.rawDutyPercent = 0;
        pollDriverFault(false);
        if (!m_calibration.active) return;
        if (!finishCalibrationPulse()) {
          finishCalibration(CalibrationPhase::Failed, "pulse_samples");
          return;
        }
        if (m_calibration.phase == CalibrationPhase::Forward) {
          enterCalibrationPhase(CalibrationPhase::Settle, now);
        } else {
          const bool known = m_calibration.candidate != TecDirection::Unknown;
          finishCalibration(known ? CalibrationPhase::Complete : CalibrationPhase::Failed,
                            known ? "" : "response_ambiguous");
        }
      } else {
        writeRawHBridge(m_calibration.rawDutyPercent);
      }
      return;
    case CalibrationPhase::Settle:
      if (now - m_calPhaseMs >= m_calSettleMs) {
        enterCalibrationPhase(CalibrationPhase::BaselineReverse, now);
      }
      return;
    default:
      finishCalibration(CalibrationPhase::Failed, "invalid_phase");
  }
}

#if defined(ARDUINO) && defined(ESP32)
void IRAM_ATTR ThermalController::onDriverFault(void* argument) {
  auto* controller = static_cast<ThermalController*>(argument);
  portENTER_CRITICAL_ISR(&controller->m_guardMux);
  if (controller->m_monitorDriverFault) {
#if defined(ESP_PLATFORM)
    // On physical DRV8701 hardware: if the driver is asleep (nSLEEP == LOW),
    // nFAULT is naturally pulled LOW as part of sleep mode. It is NOT a fault!
    if (digitalRead(PIN_DRV_SLEEP) == LOW) {
      GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
      portEXIT_CRITICAL_ISR(&controller->m_guardMux);
      return;
    }
    // On physical DRV8701 hardware, a real fault holds nFAULT LOW for >=1.6ms (1600us).
    // Inductive switching glitches last <1-5us.
    // Filter out sub-15us glitches before latching a catastrophic driver fault.
    if (digitalRead(PIN_DRV_FAULT) == LOW) {
      esp_rom_delay_us(15);
      if (digitalRead(PIN_DRV_FAULT) == LOW) {
        controller->m_driverFaultPending = true;
        if (!controller->m_manualWakeHold) {
          GPIO.out_w1tc = uint32_t{1} << PIN_DRV_SLEEP;
        }
      } else {
        GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
      }
    } else {
      GPIO.status_w1tc = uint32_t{1} << PIN_DRV_FAULT;
    }
#else
    controller->m_driverFaultPending = true;
    if (!controller->m_manualWakeHold) {
      GPIO.out_w1tc = uint32_t{1} << PIN_DRV_SLEEP;
    }
#endif
  }
  portEXIT_CRITICAL_ISR(&controller->m_guardMux);
}
#else
void ThermalController::onDriverFault(void*) {
}
#endif

float ThermalController::clampFloat(float value, float lowest, float highest) {
  return min(max(value, lowest), highest);
}

void ThermalController::setManualWakeHold(bool hold) {
  m_manualWakeHold = hold;
#if defined(ARDUINO) && defined(ESP32)
  if (hold) {
    digitalWrite(PIN_DRV_SLEEP, HIGH);
    m_status.driverAwake = true;
    Serial.printf("[MANUAL DEBUG] DRV8701 nSLEEP set to HIGH (Held Awake). Current nFAULT=%d\n",
                  digitalRead(PIN_DRV_FAULT));
  } else {
    m_debugIn1Duty = 0;
    m_debugIn2Duty = 0;
    if (m_outputStarted) {
      ledcWrite(LEDC_CHANNEL_IN1, 0);
      ledcWrite(LEDC_CHANNEL_IN2, 0);
    }
    digitalWrite(PIN_DRV_SLEEP, LOW);
    m_status.driverAwake = false;
    Serial.printf("[MANUAL DEBUG] DRV8701 nSLEEP set to LOW (Sleep Mode). Current nFAULT=%d\n",
                  digitalRead(PIN_DRV_FAULT));
  }
#endif
}

void ThermalController::setDebugPwm(int in1Duty, int in2Duty) {
  m_debugIn1Duty = static_cast<int>(clampFloat(static_cast<float>(in1Duty), 0.0f, static_cast<float>(TEC_PWM_MAX_COUNT)));
  m_debugIn2Duty = static_cast<int>(clampFloat(static_cast<float>(in2Duty), 0.0f, static_cast<float>(TEC_PWM_MAX_COUNT)));
#if defined(ARDUINO) && defined(ESP32)
  if (m_manualWakeHold && m_outputStarted) {
    ledcWrite(LEDC_CHANNEL_IN1, m_debugIn1Duty);
    ledcWrite(LEDC_CHANNEL_IN2, m_debugIn2Duty);
    Serial.printf("[MANUAL DEBUG] PWM output updated: IN1=%d IN2=%d (nSLEEP=%d nFAULT=%d)\n",
                  m_debugIn1Duty, m_debugIn2Duty,
                  digitalRead(PIN_DRV_SLEEP), digitalRead(PIN_DRV_FAULT));
  } else {
    Serial.println("[MANUAL DEBUG] Wake hold not active or PWM not started. Call setManualWakeHold(true) first.");
  }
#endif
}
