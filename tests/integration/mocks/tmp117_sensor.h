#pragma once
#include <Arduino.h>
#include <Wire.h>
class Tmp117Sensor {
 public:
  bool beginResult = false, readResult = false;
  int beginCalls = 0, readCalls = 0;
  uint32_t beginDelayMs = 0, readDelayMs = 0;
  float temperature = 25;
  bool begin(TwoWire&) { ++beginCalls; mockNow += beginDelayMs; return beginResult; }
  bool begin(unsigned char, unsigned char) {
    ++beginCalls;
    mockNow += beginDelayMs;
    return beginResult;
  }
  void end() {}
  bool readTemperature(float& value) {
    ++readCalls; mockNow += readDelayMs; value = readResult ? temperature : NAN; return readResult;
  }
  const char* lastError() const { return "mock sensor unavailable"; }
};
