import importlib.util
import unittest
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "analyze_log", Path(__file__).resolve().parents[1] / "tools" / "analyze_log.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def logs(raw=25.0078125, duration_ms=1800000, start_ms=0, state="NormalControl"):
    return [f"PIDCSV,{(start_ms + t) & 0xFFFFFFFF},{raw},25,25,-10,0,-10,0,{state}\n"
            for t in range(0, duration_ms + 1, 200)]


class LogAcceptance(unittest.TestCase):
    def test_full_duration_raw_temperature(self):
        result = module.analyze(logs(), 0, 1800)
        self.assertTrue(result["passed"])
        self.assertAlmostEqual(result["maximum_error_mk"], 7.8125)

    def test_flat_above_target_cannot_pass(self):
        result = module.analyze(logs(25.03125), 0, 1800)
        self.assertFalse(result["passed"])
        self.assertEqual(result["peak_to_peak_mk"], 0)

    def test_filtered_value_cannot_hide_raw_noise(self):
        self.assertFalse(module.analyze(logs(25.1), 0, 1800)["passed"])

    def test_missing_time(self):
        self.assertFalse(module.analyze(logs(duration_ms=600000), 0, 1800)["passed"])

    def test_sensor_gap(self):
        data = logs()
        del data[200:210]
        self.assertFalse(module.analyze(data, 0, 1800)["passed"])

    def test_fault_or_disabled_state(self):
        self.assertFalse(module.analyze(logs(state="Disabled"), 0, 1800)["passed"])

    def test_nonfinite_and_malformed(self):
        data = logs()
        data[300] = "PIDCSV,broken\n"
        self.assertFalse(module.analyze(data, 0, 1800)["passed"])
        self.assertFalse(module.analyze(logs(float("nan")), 0, 1800)["passed"])

    def test_wrap(self):
        self.assertTrue(module.analyze(logs(start_ms=0xFFFFFF00), 0, 1800)["passed"])

    def test_bad_target_still_produces_valid_report(self):
        data = logs()
        data[0] = data[0].replace(",25,25,-10", ",25,nan,-10")
        result = module.analyze(data, 0, 1800)
        self.assertFalse(result["passed"])
        json.dumps(result, allow_nan=False)


if __name__ == "__main__":
    unittest.main()
