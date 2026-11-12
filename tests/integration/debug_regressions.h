void debugArgumentsRejectBeforeMutation() {
  const std::vector<String> badCounts = {"", "NaN", "1junk", "1.5", "-1", "+1", " 1",
      "4294967296", std::string("1\0tail", 6), String(TEC_PWM_MAX_COUNT + 1u)};
  for (const auto& value : badCounts) {
    for (const char* field : {"in1", "in2"}) {
      reset(); setupRoutes(); server.args = {{field, value}};
      server.dispatch("/api/debug/pwm", HTTP_GET); response(400, "debug-invalid-count");
      require(!controller.manualWakeHold() && controller.debugIn1Duty() == 0 &&
                  controller.debugIn2Duty() == 0, "bad PWM input must not wake or drive");
    }
  }
  reset(); setupRoutes(); server.args.clear();
  server.dispatch("/api/debug/pwm", HTTP_GET); response(400, "debug-missing-counts");
  require(!controller.manualWakeHold(), "missing PWM input must not wake");
  for (const String& value : std::vector<String>{"", "2", "1junk", " 1", std::string("1\0tail", 6)}) {
    reset(); setupRoutes(); controller.setManualWakeHold(true); controller.setDebugPwm(10, 0);
    server.args = {{"state", value}};
    server.dispatch("/api/debug/wake", HTTP_GET); response(400, "debug-invalid-wake");
    require(controller.manualWakeHold() && controller.debugIn1Duty() == 10,
            "invalid wake state must not change existing output");
  }
  for (unsigned count : {0u, 1u, static_cast<unsigned>(TEC_PWM_MAX_COUNT)}) {
    reset(); setupRoutes(); server.args = {{"in1", String(count)}};
    server.dispatch("/api/debug/pwm", HTTP_GET); response(200, "debug-valid-count");
    require(controller.manualWakeHold() && controller.debugIn1Duty() == static_cast<int>(count) &&
                controller.debugIn2Duty() == 0, "valid count and omitted other channel must apply exactly");
    server.args = {{"state", "0"}};
    server.dispatch("/api/debug/wake", HTTP_GET); response(200, "debug-valid-sleep");
    require(!controller.manualWakeHold() && controller.debugIn1Duty() == 0,
            "explicit sleep must remain available");
  }
}

void serialDirectionChecksSaveAndState() {
  for (char command : {'r', 'n', 'u'}) {
    reset(); controller.setManualWakeHold(true); Serial.input = command; loop();
    require(configStore.directionSaves.empty() && controller.manualWakeHold(),
            "serial direction must reject a live manual output without changing NVS");
    require(Serial.output.find("REJECTED") != std::string::npos,
            "serial refusal must be reported");
  }
  reset(); configStore.directionSaveOk = false; Serial.input = "r"; loop();
  require(!directionSaved && controller.tecDirection() == TecDirection::Unknown &&
              !directionError.isEmpty(), "serial write failure must not claim saved direction");
  require(Serial.output.find("REJECTED") != std::string::npos,
          "serial storage failure must be reported");
  reset(); mutableStatus().state = ControllerState::DriverFaultLatched;
  Serial.input = "n"; loop();
  require(configStore.directionSaves.empty() && MockController::resetCalls == 0,
          "direction assignment must not implicitly clear a driver fault");
  reset(); Serial.input = "r"; loop();
  require(directionSaved && controller.tecDirection() == TecDirection::Reversed &&
              configStore.directionSaves == std::vector<TecDirection>{TecDirection::Reversed},
          "stopped serial override must save and apply the requested direction");
  reset(); MockController::directionApplyOk = false; Serial.input = "n"; loop();
  require(!directionError.isEmpty() && Serial.output.find("REJECTED") != std::string::npos,
          "runtime application failure must not print success");
}

void serialCalibrationInvalidatesAndSaves() {
  reset(); configStore.storedDirection = TecDirection::Normal;
  configStore.directionSaveOk = false; Serial.input = "d"; loop();
  require(MockController::pulseStarts == 0 && controller.tecDirection() == TecDirection::Unknown &&
              !directionSaved && !directionError.isEmpty(),
          "failed durable invalidation must prevent serial calibration pulses");
  reset(); mutableStatus().state = ControllerState::DriverFaultLatched;
  Serial.input = "d"; loop();
  require(MockController::pulseStarts == 0 && MockController::resetCalls == 0 &&
              configStore.directionSaves.empty(), "serial detection must preserve a fault latch");
  reset(); configStore.storedDirection = TecDirection::Normal;
  configStore.beforeDirectionSave = [] {
    require(MockController::startCalls == 0, "serial detection must invalidate before starting");
  };
  Serial.input = "d"; loop();
  require(configStore.directionSaves == std::vector<TecDirection>{TecDirection::Unknown} &&
              controller.tecDirection() == TecDirection::Unknown && !calibrationResultHandled &&
              MockController::calibration.active, "serial detection must use the durable lifecycle");
  configStore.beforeDirectionSave = nullptr;
  finishDirection(CalibrationPhase::Complete, TecDirection::Reversed);
  serviceDirectionCalibration();
  require(configStore.directionSaves ==
              std::vector<TecDirection>{TecDirection::Unknown, TecDirection::Reversed} &&
              directionSaved && controller.tecDirection() == TecDirection::Reversed,
          "serial detection result must be saved and applied exactly once");
}
