#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <soc/gpio_struct.h>

#include <array>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "thermal_controller.h"

namespace {
#if defined(ESP_PLATFORM)
constexpr uint32_t CALIBRATION_PWM_COUNT = TEC_PWM_MAX_COUNT * TEC_CAL_OUTPUT_LIMIT_PERCENT / 100;
#else
constexpr uint32_t CALIBRATION_PWM_COUNT = TEC_PWM_MAX_COUNT;
#endif
struct Event {
  std::string kind;
  uint32_t index;
  uint32_t value;
};
std::array<uint8_t, 64> pinValues{};
std::array<uint32_t, 16> duties{};
std::vector<Event> events;
void (*faultCallback)(void*) = nullptr;
void* faultArgument = nullptr;
int failingChannel = -1;
int failingAttachChannel = -1;
bool wrongFrequency = false;
uint64_t fakeMicros = 0;
int criticalDepth = 0;
bool holdFaultInterrupt = false;
enum class FaultIrqFailure { None, InstallMemory, InstallInterrupt, ExistingService, Configure, Add };
FaultIrqFailure faultIrqFailure = FaultIrqFailure::None;
bool gpioServiceInstalled = false;
bool faultInterruptEnabled = false;
bool flashCacheOff = false;
int gpioServiceFlags = 0;
gpio_int_type_t faultInterruptType = GPIO_INTR_DISABLE;
std::function<void()> beforeSleepCut;
std::function<void()> afterSleepCut;
std::function<void()> sleepDelayHook;
std::function<void()> wakeDelayHook;
std::function<void(uint8_t, uint32_t)> pwmWriteHook;
std::function<void()> criticalExitHook;

void setTimeMs(uint32_t now) { fakeMicros = uint64_t{now} * 1000; }

class TestController : public ThermalController {
 public:
  void submitTemperature(bool valid, float temperature, uint32_t sampleMs) {
    setTimeMs(sampleMs);
    ThermalController::submitTemperature(valid, temperature, sampleMs);
  }
};

void runOnce(std::function<void()>& hook) {
  auto action = std::move(hook);
  hook = nullptr;
  if (action) action();
}

void deliverFaultInterrupt() {
  const uint32_t mask = uint32_t{1} << PIN_DRV_FAULT;
  if (criticalDepth == 0 && !holdFaultInterrupt && faultInterruptEnabled &&
      (!flashCacheOff || (gpioServiceFlags & ESP_INTR_FLAG_IRAM) != 0) &&
      (GPIO.status & mask) != 0) {
    GPIO.status &= ~mask;  // GPIO service acknowledges an edge before the callback.
    if (faultCallback) faultCallback(faultArgument);
  }
}

void queueFaultPulse() {
  pinValues[PIN_DRV_FAULT] = LOW;
  GPIO.status |= uint32_t{1} << PIN_DRV_FAULT;
  pinValues[PIN_DRV_FAULT] = HIGH;
  deliverFaultInterrupt();
}

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void prepare(ThermalController& controller, bool startPwm = true, bool confirmed = true,
             FaultIrqFailure irqFailure = FaultIrqFailure::None) {
  pinValues.fill(LOW);
  pinValues[PIN_DRV_FAULT] = HIGH;
  duties.fill(0);
  events.clear();
  faultCallback = nullptr;
  faultArgument = nullptr;
  failingChannel = -1;
  failingAttachChannel = -1;
  wrongFrequency = false;
  fakeMicros = 0;
  criticalDepth = 0;
  holdFaultInterrupt = false;
  faultIrqFailure = irqFailure;
  gpioServiceInstalled = irqFailure == FaultIrqFailure::ExistingService;
  faultInterruptEnabled = false;
  flashCacheOff = false;
  gpioServiceFlags = 0;
  faultInterruptType = GPIO_INTR_DISABLE;
  GPIO.status = 0;
  beforeSleepCut = nullptr;
  afterSleepCut = nullptr;
  sleepDelayHook = nullptr;
  wakeDelayHook = nullptr;
  pwmWriteHook = nullptr;
  criticalExitHook = nullptr;
  controller.begin();
  if (confirmed) {
    require(controller.setTecDirection(TecDirection::Normal), "legacy cases use a confirmed mapping");
  }
  if (startPwm) {
    controller.startPwm(true);
  }
}

void enable(ThermalController& controller) {
  controller.setSettings(25.0f, 30.0f, 20.0f, true);
}

void assertOff(const ThermalController& controller) {
  require(pinValues[PIN_DRV_SLEEP] == LOW, "nSLEEP must be low");
  require(duties[LEDC_CHANNEL_IN1] == 0 && duties[LEDC_CHANNEL_IN2] == 0,
          "both PWM channels must be zero");
  require(!controller.status().driverAwake, "driver status must be asleep");
  require(controller.status().signedDutyPercent == 0.0f, "reported output must be zero");
}

void faultPulse() {
  pinValues[PIN_DRV_FAULT] = LOW;
  require(faultCallback != nullptr, "fault ISR must be registered");
  faultCallback(faultArgument);
  require(pinValues[PIN_DRV_SLEEP] == LOW, "ISR must lower nSLEEP before main-loop polling");
  pinValues[PIN_DRV_FAULT] = HIGH;
}

void startupOff() {
  TestController controller;
  prepare(controller);
  controller.submitTemperature(true, 60.0f, 1000);
  assertOff(controller);
  enable(controller);
  assertOff(controller);
  controller.submitTemperature(true, 30.0f, 1200);
  require(controller.status().driverAwake, "a valid sample may start the enabled controller");
  require(duties[LEDC_CHANNEL_IN1] > 0 && duties[LEDC_CHANNEL_IN2] == 0,
          "cooling must use IN1 only");
  require(std::any_of(events.begin(), events.end(), [](const Event& e) {
    return e.kind == "delay" && e.value >= DRV_WAKE_DELAY_US;
  }), "wake delay must precede output");
  controller.setSettings(25, 30, 20, false);
  assertOff(controller);
}

void invalidSensorStopsOutput() {
  for (const float value : {NAN, INFINITY, -INFINITY}) {
    TestController controller;
    prepare(controller);
    enable(controller);
    controller.submitTemperature(true, 20.0f, 1000);
    controller.submitTemperature(true, value, 1200);
    assertOff(controller);
    require(!controller.status().sensorOk, "nonfinite samples must be rejected");
  }
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 20.0f, 1000);
  controller.submitTemperature(false, 20.0f, 1200);
  assertOff(controller);
}

void wakeFailureLatches() {
  TestController controller;
  prepare(controller);
  pinValues[PIN_DRV_FAULT] = LOW;
  controller.pollDriverFault();
  require(controller.status().state == ControllerState::Disabled,
          "a sleeping driver's low fault pin is not an active fault");
  enable(controller);
  controller.submitTemperature(true, 30.0f, 1000);
  require(controller.status().state == ControllerState::DriverFaultLatched,
          "persistent nFAULT after wake must latch");
  assertOff(controller);
}

void transientFaultLatchesAndNeedsManualEnable() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30.0f, 1000);
  faultPulse();
  controller.pollDriverFault();
  require(controller.status().state == ControllerState::DriverFaultLatched,
          "a fault pulse must remain latched after the pin recovers");
  assertOff(controller);
  controller.submitTemperature(false, NAN, 1200);
  require(controller.status().state == ControllerState::DriverFaultLatched,
          "sensor failure must not erase a driver fault");
  enable(controller);
  controller.submitTemperature(true, 30.0f, 1400);
  assertOff(controller);
  require(!controller.settings().enabled, "enable must be rejected while latched");
  controller.resetFault();
  controller.submitTemperature(true, 30.0f, 1600);
  assertOff(controller);
  require(!controller.settings().enabled, "reset must not enable output");
  enable(controller);
  controller.submitTemperature(true, 30.0f, 1800);
  require(controller.status().driverAwake, "manual enable after reset may resume");
}

void reversalSleepsBeforeWaking() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 9.0f, 1000);
  require(duties[LEDC_CHANNEL_IN2] == TEC_PWM_MAX_COUNT, "low-limit recovery must heat");
  events.clear();
  controller.submitTemperature(true, 56.0f, 1200);
  require(duties[LEDC_CHANNEL_IN1] == TEC_PWM_MAX_COUNT && duties[LEDC_CHANNEL_IN2] == 0,
          "high-limit recovery must cool");
  int sleepIndex = -1;
  int delayIndex = -1;
  int wakeIndex = -1;
  for (size_t i = 0; i < events.size(); ++i) {
    const auto& e = events[i];
    if (e.kind == "pin" && e.index == PIN_DRV_SLEEP && e.value == LOW) sleepIndex = i;
    if (e.kind == "delay" && e.value == DRV_SLEEP_DELAY_US) delayIndex = i;
    if (e.kind == "pin" && e.index == PIN_DRV_SLEEP && e.value == HIGH) wakeIndex = i;
  }
  require(sleepIndex >= 0 && delayIndex > sleepIndex && wakeIndex > delayIndex,
          "reversal must sleep, wait, and then wake");
}

void pwmInitializationFailureStaysOff() {
  for (const int channel : {LEDC_CHANNEL_IN1, LEDC_CHANNEL_IN2}) {
    TestController controller;
    prepare(controller, false);
    failingChannel = channel;
    controller.startPwm(true);
    require(controller.status().state == ControllerState::SafetyFaultLatched,
            "PWM setup failure must latch a reboot-only safety fault");
    assertOff(controller);
    failingChannel = -1;
    controller.resetFault();
    enable(controller);
    controller.submitTemperature(true, 30.0f, 1000);
    require(controller.status().state == ControllerState::SafetyFaultLatched &&
                !controller.settings().enabled,
            "reset and enable must not bypass failed PWM initialization");
    assertOff(controller);
  }
}

void pwmPinBindingFailureStaysOff() {
  for (const int channel : {LEDC_CHANNEL_IN1, LEDC_CHANNEL_IN2}) {
    TestController controller;
    prepare(controller, false);
    failingAttachChannel = channel;
    controller.startPwm(true);
    require(controller.status().state == ControllerState::SafetyFaultLatched,
            "LEDC pin binding failure must latch a reboot-only safety fault");
    assertOff(controller);
    failingAttachChannel = -1;
    controller.resetFault();
    enable(controller);
    controller.submitTemperature(true, 30.0f, 1000);
    require(controller.status().state == ControllerState::SafetyFaultLatched &&
                !controller.settings().enabled,
            "reset and enable must not bypass failed LEDC pin binding");
    assertOff(controller);
  }
}

void hardRecoveryTimeoutIncludesHysteresis() {
  for (const bool hot : {true, false}) {
    TestController controller;
    prepare(controller);
    enable(controller);
    controller.submitTemperature(true, hot ? 56.0f : 9.0f, 1000);
    for (uint32_t now = 1200; now <= 31000; now += 200) {
      controller.submitTemperature(true, hot ? 54.0f : 10.5f, now);
    }
    require(controller.status().state == ControllerState::ThermalFaultLatched,
            "recovery timeout must continue inside the hysteresis band");
    assertOff(controller);
    controller.submitTemperature(false, NAN, 31200);
    require(controller.status().state == ControllerState::ThermalFaultLatched,
            "sensor failure must preserve a thermal latch");
  }
}

void hardRecoveryCanReturnToNormal() {
  for (const bool hot : {true, false}) {
    TestController controller;
    prepare(controller);
    enable(controller);
    controller.submitTemperature(true, hot ? 56.0f : 9.0f, 1000);
    controller.submitTemperature(true, hot ? 53.0f : 12.0f, 1200);
    require(controller.status().state == ControllerState::NormalControl,
            "a recovered temperature should return to normal control");
  }
}

