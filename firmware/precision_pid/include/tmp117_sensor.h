#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "config.h"

class Tmp117Sensor {
public:
  bool begin(TwoWire& wire);
  bool begin(uint8_t sda, uint8_t scl);
  void end();
  bool readTemperature(float& temperatureC);
  const char* lastError() const { return m_lastError.c_str(); }

private:
  enum class BusMode : uint8_t { None, Hardware, Software };

  bool initializeDevice();
  bool readRegister(uint8_t registerAddress, uint16_t& value);
  bool writeRegister(uint8_t registerAddress, uint16_t value);
  bool readHardwareRegister(uint8_t registerAddress, uint16_t& value);
  bool writeHardwareRegister(uint8_t registerAddress, uint16_t value);
  bool readSoftwareRegister(uint8_t registerAddress, uint16_t& value);
  bool writeSoftwareRegister(uint8_t registerAddress, uint16_t value);
  void releaseLine(uint8_t pin);
  void driveLow(uint8_t pin);
  void busDelay();
  bool waitHigh(uint8_t pin);
  bool startCondition();
  bool stopCondition();
  void recoverBus();
  bool writeByte(uint8_t value);
  bool readByte(uint8_t& value, bool acknowledge);

  TwoWire* m_wire = nullptr;
  BusMode m_busMode = BusMode::None;
  uint8_t m_sda = 0;
  uint8_t m_scl = 0;
  bool m_initialized = false;
  String m_lastError = "not initialized";
};
