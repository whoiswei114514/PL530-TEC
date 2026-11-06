#include "config_store.h"
#include "nvs.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
constexpr const char* CURRENT = "pl530_tec";
constexpr const char* LEGACY = "pl530-tec";
constexpr const char* CONTROL = "controller_cfg";
constexpr const char* WIFI = "wifi_cfg";
constexpr const char* DIRECTION = "tec_direction";
int checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
  std::exit(1); } } while (false)

void reset() {
  CHECK(fakeNvs::state.handles.empty());
  fakeNvs::reset();
  fakeArduino::failConcat = false;
}
void oldController(const char* name, float target = 27.0f) {
  fakeNvs::number(name, "target", target);
  fakeNvs::number(name, "pwm_high", 32.0f);
  fakeNvs::number(name, "pwm_low", 22.0f);
}
void oldWifi(const char* name) {
  fakeNvs::text(name, "sta_ssid", "fixture-network");
  fakeNvs::text(name, "sta_pass", "fixture-only");
}
std::vector<uint8_t>& record(const char* key) {
  return fakeNvs::state.spaces.at(CURRENT).at(key).bytes;
}
void rechecksum(std::vector<uint8_t>& bytes) {
  uint32_t crc = 0xffffffffu;
  for (size_t i = 0; i < bytes.size() - 4; ++i) {
    crc ^= bytes[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
  }
  crc = ~crc;
  for (unsigned i = 0; i < 4; ++i)
    bytes[bytes.size() - 4 + i] = uint8_t(crc >> (8 * i));
}
void expectEmptyFailure(ConfigStore& store) {
  String ssid("stale"), password("stale");
  CHECK(!store.loadWifi(ssid, password));
  CHECK(ssid.length() == 0 && password.length() == 0);
  CHECK(store.lastError()[0] != '\0');
}
void expectDefaultsFailure(ConfigStore& store) {
  ControllerSettings s;
  s.targetC = 40;
  s.enabled = true;
  CHECK(!store.loadController(s));
  CHECK(s.targetC == DEFAULT_TARGET_C && s.pwmLowC == DEFAULT_PWM_LOW_C &&
        s.pwmHighC == DEFAULT_PWM_HIGH_C && !s.enabled);
  CHECK(store.lastError()[0] != '\0');
}
void freshAndOpenErrors() {
  reset();
  ConfigStore store;
  ControllerSettings s;
  s.targetC = 40;
  s.enabled = true;
  String ssid("stale"), password("stale");
  CHECK(store.loadController(s));
  CHECK(s.targetC == DEFAULT_TARGET_C && !s.enabled);
  CHECK(store.loadWifi(ssid, password));
  CHECK(ssid.length() == 0 && password.length() == 0);
  CHECK(fakeNvs::state.spaces.empty() && fakeNvs::state.puts == 0);
  fakeNvs::state.spaces[CURRENT] = {};
  CHECK(store.loadController(s) && store.loadWifi(ssid, password));
  fakeNvs::state.openFailNamespace = CURRENT;
  expectDefaultsFailure(store);
  expectEmptyFailure(store);
  CHECK(!store.clearWifi());
  fakeNvs::state.openFailNamespace = LEGACY;
  expectDefaultsFailure(store);
  expectEmptyFailure(store);
  fakeNvs::state.openFailNamespace = "";
  fakeNvs::state.failBeginAt = fakeNvs::state.beginCalls + 1;
  expectEmptyFailure(store);
  CHECK(fakeNvs::state.puts == 0);
}
void roundTripAndWear() {
  reset();
  ConfigStore store;
  ControllerSettings s;
  s.targetC = 26;
  s.enabled = true;
  CHECK(store.saveController(s));
  CHECK(record(CONTROL).size() == 24);
  CHECK(fakeNvs::state.puts == 1 && fakeNvs::state.commits == 1);
  s.enabled = false;
  CHECK(store.saveController(s));
  ConfigStore reboot;
  CHECK(reboot.saveController(s));
  CHECK(fakeNvs::state.puts == 1);
  CHECK(reboot.loadController(s) && s.targetC == 26 && !s.enabled);
  CHECK(store.saveWifi("fixture-network", "fixture-only"));
  CHECK(record(WIFI).size() == 112);
  CHECK(fakeNvs::state.puts == 2 && fakeNvs::state.commits == 2);
  CHECK(reboot.saveWifi("fixture-network", "fixture-only"));
  CHECK(fakeNvs::state.puts == 2);
  String ssid, pass;
  CHECK(reboot.loadWifi(ssid, pass));
  CHECK(ssid == "fixture-network" && pass == "fixture-only");
  CHECK(store.saveWifi("fixture-network", "changed-fixture"));
  CHECK(fakeNvs::state.puts == 3);
}
void controllerValidation() {
  reset();
  ConfigStore store;
  ControllerSettings s;
  s.targetC = 10.2f; s.pwmLowC = 10; s.pwmHighC = 10.4f;
  CHECK(store.saveController(s));
  s.targetC = 54.8f; s.pwmLowC = 54.6f; s.pwmHighC = 55;
  CHECK(store.saveController(s));
  s.targetC = 25.1f; s.pwmLowC = 24.9f; s.pwmHighC = 25.3f;
  CHECK(store.saveController(s));
  s.pwmLowC = s.targetC - 0.2f + 1e-5f;
  s.pwmHighC = s.targetC + 0.2f - 1e-5f;
  CHECK(store.saveController(s));
  const ControllerSettings valid = s;
  const int writes = fakeNvs::state.puts;
  for (int field = 0; field < 3; ++field) {
    for (float value : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
      s = valid;
      if (field == 0) s.targetC = value;
      if (field == 1) s.pwmHighC = value;
      if (field == 2) s.pwmLowC = value;
      CHECK(!store.saveController(s));
    }
  }
  s = valid; s.targetC = 10.199f; CHECK(!store.saveController(s));
  s = valid; s.targetC = 54.801f; CHECK(!store.saveController(s));
  s = valid; s.pwmLowC = 9.999f; CHECK(!store.saveController(s));
  s = valid; s.pwmHighC = 55.001f; CHECK(!store.saveController(s));
  s = valid; s.pwmLowC = s.targetC - 0.2f + 2e-5f;
  CHECK(!store.saveController(s));
  s = valid; s.pwmHighC = s.targetC + 0.2f - 2e-5f;
  CHECK(!store.saveController(s));
  CHECK(fakeNvs::state.puts == writes);
  CHECK(store.saveController(valid) && store.lastError()[0] == '\0');
}
void wifiValidation() {
  reset();
  ConfigStore store;
  CHECK(store.saveWifi(String(std::string(32, 's')), ""));
  CHECK(store.saveWifi("fixture", String(std::string(8, 'p'))));
  CHECK(store.saveWifi("fixture", String(std::string(63, 'p'))));
  CHECK(store.saveWifi("fixture", String(std::string(64, 'a'))));
  CHECK(store.saveWifi("fixture", String(std::string(64, 'F'))));
  const int writes = fakeNvs::state.puts;
  CHECK(!store.saveWifi(String(std::string(33, 's')), ""));
  CHECK(!store.saveWifi("fixture", "short"));
  CHECK(!store.saveWifi("fixture", String(std::string(64, 'g'))));
  CHECK(!store.saveWifi("fixture", String(std::string(65, 'a'))));
  CHECK(!store.saveWifi("", "fixture-only"));
  CHECK(!store.saveWifi(String(std::string("ab\0cd", 5)), ""));
  CHECK(!store.saveWifi("fixture", String(std::string("1234\0" "678", 8))));
  CHECK(fakeNvs::state.puts == writes);
  CHECK(store.clearWifi());
  String ssid, pass;
  CHECK(store.loadWifi(ssid, pass) && ssid.length() == 0 && pass.length() == 0);
}
void migrations() {
  for (const char* name : {CURRENT, LEGACY}) {
    reset();
    ConfigStore store;
    oldController(name);
    oldWifi(name);
    const auto before = fakeNvs::state.spaces.at(name);
    ControllerSettings s;
    String ssid, pass;
    CHECK(store.loadController(s) && s.targetC == 27 && !s.enabled);
    CHECK(store.loadWifi(ssid, pass));
    CHECK(ssid == "fixture-network" && pass == "fixture-only");
    for (const auto& item : before) {
      const auto& retained = fakeNvs::state.spaces.at(name).at(item.first);
      CHECK(retained.type == item.second.type && retained.bytes == item.second.bytes);
    }
    CHECK(fakeNvs::state.puts == 2);
    CHECK(store.loadController(s) && store.loadWifi(ssid, pass));
    CHECK(fakeNvs::state.puts == 2);
  }
  reset();
  oldController(CURRENT, 28);
  oldController(LEGACY, 29);
  ConfigStore store;
  ControllerSettings s;
  CHECK(store.loadController(s) && s.targetC == 28);
  reset();
  oldWifi(LEGACY);
  fakeNvs::text(LEGACY, "sta_pass", "");
  String ssid, pass;
  CHECK(store.loadWifi(ssid, pass) && pass.length() == 0);
  reset();
  fakeNvs::text(LEGACY, "sta_ssid", "");
  fakeNvs::text(LEGACY, "sta_pass", "");
  CHECK(store.loadWifi(ssid, pass));
  CHECK(record(WIFI).size() == 112);
}
void incompleteAndInvalidLegacy() {
  ConfigStore store;
  for (unsigned mask = 1; mask < 7; ++mask) {
    reset();
    oldController(LEGACY);
    if (mask & 1) fakeNvs::number(CURRENT, "target", 25);
    if (mask & 2) fakeNvs::number(CURRENT, "pwm_high", 30);
    if (mask & 4) fakeNvs::number(CURRENT, "pwm_low", 20);
    expectDefaultsFailure(store);
    CHECK(fakeNvs::state.puts == 0);
  }
  for (const char* key : {"sta_ssid", "sta_pass"}) {
    reset();
    oldWifi(LEGACY);
    fakeNvs::text(CURRENT, key, "fixture-only");
    expectEmptyFailure(store);
    CHECK(fakeNvs::state.puts == 0);
  }
  reset();
  oldController(LEGACY);
  fakeNvs::number(LEGACY, "target", std::numeric_limits<float>::quiet_NaN());
  expectDefaultsFailure(store);
  reset();
  oldController(LEGACY);
  fakeNvs::text(LEGACY, "target", "25");
  expectDefaultsFailure(store);
  reset();
  oldWifi(LEGACY);
  fakeNvs::text(LEGACY, "sta_pass", "short");
  expectEmptyFailure(store);
  reset();
  oldWifi(LEGACY);
  fakeNvs::text(LEGACY, "sta_ssid", std::string("a\0b", 3));
  expectEmptyFailure(store);
  reset();
  oldWifi(LEGACY);
  fakeNvs::text(LEGACY, "sta_pass", std::string(65, 'x'));
  expectEmptyFailure(store);
  CHECK(fakeNvs::state.puts == 0);
}
void tombstoneAndCorruption() {
  reset();
  ConfigStore store;
  oldWifi(CURRENT);
  oldWifi(LEGACY);
  CHECK(store.clearWifi());
  CHECK(store.clearWifi() && fakeNvs::state.puts == 1);
  String ssid, pass;
  ConfigStore reboot;
  CHECK(reboot.loadWifi(ssid, pass) && ssid.length() == 0 && pass.length() == 0);
  CHECK(fakeNvs::state.spaces.at(LEGACY).count("sta_ssid") == 1);
  const auto tombstone = record(WIFI);
  for (size_t i = 0; i < tombstone.size(); ++i) {
    record(WIFI) = tombstone;
    record(WIFI)[i] ^= 1;
    expectEmptyFailure(reboot);
  }
  record(WIFI) = tombstone;
  record(WIFI)[4] = 2;
  rechecksum(record(WIFI));
  expectEmptyFailure(reboot);
  record(WIFI) = tombstone;
  record(WIFI)[6] = 111;
  rechecksum(record(WIFI));
  expectEmptyFailure(reboot);
  record(WIFI) = tombstone;
  record(WIFI).resize(20);
  expectEmptyFailure(reboot);
  fakeNvs::text(CURRENT, WIFI, "not a blob");
  expectEmptyFailure(reboot);
  CHECK(fakeNvs::state.puts == 1);
  CHECK(store.clearWifi());  // Explicit user intent repairs an invalid record.
  CHECK(store.loadWifi(ssid, pass) && ssid.length() == 0);
}
void semanticRecordCorruption() {
  reset();
  ConfigStore store;
  ControllerSettings s;
  CHECK(store.saveController(s));
  oldController(LEGACY);
  const auto control = record(CONTROL);
  for (size_t i = 0; i < control.size(); ++i) {
    record(CONTROL) = control;
    record(CONTROL)[i] ^= 1;
    expectDefaultsFailure(store);
  }
  for (int offset : {8, 12, 16}) {
    record(CONTROL) = control;
    const float bad = std::numeric_limits<float>::infinity();
    std::memcpy(record(CONTROL).data() + offset, &bad, sizeof(bad));
    rechecksum(record(CONTROL));
    expectDefaultsFailure(store);
  }
  for (const auto& invalid : std::vector<std::pair<int, float>>{
           {8, 10.19f}, {8, 54.81f}, {12, 55.01f}, {16, 9.99f},
           {12, 25.19f}, {16, 24.81f}}) {
    record(CONTROL) = control;
    std::memcpy(record(CONTROL).data() + invalid.first, &invalid.second, sizeof(float));
    rechecksum(record(CONTROL));
    expectDefaultsFailure(store);
  }
  record(CONTROL) = control;
  record(CONTROL)[4] = 9;
  rechecksum(record(CONTROL));
  expectDefaultsFailure(store);
  CHECK(store.saveWifi("fixture", "fixture-only"));
  oldWifi(LEGACY);
  const auto wifi = record(WIFI);
  for (int offset : {8, 9, 10, 11, 12, 44, 43, 107}) {
    record(WIFI) = wifi;
    record(WIFI)[offset] = (offset == 12 || offset == 44) ? 0 : 255;
    rechecksum(record(WIFI));
    expectEmptyFailure(store);
  }
  record(WIFI) = wifi;
  fakeArduino::failConcat = true;
  expectEmptyFailure(store);
  fakeArduino::failConcat = false;
}
void lengthsAndRecovery() {
  reset();
  ConfigStore store;
  CHECK(store.saveWifi("fixture", "fixture-only"));
  oldWifi(CURRENT);
  oldWifi(LEGACY);
  const auto wifi = record(WIFI);
  for (size_t size : {size_t(0), size_t(1), size_t(111), size_t(113), size_t(512)}) {
    record(WIFI) = wifi;
    record(WIFI).resize(size);
    expectEmptyFailure(store);
  }
  CHECK(store.clearWifi());
  const int writes = fakeNvs::state.puts;
  ControllerSettings s;
  CHECK(store.saveController(s));
  String ssid, pass;
  CHECK(store.loadWifi(ssid, pass) && ssid.length() == 0);
  CHECK(fakeNvs::state.puts == writes + 1);
  const auto control = record(CONTROL);
  for (size_t size : {size_t(0), size_t(1), size_t(23), size_t(25), size_t(512)}) {
    record(CONTROL) = control;
    record(CONTROL).resize(size);
    expectDefaultsFailure(store);
  }
  reset();
  oldWifi(LEGACY);
  fakeNvs::state.failStringReadAt = 2;
  expectEmptyFailure(store);  // Password read failure, not an open-network fallback.
  CHECK(fakeNvs::state.puts == 0);
  fakeNvs::state.failStringReadAt = 0;
  CHECK(store.loadWifi(ssid, pass));
  CHECK(fakeNvs::state.puts == 1 && store.lastError()[0] == '\0');
  reset();
  oldWifi(LEGACY);
  fakeNvs::state.failBeginAt = 2;  // Migration destination write open.
  expectEmptyFailure(store);
  CHECK(fakeNvs::state.puts == 0);
  CHECK(fakeNvs::state.spaces.at(LEGACY).count("sta_ssid") == 1);
  CHECK(store.loadWifi(ssid, pass));
  CHECK(fakeNvs::state.puts == 1);
}
void readFailures() {
  reset();
  ConfigStore store;
  ControllerSettings s;
  CHECK(store.saveController(s));
  CHECK(store.saveWifi("fixture", "fixture-only"));
  oldWifi(LEGACY);
  oldController(LEGACY);
  const int writes = fakeNvs::state.puts;
  fakeNvs::state.blobProbeFailKey = WIFI;
  expectEmptyFailure(store);
  CHECK(!store.clearWifi());
  fakeNvs::state.blobProbeFailKey = CONTROL;
  expectDefaultsFailure(store);
  CHECK(!store.saveController(s));
  fakeNvs::state.blobProbeFailKey = "";
  fakeNvs::state.failLengthAt = fakeNvs::state.lengthCalls + 1;
  expectEmptyFailure(store);
  fakeNvs::state.failLengthAt = fakeNvs::state.lengthCalls + 2;
  expectDefaultsFailure(store);
  fakeNvs::state.failReadAt = fakeNvs::state.readCalls + 1;
  expectEmptyFailure(store);
  fakeNvs::state.failReadAt = fakeNvs::state.readCalls + 1;
  CHECK(!store.saveController(s));
  CHECK(fakeNvs::state.puts == writes);
  reset();
  oldWifi(LEGACY);
  fakeNvs::state.stringProbeFailKey = "sta_ssid";
  expectEmptyFailure(store);
  fakeNvs::state.stringProbeFailKey = "";
  fakeNvs::state.failStringReadAt = fakeNvs::state.stringReadCalls + 1;
  expectEmptyFailure(store);
  reset();
  oldController(LEGACY);
  fakeNvs::state.failReadAt = 2;
  expectDefaultsFailure(store);
  CHECK(fakeNvs::state.puts == 0);
}
void writeAndMigrationFailures() {
  ConfigStore store;
  for (fakeNvs::Write mode : {fakeNvs::Write::Fail, fakeNvs::Write::Short,
                              fakeNvs::Write::Corrupt, fakeNvs::Write::CommitThenFail}) {
    reset();
    oldWifi(LEGACY);
    const auto old = fakeNvs::state.spaces.at(LEGACY).at("sta_pass").bytes;
    fakeNvs::state.write = mode;
    expectEmptyFailure(store);
    CHECK(fakeNvs::state.spaces.at(LEGACY).at("sta_pass").bytes == old);
    CHECK(fakeNvs::state.puts == 1);
    reset();
    oldController(LEGACY);
    fakeNvs::state.write = mode;
    expectDefaultsFailure(store);
    CHECK(fakeNvs::state.spaces.at(LEGACY).count("target") == 1);
    CHECK(fakeNvs::state.puts == 1);
  }
  reset();
  fakeNvs::state.failBeginAt = 1;
  CHECK(!store.clearWifi());
  CHECK(fakeNvs::state.puts == 0);
  reset();
  fakeNvs::state.failBeginAt = 2;  // Reopen for verification after commit.
  CHECK(!store.clearWifi());
  CHECK(fakeNvs::state.puts == 1);
  reset();
  fakeNvs::state.failReadAt = 1;
  CHECK(!store.saveWifi("fixture", "fixture-only"));
  CHECK(fakeNvs::state.puts == 1);
  reset();
  CHECK(store.saveWifi("fixture", "fixture-only"));
  const auto before = record(WIFI);
  fakeNvs::state.write = fakeNvs::Write::Fail;
  CHECK(!store.clearWifi());
  CHECK(record(WIFI) == before);
  String ssid, pass;
  CHECK(store.loadWifi(ssid, pass) && ssid == "fixture");
}

using Spaces = decltype(fakeNvs::state.spaces);
void expectSpaces(const Spaces& before, const Spaces& after = fakeNvs::state.spaces) {
  CHECK(after.size() == before.size());
  for (const auto& space : before) {
    CHECK(after.count(space.first) == 1);
    CHECK(after.at(space.first).size() == space.second.size());
    for (const auto& item : space.second) {
      CHECK(after.at(space.first).count(item.first) == 1);
      const auto& value = after.at(space.first).at(item.first);
      CHECK(value.type == item.second.type && value.bytes == item.second.bytes);
    }
  }
}
void expectDirection(ConfigStore& store, TecDirection expected, bool success = true) {
  const auto before = fakeNvs::state.spaces;
  const int puts = fakeNvs::state.puts, commits = fakeNvs::state.commits;
  TecDirection actual = expected == TecDirection::Reversed ?
      TecDirection::Normal : TecDirection::Reversed;
  CHECK(store.loadDirection(actual) == success);
  CHECK(actual == expected);
  CHECK((store.lastError()[0] == '\0') == success);
  CHECK(fakeNvs::state.puts == puts && fakeNvs::state.commits == commits);
  CHECK(fakeNvs::state.handles.empty());
  expectSpaces(before);
}
void directionMissing() {
  ConfigStore store;
  reset();
  CHECK(!store.saveDirection(static_cast<TecDirection>(255)));
  expectDirection(store, TecDirection::Unknown);
  CHECK(fakeNvs::state.spaces.empty());
  fakeNvs::state.spaces[CURRENT] = {};
  expectDirection(store, TecDirection::Unknown);
  // Old records and even a direction key in the legacy namespace cannot infer polarity.
  for (const char* name : {CURRENT, LEGACY}) {
    reset();
    oldController(name);
    oldWifi(name);
    const uint8_t irrelevant[] = {1, 2, 3};
    fakeNvs::blob(LEGACY, DIRECTION, irrelevant, sizeof(irrelevant));
    expectDirection(store, TecDirection::Unknown);
    CHECK(fakeNvs::state.puts == 0 && fakeNvs::state.commits == 0);
  }
}
void directionRoundTrips() {
  // Golden IEEE CRC32 bytes independently computed with Python zlib.crc32.
  const uint8_t crcs[][4] = {{57, 14, 36, 89}, {92, 105, 152, 225}, {178, 198, 45, 243}};
  for (TecDirection value : {TecDirection::Unknown, TecDirection::Normal,
                             TecDirection::Reversed}) {
    reset();
    ConfigStore store;
    CHECK(store.saveDirection(value));
    std::vector<uint8_t> expected = {'P', 'L', '5', 'D', 1, 0, 16, 0,
                                    static_cast<uint8_t>(value), 0, 0, 0};
    const auto& crc = crcs[static_cast<uint8_t>(value)];
    expected.insert(expected.end(), crc, crc + 4);
    CHECK(record(DIRECTION) == expected);
    CHECK(store.lastError()[0] == '\0');
    CHECK(fakeNvs::state.puts == 1 && fakeNvs::state.commits == 1);
    expectDirection(store, value);
    ConfigStore reboot;
    expectDirection(reboot, value);
    const int begins = fakeNvs::state.beginCalls;
    fakeNvs::state.write = fakeNvs::Write::Fail;
    CHECK(!store.saveDirection(static_cast<TecDirection>(255)));
    CHECK(store.saveDirection(value) && reboot.saveDirection(value));
    CHECK(store.lastError()[0] == '\0');
    CHECK(fakeNvs::state.beginCalls == begins + 2);  // Comparison opens only.
    CHECK(fakeNvs::state.puts == 1 && fakeNvs::state.commits == 1);
    CHECK(reboot.lastError()[0] == '\0' && fakeNvs::state.handles.empty());
    fakeNvs::state.write = fakeNvs::Write::Normal;
    for (TecDirection next : {TecDirection::Reversed, TecDirection::Normal,
                              TecDirection::Unknown}) {
      CHECK(reboot.saveDirection(value));
      const bool changed = next != value;
      const int puts = fakeNvs::state.puts, commits = fakeNvs::state.commits;
      CHECK(reboot.saveDirection(next));
      CHECK(fakeNvs::state.puts == puts + changed &&
            fakeNvs::state.commits == commits + changed);
      expectDirection(store, next);
    }
  }
}
void directionBitCorruption() {
  reset();
  ConfigStore store;
  CHECK(store.saveDirection(TecDirection::Normal));
  const auto valid = record(DIRECTION);
  for (size_t offset = 0; offset < valid.size(); ++offset) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      record(DIRECTION) = valid;
      record(DIRECTION)[offset] ^= uint8_t(1u << bit);
      expectDirection(store, TecDirection::Unknown, false);
    }
  }
  record(DIRECTION) = valid;
  expectDirection(store, TecDirection::Normal);
}
void directionSemanticCorruption() {
  reset();
  ConfigStore store;
  CHECK(store.saveDirection(TecDirection::Normal));
  const auto valid = record(DIRECTION);
  // Recompute CRC so header, reserved, and enum checks must reject independently.
  for (size_t offset : {size_t(0), size_t(1), size_t(2), size_t(3),
                        size_t(4), size_t(5), size_t(6), size_t(7),
                        size_t(9), size_t(10), size_t(11)}) {
    for (unsigned byte = 0; byte < 256; ++byte) {
      if (byte == valid[offset]) continue;
      record(DIRECTION) = valid;
      record(DIRECTION)[offset] = uint8_t(byte);
      rechecksum(record(DIRECTION));
      expectDirection(store, TecDirection::Unknown, false);
    }
  }
  for (unsigned byte = 3; byte < 256; ++byte) {
    record(DIRECTION) = valid;
    record(DIRECTION)[8] = uint8_t(byte);
    rechecksum(record(DIRECTION));
    expectDirection(store, TecDirection::Unknown, false);
  }
  CHECK(store.saveDirection(TecDirection::Unknown));
  expectDirection(store, TecDirection::Unknown);
}
void directionSizesAndTypes() {
  reset();
  ConfigStore store;
  CHECK(store.saveDirection(TecDirection::Normal));
  const auto valid = record(DIRECTION);
  for (size_t size = 0; size <= 32; ++size) {
    if (size == valid.size()) continue;
    record(DIRECTION) = valid;
    record(DIRECTION).resize(size);
    expectDirection(store, TecDirection::Unknown, false);
    CHECK(store.saveDirection(TecDirection::Normal));
    expectDirection(store, TecDirection::Normal);
  }
  record(DIRECTION).resize(512);
  expectDirection(store, TecDirection::Unknown, false);
  CHECK(store.saveDirection(TecDirection::Normal));
  for (fakeNvs::Type type : {fakeNvs::Type::String, fakeNvs::Type::Other}) {
    fakeNvs::state.spaces.at(CURRENT).at(DIRECTION).type = type;
    expectDirection(store, TecDirection::Unknown, false);
    CHECK(store.saveDirection(TecDirection::Normal));
    expectDirection(store, TecDirection::Normal);
  }
}
void directionInvalidSaves() {
  for (bool existing : {false, true}) {
    reset();
    ConfigStore store;
    if (existing) CHECK(store.saveDirection(TecDirection::Reversed));
    const auto before = fakeNvs::state.spaces;
    const int opens = fakeNvs::state.openCalls;
    for (unsigned byte = 3; byte < 256; ++byte) {
      CHECK(!store.saveDirection(static_cast<TecDirection>(byte)));
      CHECK(store.lastError()[0] != '\0');
      CHECK(fakeNvs::state.openCalls == opens);
      CHECK(fakeNvs::state.puts == int(existing) &&
            fakeNvs::state.commits == int(existing));
      CHECK(fakeNvs::state.handles.empty());
      expectSpaces(before);
    }
    expectDirection(store, existing ? TecDirection::Reversed : TecDirection::Unknown);
  }
}
enum class DirectionReadFault {
  NvsOpen, PreferencesOpen, PreferencesNvsOpen, Probe, Length, NestedLength, Payload
};
constexpr DirectionReadFault directionReadFaults[] = {
  DirectionReadFault::NvsOpen, DirectionReadFault::PreferencesOpen,
  DirectionReadFault::PreferencesNvsOpen, DirectionReadFault::Probe,
  DirectionReadFault::Length, DirectionReadFault::NestedLength, DirectionReadFault::Payload
};
void armDirectionReadFault(DirectionReadFault fault, bool verification = false) {
  auto& state = fakeNvs::state;
  // Verification follows a full existing-record comparison and one write open.
  switch (fault) {
    case DirectionReadFault::NvsOpen:
      state.failOpenAt = state.openCalls + (verification ? 4 : 1); break;
    case DirectionReadFault::PreferencesOpen:
      state.failBeginAt = state.beginCalls + (verification ? 3 : 1); break;
    case DirectionReadFault::PreferencesNvsOpen:
      state.failOpenAt = state.openCalls + (verification ? 5 : 2); break;
    case DirectionReadFault::Probe:
      state.failBlobProbeAt = state.blobProbeCalls + (verification ? 4 : 1); break;
    case DirectionReadFault::Length:
      state.failLengthAt = state.lengthCalls + (verification ? 3 : 1); break;
    case DirectionReadFault::NestedLength:
      state.failLengthAt = state.lengthCalls + (verification ? 4 : 2); break;
    case DirectionReadFault::Payload:
      state.failReadAt = state.readCalls + (verification ? 2 : 1); break;
  }
}
void directionReadFailures() {
  ConfigStore store;
  for (DirectionReadFault fault : directionReadFaults) {
    reset();
    CHECK(store.saveDirection(TecDirection::Normal));
    armDirectionReadFault(fault);
    expectDirection(store, TecDirection::Unknown, false);
    expectDirection(store, TecDirection::Normal);  // One-shot fault, error cleared.
  }
  reset();
  fakeNvs::state.openFailNamespace = CURRENT;
  expectDirection(store, TecDirection::Unknown, false);  // Not an absent namespace.
  fakeNvs::state.openFailNamespace = "";
  expectDirection(store, TecDirection::Unknown);
}
void directionComparisonFailures() {
  ConfigStore store;
  for (TecDirection desired : {TecDirection::Normal, TecDirection::Reversed,
                               TecDirection::Unknown}) {
    for (DirectionReadFault fault : directionReadFaults) {
      reset();
      CHECK(store.saveDirection(TecDirection::Normal));
      const auto before = fakeNvs::state.spaces;
      armDirectionReadFault(fault);
      CHECK(!store.saveDirection(desired));
      CHECK(store.lastError()[0] != '\0');
      CHECK(fakeNvs::state.puts == 1 && fakeNvs::state.commits == 1);
      CHECK(fakeNvs::state.handles.empty());
      expectSpaces(before);
      expectDirection(store, TecDirection::Normal);
      CHECK(store.saveDirection(desired));
      CHECK(store.lastError()[0] == '\0');
      expectDirection(store, desired);
    }
  }
}
void directionSaveFailures() {
  ConfigStore store;
  for (bool existing : {false, true}) {
    for (bool nativeOpen : {false, true}) {
      reset();
      if (existing) CHECK(store.saveDirection(TecDirection::Normal));
      const auto before = fakeNvs::state.spaces;
      if (nativeOpen)
        fakeNvs::state.failOpenAt = fakeNvs::state.openCalls + (existing ? 3 : 2);
      else
        fakeNvs::state.failBeginAt = fakeNvs::state.beginCalls + (existing ? 2 : 1);
      CHECK(!store.saveDirection(TecDirection::Unknown));
      CHECK(std::strcmp(store.lastError(), "Storage write open failed") == 0);
      CHECK(fakeNvs::state.puts == int(existing) &&
            fakeNvs::state.commits == int(existing));
      CHECK(fakeNvs::state.handles.empty());
      expectSpaces(before);
      CHECK(store.saveDirection(TecDirection::Unknown));
      expectDirection(store, TecDirection::Unknown);
    }
    for (fakeNvs::Write mode : {fakeNvs::Write::Fail, fakeNvs::Write::Short,
                                fakeNvs::Write::CommitThenFail}) {
      reset();
      if (existing) CHECK(store.saveDirection(TecDirection::Normal));
      fakeNvs::state.write = mode;
      CHECK(!store.saveDirection(TecDirection::Unknown));
      CHECK(std::strcmp(store.lastError(), "Storage write or commit failed") == 0);
      CHECK(fakeNvs::state.puts == int(existing) + 1);
      CHECK(fakeNvs::state.commits == int(existing) + (mode != fakeNvs::Write::Fail));
      CHECK(fakeNvs::state.handles.empty());
      if (mode == fakeNvs::Write::Short)
        expectDirection(store, TecDirection::Unknown, false);
      else
        expectDirection(store, existing && mode == fakeNvs::Write::Fail ?
                        TecDirection::Normal : TecDirection::Unknown);
      fakeNvs::state.write = fakeNvs::Write::Normal;
      CHECK(store.saveDirection(TecDirection::Unknown));
      expectDirection(store, TecDirection::Unknown);
    }
  }
}
void directionVerificationFailures() {
  ConfigStore store;
  for (DirectionReadFault fault : directionReadFaults) {
    reset();
    CHECK(store.saveDirection(TecDirection::Normal));
    armDirectionReadFault(fault, true);
    CHECK(!store.saveDirection(TecDirection::Unknown));
    CHECK(std::strcmp(store.lastError(), "Storage write verification failed") == 0);
    CHECK(fakeNvs::state.puts == 2 && fakeNvs::state.commits == 2);
    CHECK(fakeNvs::state.handles.empty());
    expectDirection(store, TecDirection::Unknown);  // Commit succeeded before failed readback.
    CHECK(store.saveDirection(TecDirection::Unknown));
    CHECK(fakeNvs::state.puts == 2 && fakeNvs::state.commits == 2);
  }
  for (bool existing : {false, true}) {
    for (fakeNvs::Write mode : {fakeNvs::Write::Corrupt, fakeNvs::Write::AckWithoutWrite,
                                fakeNvs::Write::AckShort, fakeNvs::Write::AckWrongType,
                                fakeNvs::Write::AckMissing}) {
      reset();
      if (existing) CHECK(store.saveDirection(TecDirection::Normal));
      fakeNvs::state.write = mode;
      CHECK(!store.saveDirection(TecDirection::Unknown));
      CHECK(std::strcmp(store.lastError(), "Storage write verification failed") == 0);
      CHECK(fakeNvs::state.puts == int(existing) + 1);
      CHECK(fakeNvs::state.commits == int(existing) +
            (mode != fakeNvs::Write::AckWithoutWrite));
      CHECK(fakeNvs::state.handles.empty());
      if (mode == fakeNvs::Write::AckWithoutWrite)
        expectDirection(store, existing ? TecDirection::Normal : TecDirection::Unknown);
      else if (mode == fakeNvs::Write::AckMissing)
        expectDirection(store, TecDirection::Unknown);
      else
        expectDirection(store, TecDirection::Unknown, false);
      fakeNvs::state.write = fakeNvs::Write::Normal;
      CHECK(store.saveDirection(TecDirection::Unknown));
      expectDirection(store, TecDirection::Unknown);
    }
  }
}
void directionIsolationAndInvalidation() {
  reset();
  ConfigStore store;
  oldController(CURRENT, 28);
  oldController(LEGACY, 29);
  oldWifi(CURRENT);
  oldWifi(LEGACY);
  ControllerSettings settings;
  settings.targetC = 26;
  CHECK(store.saveController(settings));
  CHECK(store.saveWifi("current-network", "current-password"));
  CHECK(store.saveDirection(TecDirection::Reversed));
  const auto reversed = record(DIRECTION);
  fakeNvs::blob(LEGACY, DIRECTION, reversed.data(), reversed.size());
  Spaces unrelated = fakeNvs::state.spaces;
  unrelated.at(CURRENT).erase(DIRECTION);
  for (fakeNvs::Write mode : {fakeNvs::Write::Fail, fakeNvs::Write::Short,
                              fakeNvs::Write::Corrupt, fakeNvs::Write::CommitThenFail,
                              fakeNvs::Write::AckWithoutWrite, fakeNvs::Write::AckShort,
                              fakeNvs::Write::AckWrongType, fakeNvs::Write::AckMissing}) {
    CHECK(store.saveDirection(TecDirection::Reversed));
    fakeNvs::state.write = mode;
    CHECK(!store.saveDirection(TecDirection::Unknown));
    CHECK(store.lastError()[0] != '\0');
    CHECK(fakeNvs::state.handles.empty());
    auto after = fakeNvs::state.spaces;
    after.at(CURRENT).erase(DIRECTION);
    expectSpaces(unrelated, after);
    fakeNvs::state.write = fakeNvs::Write::Normal;
  }
  for (TecDirection previous : {TecDirection::Normal, TecDirection::Reversed}) {
    CHECK(store.saveDirection(previous));
    CHECK(store.saveDirection(TecDirection::Unknown));
    const auto tombstone = record(DIRECTION);
    ConfigStore reboot;
    expectDirection(reboot, TecDirection::Unknown);
    const int puts = fakeNvs::state.puts, commits = fakeNvs::state.commits;
    CHECK(reboot.saveDirection(TecDirection::Unknown));
    CHECK(fakeNvs::state.puts == puts && fakeNvs::state.commits == commits);
    CHECK(reboot.loadController(settings) && settings.targetC == 26 && !settings.enabled);
    String ssid, password;
    CHECK(reboot.loadWifi(ssid, password));
    CHECK(ssid == "current-network" && password == "current-password");
    CHECK(record(DIRECTION) == tombstone);
    record(DIRECTION)[12] ^= 1;
    expectDirection(reboot, TecDirection::Unknown, false);
    CHECK(reboot.saveDirection(TecDirection::Unknown));
    CHECK(record(DIRECTION) == tombstone);
  }
  const auto tombstone = record(DIRECTION);
  fakeNvs::state.spaces.at(CURRENT).erase(DIRECTION);
  expectSpaces(unrelated);
  expectDirection(store, TecDirection::Unknown);
  fakeNvs::blob(CURRENT, DIRECTION, tombstone.data(), tombstone.size());
  settings.targetC = 27;
  CHECK(store.saveController(settings) && store.clearWifi());
  CHECK(record(DIRECTION) == tombstone);
  expectDirection(store, TecDirection::Unknown);
}
void runDirectionGroup(const char* name, void (*test)()) {
  const int before = checks;
  test();
  CHECK(fakeNvs::state.handles.empty());
  std::printf("  PASS %s: %d checks\n", name, checks - before);
}
}  // namespace