void boardFaultPullupIsRedundantAndEnabled() {
  TestController controller;
  prepare(controller);
  require(std::any_of(events.begin(), events.end(), [](const Event& e) {
    return e.kind == "mode" && e.index == PIN_DRV_FAULT && e.value == INPUT_PULLUP;
  }), "nFAULT must enable the internal pullup alongside the board pullup");
}

void faultIrqInitializesWithoutPreexistingHandler() {
  TestController controller;
  prepare(controller);
  require(std::none_of(events.begin(), events.end(), [](const Event& e) {
    return e.kind == "irq-remove";
  }), "clean initialization must not remove a nonexistent nFAULT handler");
  require(std::any_of(events.begin(), events.end(), [](const Event& e) {
    return e.kind == "irq-install" && e.value == ESP_INTR_FLAG_IRAM;
  }), "nFAULT service must be installed with the IRAM flag");
  require(std::any_of(events.begin(), events.end(), [](const Event& e) {
    return e.kind == "irq-type" && e.index == PIN_DRV_FAULT &&
        e.value == GPIO_INTR_NEGEDGE;
  }), "nFAULT must be configured for falling edges");
  require(std::any_of(events.begin(), events.end(), [](const Event& e) {
    return e.kind == "irq-add" && e.index == PIN_DRV_FAULT;
  }), "nFAULT handler must be registered");
  require(controller.status().state != ControllerState::SafetyFaultLatched,
          "clean nFAULT initialization must not latch safety fault");
}

void staleSampleCutsBeforePollingAndLatches() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  controller.checkSampleAgeFromTimer(1999);
  require(pinValues[PIN_DRV_SLEEP] == HIGH, "fresh sample must not trip early");
  controller.checkSampleAgeFromTimer(2000);
  require(pinValues[PIN_DRV_SLEEP] == LOW, "timer must cut nSLEEP without main-loop polling");
  controller.pollDriverFault();
  require(controller.status().state == ControllerState::SensorTimeoutLatched,
          "pollDriverFault must latch sensor timeout");
  assertOff(controller);
  controller.submitTemperature(true, 30, 2001);
  require(controller.status().state != ControllerState::SensorTimeoutLatched,
          "a fresh sample must auto-unlock the timeout latch");
  require(controller.status().driverAwake, "fresh sample automatically resumes control");
}

void staleWatchdogHandlesWrapAndOlderCallback() {
  TestController controller;
  prepare(controller);
  enable(controller);
  const uint32_t start = UINT32_MAX - 500;
  controller.submitTemperature(true, 30, start);
  controller.checkSampleAgeFromTimer(start - 1);
  require(pinValues[PIN_DRV_SLEEP] == HIGH, "an older timer timestamp must not trip");
  controller.checkSampleAgeFromTimer(start + 999u);
  require(pinValues[PIN_DRV_SLEEP] == HIGH, "wrap must preserve elapsed time");
  controller.checkSampleAgeFromTimer(start + 1000u);
  controller.pollDriverFault();
  require(controller.status().state == ControllerState::SensorTimeoutLatched,
          "timeout must work across millis wrap");
  assertOff(controller);
}

void noGuardNoOutput() {
  TestController controller;
  prepare(controller, false);
  controller.startPwm(false);
  require(controller.status().state == ControllerState::SafetyFaultLatched,
          "missing guard must latch a safety fault");
  controller.resetFault();
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  assertOff(controller);
  require(controller.status().state == ControllerState::SafetyFaultLatched,
          "reset must not bypass missing guard");
}

void nonfiniteSettingsFailClosed() {
  for (const float value : {NAN, INFINITY, -INFINITY}) {
    TestController controller;
    prepare(controller);
    enable(controller);
    controller.submitTemperature(true, 30, 1000);
    controller.setSettings(value, 30, 20, true);
    assertOff(controller);
    require(isfinite(controller.settings().targetC), "invalid target must not be stored");
  }
}

void balanceKeepsApproachDirection() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  controller.submitTemperature(true, 30, 1200);
  // Set the target equal to the already-filtered sample while still cooling.
  controller.setSettings(30, 35, 20, true);
  controller.submitTemperature(true, 30, 1400);
  require(controller.status().state == ControllerState::BalanceControl, "must enter balance");
  require(controller.status().balanceDrivePercent < 0, "balance entry must retain cooling sign");
}

void wrongPwmFrequencyFailsClosed() {
  TestController controller;
  prepare(controller, false);
  wrongFrequency = true;
  controller.startPwm(true);
  require(controller.status().state == ControllerState::SafetyFaultLatched,
          "wrong PWM frequency must latch a reboot-only safety fault");
  assertOff(controller);
  wrongFrequency = false;
  controller.resetFault();
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  require(controller.status().state == ControllerState::SafetyFaultLatched &&
              !controller.settings().enabled,
          "reset and enable must not bypass a failed PWM frequency check");
  assertOff(controller);
}

void lostSensorHidesStaleTemperature() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  controller.submitTemperature(false, NAN, 1200);
  require(std::isnan(controller.status().filteredTemperatureC), "stale filtered temperature must clear");
  assertOff(controller);
}

void assertLatchedOff(const ThermalController& controller, ControllerState expected) {
  require(controller.status().state == expected, "expected safety latch must be retained");
  require(!controller.settings().enabled, "a safety latch must disable control");
  assertOff(controller);
}

void assertNoWakeSince(size_t firstEvent) {
  for (size_t i = firstEvent; i < events.size(); ++i) {
    const auto& event = events[i];
    require(!(event.kind == "pin" && event.index == PIN_DRV_SLEEP && event.value == HIGH),
            "nSLEEP must not rise after the safety event");
  }
}

void assertNoDriveSince(size_t firstEvent) {
  assertNoWakeSince(firstEvent);
  for (size_t i = firstEvent; i < events.size(); ++i) {
    require(events[i].kind != "pwm" || events[i].value == 0,
            "a rejected sample or fault handoff must not issue nonzero PWM");
  }
}

enum class RecoveryAbort { InvalidFlag, Nonfinite, ManualDisable };

void hardRecoveryAbortLatches(bool hot, RecoveryAbort abort, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, hot ? 56.0f : 9.0f, start);
  for (uint32_t elapsed = 200; elapsed <= 8800; elapsed += 200) {
    controller.submitTemperature(true, hot ? 54.0f : 10.5f, start + elapsed);
  }
  require(controller.status().state == (hot ? ControllerState::HardOverTemperature
                                           : ControllerState::HardUnderTemperature),
          "the recovery must still be active inside the hysteresis band");
  require(pinValues[PIN_DRV_SLEEP] == HIGH &&
              duties[hot ? LEDC_CHANNEL_IN1 : LEDC_CHANNEL_IN2] == TEC_PWM_MAX_COUNT,
          "the abort must interrupt actual full-power recovery");
  setTimeMs(start + 9000u);
  if (abort == RecoveryAbort::ManualDisable) {
    controller.setSettings(25, 30, 20, false);
  } else {
    controller.submitTemperature(abort == RecoveryAbort::Nonfinite,
                                 abort == RecoveryAbort::Nonfinite ? NAN : 25.0f,
                                 start + 9000u);
    require(!controller.status().sensorOk, "the interrupted recovery must lose sensor validity");
  }
  assertLatchedOff(controller, ControllerState::ThermalFaultLatched);
  const size_t stopped = events.size();
  for (uint32_t retry = 1; retry <= 3; ++retry) {
    const uint32_t now = start + 9000u + retry * 5022u;
    controller.setSettings(25, 30, 20, false);
    assertLatchedOff(controller, ControllerState::ThermalFaultLatched);
    enable(controller);
    controller.submitTemperature(true, hot ? 56.0f : 9.0f, now);
    assertLatchedOff(controller, ControllerState::ThermalFaultLatched);
    controller.submitTemperature(false, NAN, now + 200u);
    assertLatchedOff(controller, ControllerState::ThermalFaultLatched);
  }
  assertNoDriveSince(stopped);
  controller.resetFault();
  controller.submitTemperature(true, 30, start + 26000u);
  assertOff(controller);
  require(!controller.settings().enabled, "thermal reset alone must not enable output");
  enable(controller);
  controller.submitTemperature(true, 30, start + 26200u);
  require(controller.status().driverAwake, "reset plus manual enable must permit recovery");
}

void sampleExpiresBeforeWake(bool reversal, uint32_t age, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  if (reversal) {
    controller.submitTemperature(true, 30, start);
    require(pinValues[PIN_DRV_SLEEP] == HIGH && duties[LEDC_CHANNEL_IN1] > 0,
            "reversal must start from active cooling");
  }
  const uint32_t acquired = start + 200u;
  int injections = 0;
  size_t paused = events.size();
  auto stall = [&] {
    ++injections;
    require(criticalDepth == 0, "the timer must run while the main task is outside its lock");
    require(pinValues[PIN_DRV_SLEEP] == LOW &&
                duties[LEDC_CHANNEL_IN1] == 0 && duties[LEDC_CHANNEL_IN2] == 0,
            "sample aging must occur with the bridge safely asleep");
    paused = events.size();
    // Keep the independent timer running while the main task is stalled.
    for (uint32_t elapsed = 50; elapsed <= age; elapsed += 50) {
      setTimeMs(acquired + elapsed);
      controller.checkSampleAgeFromTimer(millis());
    }
    setTimeMs(acquired + age);
    controller.checkSampleAgeFromTimer(millis());
  };
  if (reversal) {
    sleepDelayHook = stall;
  } else {
    pwmWriteHook = [&](uint8_t channel, uint32_t duty) {
      if (channel != LEDC_CHANNEL_IN2 || duty != 0) return;
      pwmWriteHook = nullptr;
      stall();
    };
  }
  controller.submitTemperature(true, reversal ? 9.0f : 30.0f, acquired);
  require(injections == 1, "the pre-wake stall hook must execute exactly once");
  if (age < 1000u) {
    require(pinValues[PIN_DRV_SLEEP] == HIGH && controller.status().driverAwake,
            "a still-fresh sample must be allowed to wake");
    require(duties[reversal ? LEDC_CHANNEL_IN2 : LEDC_CHANNEL_IN1] > 0,
            "the fresh control case must actually drive the requested direction");
  } else {
    const auto expected = reversal ? ControllerState::ThermalFaultLatched
                                   : ControllerState::SensorTimeoutLatched;
    assertLatchedOff(controller, expected);
    assertNoDriveSince(paused);
    controller.submitTemperature(true, 30, acquired + age + 200u);
    if (reversal) {
      enable(controller);
      assertLatchedOff(controller, expected);
      assertNoDriveSince(paused);
    } else {
      require(controller.status().state != ControllerState::SensorTimeoutLatched,
              "fresh sample after timeout must auto-unlock");
      require(controller.status().driverAwake, "control must resume once probe recovers");
    }
  }
}

void acquisitionStartControlsFreshness(uint32_t age, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  const size_t submitted = events.size();
  setTimeMs(start + age);
  // Deliberately bypass TestController's clock-setting convenience overload.
  controller.ThermalController::submitTemperature(true, 30, start);
  if (age >= 1000u) {
    assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
    require(!controller.status().sensorOk &&
                std::isnan(controller.status().rawTemperatureC) &&
                std::isnan(controller.status().filteredTemperatureC),
            "an expired acquisition must not be published as a valid temperature");
    assertNoDriveSince(submitted);
    controller.submitTemperature(true, 30, start + age + 200u);
    require(controller.status().state != ControllerState::SensorTimeoutLatched,
            "subsequent fresh sample must auto-unlock sensor timeout");
    require(controller.status().driverAwake, "control resumes after probe recovers");
  } else {
    require(pinValues[PIN_DRV_SLEEP] == HIGH && duties[LEDC_CHANNEL_IN1] > 0,
            "a bounded acquisition delay must still allow valid cooling");
    setTimeMs(start + 999u);
    controller.checkSampleAgeFromTimer(millis());
    require(pinValues[PIN_DRV_SLEEP] == HIGH, "acquisition must remain valid at age 999 ms");
    setTimeMs(start + 1000u);
    controller.checkSampleAgeFromTimer(millis());
    require(pinValues[PIN_DRV_SLEEP] == LOW,
            "deadline must be based on acquisition start, not delayed return");
    controller.pollDriverFault();
    assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
  }
}

