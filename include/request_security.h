#pragma once

#include <Arduino.h>
#include <cstring>

namespace RequestSecurity {
inline bool exact(const String& value, const String& expected) {
  // Arduino's comparison with a C-string literal ignores bytes after a NUL.
  if (value.length() != expected.length()) return false;
  if (value.length() == 0) return true;
  const char* actual = value.c_str();
  const char* wanted = expected.c_str();
  return actual != nullptr && wanted != nullptr &&
      std::memcmp(actual, wanted, value.length()) == 0;
}

inline bool authority(const String& value, const String& address) {
  return exact(value, address) || exact(value, address + ":80");
}

inline bool origin(const String& value, const String& address) {
  const String expected = String("http://") + address;
  return exact(value, expected) || exact(value, expected + ":80");
}
}  // namespace RequestSecurity
