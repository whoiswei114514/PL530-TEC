#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>

using std::isfinite;
using std::max;
using std::min;

#define IRAM_ATTR
constexpr uint8_t LOW = 0;
constexpr uint8_t HIGH = 1;
constexpr uint8_t OUTPUT = 1;
constexpr uint8_t OUTPUT_OPEN_DRAIN = 3;
constexpr uint8_t INPUT_PULLUP = 2;
constexpr uint8_t INPUT = 0;
constexpr int FALLING = 3;
constexpr int HEX = 16;
#define F(value) value
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
void portENTER_CRITICAL(portMUX_TYPE*);
void portEXIT_CRITICAL(portMUX_TYPE*);
void portENTER_CRITICAL_ISR(portMUX_TYPE*);
void portEXIT_CRITICAL_ISR(portMUX_TYPE*);

class String {
 public:
  String() = default;
  String(const char* text) : value(text) {}
  String(unsigned int number, int base) {
    std::ostringstream stream;
    if (base == HEX) stream << std::hex;
    stream << number;
    value = stream.str();
  }
  void clear() { value.clear(); }
  bool isEmpty() const { return value.empty(); }
  const char* c_str() const { return value.c_str(); }
  String operator+(const String& other) const { return String((value + other.value).c_str()); }
 private:
  std::string value;
};

struct IPAddress {
  IPAddress(uint8_t, uint8_t, uint8_t, uint8_t) {}
};

void digitalWrite(uint8_t pin, uint8_t value);
int digitalRead(uint8_t pin);
void pinMode(uint8_t pin, uint8_t mode);
void attachInterruptArg(uint8_t pin, void (*callback)(void*), void* argument, int mode);
void delayMicroseconds(uint32_t duration);
void delay(uint32_t duration);
uint32_t millis();
uint32_t micros();
double ledcSetup(uint8_t channel, double frequency, uint8_t resolution);
void ledcAttachPin(uint8_t pin, uint8_t channel);
void ledcWrite(uint8_t channel, uint32_t duty);

struct MockSerial {
  int printf(const char*, ...) { return 0; }
  void println(const char*) {}
};
extern MockSerial Serial;