void queuedFaultAtSleepCutLatches(bool afterCut, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, start);
  require(duties[LEDC_CHANNEL_IN1] > 0, "handoff test must begin with active cooling");
  int injections = 0;
  size_t injected = events.size();
  auto edge = [&] {
    ++injections;
    require(criticalDepth > 0, "the sleep-cut edge must be queued inside the critical section");
    require(pinValues[PIN_DRV_SLEEP] == (afterCut ? LOW : HIGH),
            "the edge must straddle the requested side of the nSLEEP cut");
    require(duties[LEDC_CHANNEL_IN1] > 0, "PWM must still be active at the handoff");
    injected = events.size();
    queueFaultPulse();
    require((GPIO.status & (uint32_t{1} << PIN_DRV_FAULT)) != 0,
            "masked fault edge must remain queued even after the pin recovers");
    require(pinValues[PIN_DRV_FAULT] == HIGH, "this test must not rely on a persistent low");
  };
  if (afterCut) {
    // The immediate cut boundary is ambiguous, unlike the settled sleep interval.
    afterSleepCut = edge;
  } else {
    beforeSleepCut = edge;
  }
  controller.submitTemperature(true, 9, start + 200u);
  require(injections == 1, "the sleep-cut fault hook must execute exactly once");
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  assertNoDriveSince(injected);
  enable(controller);
  controller.submitTemperature(true, 30, start + 400u);
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  assertNoDriveSince(injected);
}

enum class BlankedWindow { Sleeping, ReversalSleep, Waking };

void faultInBlankedWindow(BlankedWindow window, bool persistent, bool held, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  if (window == BlankedWindow::ReversalSleep) {
    controller.submitTemperature(true, 30, start);
    require(duties[LEDC_CHANNEL_IN1] > 0, "blanking reversal must begin with active cooling");
  }
  int injections = 0;
  auto edge = [&] {
    ++injections;
    require(criticalDepth == 0, "blanked interval must be outside the handoff critical section");
    require(pinValues[PIN_DRV_SLEEP] == (window == BlankedWindow::Waking ? HIGH : LOW),
            "fault must occur in the requested sleep/wake interval");
    require(duties[LEDC_CHANNEL_IN1] == 0 && duties[LEDC_CHANNEL_IN2] == 0,
            "both PWM channels must be zero throughout blanking");
    holdFaultInterrupt = held;
    if (persistent) {
      pinValues[PIN_DRV_FAULT] = LOW;
      GPIO.status |= uint32_t{1} << PIN_DRV_FAULT;
      deliverFaultInterrupt();
    } else {
      queueFaultPulse();
    }
    if (held) {
      require((GPIO.status & (uint32_t{1} << PIN_DRV_FAULT)) != 0,
              "the delayed blanking interrupt must actually be pending");
    }
  };
  if (window == BlankedWindow::Sleeping) {
    edge();
    controller.pollDriverFault();
    require(controller.status().state == ControllerState::WaitingForSensor,
            "sleeping nFAULT must not latch before wake");
  } else if (window == BlankedWindow::ReversalSleep) {
    sleepDelayHook = edge;
  } else {
    wakeDelayHook = edge;
  }
  events.clear();
  controller.submitTemperature(true, window == BlankedWindow::ReversalSleep ? 9.0f : 30.0f,
                               start + 200u);
  require(injections == 1, "the blanking fault injection must execute exactly once");
  holdFaultInterrupt = false;
  deliverFaultInterrupt();
  controller.pollDriverFault();
  if (persistent) {
    assertLatchedOff(controller, ControllerState::DriverFaultLatched);
    require(std::none_of(events.begin(), events.end(), [](const Event& event) {
      return event.kind == "pwm" && event.value > 0;
    }), "persistent nFAULT must be detected before nonzero PWM");
  } else {
    require(controller.settings().enabled && controller.status().driverAwake &&
                pinValues[PIN_DRV_SLEEP] == HIGH,
            "a recovered blanking pulse must not prevent normal wake");
    require(duties[window == BlankedWindow::ReversalSleep ? LEDC_CHANNEL_IN2
                                                       : LEDC_CHANNEL_IN1] > 0,
            "ignoring a blanking pulse must permit actual output");
    // Blanking must end: the next active-drive pulse still needs immediate cutoff.
    queueFaultPulse();
    require(pinValues[PIN_DRV_SLEEP] == LOW, "fault monitoring must resume after blanking");
    controller.pollDriverFault();
    assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  }
}

enum class TimerWindow { WakeDelay, InactivePwmWrite, ActivePwmWrite };

void timerCutsInFlightOutput(TimerWindow window, bool heating, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  int injections = 0;
  size_t cutoff = events.size();
  auto timeout = [&] {
    ++injections;
    require(criticalDepth == 0 && pinValues[PIN_DRV_SLEEP] == HIGH,
            "timer injection must interrupt an unlocked, awake output transaction");
    setTimeMs(start + 1000u);
    controller.checkSampleAgeFromTimer(millis());
    require(pinValues[PIN_DRV_SLEEP] == LOW, "timer must cut nSLEEP before returning to output code");
    cutoff = events.size();
    controller.checkSampleAgeFromTimer(millis());
  };
  if (window == TimerWindow::WakeDelay) {
    wakeDelayHook = [&] {
      require(duties[LEDC_CHANNEL_IN1] == 0 && duties[LEDC_CHANNEL_IN2] == 0,
              "wake timeout must precede nonzero PWM");
      timeout();
    };
  } else {
    const uint8_t activeChannel = heating ? LEDC_CHANNEL_IN2 : LEDC_CHANNEL_IN1;
    pwmWriteHook = [&, activeChannel](uint8_t channel, uint32_t duty) {
      if (pinValues[PIN_DRV_SLEEP] != HIGH) return;
      const bool match = window == TimerWindow::ActivePwmWrite
                             ? channel == activeChannel && duty > 0
                             : channel != activeChannel && duty == 0;
      if (!match) return;
      pwmWriteHook = nullptr;
      timeout();
    };
  }
  controller.submitTemperature(true, heating ? 20.0f : 30.0f, start);
  require(injections == 1, "the in-flight timer hook must execute exactly once");
  assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
  // An interrupted LEDC write may finish with nSLEEP low; it must never re-energize.
  assertNoWakeSince(cutoff);
  if (window == TimerWindow::WakeDelay) assertNoDriveSince(cutoff);
  controller.submitTemperature(true, heating ? 20.0f : 30.0f, start + 1200u);
  require(controller.status().state != ControllerState::SensorTimeoutLatched,
          "probe recovery auto-unlocks sensor timeout");
  require(controller.status().driverAwake, "recovered probe resumes drive");
}

enum class BalanceExit { HighBand, LowBand, TargetChange, BandChange };

void balanceLearnsUntilApproachNeeded(bool heating, BalanceExit exit, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  uint32_t now = start;
  controller.submitTemperature(true, heating ? 20.0f : 30.0f, now);
  require(controller.status().state == ControllerState::NormalControl,
          "balance regression must begin in normal approach control");
  bool reachedBalance = false;
  for (int sample = 0; sample < 80; ++sample) {
    now += 200u;
    controller.submitTemperature(true, 25, now);
    if (controller.status().state == ControllerState::BalanceControl) {
      reachedBalance = true;
      break;
    }
  }
  require(reachedBalance, "approach to the 25 C target must enter balance");
  require(heating ? controller.status().balanceDrivePercent > 0
                  : controller.status().balanceDrivePercent < 0,
          "balance entry must retain the heating or cooling approach sign");
  require(heating ? controller.status().signedDutyPercent > 0
                  : controller.status().signedDutyPercent < 0,
          "actual output must retain approach polarity on balance entry");
  const float enteredDrive = controller.status().balanceDrivePercent;
  for (int sample = 0; sample < 30; ++sample) {
    const float previousDrive = controller.status().balanceDrivePercent;
    now += 200u;
    controller.submitTemperature(true, 26, now);
    require(controller.status().state == ControllerState::BalanceControl,
            "26 C within the soft band must adapt balance, not restart approach");
    require(controller.status().driverAwake, "balance adaptation must maintain active output");
    if (sample >= 20) {
      require(controller.status().balanceDrivePercent < previousDrive,
              "settled off-target balance must keep learning, not reset its duty each sample");
    }
  }
  require(controller.status().filteredTemperatureC > 25.06f,
          "the balance retention test must leave the narrow entry deadband");
  require(controller.status().balanceDrivePercent < enteredDrive,
          "sustained temperature above target must adapt toward less heating/more cooling");
  if (exit == BalanceExit::TargetChange || exit == BalanceExit::BandChange) {
    controller.setSettings(exit == BalanceExit::TargetChange ? 27.0f : 25.0f,
                           exit == BalanceExit::BandChange ? 35.0f : 30.0f, 20, true);
    now += 200u;
    controller.submitTemperature(true, 26, now);
    require(controller.status().state == ControllerState::NormalControl,
            "changed target or soft band must invalidate retained balance");
  } else {
    bool crossedBand = false;
    for (int sample = 0; sample < 20; ++sample) {
      now += 200u;
      controller.submitTemperature(true, exit == BalanceExit::HighBand ? 40.0f : 12.0f, now);
      const float filtered = controller.status().filteredTemperatureC;
      if (filtered >= 30.0f || filtered <= 20.0f) {
        crossedBand = true;
        require(controller.status().state == ControllerState::NormalControl,
                "crossing either soft-band boundary must restart normal approach");
        break;
      }
      require(controller.status().state == ControllerState::BalanceControl,
              "balance must be retained until filtered temperature leaves the soft band");
    }
    require(crossedBand, "the exit stimulus must actually cross the soft band");
  }
}

