#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

struct PrecisionPidSettings {
  double kp = 20.0;                 // % / C
  double ki = 0.20;                 // % / (C s)
  double kd = 40.0;                 // % s / C, derivative on measurement
  double measurementTauSeconds = 0.6;
  double derivativeTauSeconds = 2.0;
  double outputLimitPercent = 30.0;
  double slewPercentPerSecond = 15.0;
  double integralLimitPercent = 30.0;

  bool valid() const {
    const double values[] = {kp, ki, kd, measurementTauSeconds, derivativeTauSeconds,
                             outputLimitPercent, slewPercentPerSecond, integralLimitPercent};
    for (double value : values) if (!std::isfinite(value)) return false;
    return kp >= 0 && kp <= 1000 && ki >= 0 && ki <= 100 && kd >= 0 && kd <= 10000 &&
        measurementTauSeconds >= 0 && measurementTauSeconds <= 10 &&
        derivativeTauSeconds >= 0.05 && derivativeTauSeconds <= 30 &&
        outputLimitPercent > 0 && outputLimitPercent <= 100 &&
        slewPercentPerSecond > 0 && slewPercentPerSecond <= 100 &&
        integralLimitPercent >= 0 && integralLimitPercent <= outputLimitPercent;
  }
};

struct PrecisionPidResult {
  double measurementC = NAN;
  double errorC = NAN;
  double proportionalPercent = 0;
  double integralPercent = 0;
  double derivativePercent = 0;
  double outputPercent = 0;
  double slopeCPerSecond = 0;
  bool saturated = false;
};

class PrecisionPid {
 public:
  bool configure(const PrecisionPidSettings& settings) {
    if (!settings.valid()) return false;
    settings_ = settings;
    reset();
    return true;
  }
  const PrecisionPidSettings& settings() const { return settings_; }
  void reset() {
    initialized_ = false;
    integral_ = 0;
    result_ = PrecisionPidResult{};
  }
  const PrecisionPidResult& result() const { return result_; }
  bool initialized() const { return initialized_; }

  bool update(double measurementC, double targetC, double dtSeconds, double appliedPercent) {
    if (!std::isfinite(measurementC) || !std::isfinite(targetC) ||
        !std::isfinite(dtSeconds) || dtSeconds <= 0 || dtSeconds >= 1.0 ||
        !std::isfinite(appliedPercent)) return false;
    const double previous = initialized_ ? result_.measurementC : measurementC;
    const double alpha = settings_.measurementTauSeconds == 0 ? 1.0 :
        -std::expm1(-dtSeconds / settings_.measurementTauSeconds);
    result_.measurementC = previous + alpha * (measurementC - previous);
    result_.errorC = targetC - result_.measurementC;
    result_.proportionalPercent = settings_.kp * result_.errorC;
    const double slope = initialized_ ? (result_.measurementC - previous) / dtSeconds : 0;
    const double derivativeAlpha = -std::expm1(-dtSeconds / settings_.derivativeTauSeconds);
    result_.slopeCPerSecond += derivativeAlpha * (slope - result_.slopeCPerSecond);
    result_.derivativePercent = -settings_.kd * result_.slopeCPerSecond;
    const double nonIntegral = result_.proportionalPercent + result_.derivativePercent;

    // Integrate only when the error does not push further into amplitude or
    // slew saturation. Existing integral is preserved when the load is balanced.
    const double step = settings_.slewPercentPerSecond * dtSeconds;
    appliedPercent = clamp(appliedPercent, -settings_.outputLimitPercent,
                           settings_.outputLimitPercent);
    const double low = std::max(-settings_.outputLimitPercent, appliedPercent - step);
    const double high = std::min(settings_.outputLimitPercent, appliedPercent + step);
    const double increment = settings_.ki * result_.errorC * dtSeconds;
    const double proposedIntegral = clamp(integral_ + increment,
        -settings_.integralLimitPercent, settings_.integralLimitPercent);
    const double before = nonIntegral + integral_;
    double acceptedIncrement = proposedIntegral - integral_;
    if (acceptedIncrement > 0 && before + acceptedIncrement > high)
      acceptedIncrement = before >= high ? 0 : high - before;
    if (acceptedIncrement < 0 && before + acceptedIncrement < low)
      acceptedIncrement = before <= low ? 0 : low - before;
    integral_ += acceptedIncrement;
    result_.integralPercent = integral_;
    const double requested = nonIntegral + integral_;
    result_.outputPercent = clamp(requested, low, high);
    result_.saturated = std::fabs(requested - result_.outputPercent) > 1e-9 ||
        (result_.outputPercent <= -settings_.outputLimitPercent + 1e-9 && result_.errorC < 0) ||
        (result_.outputPercent >= settings_.outputLimitPercent - 1e-9 && result_.errorC > 0);
    initialized_ = true;
    return true;
  }

