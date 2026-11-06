#include "config_store.h"
#include "request_validation.h"

#include <Preferences.h>
#include <nvs.h>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace {
constexpr const char* CURRENT = "pl530_tec";
constexpr const char* LEGACY = "pl530-tec";
constexpr const char* CONTROLLER_KEY = "controller_cfg";
constexpr const char* WIFI_KEY = "wifi_cfg";
constexpr const char* DIRECTION_KEY = "tec_direction";
constexpr size_t CONTROLLER_SIZE = 24;
constexpr size_t WIFI_SIZE = 112;
constexpr size_t DIRECTION_SIZE = 16;
constexpr uint16_t VERSION = 1;
static_assert(sizeof(float) == 4, "Storage requires 32-bit floats");

enum class Read { Missing, Ok, Invalid, Error };

// Preferences hides NOT_FOUND behind bool/default values. Probe with NVS only
// to distinguish absence/type/IO errors; all payload IO remains Preferences-based.
class Reader {
 public:
  ~Reader() {
    preferences.end();
    if (opened_) nvs_close(handle_);
  }
  Read open(const char* name) {
    const esp_err_t result = nvs_open(name, NVS_READONLY, &handle_);
    if (result == ESP_ERR_NVS_NOT_FOUND) return Read::Missing;
    if (result != ESP_OK) return Read::Error;
    opened_ = true;
    return preferences.begin(name, true) ? Read::Ok : Read::Error;
  }
  Read blob(const char* key, void* data, size_t expected) {
    size_t length = 0;
    const esp_err_t result = nvs_get_blob(handle_, key, nullptr, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) return Read::Missing;
    if (result == ESP_ERR_NVS_TYPE_MISMATCH) return Read::Invalid;
    if (result != ESP_OK) return Read::Error;
    if (preferences.getBytesLength(key) != length) return Read::Error;
    if (length != expected) return Read::Invalid;
    return preferences.getBytes(key, data, expected) == expected
        ? Read::Ok : Read::Error;
  }
  Read string(const char* key, char* data, size_t capacity) {
    size_t length = 0;
    const esp_err_t result = nvs_get_str(handle_, key, nullptr, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) return Read::Missing;
    if (result == ESP_ERR_NVS_TYPE_MISMATCH) return Read::Invalid;
    if (result != ESP_OK) return Read::Error;
    if (length == 0 || length > capacity) return Read::Invalid;
    if (preferences.getString(key, data, capacity) != length) return Read::Error;
    if (data[length - 1] != '\0' || std::memchr(data, '\0', length - 1))
      return Read::Invalid;
    return Read::Ok;
  }

 private:
  Preferences preferences;
  nvs_handle_t handle_ = 0;
  bool opened_ = false;
};