void modelOnlyFirstOrderPlantLearnsLoad(bool heatLoad) {
  TestController controller;
  prepare(controller);
  enable(controller);
  float temperature = 25.0f;
  float peakError = 0.0f;
  uint32_t now = 1000;
  controller.submitTemperature(true, temperature, now);
  require(controller.status().state == ControllerState::BalanceControl &&
              fabs(controller.status().balanceDrivePercent - 10.0f) < 0.001f,
          "the synthetic plant must start at target with default 10% balance drive");

  // Model-only functional test, not real TEC validation. Arbitrary first-order
  // plant: dT/dt = (25-T)/20 + 0.005*duty + load, Euler step 0.2 s.
  // A steady +/-0.15 C/s load needs -/+30% drive at 25 C.
  const float externalLoad = heatLoad ? 0.15f : -0.15f;
  for (int sample = 0; sample < 1500; ++sample) {
    require(duties[LEDC_CHANNEL_IN1] <= TEC_PWM_MAX_COUNT &&
                duties[LEDC_CHANNEL_IN2] <= TEC_PWM_MAX_COUNT,
            "synthetic plant PWM must remain within hardware count bounds");
    const float bridgeDrive = pinValues[PIN_DRV_SLEEP] == HIGH
                                  ? 100.0f * (static_cast<float>(duties[LEDC_CHANNEL_IN2]) -
                                              static_cast<float>(duties[LEDC_CHANNEL_IN1])) /
                                        TEC_PWM_MAX_COUNT
                                  : 0.0f;
    // Feed back the gated PWM outputs, not the controller's requested duty.
    temperature += 0.2f * ((25.0f - temperature) / 20.0f +
                            0.005f * bridgeDrive + externalLoad);
    require(isfinite(temperature), "synthetic plant temperature must remain finite");
    peakError = max(peakError, fabsf(temperature - 25.0f));
    now += 200u;
    controller.submitTemperature(true, temperature, now);
    require(controller.settings().enabled &&
                controller.status().state == ControllerState::BalanceControl,
            "the modest synthetic load must be handled within retained balance");
    require(isfinite(controller.status().signedDutyPercent) &&
                fabs(controller.status().signedDutyPercent) <= 100.0f &&
                isfinite(controller.status().balanceDrivePercent) &&
                fabs(controller.status().balanceDrivePercent) <= 100.0f,
            "requested and learned output must stay finite and bounded");
    if (sample >= 1400) {
      require(fabs(temperature - 25.0f) < 0.15f,
              "synthetic plant must stay near target over the final 20 seconds");
    }
  }
  require(peakError > 0.1f, "the load must produce a meaningful temperature disturbance");
  const float expectedDrive = heatLoad ? -30.0f : 30.0f;
  require(fabs(controller.status().balanceDrivePercent - expectedDrive) < 2.0f &&
              fabs(controller.status().signedDutyPercent - expectedDrive) < 2.0f,
          "learned and applied drive must leave default 10% and cancel the synthetic load");
  std::cout << "MODEL-ONLY first-order " << (heatLoad ? "heat" : "cold")
            << " load: final C=" << temperature
            << ", learned duty %=" << controller.status().balanceDrivePercent << '\n';
}

void timeoutTelemetryCanRecover(uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, start);
  controller.submitTemperature(true, 32, start + 200u);
  require(controller.status().slopeCPerSecond > 0, "test must establish slope history");
  setTimeMs(start + 1200u);
  controller.checkSampleAgeFromTimer(millis());
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
  require(!controller.status().sensorOk && controller.status().slopeCPerSecond == 0,
          "timeout must invalidate slope as well as temperature");

  controller.submitTemperature(true, 29, start + 1400u);
  require(controller.status().sensorOk && controller.status().rawTemperatureC == 29 &&
              controller.status().filteredTemperatureC == 29 &&
              controller.status().slopeCPerSecond == 0,
          "fresh telemetry must reseed the filter without NaN or old slope");
  require(controller.status().state != ControllerState::SensorTimeoutLatched,
          "fresh sample must auto-unlock timeout");
  require(controller.status().driverAwake, "output resumes on fresh sample");

  // Subsequent stale interval re-trips timeout
  setTimeMs(start + 2500u);
  controller.checkSampleAgeFromTimer(millis());
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
  require(!controller.status().sensorOk && std::isnan(controller.status().filteredTemperatureC),
          "stale timeout must re-trip when no new sample arrives");

  // And recovers again upon a fresh sample:
  controller.submitTemperature(true, 26, start + 2700u);
  require(controller.status().sensorOk && controller.status().filteredTemperatureC == 26,
          "telemetry must recover again after a second timeout");
  require(controller.status().state != ControllerState::SensorTimeoutLatched,
          "second recovery must also auto-unlock");
  require(controller.status().driverAwake, "control resumes after second recovery");
}

enum class IdleMode { Disabled, ManualDisable, StopOutput };

void idleSamplesExpire(IdleMode mode, bool pollWhileIdle, uint32_t start) {
  TestController controller;
  prepare(controller);
  if (mode != IdleMode::Disabled) enable(controller);
  controller.submitTemperature(true, 30, start);
  if (mode == IdleMode::ManualDisable) controller.setSettings(25, 30, 20, false);
  if (mode == IdleMode::StopOutput) controller.stopOutput();
  assertOff(controller);
  const size_t stopped = events.size();
  if (pollWhileIdle) {
    setTimeMs(start + 999u);
    controller.pollDriverFault();
    require(controller.status().sensorOk, "idle diagnostics must remain valid before expiry");
    setTimeMs(start + 1000u);
    controller.pollDriverFault();
    require(!controller.status().sensorOk &&
                std::isnan(controller.status().rawTemperatureC) &&
                std::isnan(controller.status().filteredTemperatureC) &&
                controller.status().slopeCPerSecond == 0,
            "idle readings must not remain fresh indefinitely with the watchdog disarmed");
    require(controller.status().state == (mode == IdleMode::StopOutput
                                             ? ControllerState::WaitingForSensor
                                             : ControllerState::Disabled),
            "idle sample expiry must not manufacture an output timeout latch");
  }
  setTimeMs(start + 1200u);
  enable(controller);
  assertOff(controller);
  assertNoDriveSince(stopped);
  controller.submitTemperature(true, 20, start + 1400u);
  require(controller.status().sensorOk && controller.status().filteredTemperatureC == 20 &&
              controller.status().slopeCPerSecond == 0,
          "restart after an idle gap must not blend a new sample with expired history");
  require(controller.status().signedDutyPercent > 0 && duties[LEDC_CHANNEL_IN2] > 0,
          "a fresh below-target sample must heat, not follow old cooling history");
}

enum class PendingFault { DeliveredPulse, QueuedPulse, Timeout };
enum class FaultAction { Poll, Enable, Disable, Reset };

void pendingFaultAtControlBoundary(PendingFault fault, FaultAction action, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, start);
  if (fault == PendingFault::Timeout) {
    setTimeMs(start + 1000u);
    controller.checkSampleAgeFromTimer(millis());
  } else if (fault == PendingFault::QueuedPulse) {
    holdFaultInterrupt = true;
    queueFaultPulse();
    require((GPIO.status & (uint32_t{1} << PIN_DRV_FAULT)) != 0,
            "the active-drive edge must still await ISR delivery");
  } else {
    faultPulse();
  }
  const size_t injected = events.size();
  if (action == FaultAction::Poll) controller.pollDriverFault();
  if (action == FaultAction::Enable) enable(controller);
  if (action == FaultAction::Disable) controller.setSettings(25, 30, 20, false);
  if (action == FaultAction::Reset) controller.resetFault();
  if (action == FaultAction::Reset) {
    assertOff(controller);
    require(controller.status().state == ControllerState::Disabled &&
                !controller.settings().enabled && !controller.status().sensorOk,
            "explicit reset must acknowledge a pending fault and leave control disabled");
  } else {
    assertLatchedOff(controller, fault == PendingFault::Timeout
                                    ? ControllerState::SensorTimeoutLatched
                                    : ControllerState::DriverFaultLatched);
  }
  holdFaultInterrupt = false;
  deliverFaultInterrupt();
  controller.pollDriverFault();
  require(controller.status().state == (action == FaultAction::Reset
                                           ? ControllerState::Disabled
                                           : fault == PendingFault::Timeout
                                                 ? ControllerState::SensorTimeoutLatched
                                                 : ControllerState::DriverFaultLatched),
          "late ISR delivery must preserve the completed control operation");
  assertNoDriveSince(injected);
}

void disableCapturesBoundaryFault(bool afterCut) {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  if (afterCut) afterSleepCut = queueFaultPulse;
  else beforeSleepCut = queueFaultPulse;
  const size_t injected = events.size();
  controller.setSettings(25, 30, 20, false);
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  assertNoDriveSince(injected);
}

void pendingDriverFaultCannotDowngradeSafetyLatch() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  faultPulse();
  controller.startPwm(false);
  assertLatchedOff(controller, ControllerState::SafetyFaultLatched);
  controller.startPwm(true);
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::SafetyFaultLatched);
  controller.resetFault();
  enable(controller);
  controller.submitTemperature(true, 30, 1200);
  assertLatchedOff(controller, ControllerState::SafetyFaultLatched);
}

void faultIrqInitializationFailsClosed(FaultIrqFailure failure) {
  TestController controller;
  prepare(controller, false, false, failure);
  assertLatchedOff(controller, ControllerState::SafetyFaultLatched);
  const size_t stopped = events.size();
  faultIrqFailure = FaultIrqFailure::None;
  for (uint32_t attempt = 1; attempt <= 3; ++attempt) {
    controller.startPwm(true);
    controller.resetFault();
    require(!controller.setTecDirection(TecDirection::Normal),
            "missing fault IRQ must reject a confirmed direction");
    enable(controller);
    controller.submitTemperature(true, 30, attempt * 200u);
    require(!controller.startDirectionCalibration(), "missing fault IRQ must reject calibration");
    assertLatchedOff(controller, ControllerState::SafetyFaultLatched);
  }
  assertNoDriveSince(stopped);
}

void faultIrqCutsWithFlashCacheDisabled() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  require(pinValues[PIN_DRV_SLEEP] == HIGH && duties[LEDC_CHANNEL_IN1] > 0,
          "cache-off test must interrupt actual drive");
  flashCacheOff = true;
  queueFaultPulse();
  require(pinValues[PIN_DRV_SLEEP] == LOW,
          "nFAULT must cut output while flash-backed interrupts are suspended");
  flashCacheOff = false;
  deliverFaultInterrupt();
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
}

void missingFaultIrqNeverEnergizes(FaultIrqFailure failure) {
  TestController controller;
  prepare(controller, true, false, failure);
  const size_t initialized = events.size();
  controller.setTecDirection(TecDirection::Normal);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  assertOff(controller);
  assertNoDriveSince(initialized);
  assertLatchedOff(controller, ControllerState::SafetyFaultLatched);
}

void unchangedSettingsKeepLearnedBalance(bool aboveTarget, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  uint32_t now = start;
  controller.submitTemperature(true, 25, now);
  const float temperature = aboveTarget ? 26.0f : 24.0f;
  for (int sample = 0; sample < 40; ++sample) {
    now += 200u;
    controller.submitTemperature(true, temperature, now);
  }
  require(fabs(controller.status().balanceDrivePercent) > BALANCE_BASE_DUTY_PERCENT + 2,
          "test must learn a non-default balance drive");
  for (int sample = 0; sample < 10; ++sample) {
    const float learned = controller.status().balanceDrivePercent;
    controller.setSettings(controller.settings());
    require(controller.status().balanceDrivePercent == learned,
            "unchanged settings must not reset learned equilibrium");
    now += 200u;
    controller.submitTemperature(true, temperature, now);
    require(controller.status().state == ControllerState::BalanceControl &&
                (aboveTarget ? controller.status().balanceDrivePercent < learned
                             : controller.status().balanceDrivePercent > learned),
            "unchanged settings must preserve balance adaptation on the next sample");
  }
}

void alternatingHardLimitsCannotRestartBudget(bool firstHot, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  for (uint32_t elapsed = 0; elapsed <= RUNAWAY_LIMIT_MS; elapsed += 200u) {
    const bool hot = ((elapsed / 200u) % 2u == 0) == firstHot;
    controller.submitTemperature(true, hot ? HARD_MAX_TEMPERATURE_C : HARD_MIN_TEMPERATURE_C,
                                 start + elapsed);
    if (controller.status().state == ControllerState::ThermalFaultLatched) break;
    require(controller.status().state == (hot ? ControllerState::HardOverTemperature
                                             : ControllerState::HardUnderTemperature),
            "every acquisition is outside a hard limit, with no successful recovery");
    require(duties[hot ? LEDC_CHANNEL_IN1 : LEDC_CHANNEL_IN2] == TEC_PWM_MAX_COUNT,
            "the alternating episode must exercise actual full-power commands");
  }
  assertLatchedOff(controller, ControllerState::ThermalFaultLatched);
  const size_t stopped = events.size();
  enable(controller);
  controller.submitTemperature(true, 25, millis() + 200u);
  assertLatchedOff(controller, ControllerState::ThermalFaultLatched);
  assertNoDriveSince(stopped);
}

