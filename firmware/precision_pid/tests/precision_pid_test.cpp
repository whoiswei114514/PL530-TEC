#include "precision_pid.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message) {
  require(std::fabs(actual - expected) <= tolerance, message);
}
void plateau(bool hot) {
  PrecisionPid pid;
  double output = 0;
  const double measurement = hot ? 25.03125 : 24.96875;
  for (int i = 0; i < 1000; ++i) {
    require(pid.update(measurement, 25, 0.2, output), "valid plateau sample");
    const double next = pid.result().outputPercent;
    require(hot ? next < output : next > output,
            "nonzero target error must keep correcting a zero-slope plateau");
    output = next;
  }
  require(std::fabs(pid.result().integralPercent) > 1.0, "plateau correction must accumulate");
}
void model(double tau, double gain, double load, double delaySeconds) {
  PrecisionPid pid;
  double temperature = 27, actuator = 0, output = 0, target = 25, elapsed = 0;
  double maxError = 0, minC = INFINITY, maxC = -INFINITY;
  std::vector<double> history;
  uint32_t random = 1234567;
  while (elapsed < 2400) {
    random = random * 1664525u + 1013904223u;
    const double dt = 0.20 + (int(random % 31) - 15) * 0.001;
    target = elapsed > 900 ? 25.2 : 25.0;
    const double disturbance = elapsed > 600 ? load + 0.2 : load;
    const double delayedOutput = history.size() > static_cast<size_t>(delaySeconds / 0.2)
        ? history[history.size() - 1 - static_cast<size_t>(delaySeconds / 0.2)] : 0;
    actuator += -std::expm1(-dt / 2.0) * (delayedOutput - actuator);
    temperature += (25.0 + disturbance + gain * actuator - temperature) *
        -std::expm1(-dt / tau);
    const double noise = (int(random % 2001) - 1000) * 0.000002;
    const double sensor = std::round((temperature + noise) * 128) / 128;
    require(pid.update(sensor, target, dt, output), "model update");
    output = std::round(pid.result().outputPercent * 16383 / 100) * 100 / 16383;
    require(std::isfinite(output) && std::fabs(output) <= 30.01, "model bounded output");
    history.push_back(output);
    elapsed += dt;
    if (elapsed > 2280) {
      maxError = std::max(maxError, std::fabs(temperature - target));
      minC = std::min(minC, temperature);
      maxC = std::max(maxC, temperature);
    }
  }
  std::cout << "MODEL tau=" << tau << " gain=" << gain << " load=" << load
            << " delay=" << delaySeconds << " maxErrorMk=" << maxError * 1000
            << " peakToPeakMk=" << (maxC - minC) * 1000 << '\n';
  require(maxError <= 0.010 && maxC - minC <= 0.020,
          "synthetic steady-state target error must be within 10 mK");
}
}

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests = {
    {"above_target_plateau", [] { plateau(true); }},
    {"below_target_plateau", [] { plateau(false); }},
    {"no_derivative_setpoint_kick", [] {
      PrecisionPid pid;
      require(pid.update(25, 25, 0.2, 0), "initial update");
      require(pid.update(25, 25.2, 0.2, 0), "target step");
      near(pid.result().derivativePercent, 0, 1e-12, "D must not react to a target step");
    }},
    {"antiwindup_and_reverse_error", [] {
      PrecisionPid pid;
      double output = 0;
      for (int i = 0; i < 1000; ++i) {
        pid.update(40, 25, 0.2, output);
        output = pid.result().outputPercent;
      }
      near(output, -30, 1e-9, "cooling cap");
      near(pid.result().integralPercent, 0, 1e-9, "no windup while saturated");
      for (int i = 0; i < 20; ++i) {
        pid.update(20, 25, 0.2, output);
        output = pid.result().outputPercent;
      }
      require(output > 0, "saturation must not trap the controller in cooling");
    }},
    {"small_error_no_deadband", [] {
      PrecisionPid pid;
      pid.update(25.001, 25, 0.2, 0);
      require(pid.result().outputPercent < 0, "1 mK error must produce a correction");
    }},
    {"timing_validation", [] {
      PrecisionPid pid;
      for (double dt : {0.0, -0.2, 1.0, 2.0, double(NAN)})
        require(!pid.update(25, 25, dt, 0), "reject invalid/stale dt");
      require(!pid.update(NAN, 25, 0.2, 0), "reject NaN measurement");
      require(!pid.update(25, INFINITY, 0.2, 0), "reject infinite target");
    }},
    {"configuration_validation", [] {
      PrecisionPid pid;
      PrecisionPidSettings settings;
      settings.ki = NAN;
      require(!pid.configure(settings), "reject NaN gains");
      settings = PrecisionPidSettings{};
      settings.integralLimitPercent = 31;
      require(!pid.configure(settings), "integral cap must fit output cap");
    }},
    {"hard_output_transfer", [] {
      PrecisionPid pid;
      pid.update(53, 25, 0.2, -100);
      require(std::fabs(pid.result().outputPercent) <= 30,
              "normal output cap must apply after hard recovery");
    }},
    {"unfiltered_window_rejects_offset", [] {
      TemperatureStability window;
      for (uint32_t t = 0; t <= 120000; t += 200) window.add(25.03125, 25, t);
      const auto result = window.result();
      require(result.ready && !result.within10Mk, "zero variance off target is not stable");
      near(result.peakToPeakMk, 0, 1e-9, "flat plateau variation");
      near(result.maximumErrorMk, 31.25, 1e-9, "unfiltered target error");
    }},
    {"unfiltered_window_and_gap_reset", [] {
      TemperatureStability window;
      for (uint32_t t = 0; t <= 120000; t += 200)
        window.add(t % 400 == 0 ? 25.0078125 : 24.9921875, 25, t);
      require(window.result().within10Mk, "full +/- one-LSB window");
      window.add(25, 25, 121000);
      require(!window.result().ready, "missing conversions must invalidate window");
    }},
    {"unfiltered_window_target_change_and_wrap", [] {
      TemperatureStability window;
      const uint32_t start = UINT32_MAX - 1000;
      for (uint32_t t = 0; t <= 120000; t += 200) window.add(25, 25, start + t);
      require(window.result().within10Mk, "millis wrap must preserve valid window");
      window.add(25, 25.01, start + 120200);
      require(!window.result().ready, "target change resets evidence");
    }},
    {"insufficient_actuator_authority_is_not_stability", [] {
      PrecisionPid pid;
      TemperatureStability window;
      double output = 0;
      for (uint32_t t = 0; t <= 180000; t += 200) {
        pid.update(26, 25, 0.2, output);
        output = pid.result().outputPercent;
        window.add(26, 25, t);
      }
      require(pid.result().saturated && output == -30, "insufficient cooling remains capped");
      require(window.result().ready && !window.result().within10Mk,
              "saturated, zero-slope but off-target operation cannot pass");
    }},
  };
  for (double tau : {20.0, 60.0, 120.0}) {
    for (double load : {-0.8, 0.8}) {
      for (double delay : {0.0, 4.0}) {
        for (double gain : {0.04, 0.08}) {
          tests.emplace_back("model_" + std::to_string(tau) + "_" +
              std::to_string(load) + "_" + std::to_string(delay) + "_" + std::to_string(gain),
              [=] { model(tau, gain, load, delay); });
        }
      }
    }
  }
  int failed = 0;
  for (const auto& test : tests) {
    try { test.second(); std::cout << "PASS " << test.first << '\n'; }
    catch (const std::exception& error) {
      ++failed;
      std::cerr << "FAIL " << test.first << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failed << '/' << tests.size() << " precision PID tests passed\n";
  return failed ? 1 : 0;
}
