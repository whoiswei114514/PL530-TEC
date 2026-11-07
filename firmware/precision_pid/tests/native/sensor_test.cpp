#include "tmp117_sensor.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

void delay(uint32_t) {}
void delayMicroseconds(uint32_t) {}
uint32_t micros() { return 0; }
void digitalWrite(uint8_t, uint8_t) {}
int digitalRead(uint8_t) { return HIGH; }
void pinMode(uint8_t, uint8_t) {}
void require(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
void setup(Tmp117Sensor& sensor, TwoWire& wire) {
  wire.registers[0x0F] = 0x0117;
  require(sensor.begin(wire), "sensor initialization");
  require(wire.lastAddress == 0x48, "board TMP117 address");
}
void result(TwoWire& wire, float temp) {
  wire.registers[1] = 0x2020;
  wire.registers[0] = static_cast<uint16_t>(static_cast<int16_t>(temp * 128));
}

int main() {
  std::vector<std::pair<const char*, std::function<void()>>> tests = {
    {"signed_temperature_and_freshness", [] {
      Tmp117Sensor sensor; TwoWire wire; setup(sensor, wire);
      for (float expected : {-55.0f, -0.125f, 0.0f, 25.0f, 150.0f}) {
        float temp = NAN; result(wire, expected);
        require(sensor.readTemperature(temp) && temp == expected, "signed conversion");
        require(!sensor.readTemperature(temp) && std::isnan(temp), "reject duplicate conversion");
      }
    }},
    {"wrong_id_and_nack", [] {
      Tmp117Sensor sensor; TwoWire wire; float temp = 25;
      wire.registers[15] = 0x0116;
      require(!sensor.begin(wire), "reject non-TMP117");
      require(!sensor.readTemperature(temp), "failed begin must remain uninitialized");
      wire.nack = true;
      require(!sensor.begin(wire), "reject NACK");
    }},
    {"configuration_readback", [] {
      Tmp117Sensor sensor; TwoWire wire;
      wire.registers[15] = 0x0117; wire.registers[1] = 0x0220;
      wire.ignoreConfigurationWrite = true;
      require(!sensor.begin(wire), "ACK without applying config must fail");
    }},
    {"runtime_reset_or_shutdown", [] {
      for (uint16_t config : {0x2220, 0x2400, 0x3000, 0x2000}) {
        Tmp117Sensor sensor; TwoWire wire; setup(sensor, wire);
        wire.registers[1] = config;
        float temp = 0;
        require(!sensor.readTemperature(temp) && std::isnan(temp), "reject reset/default/busy config");
      }
    }},
    {"short_read_and_error_text", [] {
      Tmp117Sensor sensor; TwoWire wire; setup(sensor, wire);
      float temp = 0; result(wire, 25);
      require(sensor.readTemperature(temp), "first read");
      result(wire, 26); wire.shortRead = true;
      require(!sensor.readTemperature(temp) && std::isnan(temp), "short I2C read");
      require(std::string(sensor.lastError()).find("正常") == std::string::npos, "failure must not say normal");
    }},
    {"reset_value_and_range", [] {
      Tmp117Sensor sensor; TwoWire wire; setup(sensor, wire);
      float temp = 0;
      for (uint16_t raw : {0x8000, 0x7FFF}) {
        wire.registers[1] = 0x2020; wire.registers[0] = raw;
        require(!sensor.readTemperature(temp) && std::isnan(temp), "reject invalid range");
      }
    }},
  };
  int failed = 0;
  for (const auto& test : tests) {
    try { test.second(); std::cout << "PASS " << test.first << '\n'; }
    catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << test.first << ": " << error.what() << '\n'; }
  }
  std::cout << tests.size() - failed << '/' << tests.size() << " sensor tests passed\n";
  return failed ? 1 : 0;
}
