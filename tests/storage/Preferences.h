#pragma once
#include "nvs.h"

// Match the installed Arduino-ESP32 Preferences return conventions:
// putBytes commits once, getString(buffer) counts the terminating NUL.
class Preferences {
 public:
  ~Preferences() { end(); }
  bool begin(const char* name, bool readOnly = false) {
    auto& state = fakeNvs::state;
    ++state.beginCalls;
    if (started_ || state.beginCalls == state.failBeginAt) return false;
    if (nvs_open(name, readOnly ? NVS_READONLY : NVS_READWRITE, &handle_) != ESP_OK)
      return false;
    started_ = true;
    readOnly_ = readOnly;
    return true;
  }
  void end() {
    if (started_) nvs_close(handle_);
    started_ = false;
  }
  size_t getBytesLength(const char* key) {
    auto& state = fakeNvs::state;
    if (++state.lengthCalls == state.failLengthAt) return 0;
    size_t size = 0;
    return started_ && nvs_get_blob(handle_, key, nullptr, &size) == ESP_OK ? size : 0;
  }
  size_t getBytes(const char* key, void* buffer, size_t capacity) {
    auto& state = fakeNvs::state;
    const size_t length = getBytesLength(key);
    if (!length || !buffer || !capacity) return length;
    if (length > capacity) return 0;
    if (++state.readCalls == state.failReadAt) return 0;
    size_t actual = capacity;
    return nvs_get_blob(handle_, key, buffer, &actual) == ESP_OK ? actual : 0;
  }
  size_t getString(const char* key, char* buffer, size_t capacity) {
    auto& state = fakeNvs::state;
    if (++state.stringReadCalls == state.failStringReadAt) return 0;
    size_t length = 0;
    if (!started_ || nvs_get_str(handle_, key, nullptr, &length) != ESP_OK ||
        length > capacity) return 0;
    return nvs_get_str(handle_, key, buffer, &length) == ESP_OK ? length : 0;
  }
  size_t putBytes(const char* key, const void* data, size_t length) {
    using namespace fakeNvs;
    if (!started_ || readOnly_ || !data || !length) return 0;
    ++state.puts;
    if (state.write == Write::Fail) return 0;
    if (state.write == Write::AckWithoutWrite) return length;
    const auto* p = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> bytes(p, p + length);
    if (state.write == Write::Short || state.write == Write::AckShort)
      bytes.resize(length / 2);
    if (state.write == Write::Corrupt) bytes.back() ^= 1;
    state.spaces.at(state.handles.at(handle_).name)[key] = {Type::Blob, bytes};
    if (state.write == Write::AckWrongType)
      state.spaces.at(state.handles.at(handle_).name)[key].type = Type::Other;
    if (state.write == Write::AckMissing)
      state.spaces.at(state.handles.at(handle_).name).erase(key);
    ++state.commits;
    if (state.write == Write::CommitThenFail) return 0;
    return state.write == Write::Short ? bytes.size() : length;
  }

 private:
  nvs_handle_t handle_ = 0;
  bool started_ = false;
  bool readOnly_ = true;
};
