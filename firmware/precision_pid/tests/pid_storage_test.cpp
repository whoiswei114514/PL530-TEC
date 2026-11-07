#include "config_store.h"
#include "nvs.h"
#include <cstdio>
#include <cstdlib>

namespace {
constexpr const char* CURRENT = "pl530_tec";
int checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
  std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
  std::exit(1); } } while (false)
void reset() { fakeNvs::reset(); }
void oldController(const char* name, float target) {
  fakeNvs::number(name, "target", target);
  fakeNvs::number(name, "pwm_high", 32.0f);
  fakeNvs::number(name, "pwm_low", 22.0f);
}
}

int main() {
  reset();
  ConfigStore store;
  PrecisionPidSettings settings;
  CHECK(store.loadPid(settings));
  CHECK(settings.kp == 20 && fakeNvs::state.puts == 0);
  oldController(CURRENT, 27);
  const auto originalBytes = fakeNvs::state.spaces.at(CURRENT).at("target").bytes;
  settings.kp = 31.125; settings.ki = 0.125;
  CHECK(store.savePid(settings));
  CHECK(fakeNvs::state.spaces.at("pl530_pid").at("pid_cfg").bytes.size() == 44);
  CHECK(fakeNvs::state.spaces.at(CURRENT).at("target").bytes == originalBytes);
  const int writes = fakeNvs::state.puts;
  CHECK(store.savePid(settings) && fakeNvs::state.puts == writes);
  PrecisionPidSettings loaded;
  CHECK(store.loadPid(loaded));
  CHECK(loaded.kp == 31.125 && loaded.ki == 0.125);
  ControllerSettings original;
  CHECK(store.loadController(original) && original.targetC == 25 && !original.enabled);
  TecDirection direction;
  CHECK(store.loadDirection(direction) && direction == TecDirection::Unknown);
  settings.kp = NAN;
  CHECK(!store.savePid(settings));
  CHECK(fakeNvs::state.puts == writes);
  const auto saved = fakeNvs::state.spaces.at("pl530_pid").at("pid_cfg").bytes;
  for (size_t i = 0; i < saved.size(); ++i) {
    auto& bytes = fakeNvs::state.spaces.at("pl530_pid").at("pid_cfg").bytes;
    bytes = saved;
    bytes[i] ^= 1;
    loaded.kp = 99;
    CHECK(!store.loadPid(loaded) && loaded.kp == 20);
  }
  fakeNvs::state.spaces.at("pl530_pid").at("pid_cfg").bytes = saved;
  fakeNvs::state.openFailNamespace = "pl530_pid";
  CHECK(!store.loadPid(loaded) && loaded.kp == 20);
  CHECK(!store.savePid(loaded));
  fakeNvs::state.openFailNamespace = "";
  CHECK(store.loadPid(loaded) && loaded.kp == 31.125);
  std::printf("%d PID storage checks passed\n", checks);
  return 0;
}