void hardRecoveryDiscardsLearnedBalance(bool hot, uint32_t start) {
  TestController controller;
  prepare(controller);
  controller.setSettings(25, 55, 10, true);
  controller.submitTemperature(true, 25, start);
  require(controller.status().state == ControllerState::BalanceControl &&
              controller.status().balanceDrivePercent > 0,
          "the episode must begin with a learned balance command");
  controller.submitTemperature(true, hot ? HARD_MAX_TEMPERATURE_C : HARD_MIN_TEMPERATURE_C,
                               start + 200u);
  require(controller.status().state == (hot ? ControllerState::HardOverTemperature
                                           : ControllerState::HardUnderTemperature) &&
              controller.status().balanceDrivePercent == 0,
          "hard-limit entry must invalidate the interrupted equilibrium");
  controller.submitTemperature(true, hot ? HARD_RECOVER_MAX_C : HARD_RECOVER_MIN_C,
                               start + 400u);
  const auto& status = controller.status();
  require(status.filteredTemperatureC > 10 && status.filteredTemperatureC < 55 &&
              fabs(status.filteredTemperatureC - 25) > BALANCE_DEADBAND_C,
          "recovered filtered data must stay inside the soft band but outside balance entry");
  require(status.state == ControllerState::NormalControl && status.balanceDrivePercent == 0,
          "hard recovery must return to approach control, not reuse the old equilibrium");
  require(hot ? status.signedDutyPercent < 0 : status.signedDutyPercent > 0,
          "the recovered output must retain the required thermal direction");
}

void successfulHardRecoveryStartsNewBudget(bool hot, uint32_t start) {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, hot ? 56.0f : 9.0f, start);
  for (uint32_t elapsed = 200; elapsed < RUNAWAY_NO_PROGRESS_MS; elapsed += 200u) {
    controller.submitTemperature(true, hot ? 54.0f : 10.5f, start + elapsed);
  }
  controller.submitTemperature(true, hot ? HARD_RECOVER_MAX_C : HARD_RECOVER_MIN_C,
                               start + RUNAWAY_NO_PROGRESS_MS);
  require(controller.status().state == ControllerState::NormalControl,
          "a sample inside the recovery thresholds must end the previous episode");
  controller.submitTemperature(true, hot ? 56.0f : 9.0f, start + RUNAWAY_NO_PROGRESS_MS + 200u);
  require(controller.status().state == (hot ? ControllerState::HardOverTemperature
                                           : ControllerState::HardUnderTemperature),
          "a later hard-limit episode must get its own recovery budget");
  require(duties[hot ? LEDC_CHANNEL_IN1 : LEDC_CHANNEL_IN2] == TEC_PWM_MAX_COUNT,
          "a new episode must still permit full-power recovery");
}

void startCalibration(TestController& controller, uint32_t start = 1000u, float temperature = 25) {
  prepare(controller);
  controller.submitTemperature(true, temperature, start);
  require(controller.startDirectionCalibration(), "eligible calibration must start");
  require(controller.tecDirection() == TecDirection::Unknown && !controller.settings().enabled,
          "starting calibration must invalidate direction without enabling regulation");
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::BaselineForward,
          "first phase must collect an unpowered baseline");
  assertOff(controller);
}

uint32_t phaseStart(const ThermalController& controller) {
  return millis() - controller.directionCalibrationStatus().phaseElapsedMs;
}

void completeBaseline(TestController& controller, float temperature) {
  const auto phase = controller.directionCalibrationStatus().phase;
  require(phase == CalibrationPhase::BaselineForward || phase == CalibrationPhase::BaselineReverse,
          "baseline helper requires baseline phase");
  const uint32_t start = phaseStart(controller);
  for (uint32_t dt = 200; dt <= 2000; dt += 200) {
    controller.submitTemperature(true, temperature, start + dt);
    if (dt < 2000) assertOff(controller);
  }
  const bool forward = phase == CalibrationPhase::BaselineForward;
  const auto status = controller.directionCalibrationStatus();
  require(status.active && status.phase == (forward ? CalibrationPhase::Forward : CalibrationPhase::Reverse),
          "stable baseline must enter the matching powered phase");
  require(status.rawDutyPercent == (forward ? 100 : -100) &&
              duties[forward ? LEDC_CHANNEL_IN1 : LEDC_CHANNEL_IN2] == CALIBRATION_PWM_COUNT &&
              duties[forward ? LEDC_CHANNEL_IN2 : LEDC_CHANNEL_IN1] == 0 &&
              pinValues[PIN_DRV_SLEEP] == HIGH,
          "pulse must drive exactly one input at the build's calibration limit");
  require(controller.status().signedDutyPercent == 0 &&
              controller.status().heatingDutyPercent == 0 && controller.status().coolingDutyPercent == 0,
          "unconfirmed physical output must not claim to be thermal heating or cooling");
}

void completePulse(TestController& controller, float baseline, float delta) {
  const uint32_t start = phaseStart(controller);
  for (uint32_t dt = 200; dt <= 2800; dt += 200) {
    controller.submitTemperature(true, baseline + delta * dt / 2800.0f, start + dt);
  }
  setTimeMs(start + 2999u);
  controller.checkSampleAgeFromTimer(millis());
  require(pinValues[PIN_DRV_SLEEP] == HIGH, "pulse must not cut before its deadline");
  setTimeMs(start + 3000u);
  controller.checkSampleAgeFromTimer(millis());
  require(pinValues[PIN_DRV_SLEEP] == LOW, "independent timer must cut exactly at pulse deadline in mock");
  require(controller.directionCalibrationStatus().rawDutyPercent == 0,
          "timer-cut output must not be reported as still at full duty");
  controller.pollDriverFault();
  assertOff(controller);
}

void completeSettle(TestController& controller, float temperature) {
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::Settle,
          "first pulse must be followed by an unpowered interval");
  const uint32_t start = phaseStart(controller);
  for (uint32_t dt = 200; dt <= 3000; dt += 200) {
    controller.submitTemperature(true, temperature, start + dt);
    assertOff(controller);
  }
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::BaselineReverse,
          "settle must last 3 seconds before the second 2-second baseline");
}

void runCalibration(TestController& controller, uint32_t start, float firstDelta, float secondDelta) {
  startCalibration(controller, start);
  completeBaseline(controller, 25);
  completePulse(controller, 25, firstDelta);
  completeSettle(controller, 25 + firstDelta);
  completeBaseline(controller, 25 + firstDelta);
  completePulse(controller, 25 + firstDelta, secondDelta);
}

void unknownBootCannotDrive() {
  TestController controller;
  prepare(controller, true, false);
  require(controller.tecDirection() == TecDirection::Unknown, "new device must have no assumed polarity");
  const size_t initial = events.size();
  for (const float temperature : {9.0f, 25.0f, 56.0f}) {
    enable(controller);
    controller.submitTemperature(true, temperature, millis() + 200u);
    assertOff(controller);
    require(!controller.settings().enabled, "unknown direction must also block emergency output");
  }
  assertNoDriveSince(initial);
  require(!controller.setTecDirection(static_cast<TecDirection>(3)), "invalid enum must fail closed");
}

void detectedMappingRequiresApplication(bool reversed, uint32_t start) {
  TestController controller;
  runCalibration(controller, start, reversed ? 0.6f : -0.6f, reversed ? -0.6f : 0.6f);
  const auto result = controller.directionCalibrationStatus();
  const auto expected = reversed ? TecDirection::Reversed : TecDirection::Normal;
  require(!result.active && result.phase == CalibrationPhase::Complete && result.candidate == expected,
          "opposite raw/corrected thermal responses must identify the physical mapping");
  require(controller.tecDirection() == TecDirection::Unknown, "candidate is not a persisted direction");
  enable(controller);
  require(!controller.settings().enabled, "unsaved candidate cannot enable normal control");
  assertOff(controller);
  require(controller.setTecDirection(expected), "saved candidate may be applied while disabled");
  assertOff(controller);
  const uint32_t now = millis();
  controller.submitTemperature(true, 30, now + 200u);
  enable(controller);
  controller.submitTemperature(true, 30, now + 400u);
  require(duties[reversed ? LEDC_CHANNEL_IN2 : LEDC_CHANNEL_IN1] > 0,
          "normal cooling must use learned polarity");
  require(!controller.setTecDirection(reversed ? TecDirection::Normal : TecDirection::Reversed),
          "polarity must not change while enabled");
  controller.submitTemperature(true, 9, now + 600u);
  require(duties[reversed ? LEDC_CHANNEL_IN1 : LEDC_CHANNEL_IN2] == TEC_PWM_MAX_COUNT,
          "hard cold recovery must use learned heating polarity");
  controller.submitTemperature(true, 56, now + 800u);
  require(duties[reversed ? LEDC_CHANNEL_IN2 : LEDC_CHANNEL_IN1] == TEC_PWM_MAX_COUNT,
          "hard hot recovery must use learned cooling polarity");
}

void ambiguousCalibration(float forward, float reverse) {
  TestController controller;
  runCalibration(controller, 1000, forward, reverse);
  const auto result = controller.directionCalibrationStatus();
  require(!result.active && result.phase == CalibrationPhase::Failed &&
              result.candidate == TecDirection::Unknown && std::string(result.error) == "response_ambiguous",
          "weak or same-sign responses must not guess a wiring direction");
  assertOff(controller);
  require(controller.tecDirection() == TecDirection::Unknown, "failed recalibration invalidates old mapping");
}

void calibrationEligibility() {
  for (const float temperature : {14.99f, 15.0f, 45.0f, 45.01f}) {
    TestController controller;
    prepare(controller);
    controller.submitTemperature(true, temperature, 1000);
    const bool eligible = temperature >= 15 && temperature <= 45;
    require(controller.canStartDirectionCalibration() == eligible, "starting temperature limits inclusive");
    require(controller.startDirectionCalibration() == eligible, "start must enforce eligibility");
    assertOff(controller);
  }
  for (const uint32_t age : {400u, 401u, 1000u}) {
    TestController controller;
    prepare(controller);
    controller.submitTemperature(true, 25, 1000);
    setTimeMs(1000 + age);
    require(controller.startDirectionCalibration() == (age <= 400), "start requires a recent sample");
    assertOff(controller);
  }
  for (const bool pwm : {false, true}) {
    TestController controller;
    prepare(controller, pwm);
    if (pwm) controller.startPwm(false);
    controller.submitTemperature(true, 25, 1000);
    require(!controller.startDirectionCalibration(), "unavailable output guard/PWM must block detection");
    assertOff(controller);
  }
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 25, 1000);
  require(!controller.startDirectionCalibration(), "normal control must be stopped before calibration");
}

enum class CalAbort { Cancel, Stop, Disable, Reset, Invalid, Nonfinite, Hot, Cold, Excursion, Fault, Stale };