uint32_t get32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
      (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
void put32(uint8_t* p, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(value >> (8 * i));
}
uint16_t get16(const uint8_t* p) {
  return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
void put16(uint8_t* p, uint16_t value) {
  p[0] = uint8_t(value);
  p[1] = uint8_t(value >> 8);
}
uint32_t crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
  }
  return ~crc;
}
void header(uint8_t* data, size_t size, const char* magic) {
  std::memset(data, 0, size);
  std::memcpy(data, magic, 4);
  put16(data + 4, VERSION);
  put16(data + 6, uint16_t(size));
}
void checksum(uint8_t* data, size_t size) {
  put32(data + size - 4, crc32(data, size - 4));
}
bool validHeader(const uint8_t* data, size_t size, const char* magic) {
  return std::memcmp(data, magic, 4) == 0 &&
      get16(data + 4) == VERSION && get16(data + 6) == size &&
      get32(data + size - 4) == crc32(data, size - 4);
}
void putFloat(uint8_t* p, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  put32(p, bits);
}
float getFloat(const uint8_t* p) {
  const uint32_t bits = get32(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
bool validController(const ControllerSettings& s) {
  return RequestValidation::temperatures(s.targetC, s.pwmHighC, s.pwmLowC);
}
void encodeController(const ControllerSettings& s, uint8_t* data) {
  header(data, CONTROLLER_SIZE, "PL5C");
  putFloat(data + 8, s.targetC);
  putFloat(data + 12, s.pwmHighC);
  putFloat(data + 16, s.pwmLowC);
  checksum(data, CONTROLLER_SIZE);
}
bool decodeController(const uint8_t* data, ControllerSettings& s) {
  if (!validHeader(data, CONTROLLER_SIZE, "PL5C")) return false;
  s.targetC = getFloat(data + 8);
  s.pwmHighC = getFloat(data + 12);
  s.pwmLowC = getFloat(data + 16);
  s.enabled = false;
  return validController(s);
}
bool validWifi(const String& ssid, const String& password) {
  if (ssid.isEmpty()) return password.isEmpty();  // Empty tuple is a tombstone.
  return RequestValidation::wifi(ssid.c_str(), ssid.length(), password.c_str(), password.length());
}
bool validDirection(TecDirection direction) {
  return direction == TecDirection::Unknown || direction == TecDirection::Normal ||
      direction == TecDirection::Reversed;
}
void encodeDirection(TecDirection direction, uint8_t* data) {
  header(data, DIRECTION_SIZE, "PL5D");
  data[8] = static_cast<uint8_t>(direction);
  checksum(data, DIRECTION_SIZE);
}
bool decodeDirection(const uint8_t* data, TecDirection& direction) {
  if (!validHeader(data, DIRECTION_SIZE, "PL5D") || data[9] || data[10] || data[11])
    return false;
  direction = static_cast<TecDirection>(data[8]);
  return validDirection(direction);
}
void encodeWifi(const String& ssid, const String& password, uint8_t* data) {
  header(data, WIFI_SIZE, "PL5W");
  data[8] = uint8_t(ssid.length());
  data[9] = uint8_t(password.length());
  std::memcpy(data + 12, ssid.c_str(), ssid.length());
  std::memcpy(data + 44, password.c_str(), password.length());
  checksum(data, WIFI_SIZE);
}
bool assignString(String& output, const char* bytes, size_t length) {
  output = "";
  if (length && !output.concat(bytes, static_cast<unsigned int>(length)))
    return false;
  return output.length() == length;
}
bool decodeWifi(const uint8_t* data, String& ssid, String& password) {
  if (!validHeader(data, WIFI_SIZE, "PL5W") ||
      data[8] > 32 || data[9] > 64 || data[10] || data[11]) return false;
  for (size_t i = data[8]; i < 32; ++i)
    if (data[12 + i]) return false;
  for (size_t i = data[9]; i < 64; ++i)
    if (data[44 + i]) return false;
  return assignString(ssid, reinterpret_cast<const char*>(data + 12), data[8]) &&
      assignString(password, reinterpret_cast<const char*>(data + 44), data[9]) &&
      validWifi(ssid, password);
}

Read oldController(Reader& reader, ControllerSettings& settings) {
  const Read target = reader.blob("target", &settings.targetC, sizeof(float));
  const Read high = reader.blob("pwm_high", &settings.pwmHighC, sizeof(float));
  const Read low = reader.blob("pwm_low", &settings.pwmLowC, sizeof(float));
  if (target == Read::Error || high == Read::Error || low == Read::Error)
    return Read::Error;
  if (target == Read::Missing && high == Read::Missing && low == Read::Missing)
    return Read::Missing;
  return target == Read::Ok && high == Read::Ok && low == Read::Ok &&
      validController(settings) ? Read::Ok : Read::Invalid;
}
Read oldWifi(Reader& reader, String& ssid, String& password) {
  char ssidBytes[33] = {};
  char passwordBytes[65] = {};
  const Read name = reader.string("sta_ssid", ssidBytes, sizeof(ssidBytes));
  const Read pass = reader.string("sta_pass", passwordBytes, sizeof(passwordBytes));
  if (name == Read::Error || pass == Read::Error) return Read::Error;
  if (name == Read::Missing && pass == Read::Missing) return Read::Missing;
  if (name != Read::Ok || pass != Read::Ok) return Read::Invalid;
  if (!assignString(ssid, ssidBytes, std::strlen(ssidBytes)) ||
      !assignString(password, passwordBytes, std::strlen(passwordBytes)))
    return Read::Error;
  return validWifi(ssid, password) ? Read::Ok : Read::Invalid;
}

const char* writeRecord(const char* key, const uint8_t* data, size_t size) {
  uint8_t previous[WIFI_SIZE] = {};
  {
    Reader reader;
    const Read opened = reader.open(CURRENT);
    if (opened == Read::Error) return "Storage read open failed";
    if (opened == Read::Ok) {
      const Read read = reader.blob(key, previous, size);
      if (read == Read::Error) return "Storage comparison read failed";
      if (read == Read::Ok && std::memcmp(previous, data, size) == 0) return nullptr;
    }
  }
  Preferences preferences;
  if (!preferences.begin(CURRENT, false)) return "Storage write open failed";
  const size_t written = preferences.putBytes(key, data, size);
  preferences.end();
  if (written != size) return "Storage write or commit failed";
  // Reopen to avoid reporting success on a short/corrupt persisted record.
  Reader verify;
  if (verify.open(CURRENT) != Read::Ok ||
      verify.blob(key, previous, size) != Read::Ok ||
      std::memcmp(previous, data, size) != 0)
    return "Storage write verification failed";
  return nullptr;
}
}  // namespace

bool ConfigStore::loadController(ControllerSettings& settings) {
  error_ = "";
  settings = ControllerSettings{};
  settings.enabled = false;
  for (const char* name : {CURRENT, LEGACY}) {
    ControllerSettings candidate;
    Read old = Read::Missing;
    {
      Reader reader;
      const Read opened = reader.open(name);
      if (opened == Read::Missing) continue;
      if (opened != Read::Ok) return fail("Controller storage open failed");
      if (name == CURRENT) {
        uint8_t data[CONTROLLER_SIZE] = {};
        const Read read = reader.blob(CONTROLLER_KEY, data, sizeof(data));
        if (read == Read::Error) return fail("Controller record read failed");
        if (read != Read::Missing) {
          if (read != Read::Ok || !decodeController(data, candidate))
            return fail("Controller record invalid or unsupported");
          settings = candidate;
          return true;
        }
      }
      old = oldController(reader, candidate);
    }
    if (old == Read::Missing) continue;
    if (old == Read::Error) return fail("Controller migration read failed");
    if (old != Read::Ok) return fail("Controller migration tuple invalid or incomplete");
    if (!saveController(candidate)) return false;
    settings = candidate;
    settings.enabled = false;
    return true;
  }
  return true;
}

bool ConfigStore::saveController(const ControllerSettings& settings) {
  error_ = "";
  if (!validController(settings)) return fail("Controller settings invalid");
  uint8_t data[CONTROLLER_SIZE];
  encodeController(settings, data);
  const char* error = writeRecord(CONTROLLER_KEY, data, sizeof(data));
  return error ? fail(error) : true;
}

bool ConfigStore::loadWifi(String& ssid, String& password) {
  error_ = "";
  ssid = "";
  password = "";
  for (const char* name : {CURRENT, LEGACY}) {
    String candidateSsid;
    String candidatePassword;
    Read old = Read::Missing;
    {
      Reader reader;
      const Read opened = reader.open(name);
      if (opened == Read::Missing) continue;
      if (opened != Read::Ok) return fail("WiFi storage open failed");
      if (name == CURRENT) {
        uint8_t data[WIFI_SIZE] = {};
        const Read read = reader.blob(WIFI_KEY, data, sizeof(data));
        if (read == Read::Error) return fail("WiFi record read failed");
        if (read != Read::Missing) {
          if (read != Read::Ok || !decodeWifi(data, candidateSsid, candidatePassword))
            return fail("WiFi record invalid or unsupported");
          // Move assignment transfers the already checked String allocations.
          ssid = static_cast<String&&>(candidateSsid);
          password = static_cast<String&&>(candidatePassword);
          return true;
        }
      }
      old = oldWifi(reader, candidateSsid, candidatePassword);
    }
    if (old == Read::Missing) continue;
    if (old == Read::Error) return fail("WiFi migration read failed");
    if (old != Read::Ok) return fail("WiFi migration tuple invalid or incomplete");
    if (!saveWifi(candidateSsid, candidatePassword)) return false;
    ssid = static_cast<String&&>(candidateSsid);
    password = static_cast<String&&>(candidatePassword);
    return true;
  }
  return true;
}

bool ConfigStore::saveWifi(const String& ssid, const String& password) {
  error_ = "";
  if (!validWifi(ssid, password)) return fail("WiFi credentials invalid");
  uint8_t data[WIFI_SIZE];
  encodeWifi(ssid, password, data);
  const char* error = writeRecord(WIFI_KEY, data, sizeof(data));
  return error ? fail(error) : true;
}

bool ConfigStore::clearWifi() {
  return saveWifi(String(), String());
}

bool ConfigStore::loadDirection(TecDirection& direction) {
  error_ = "";
  direction = TecDirection::Unknown;
  Reader reader;
  const Read opened = reader.open(CURRENT);
  if (opened == Read::Missing) return true;
  if (opened != Read::Ok) return fail("Direction storage open failed");
  uint8_t data[DIRECTION_SIZE] = {};
  const Read read = reader.blob(DIRECTION_KEY, data, sizeof(data));
  if (read == Read::Missing) return true;
  if (read != Read::Ok || !decodeDirection(data, direction)) {
    direction = TecDirection::Unknown;
    return fail("Direction record invalid or unsupported");
  }
  return true;
}

bool ConfigStore::saveDirection(TecDirection direction) {
  error_ = "";
  if (!validDirection(direction)) return fail("Direction value invalid");
  uint8_t data[DIRECTION_SIZE] = {};
  encodeDirection(direction, data);
  const char* error = writeRecord(DIRECTION_KEY, data, sizeof(data));
  return error ? fail(error) : true;
}