 private:
  static double clamp(double value, double low, double high) {
    return std::max(low, std::min(high, value));
  }
  PrecisionPidSettings settings_;
  PrecisionPidResult result_;
  double integral_ = 0;
  bool initialized_ = false;
};

struct StabilityResult {
  double meanC = NAN;
  double standardDeviationMk = NAN;
  double peakToPeakMk = NAN;
  double maximumErrorMk = NAN;
  uint32_t durationMs = 0;
  uint16_t samples = 0;
  bool ready = false;
  bool within10Mk = false;
};

class TemperatureStability {
 public:
  static constexpr uint32_t WINDOW_MS = 120000;
  void reset() {
    start_ = count_ = 0;
    haveSample_ = false;
  }
  void add(double rawC, double targetC, uint32_t ms) {
    if (!std::isfinite(rawC) || !std::isfinite(targetC)) { reset(); return; }
    if (haveSample_) {
      const int32_t gap = static_cast<int32_t>(ms - lastMs_);
      if (gap <= 0) return;
      if (gap >= 1000 || targetC != targetC_) reset();
    }
    lastMs_ = ms;
    targetC_ = targetC;
    haveSample_ = true;
    while (count_ && static_cast<uint32_t>(ms - samples_[start_].ms) > WINDOW_MS) {
      start_ = (start_ + 1) % CAPACITY;
      --count_;
    }
    if (count_ == CAPACITY) {
      start_ = (start_ + 1) % CAPACITY;
      --count_;
    }
    samples_[(start_ + count_) % CAPACITY] = {ms, static_cast<float>(rawC)};
    ++count_;
  }
  StabilityResult result() const {
    StabilityResult result;
    if (!count_) return result;
    double mean = 0, m2 = 0, minC = INFINITY, maxC = -INFINITY, maxError = 0;
    for (uint16_t i = 0; i < count_; ++i) {
      const double value = samples_[(start_ + i) % CAPACITY].rawC;
      const double delta = value - mean;
      mean += delta / (i + 1);
      m2 += delta * (value - mean);
      minC = std::min(minC, value);
      maxC = std::max(maxC, value);
      maxError = std::max(maxError, std::fabs(value - targetC_));
    }
    result.meanC = mean;
    result.standardDeviationMk = std::sqrt(m2 / count_) * 1000;
    result.peakToPeakMk = (maxC - minC) * 1000;
    result.maximumErrorMk = maxError * 1000;
    result.durationMs = lastMs_ - samples_[start_].ms;
    result.samples = count_;
    // add() already resets evidence after a freshness gap. Software I2C
    // acquisition time is added to the polling interval, so a continuous
    // 120-second window can legitimately contain fewer than 500 samples.
    result.ready = result.durationMs >= WINDOW_MS - 400;
    result.within10Mk = result.ready && result.maximumErrorMk <= 10.0 &&
        result.peakToPeakMk <= 20.0;
    return result;
  }

 private:
  static constexpr uint16_t CAPACITY = 601;
  struct Sample { uint32_t ms; float rawC; };
  Sample samples_[CAPACITY]{};
  uint16_t start_ = 0;
  uint16_t count_ = 0;
  uint32_t lastMs_ = 0;
  double targetC_ = NAN;
  bool haveSample_ = false;
};