void calibrationAbort(CalAbort reason, bool reverse, uint32_t start) {
  TestController controller;
  startCalibration(controller, start);
  completeBaseline(controller, 25);
  if (reverse) {
    completePulse(controller, 25, -0.6f);
    completeSettle(controller, 24.4f);
    completeBaseline(controller, 24.4f);
  }
  const uint32_t now = millis();
  const size_t abortAt = events.size();
  if (reason == CalAbort::Cancel) controller.cancelDirectionCalibration();
  if (reason == CalAbort::Stop) controller.stopOutput();
  if (reason == CalAbort::Disable) controller.setSettings(25, 30, 20, false);
  if (reason == CalAbort::Reset) controller.resetFault();
  if (reason == CalAbort::Invalid) controller.submitTemperature(false, 25, now + 200u);
  if (reason == CalAbort::Nonfinite) controller.submitTemperature(true, NAN, now + 200u);
  if (reason == CalAbort::Hot) controller.submitTemperature(true, 50, now + 200u);
  if (reason == CalAbort::Cold) controller.submitTemperature(true, 12, now + 200u);
  if (reason == CalAbort::Excursion) controller.submitTemperature(true, 30, now + 200u);
  if (reason == CalAbort::Fault) { faultPulse(); controller.pollDriverFault(); }
  if (reason == CalAbort::Stale) {
    setTimeMs(now + 1000u);
    controller.checkSampleAgeFromTimer(millis());
    require(pinValues[PIN_DRV_SLEEP] == LOW, "missing sample must cut via timer");
    controller.pollDriverFault();
  }
  const auto result = controller.directionCalibrationStatus();
  require(!result.active && result.candidate == TecDirection::Unknown &&
              controller.tecDirection() == TecDirection::Unknown,
          "every abort must discard candidate and mapping");
  require(result.phase == CalibrationPhase::Cancelled || result.phase == CalibrationPhase::Failed,
          "abort must have a terminal non-success status");
  assertOff(controller);
  assertNoDriveSince(abortAt);
  if (reason == CalAbort::Fault) assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  if (reason == CalAbort::Stale) assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
  const size_t stopped = events.size();
  controller.submitTemperature(true, 25, millis() + 200u);
  enable(controller);
  controller.pollDriverFault();
  assertOff(controller);
  assertNoDriveSince(stopped);
}

void calibrationBaselineRejection(int mode) {
  TestController controller;
  startCalibration(controller);
  const size_t began = events.size();
  for (uint32_t dt = 200; dt <= 2000; dt += 200) {
    if (mode == 2 && (dt == 800 || dt == 1000)) continue;
    const float temp = mode == 0 ? 25.0f + dt * 0.0001f
                      : mode == 1 ? 25.0f + (dt % 400 ? 0.1f : -0.1f) : 25.0f;
    controller.submitTemperature(true, temp, 1000 + dt);
  }
  require(std::string(controller.directionCalibrationStatus().error) == "baseline_unstable",
          "slope, range or a gap must reject an unreliable baseline");
  assertOff(controller);
  assertNoDriveSince(began);
}

void calibrationSeedsEligibleSampleBeforeFirstPulse() {
  TestController controller;
  startCalibration(controller);
  const uint32_t start = phaseStart(controller);
  // The first scheduled sample is delayed to 400 ms and the last one arrives
  // at 1800 ms. Without the eligible sample seeded at phase start, the
  // baseline span is only 1400 ms and the implementation rejects it before
  // any bridge pulse can start.
  for (uint32_t dt = 400; dt <= 1800; dt += 200) {
    controller.submitTemperature(true, 25.0f, start + dt);
  }
  setTimeMs(start + TEC_CAL_BASELINE_MS);
  controller.pollDriverFault();
  const auto status = controller.directionCalibrationStatus();
  require(status.active && status.phase == CalibrationPhase::Forward &&
              status.rawDutyPercent == 100.0f &&
              duties[LEDC_CHANNEL_IN1] == CALIBRATION_PWM_COUNT &&
              duties[LEDC_CHANNEL_IN2] == 0 &&
              pinValues[PIN_DRV_SLEEP] == HIGH,
          "eligible baseline sample must prevent jitter from suppressing the first physical pulse");
}

void calibrationPulseSamplingRejection(int mode) {
  TestController controller;
  startCalibration(controller);
  completeBaseline(controller, 25);
  const uint32_t start = phaseStart(controller);
  for (uint32_t dt = 200; dt <= 2800; dt += 200) {
    if (mode == 0 && (dt == 1000 || dt == 1200)) continue;
    if (mode == 1 && dt > 2200) continue;
    if (mode == 2) {
      setTimeMs(start + dt);
      // A repeated acquisition timestamp must not count as multiple observations.
      controller.ThermalController::submitTemperature(true, 25, start + (dt / 800u) * 800u);
    } else {
      controller.submitTemperature(true, 25 - dt * 0.0002f, start + dt);
    }
  }
  setTimeMs(start + 3000u);
  controller.pollDriverFault();
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::Failed &&
              std::string(controller.directionCalibrationStatus().error) == "pulse_samples",
          "gaps, missing endpoints or duplicate acquisitions must reject pulse evidence");
  assertOff(controller);
}

void invalidSampleAtPhaseBoundary() {
  for (const bool invalid : {false, true}) {
    TestController controller;
    startCalibration(controller);
    for (uint32_t dt = 200; dt < 2000; dt += 200) {
      controller.submitTemperature(true, 25, 1000 + dt);
    }
    const size_t boundary = events.size();
    if (invalid) controller.submitTemperature(false, NAN, 3000);
    else {
      setTimeMs(3000);
      controller.setSettings(25, 30, 20, false);
    }
    assertOff(controller);
    assertNoDriveSince(boundary);
    require(!controller.directionCalibrationStatus().active, "boundary abort must not launch a pulse");
  }
}

void calibrationStopFaultAtDeadline(bool reverse) {
  TestController controller;
  startCalibration(controller);
  completeBaseline(controller, 25);
  if (reverse) {
    completePulse(controller, 25, -0.6f);
    completeSettle(controller, 24.4f);
    completeBaseline(controller, 24.4f);
  }
  const uint32_t start = phaseStart(controller);
  for (uint32_t dt = 200; dt <= 2800; dt += 200) {
    controller.submitTemperature(true, 25, start + dt);
  }
  // A queued edge at the timer cutoff must not be hidden by entering settle/complete.
  holdFaultInterrupt = true;
  queueFaultPulse();
  setTimeMs(start + 3000u);
  controller.checkSampleAgeFromTimer(millis());
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::Failed &&
              controller.directionCalibrationStatus().candidate == TecDirection::Unknown,
          "cutoff fault must win over normal phase completion");
}

void calibrationTimerCannotRearm(bool reverse) {
  TestController controller;
  startCalibration(controller);
  completeBaseline(controller, 25);
  if (reverse) {
    completePulse(controller, 25, -0.6f);
    completeSettle(controller, 24.4f);
    completeBaseline(controller, 24.4f);
  }
  const uint32_t start = phaseStart(controller);
  for (uint32_t dt = 200; dt <= 2800; dt += 200) controller.submitTemperature(true, 25, start + dt);
  setTimeMs(start + 3000u);
  controller.checkSampleAgeFromTimer(millis());
  const size_t cut = events.size();
  for (uint32_t dt = 3050; dt <= 3250; dt += 50) {
    setTimeMs(start + dt);
    controller.checkSampleAgeFromTimer(millis());
    require(pinValues[PIN_DRV_SLEEP] == LOW, "timer must retain sleep while main task stalls");
  }
  controller.pollDriverFault();
  enable(controller);
  assertOff(controller);
  assertNoDriveSince(cut);
  require(!controller.settings().enabled, "no auto-enable after a timed pulse");
}

void calibrationCancelCompletedCandidate() {
  TestController controller;
  runCalibration(controller, 1000, -0.6f, 0.6f);
  controller.cancelDirectionCalibration();
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::Cancelled &&
              controller.directionCalibrationStatus().candidate == TecDirection::Unknown,
          "stop before candidate persistence must discard the completed result");
  assertOff(controller);
  runCalibration(controller, 1000, -0.6f, 0.6f);
  require(controller.setTecDirection(TecDirection::Normal), "completed candidate may be saved/applied");
  controller.cancelDirectionCalibration();
  require(controller.tecDirection() == TecDirection::Normal, "ordinary stop must preserve an applied mapping");
}

void calibrationEnableCannotMutate() {
  TestController controller;
  startCalibration(controller);
  completeBaseline(controller, 25);
  controller.setSettings(28, 35, 18, true);
  require(!controller.settings().enabled && controller.settings().targetC == 25 &&
              controller.directionCalibrationStatus().active,
          "enable during calibration must neither change settings nor cancel the raw test");
  require(!controller.setTecDirection(TecDirection::Normal), "direction cannot change mid-test");
  controller.cancelDirectionCalibration();
  assertOff(controller);
}

void calibrationStaleDuringOffPhase(bool settle) {
  TestController controller;
  startCalibration(controller);
  if (settle) {
    completeBaseline(controller, 25);
    completePulse(controller, 25, -0.6f);
  }
  setTimeMs(millis() + 1000u);
  controller.checkSampleAgeFromTimer(millis());
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
  require(!controller.directionCalibrationStatus().active,
          "unpowered portions of calibration still require sensor freshness");
}

void calibrationTotalDeadline(uint32_t start) {
  TestController controller;
  startCalibration(controller, start);
  for (uint32_t dt = 200; dt < 2000; dt += 200) controller.submitTemperature(true, 25, start + dt);
  const size_t beforeWake = events.size();
  int stalled = 0;
  wakeDelayHook = [&] {
    ++stalled;
    const uint32_t began = millis();
    // Keep acquisition fresh while the phase service is held inside driver wake.
    for (uint32_t dt = 200; dt <= 19000; dt += 200) {
      controller.submitTemperature(true, 25, began + dt);
      controller.checkSampleAgeFromTimer(millis());
    }
    require(pinValues[PIN_DRV_SLEEP] == LOW, "total deadline must cut independently of phase service");
  };
  controller.submitTemperature(true, 25, start + 2000u);
  controller.pollDriverFault();
  require(stalled == 1 && std::string(controller.directionCalibrationStatus().error) == "total_timeout",
          "20-second total deadline must survive a stalled wake with fresh samples");
  assertOff(controller);
  require(std::none_of(events.begin() + beforeWake, events.end(), [](const Event& event) {
    return event.kind == "pwm" && event.value > 0;
  }), "a pulse must not begin after the total deadline");
}

void calibrationCutDuringPwmWrite(bool activeWrite) {
  TestController controller;
  startCalibration(controller);
  for (uint32_t dt = 200; dt < 2000; dt += 200) controller.submitTemperature(true, 25, 1000 + dt);
  int interrupted = 0;
  size_t cut = events.size();
  pwmWriteHook = [&](uint8_t channel, uint32_t duty) {
    if (pinValues[PIN_DRV_SLEEP] != HIGH) return;
    if (activeWrite ? channel != LEDC_CHANNEL_IN1 || duty == 0
                    : channel != LEDC_CHANNEL_IN2 || duty != 0) return;
    pwmWriteHook = nullptr;
    ++interrupted;
    const uint32_t start = millis();
    for (uint32_t dt = 200; dt <= 3000; dt += 200) {
      controller.submitTemperature(true, 25, start + dt);
      controller.checkSampleAgeFromTimer(millis());
    }
    require(pinValues[PIN_DRV_SLEEP] == LOW, "pulse cutoff must interrupt an unfinished PWM transaction");
    cut = events.size();
  };
  controller.submitTemperature(true, 25, 3000);
  controller.pollDriverFault();
  require(interrupted == 1, "test must interrupt the requested PWM write exactly once");
  assertOff(controller);
  assertNoWakeSince(cut);
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::Settle,
          "a cutoff cannot restart the same pulse when the PWM call returns");
}

void calibrationCutoffSleepFaultIsConservative() {
  TestController controller;
  startCalibration(controller);
  completeBaseline(controller, 25);
  const uint32_t start = phaseStart(controller);
  for (uint32_t dt = 200; dt <= 2800; dt += 200) controller.submitTemperature(true, 25, start + dt);
  setTimeMs(start + 3000u);
  controller.checkSampleAgeFromTimer(millis());
  queueFaultPulse();
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  require(std::string(controller.directionCalibrationStatus().error) == "driver_fault",
          "a fault straddling asynchronous cutoff must conservatively invalidate calibration");
}

