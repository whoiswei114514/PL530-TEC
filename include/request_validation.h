#pragma once

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace RequestValidation {
inline bool decimal(const char* text, size_t length, float& value) {
  if (!text || length == 0 || length > 24 || std::strlen(text) != length) return false;
  size_t i = 0;
  if (text[i] == '+' || text[i] == '-') ++i;
  bool digit = false;
  while (i < length && text[i] >= '0' && text[i] <= '9') { digit = true; ++i; }
  if (i < length && text[i] == '.') {
    ++i;
    while (i < length && text[i] >= '0' && text[i] <= '9') { digit = true; ++i; }
  }
  if (!digit || i != length) return false;
  errno = 0;
  char* end = nullptr;
  const float parsed = std::strtof(text, &end);
  if (errno == ERANGE || end != text + length || !std::isfinite(parsed)) return false;
  value = parsed;
  return true;
}

inline bool temperatures(float target, float high, float low) {
  constexpr float epsilon = 0.00001f;
  return std::isfinite(target) && std::isfinite(high) && std::isfinite(low) &&
      low >= 10.0f && high <= 55.0f && target >= 10.2f && target <= 54.8f &&
      target - low >= 0.2f - epsilon && high - target >= 0.2f - epsilon;
}

inline bool enabled(const char* text, bool& result) {
  if (!text) return false;
  if (!std::strcmp(text, "true") || !std::strcmp(text, "on") || !std::strcmp(text, "1")) {
    result = true;
    return true;
  }
  if (!std::strcmp(text, "false") || !std::strcmp(text, "off") ||
      !std::strcmp(text, "0") || !*text) {
    result = false;
    return true;
  }
  return false;
}

inline bool wifi(const char* ssid, size_t ssidLength, const char* password, size_t passwordLength) {
  if (!ssid || !password || ssidLength == 0 || ssidLength > 32 ||
      std::strlen(ssid) != ssidLength || std::strlen(password) != passwordLength) return false;
  if (passwordLength == 0 || (passwordLength >= 8 && passwordLength <= 63)) return true;
  if (passwordLength != 64) return false;
  for (size_t i = 0; i < passwordLength; ++i) {
    const char c = password[i];
    if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') && !(c >= 'A' && c <= 'F')) return false;
  }
  return true;
}
}  // namespace RequestValidation
