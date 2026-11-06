#pragma once
#include <cmath>
#include <cstdint>
#include <string>

namespace fakeArduino {
inline bool failConcat = false;
}

class String {
 public:
  bool isEmpty() const { return length() == 0; }
  String() = default;
  String(const char* value) : value_(value) {}
  String(const std::string& value) : value_(value) {}
  unsigned int length() const { return static_cast<unsigned int>(value_.size()); }
  const char* c_str() const { return value_.c_str(); }
  char charAt(unsigned int index) const { return value_.at(index); }
  bool concat(const char* bytes, unsigned int length) {
    if (fakeArduino::failConcat) return false;
    value_.append(bytes, length);
    return true;
  }
  bool operator==(const char* value) const { return value_ == value; }
  bool operator==(const String& value) const { return value_ == value.value_; }

 private:
  std::string value_;
};

class IPAddress {
 public:
  IPAddress(uint8_t, uint8_t, uint8_t, uint8_t) {}
};