void calibrationDriftCorrectionRejectsFalseEvidence() {
  TestController controller;
  startCalibration(controller);
  for (uint32_t dt = 200; dt <= 2000; dt += 200) {
    controller.submitTemperature(true, 25 + dt * 0.00002f, 1000 + dt);
  }
  require(controller.directionCalibrationStatus().phase == CalibrationPhase::Forward,
          "small natural drift is allowed in a stable baseline");
  completePulse(controller, 25.04f, 0.25f);
  completeSettle(controller, 25.29f);
  completeBaseline(controller, 25.29f);
  completePulse(controller, 25.29f, -0.6f);
  const auto result = controller.directionCalibrationStatus();
  require(result.forwardDeltaC > TEC_CAL_MIN_RESPONSE_C &&
              result.phase == CalibrationPhase::Failed && std::string(result.error) == "response_ambiguous",
          "raw threshold alone cannot qualify a response dominated by baseline drift");
  assertOff(controller);
}
#include "../../../../tests/native/fault_regressions.h"
}  // namespace

GpioRegisters GPIO;
MockSerial Serial;

void GpioClearRegister::operator=(uint32_t mask) {
  for (uint8_t pin = 0; pin < 32; ++pin) {
    if ((mask & (uint32_t{1} << pin)) != 0) pinValues[pin] = LOW;
  }
  events.push_back({"isr-clear", mask, 0});
}

void GpioStatusClearRegister::operator=(uint32_t mask) {
  GPIO.status &= ~mask;
}

void portENTER_CRITICAL(portMUX_TYPE* mux) { ++*mux; ++criticalDepth; }
void portEXIT_CRITICAL(portMUX_TYPE* mux) {
  require(*mux > 0 && criticalDepth > 0, "unbalanced critical section");
  --*mux;
  --criticalDepth;
  deliverFaultInterrupt();
  if (criticalDepth == 0) runOnce(criticalExitHook);
}
void portENTER_CRITICAL_ISR(portMUX_TYPE* mux) { portENTER_CRITICAL(mux); }
void portEXIT_CRITICAL_ISR(portMUX_TYPE* mux) { portEXIT_CRITICAL(mux); }
uint32_t millis() { return static_cast<uint32_t>(fakeMicros / 1000); }
void delay(uint32_t duration) { fakeMicros += uint64_t{duration} * 1000; }

void digitalWrite(uint8_t pin, uint8_t value) {
  const bool cutting = pin == PIN_DRV_SLEEP && value == LOW && pinValues[pin] == HIGH;
  if (cutting) runOnce(beforeSleepCut);
  pinValues[pin] = value;
  events.push_back({"pin", pin, value});
  if (cutting) runOnce(afterSleepCut);
}
int digitalRead(uint8_t pin) { return pinValues[pin]; }
void pinMode(uint8_t pin, uint8_t mode) { events.push_back({"mode", pin, mode}); }
esp_err_t gpio_install_isr_service(int flags) {
  require(pinValues[PIN_DRV_SLEEP] == LOW, "ISR service must install with output asleep");
  events.push_back({"irq-install", 0, static_cast<uint32_t>(flags)});
  if (faultIrqFailure == FaultIrqFailure::InstallMemory) return ESP_ERR_NO_MEM;
  if (faultIrqFailure == FaultIrqFailure::InstallInterrupt) return ESP_ERR_NOT_FOUND;
  if (gpioServiceInstalled) return ESP_ERR_INVALID_STATE;
  gpioServiceInstalled = true;
  gpioServiceFlags = flags;
  return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) {
  require(pin == PIN_DRV_FAULT && pinValues[PIN_DRV_SLEEP] == LOW,
          "handler removal must address nFAULT while asleep");
  events.push_back({"irq-remove", static_cast<uint32_t>(pin), 0});
  if (!gpioServiceInstalled) return ESP_ERR_INVALID_STATE;
  faultCallback = nullptr;
  faultArgument = nullptr;
  faultInterruptEnabled = false;
  return ESP_OK;
}
esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t type) {
  require(pin == PIN_DRV_FAULT && pinValues[PIN_DRV_SLEEP] == LOW,
          "interrupt configuration must address nFAULT while asleep");
  events.push_back({"irq-type", static_cast<uint32_t>(pin), static_cast<uint32_t>(type)});
  if (faultIrqFailure == FaultIrqFailure::Configure) return ESP_ERR_INVALID_ARG;
  faultInterruptType = type;
  return ESP_OK;
}
esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t handler, void* argument) {
  require(pin == PIN_DRV_FAULT && pinValues[PIN_DRV_SLEEP] == LOW,
          "handler registration must address nFAULT while asleep");
  events.push_back({"irq-add", static_cast<uint32_t>(pin), 0});
  if (!gpioServiceInstalled || faultIrqFailure == FaultIrqFailure::Add) return ESP_ERR_INVALID_STATE;
  faultCallback = handler;
  faultArgument = argument;
  faultInterruptEnabled = faultInterruptType == GPIO_INTR_NEGEDGE;
  return ESP_OK;
}
void attachInterruptArg(uint8_t pin, void (*callback)(void*), void* argument, int mode) {
  require(pin == PIN_DRV_FAULT && mode == FALLING, "unexpected interrupt registration");
  // Pinned Arduino wrapper: non-IRAM service; trigger/add errors are ignored.
  const esp_err_t installed = gpio_install_isr_service(0);
  if (installed != ESP_OK && installed != ESP_ERR_INVALID_STATE) return;
  gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE);
  gpio_isr_handler_add(pin, callback, argument);
}
void esp_rom_delay_us(uint32_t duration) { delayMicroseconds(duration); }
void delayMicroseconds(uint32_t duration) {
  fakeMicros += duration;
  events.push_back({"delay", 0, duration});
  if (duration == DRV_SLEEP_DELAY_US) runOnce(sleepDelayHook);
  if (duration == DRV_WAKE_DELAY_US) runOnce(wakeDelayHook);
}
double ledcSetup(uint8_t channel, double frequency, uint8_t resolution) {
  require(frequency == TEC_PWM_FREQUENCY && resolution == TEC_PWM_RESOLUTION,
          "PWM must match configured frequency and resolution");
  return channel == failingChannel ? 0.0 : wrongFrequency ? 20000 : frequency;
}
esp_err_t ledc_channel_config(const ledc_channel_config_t* config) {
  require(config != nullptr, "LEDC channel configuration must not be null");
  require(config->gpio_num == (config->channel == LEDC_CHANNEL_IN1
                                   ? PIN_TEC_IN1 : PIN_TEC_IN2),
          "LEDC channel must bind to its configured TEC pin");
  return static_cast<int>(config->channel) == failingAttachChannel
             ? ESP_ERR_INVALID_ARG : ESP_OK;
}
void ledcAttachPin(uint8_t, uint8_t) {}
void ledcWrite(uint8_t channel, uint32_t duty) {
  duties[channel] = duty;
  events.push_back({"pwm", channel, duty});
  require(duties[LEDC_CHANNEL_IN1] == 0 || duties[LEDC_CHANNEL_IN2] == 0,
          "both inputs must never be driven during a direction change");
  auto hook = pwmWriteHook;
  if (hook) hook(channel, duty);
}

void precisionOffsetKeepsCorrecting() {
  TestController controller;
  prepare(controller);
  enable(controller);
  for (uint32_t now = 1000; now <= 31000; now += 200) {
    controller.submitTemperature(true, 25.03125f, now);
  }
  const double integral = controller.pidStatus().integralPercent;
  const float duty = controller.status().signedDutyPercent;
  controller.submitTemperature(true, 25.03125f, 31200);
  require(controller.pidStatus().integralPercent < integral,
          "a flat above-target temperature must continue integrating toward cooling");
  require(controller.status().signedDutyPercent < duty,
          "above-target thermal equilibrium must not freeze output");
  require(controller.status().state == ControllerState::NormalControl,
          "off-target equilibrium must not be reported as stable");
}

void precisionQuantizedOutputAndReset() {
  TestController controller;
  prepare(controller);
  PrecisionPidSettings settings;
  settings.kp = 1; settings.ki = 0; settings.kd = 0; settings.measurementTauSeconds = 0;
  require(controller.setPidSettings(settings), "PID tuning while stopped");
  enable(controller);
  controller.submitTemperature(true, 25.0078125f, 1000);
  require(duties[LEDC_CHANNEL_IN1] == 1, "one-count 14-bit cooling must not be dead-banded");
  require(!controller.setPidSettings(settings), "PID tuning while running must be rejected");
  controller.submitTemperature(true, 25.1f, 1000);
  require(controller.status().rawTemperatureC == 25.0078125f, "duplicate timestamps must not update PID");
  controller.setSettings(25, 30, 20, false);
  assertOff(controller);
  require(!controller.stabilityStatus().ready &&
              controller.pidStatus().integralPercent == 0,
          "stopping must invalidate precision history");
}

