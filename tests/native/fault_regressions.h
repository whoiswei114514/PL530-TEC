// Shared by both controllers. These cases also run with ESP_PLATFORM defined.
void watchdogWithoutPeripheralCalls() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  setTimeMs(2000);
  events.clear();
  const uint64_t started = fakeMicros;
  controller.checkSampleAgeFromTimer(millis());
  require(pinValues[PIN_DRV_SLEEP] == LOW, "watchdog must cut the driver immediately");
  require(fakeMicros == started, "watchdog cutoff must not wait for current decay");
  require(std::none_of(events.begin(), events.end(), [](const Event& event) {
    return event.kind == "pwm" || event.kind == "delay";
  }), "watchdog must not depend on an interrupted PWM driver call");
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::SensorTimeoutLatched);
}

void watchdogCutBeforeUnlock() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  setTimeMs(2000);
  bool observed = false;
  criticalExitHook = [&] {
    observed = true;
    require(pinValues[PIN_DRV_SLEEP] == LOW,
            "the timeout latch and physical cutoff must precede guard unlock");
    // Emulate the other core accepting a fresh sample at this exact handoff.
    controller.submitTemperature(true, 30, 2200);
    require(controller.status().driverAwake, "fresh sample may recover after the completed cutoff");
  };
  controller.checkSampleAgeFromTimer(2000);
  require(observed, "the cross-core handoff must execute");
  require(pinValues[PIN_DRV_SLEEP] == HIGH,
          "an old watchdog callback must not cut a newly authorized output");
}

void firstPoweredWriteFault(bool calibration) {
  TestController controller;
  if (calibration) startCalibration(controller);
  else { prepare(controller); enable(controller); }
  bool injected = false;
  pwmWriteHook = [&](uint8_t, uint32_t duty) {
    if (!duty || injected) return;
    injected = true;
    pwmWriteHook = nullptr;
    // A completed pulse may be delivered after nFAULT has returned high.
    queueFaultPulse();
    require(pinValues[PIN_DRV_SLEEP] == LOW,
            "nFAULT monitoring must cover the first nonzero PWM write");
  };
  if (calibration) {
    const uint32_t start = phaseStart(controller);
    for (uint32_t elapsed = 200; elapsed <= 2000; elapsed += 200)
      controller.submitTemperature(true, 25, start + elapsed);
  } else {
    controller.submitTemperature(true, 30, 1000);
  }
  require(injected, "test must reach a physical PWM write");
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  if (calibration) {
    require(controller.directionCalibrationStatus().phase == CalibrationPhase::Failed,
            "faulted calibration must not produce a direction candidate");
  }
  controller.submitTemperature(true, 30, millis() + 200);
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
}

void queuedPoweredFaultSurvivesPoll() {
  TestController controller;
  prepare(controller);
  enable(controller);
  controller.submitTemperature(true, 30, 1000);
  holdFaultInterrupt = true;
  queueFaultPulse();
  require(pinValues[PIN_DRV_FAULT] == HIGH && GPIO.status != 0,
          "a completed nFAULT pulse must be waiting for delivery");
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
  holdFaultInterrupt = false;
  deliverFaultInterrupt();
  controller.pollDriverFault();
  assertLatchedOff(controller, ControllerState::DriverFaultLatched);
}
