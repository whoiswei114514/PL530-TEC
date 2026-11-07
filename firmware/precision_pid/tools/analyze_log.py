"""Evaluate raw PIDCSV samples; never use filtered temperature for acceptance."""

import argparse
import csv
import json
import math
import statistics
from pathlib import Path


def analyze(lines, settle_seconds=600, duration_seconds=1800):
    rows = []
    issues = []
    previous_ms = None
    elapsed_ms = 0
    for line_number, line in enumerate(lines, 1):
        if not line.startswith("PIDCSV,"):
            continue
        try:
            values = next(csv.reader([line]))
            if len(values) != 10:
                raise ValueError("expected 10 fields")
            ms = int(values[1])
            if not 0 <= ms <= 0xFFFFFFFF:
                raise ValueError("timestamp outside uint32 range")
            raw, filtered, target, duty, p, i, d = map(float, values[2:9])
            if previous_ms is not None:
                gap = (ms - previous_ms) & 0xFFFFFFFF
                if gap == 0 or gap > 0x7FFFFFFF:
                    raise ValueError("duplicate or backwards sample timestamp")
                elapsed_ms += gap
            else:
                gap = 0
            previous_ms = ms
            rows.append((elapsed_ms, raw, target, values[9], gap))
        except (ValueError, csv.Error) as error:
            issues.append(f"line {line_number}: {error}")
    start = settle_seconds * 1000
    end = start + duration_seconds * 1000
    selected = [row for row in rows if start <= row[0] <= end]
    if len(selected) < 2:
        return {"passed": False, "issues": issues + ["insufficient PIDCSV samples"]}
    duration = selected[-1][0] - selected[0][0]
    if duration < duration_seconds * 1000 - 500:
        issues.append("insufficient continuous measurement duration")
    if selected[0][0] > start + 500 or selected[-1][0] < end - 500:
        issues.append("measurement interval is not fully covered")
    targets = [row[2] for row in selected if math.isfinite(row[2])]
    if not targets:
        return {"passed": False, "issues": issues + ["no finite target"]}
    expected_target = targets[0]
    temperatures = []
    for elapsed, raw, target, state, gap in selected:
        if not math.isfinite(raw) or not math.isfinite(target):
            issues.append(f"{elapsed} ms: invalid raw temperature or target")
            continue
        if abs(target - expected_target) > 1e-6:
            issues.append(f"{elapsed} ms: target changed during measurement")
        if state not in ("NormalControl", "BalanceControl"):
            issues.append(f"{elapsed} ms: control not active ({state})")
        if gap >= 1000:
            issues.append(f"{elapsed} ms: sample gap exceeds freshness timeout")
        temperatures.append(raw)
    if not temperatures:
        return {"passed": False, "issues": issues + ["no finite raw temperature"]}
    max_error_mk = max(abs(value - expected_target) for value in temperatures) * 1000
    peak_to_peak_mk = (max(temperatures) - min(temperatures)) * 1000
    if max_error_mk > 10.0:
        issues.append("maximum target error exceeds 10 mK")
    if peak_to_peak_mk > 20.0:
        issues.append("peak-to-peak variation exceeds 20 mK")
    return {
        "passed": not issues,
        "measurement": "raw TMP117 temperature, not filtered",
        "samples": len(selected),
        "duration_seconds": duration / 1000,
        "target_c": expected_target,
        "mean_c": statistics.mean(temperatures),
        "standard_deviation_mk": statistics.pstdev(temperatures) * 1000,
        "maximum_error_mk": max_error_mk,
        "peak_to_peak_mk": peak_to_peak_mk,
        "issues": issues[:20],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--settle-seconds", type=float, default=600)
    parser.add_argument("--duration-seconds", type=float, default=1800)
    arguments = parser.parse_args()
    if arguments.settle_seconds < 0 or arguments.duration_seconds < 120:
        parser.error("settle must be nonnegative and duration must be at least 120 seconds")
    with arguments.log.open(encoding="utf-8", errors="replace") as source:
        result = analyze(source, arguments.settle_seconds, arguments.duration_seconds)
    print(json.dumps(result, ensure_ascii=True, indent=2, allow_nan=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
