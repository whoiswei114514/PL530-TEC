#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using esp_err_t = int;
using nvs_handle_t = unsigned int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 1;
constexpr esp_err_t ESP_ERR_NVS_TYPE_MISMATCH = 2;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 3;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;

namespace fakeNvs {
enum class Type { Blob, String, Other };
enum class Write {
  Normal, Fail, Short, Corrupt, CommitThenFail,
  AckWithoutWrite, AckShort, AckWrongType, AckMissing
};
struct Value {
  Type type;
  std::vector<uint8_t> bytes;
};
struct Handle { std::string name; bool readOnly; };
struct State {
  std::map<std::string, std::map<std::string, Value>> spaces;
  std::map<nvs_handle_t, Handle> handles;
  nvs_handle_t nextHandle = 1;
  std::string openFailNamespace;
  std::string blobProbeFailKey;
  std::string stringProbeFailKey;
  int openCalls = 0;
  int failOpenAt = 0;
  int blobProbeCalls = 0;
  int failBlobProbeAt = 0;
  int beginCalls = 0;
  int failBeginAt = 0;
  int lengthCalls = 0;
  int failLengthAt = 0;
  int readCalls = 0;
  int failReadAt = 0;
  int stringReadCalls = 0;
  int failStringReadAt = 0;
  int puts = 0;
  int commits = 0;
  Write write = Write::Normal;
};
inline State state;
inline void reset() { state = State{}; }
inline void blob(const char* name, const char* key, const void* data, size_t size) {
  const auto* p = static_cast<const uint8_t*>(data);
  state.spaces[name][key] = {Type::Blob, std::vector<uint8_t>(p, p + size)};
}
inline void number(const char* name, const char* key, float value) {
  blob(name, key, &value, sizeof(value));
}
inline void text(const char* name, const char* key, const std::string& value) {
  std::vector<uint8_t> bytes(value.begin(), value.end());
  bytes.push_back(0);
  state.spaces[name][key] = {Type::String, bytes};
}
inline Value* find(nvs_handle_t handle, const char* key) {
  const auto h = state.handles.find(handle);
  if (h == state.handles.end()) return nullptr;
  auto& values = state.spaces.at(h->second.name);
  const auto value = values.find(key);
  return value == values.end() ? nullptr : &value->second;
}
}  // namespace fakeNvs

inline esp_err_t nvs_open(const char* name, int mode, nvs_handle_t* handle) {
  using namespace fakeNvs;
  if (++state.openCalls == state.failOpenAt || state.openFailNamespace == name)
    return ESP_FAIL;
  if (!state.spaces.count(name)) {
    if (mode == NVS_READONLY) return ESP_ERR_NVS_NOT_FOUND;
    state.spaces[name] = {};
  }
  *handle = state.nextHandle++;
  state.handles[*handle] = {name, mode == NVS_READONLY};
  return ESP_OK;
}
inline void nvs_close(nvs_handle_t handle) {
  fakeNvs::state.handles.erase(handle);
}
inline esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key,
                              void* data, size_t* size) {
  using namespace fakeNvs;
  if (!data && ++state.blobProbeCalls == state.failBlobProbeAt) return ESP_FAIL;
  if (state.blobProbeFailKey == key) return ESP_FAIL;
  const Value* value = find(handle, key);
  if (!value) return ESP_ERR_NVS_NOT_FOUND;
  if (value->type != Type::Blob) return ESP_ERR_NVS_TYPE_MISMATCH;
  if (data && *size < value->bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  if (data) std::memcpy(data, value->bytes.data(), value->bytes.size());
  *size = value->bytes.size();
  return ESP_OK;
}
inline esp_err_t nvs_get_str(nvs_handle_t handle, const char* key,
                             char* data, size_t* size) {
  using namespace fakeNvs;
  if (state.stringProbeFailKey == key) return ESP_FAIL;
  const Value* value = find(handle, key);
  if (!value) return ESP_ERR_NVS_NOT_FOUND;
  if (value->type != Type::String) return ESP_ERR_NVS_TYPE_MISMATCH;
  if (data && *size < value->bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  if (data) std::memcpy(data, value->bytes.data(), value->bytes.size());
  *size = value->bytes.size();
  return ESP_OK;
}
