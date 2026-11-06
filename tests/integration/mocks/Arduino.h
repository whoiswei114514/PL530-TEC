#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using std::isfinite;
using std::min;
using std::max;
inline uint32_t mockNow = 1000;
inline std::vector<std::string> mockEvents;
inline uint32_t millis() { return mockNow; }
constexpr int SERIAL_8N1 = 0;
#define PROGMEM

namespace MockString {
inline std::string failValue;
inline int matchingCopiesBeforeFailure = -1;
inline int failedCopies = 0;
inline void reset() {
  failValue.clear();
  matchingCopiesBeforeFailure = -1;
  failedCopies = 0;
}
}

// Preserve embedded NULs and numeric concatenation used by Arduino String.
class String {
 public:
  std::string value;
  bool valid = true;
  String() = default;
  String(const char* text) : value(text ? text : ""), valid(text != nullptr) {}
  String(const std::string& text) : value(text) {}
  String(const String& other) { copy(other); }
  String& operator=(const String& other) {
    if (this != &other) copy(other);
    return *this;
  }
  String(String&& other) noexcept : value(std::move(other.value)), valid(other.valid) {
    other.value.clear(); other.valid = false;
  }
  String& operator=(String&& other) noexcept {
    if (this != &other) {
      value = std::move(other.value); valid = other.valid;
      other.value.clear(); other.valid = false;
    }
    return *this;
  }
  void copy(const String& other) {
    if (other.value == MockString::failValue && MockString::matchingCopiesBeforeFailure >= 0 &&
        MockString::matchingCopiesBeforeFailure-- == 0) {
      value.clear(); valid = false; ++MockString::failedCopies;
      return;
    }
    value = other.value; valid = other.valid;
  }
  template <typename T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  String(T number) : value(std::to_string(number)) {}
  String(float number, unsigned int digits) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(digits) << number;
    value = out.str();
  }
  unsigned int length() const { return static_cast<unsigned int>(value.size()); }
  const char* c_str() const { return valid ? value.c_str() : nullptr; }
  char charAt(unsigned int index) const { return value.at(index); }
  void reserve(size_t count) { value.reserve(count); }
  void clear() { value.clear(); }
  bool isEmpty() const { return value.empty(); }
  bool startsWith(const char* prefix) const { return value.rfind(prefix, 0) == 0; }
  String& operator+=(const String& other) { value += other.value; return *this; }
  String& operator+=(char c) { value += c; return *this; }
  long toInt() const { return std::strtol(value.c_str(), nullptr, 10); }
  template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char>, int> = 0>
  String& operator+=(T number) { value += std::to_string(number); return *this; }
  friend String operator+(const String& a, const String& b) { return a.value + b.value; }
  friend bool operator==(const String& a, const String& b) {
    if (!a.c_str() || !b.c_str()) return a.isEmpty() && b.isEmpty();
    return a.length() == b.length() && std::strcmp(a.c_str(), b.c_str()) == 0;
  }
  friend bool operator!=(const String& a, const String& b) { return !(a == b); }
  // Arduino 2.0.17 compares C-string literals with strcmp, ignoring suffixes after NUL.
  friend bool operator==(const String& a, const char* b) {
    if (!a.c_str() || !b) return a.isEmpty() && (!b || !*b);
    return std::strcmp(a.c_str(), b) == 0;
  }
  friend bool operator!=(const String& a, const char* b) { return !(a == b); }
};

struct IPAddress {
  std::string address;
  uint32_t numeric;
  IPAddress(int a, int b, int c, int d)
      : address(std::to_string(a) + "." + std::to_string(b) + "." +
                std::to_string(c) + "." + std::to_string(d)),
        numeric(uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24)) {}
  operator uint32_t() const { return numeric; }
  String toString() const { return address; }
};
struct MockSerial {
  template <typename... T> void begin(T...) {}
  template <typename... T> void printf(const char*, T...) {}
  void println(const char*) {}
};
inline MockSerial Serial;
inline int digitalRead(uint8_t) { return 1; }
struct MockEsp { unsigned int getPsramSize() const { return 0; } };
inline MockEsp ESP;