int main() {
  freshAndOpenErrors();
  roundTripAndWear();
  controllerValidation();
  wifiValidation();
  migrations();
  incompleteAndInvalidLegacy();
  tombstoneAndCorruption();
  semanticRecordCorruption();
  lengthsAndRecovery();
  readFailures();
  writeAndMigrationFailures();
  CHECK(fakeNvs::state.handles.empty());
  const int legacyChecks = checks;
  std::printf("Existing storage regression: %d checks, 11 groups.\n", legacyChecks);
  runDirectionGroup("direction missing namespace/key", directionMissing);
  runDirectionGroup("direction roundtrips and unchanged suppression", directionRoundTrips);
  runDirectionGroup("direction single-bit corruption", directionBitCorruption);
  runDirectionGroup("direction valid-CRC semantic corruption", directionSemanticCorruption);
  runDirectionGroup("direction lengths and NVS types", directionSizesAndTypes);
  runDirectionGroup("direction invalid enum saves", directionInvalidSaves);
  runDirectionGroup("direction read failures and recovery", directionReadFailures);
  runDirectionGroup("direction comparison failures", directionComparisonFailures);
  runDirectionGroup("direction write/open/commit failures", directionSaveFailures);
  runDirectionGroup("direction readback verification failures", directionVerificationFailures);
  runDirectionGroup("direction record isolation and Unknown invalidation",
                    directionIsolationAndInvalidation);
  std::printf("Direction storage tests passed: %d checks, 11 groups.\n", checks - legacyChecks);
  std::printf("Storage tests passed: %d checks, 22 groups.\n", checks);
}
