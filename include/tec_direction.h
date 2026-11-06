#pragma once

#include <cmath>
#include <cstdint>

// Normal retains the original mapping: IN1 cools, IN2 heats.
enum class TecDirection : uint8_t { Unknown = 0, Normal = 1, Reversed = 2 };
enum class CalibrationPhase : uint8_t {
  Idle, BaselineForward, Forward, Settle, BaselineReverse, Reverse,
  Complete, Failed, Cancelled
};

constexpr uint32_t TEC_CAL_PULSE_MS = 3000u;
constexpr uint32_t TEC_CAL_BASELINE_MS = 2000u;
constexpr uint32_t TEC_CAL_SETTLE_MS = 3000u;
constexpr uint32_t TEC_CAL_TOTAL_TIMEOUT_MS = 20000u;
constexpr uint32_t TEC_CAL_MAX_SAMPLE_GAP_MS = 400u;
constexpr float TEC_CAL_START_MIN_C = 15.0f;
constexpr float TEC_CAL_START_MAX_C = 45.0f;
constexpr float TEC_CAL_ABORT_MIN_C = 12.0f;
constexpr float TEC_CAL_ABORT_MAX_C = 50.0f;
constexpr float TEC_CAL_MAX_EXCURSION_C = 5.0f;
constexpr float TEC_CAL_MIN_RESPONSE_C = 0.2f;
constexpr float TEC_CAL_MAX_BASELINE_SLOPE_C_S = 0.05f;
constexpr float TEC_CAL_MAX_BASELINE_RANGE_C = 0.15f;

struct DirectionCalibrationStatus {
  CalibrationPhase phase = CalibrationPhase::Idle;
  bool active = false;
  uint32_t phaseElapsedMs = 0;
  float rawDutyPercent = 0.0f;  // Positive is IN1, negative is IN2; not heat/cool.
  float forwardDeltaC = NAN;
  float reverseDeltaC = NAN;
  TecDirection candidate = TecDirection::Unknown;
  const char* error = "";
};

inline const char* tecDirectionKey(TecDirection value) {
  switch (value) {
    case TecDirection::Normal: return "normal";
    case TecDirection::Reversed: return "reversed";
    default: return "unknown";
  }
}
inline const char* calibrationPhaseKey(CalibrationPhase value) {
  switch (value) {
    case CalibrationPhase::BaselineForward: return "baseline_forward";
    case CalibrationPhase::Forward: return "forward";
    case CalibrationPhase::Settle: return "settle";
    case CalibrationPhase::BaselineReverse: return "baseline_reverse";
    case CalibrationPhase::Reverse: return "reverse";
    case CalibrationPhase::Complete: return "complete";
    case CalibrationPhase::Failed: return "failed";
    case CalibrationPhase::Cancelled: return "cancelled";
    default: return "idle";
  }
}