void precisionHardRecoveryClearsIntegral() {
  TestController controller;
  prepare(controller);
  enable(controller);
  for (uint32_t now = 1000; now <= 11000; now += 200)
    controller.submitTemperature(true, 25.03125f, now);
  require(controller.pidStatus().integralPercent < 0, "integral learned a heat load");
  controller.submitTemperature(true, 55.0f, 11200);
  require(controller.status().state == ControllerState::HardOverTemperature &&
              controller.status().signedDutyPercent == -100,
          "raw temperature hard limit must override PID output limit");
  require(controller.pidStatus().integralPercent == 0,
          "hard recovery must discard obsolete PID memory");
  controller.submitTemperature(true, 53.5f, 11400);
  require(fabs(controller.status().signedDutyPercent) <= 30.01f,
          "recovered normal control must honor configured PID output cap");
}

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"watchdog_without_peripheral_calls", watchdogWithoutPeripheralCalls},
      {"watchdog_cut_before_unlock", watchdogCutBeforeUnlock},
      {"first_control_pwm_fault", [] { firstPoweredWriteFault(false); }},
      {"first_calibration_pwm_fault", [] { firstPoweredWriteFault(true); }},
      {"queued_powered_fault_survives_poll", queuedPoweredFaultSurvivesPoll},
      {"startup_off", startupOff},
      {"invalid_sensor_stops_output", invalidSensorStopsOutput},
      {"wake_failure_latches", wakeFailureLatches},
      {"transient_fault_and_manual_reset", transientFaultLatchesAndNeedsManualEnable},
      {"reversal_sleep_sequence", reversalSleepsBeforeWaking},
      {"pwm_initialization_failure", pwmInitializationFailureStaysOff},
      {"pwm_pin_binding_failure", pwmPinBindingFailureStaysOff},
      {"hard_limit_hysteresis_timeout", hardRecoveryTimeoutIncludesHysteresis},
      {"hard_limit_recovery", hardRecoveryCanReturnToNormal},
       {"redundant_fault_pullup", boardFaultPullupIsRedundantAndEnabled},
       {"fault_irq_clean_initialization", faultIrqInitializesWithoutPreexistingHandler},
      {"stale_sample_cut_and_latch", staleSampleCutsBeforePollingAndLatches},
      {"stale_wrap_and_older_callback", staleWatchdogHandlesWrapAndOlderCallback},
      {"no_guard_no_output", noGuardNoOutput},
      {"nonfinite_settings", nonfiniteSettingsFailClosed},
      {"balance_direction", balanceKeepsApproachDirection},
      {"wrong_pwm_frequency", wrongPwmFrequencyFailsClosed},
      {"stale_temperature_hidden", lostSensorHidesStaleTemperature},
      {"model_only_first_order_steady_heat_load",
       [] { modelOnlyFirstOrderPlantLearnsLoad(true); }},
      {"model_only_first_order_steady_cold_load",
       [] { modelOnlyFirstOrderPlantLearnsLoad(false); }},
      {"disable_fault_before_cut", [] { disableCapturesBoundaryFault(false); }},
      {"disable_fault_at_cut", [] { disableCapturesBoundaryFault(true); }},
      {"pending_fault_preserves_safety_latch", pendingDriverFaultCannotDowngradeSafetyLatch},
      {"fault_irq_flash_cache_disabled", faultIrqCutsWithFlashCacheDisabled},
      {"calibration_unknown_boot", unknownBootCannotDrive},
      {"calibration_eligibility", calibrationEligibility},
      {"calibration_invalid_phase_boundary", invalidSampleAtPhaseBoundary},
      {"calibration_cancel_completed", calibrationCancelCompletedCandidate},
      {"calibration_enable_interlock", calibrationEnableCannotMutate},
      {"calibration_total_deadline", [] { calibrationTotalDeadline(1000); }},
      {"calibration_total_deadline_wrap", [] { calibrationTotalDeadline(UINT32_MAX - 3000u); }},
      {"calibration_cut_inactive_write", [] { calibrationCutDuringPwmWrite(false); }},
      {"calibration_cut_active_write", [] { calibrationCutDuringPwmWrite(true); }},
      {"calibration_cutoff_sleep_fault_conservative", calibrationCutoffSleepFaultIsConservative},
      {"calibration_drift_correction", calibrationDriftCorrectionRejectsFalseEvidence},
      {"calibration_seeds_eligible_sample", calibrationSeedsEligibleSampleBeforeFirstPulse},
  };
  for (const auto failure : {FaultIrqFailure::InstallMemory, FaultIrqFailure::InstallInterrupt,
                             FaultIrqFailure::ExistingService, FaultIrqFailure::Configure,
                             FaultIrqFailure::Add}) {
    tests.emplace_back("fault_irq_init_failure_" + std::to_string(static_cast<int>(failure)),
                       [=] { faultIrqInitializationFailsClosed(failure); });
  }
  for (const auto failure : {FaultIrqFailure::InstallMemory, FaultIrqFailure::Configure,
                             FaultIrqFailure::Add}) {
    tests.emplace_back("fault_irq_missing_no_drive_" + std::to_string(static_cast<int>(failure)),
                       [=] { missingFaultIrqNeverEnergizes(failure); });
  }
  for (const bool reversed : {false, true}) {
    for (const uint32_t start : {1000u, UINT32_MAX - 3000u}) {
      tests.emplace_back(std::string("calibration_success_") + (reversed ? "reversed" : "normal") +
                             (start == 1000 ? "_ordinary" : "_wrap"),
                         [=] { detectedMappingRequiresApplication(reversed, start); });
    }
    tests.emplace_back(std::string("calibration_cutoff_fault_") + (reversed ? "reverse" : "forward"),
                       [=] { calibrationStopFaultAtDeadline(reversed); });
    tests.emplace_back(std::string("calibration_timer_no_rearm_") + (reversed ? "reverse" : "forward"),
                       [=] { calibrationTimerCannotRearm(reversed); });
    tests.emplace_back(std::string("calibration_off_stale_") + (reversed ? "settle" : "baseline"),
                       [=] { calibrationStaleDuringOffPhase(reversed); });
    for (const auto reason : {CalAbort::Cancel, CalAbort::Stop, CalAbort::Disable, CalAbort::Reset,
                             CalAbort::Invalid, CalAbort::Nonfinite, CalAbort::Hot, CalAbort::Cold,
                             CalAbort::Excursion, CalAbort::Fault, CalAbort::Stale}) {
      for (const uint32_t start : {1000u, UINT32_MAX - 3000u}) {
        tests.emplace_back("calibration_abort_" + std::to_string(static_cast<int>(reason)) +
                               (reversed ? "_reverse" : "_forward") + (start == 1000 ? "_ordinary" : "_wrap"),
                           [=] { calibrationAbort(reason, reversed, start); });
      }
    }
  }
  for (const float first : {-0.6f, 0.0f, 0.1f, 0.6f}) {
    for (const float second : {-0.6f, 0.0f, 0.1f, 0.6f}) {
      if (fabs(first) > 0.2f && first * second < -0.1f) continue;
      tests.emplace_back("calibration_ambiguous_" + std::to_string(first) + "_" + std::to_string(second),
                         [=] { ambiguousCalibration(first, second); });
    }
  }
  for (int mode = 0; mode < 3; ++mode) {
    tests.emplace_back("calibration_bad_baseline_" + std::to_string(mode),
                       [=] { calibrationBaselineRejection(mode); });
    tests.emplace_back("calibration_bad_pulse_samples_" + std::to_string(mode),
                       [=] { calibrationPulseSamplingRejection(mode); });
  }
  for (const uint32_t start : {1000u, UINT32_MAX - 200u}) {
    const std::string clock = start == 1000u ? "_ordinary" : "_wrap";
    tests.emplace_back("timeout_diagnostic_recovery" + clock,
                       [=] { timeoutTelemetryCanRecover(start); });
    for (const auto mode : {IdleMode::Disabled, IdleMode::ManualDisable, IdleMode::StopOutput}) {
      const std::string idle = mode == IdleMode::Disabled ? "disabled"
                               : mode == IdleMode::ManualDisable ? "manual_disable"
                                                                  : "stop_output";
      for (const bool polled : {false, true}) {
        tests.emplace_back("idle_sample_expiry_" + idle +
                               (polled ? "_polled" : "_unpolled") + clock,
                           [=] { idleSamplesExpire(mode, polled, start); });
      }
    }
    for (const auto fault : {PendingFault::DeliveredPulse, PendingFault::QueuedPulse,
                             PendingFault::Timeout}) {
      const std::string pending = fault == PendingFault::DeliveredPulse ? "delivered_pulse"
                                  : fault == PendingFault::QueuedPulse ? "queued_pulse"
                                                                       : "timeout";
      for (const auto action : {FaultAction::Poll, FaultAction::Enable,
                                FaultAction::Disable, FaultAction::Reset}) {
        const std::string operation = action == FaultAction::Poll ? "poll"
                                     : action == FaultAction::Enable ? "enable"
                                     : action == FaultAction::Disable ? "disable"
                                                                      : "reset";
        tests.emplace_back("pending_fault_" + pending + "_" + operation + clock,
                           [=] { pendingFaultAtControlBoundary(fault, action, start); });
      }
    }
    for (const bool aboveTarget : {false, true}) {
      tests.emplace_back(std::string("unchanged_settings_balance_") +
                             (aboveTarget ? "above" : "below") + clock,
                         [=] { unchangedSettingsKeepLearnedBalance(aboveTarget, start); });
    }
    for (const bool hot : {false, true}) {
      tests.emplace_back(std::string("alternating_hard_limit_budget_") +
                             (hot ? "hot_first" : "cold_first") + clock,
                         [=] { alternatingHardLimitsCannotRestartBudget(hot, start); });
      tests.emplace_back(std::string("hard_recovery_discards_balance_") +
                             (hot ? "hot" : "cold") + clock,
                         [=] { hardRecoveryDiscardsLearnedBalance(hot, start); });
      tests.emplace_back(std::string("successful_hard_recovery_new_budget_") +
                             (hot ? "hot" : "cold") + clock,
                         [=] { successfulHardRecoveryStartsNewBudget(hot, start); });
      for (const auto abort : {RecoveryAbort::InvalidFlag, RecoveryAbort::Nonfinite,
                               RecoveryAbort::ManualDisable}) {
        const std::string reason = abort == RecoveryAbort::InvalidFlag ? "invalid_flag"
                                   : abort == RecoveryAbort::Nonfinite ? "nonfinite"
                                                                      : "manual_disable";
        tests.emplace_back("F1_recovery_abort_" + reason +
                               (hot ? "_hot" : "_cold") + clock,
                           [=] { hardRecoveryAbortLatches(hot, abort, start); });
      }
    }
    for (const bool reversal : {false, true}) {
      for (const uint32_t age : {900u, 1000u, 1500u}) {
        tests.emplace_back("F2_pre_wake_age_" + std::to_string(age) +
                               (reversal ? "_reversal" : "_startup") + clock,
                           [=] { sampleExpiresBeforeWake(reversal, age, start); });
      }
    }
    for (const uint32_t age : {0u, 900u, 1000u, 1500u}) {
      tests.emplace_back("F3_acquisition_age_" + std::to_string(age) + clock,
                         [=] { acquisitionStartControlsFreshness(age, start); });
    }
    for (const bool afterCut : {false, true}) {
      tests.emplace_back(std::string("F4_queued_edge_") + (afterCut ? "at_cut" : "before_cut") + clock,
                         [=] { queuedFaultAtSleepCutLatches(afterCut, start); });
    }
    for (const auto window : {BlankedWindow::Sleeping, BlankedWindow::ReversalSleep,
                              BlankedWindow::Waking}) {
      const std::string interval = window == BlankedWindow::Sleeping ? "sleeping"
                                   : window == BlankedWindow::ReversalSleep ? "reversal_sleep"
                                                                           : "waking";
      for (const bool persistent : {false, true}) {
        for (const bool held : {false, true}) {
          tests.emplace_back("F4_blanking_" + interval +
                                 (persistent ? "_persistent" : "_pulse") +
                                 (held ? "_queued" : "_delivered") + clock,
                             [=] { faultInBlankedWindow(window, persistent, held, start); });
        }
      }
    }
    for (const auto window : {TimerWindow::WakeDelay, TimerWindow::InactivePwmWrite,
                              TimerWindow::ActivePwmWrite}) {
      const std::string interval = window == TimerWindow::WakeDelay ? "wake_delay"
                                   : window == TimerWindow::InactivePwmWrite ? "inactive_pwm"
                                                                            : "active_pwm";
      for (const bool heating : {false, true}) {
        tests.emplace_back("timer_in_flight_" + interval +
                               (heating ? "_heating" : "_cooling") + clock,
                           [=] { timerCutsInFlightOutput(window, heating, start); });
      }
    }
    for (const bool heating : {false, true}) {
      for (const auto exit : {BalanceExit::HighBand, BalanceExit::LowBand,
                              BalanceExit::TargetChange, BalanceExit::BandChange}) {
        const std::string reason = exit == BalanceExit::HighBand ? "high_band"
                                   : exit == BalanceExit::LowBand ? "low_band"
                                   : exit == BalanceExit::TargetChange ? "target_change"
                                                                      : "band_change";
        tests.emplace_back("balance_retention_exit_" + reason +
                               (heating ? "_heating" : "_cooling") + clock,
                           [=] { balanceLearnsUntilApproachNeeded(heating, exit, start); });
      }
    }
  }
  // These cases assert the replaced 10%-bias/balance algorithm. Safety and
  // calibration cases remain; precision behavior is covered separately.
  const std::vector<std::string> replaced = {"balance_direction",
      "model_only_first_order_", "unchanged_settings_balance_",
      "hard_recovery_discards_balance_", "balance_retention_exit_"};
  tests.erase(std::remove_if(tests.begin(), tests.end(), [&](const auto& test) {
    for (const auto& prefix : replaced) if (test.first.rfind(prefix, 0) == 0) return true;
    return false;
  }), tests.end());
  tests.emplace_back("precision_flat_above_target", precisionOffsetKeepsCorrecting);
  tests.emplace_back("precision_quantized_output_reset", precisionQuantizedOutputAndReset);
  tests.emplace_back("precision_hard_recovery", precisionHardRecoveryClearsIntegral);
  int failed = 0;
  for (const auto& test : tests) {
    try {
      test.second();
      std::cout << "PASS " << test.first << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failed << '/' << tests.size() << " tests passed\n";
  return failed == 0 ? 0 : 1;
}
