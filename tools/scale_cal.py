#!/usr/bin/env python3
"""Two-point HX711 calibration against the tools/scale_cal firmware.

  scale_cal.py tare [SECONDS]            # scale empty: record the zero point
  scale_cal.py weigh GRAMS [SECONDS]     # known weight on: compute the factor
  scale_cal.py check [SECONDS]           # print the weight using the saved calibration

Results are stored in tools/scale_cal/calibration.json.
Run with the project venv: venv/bin/python tools/scale_cal.py ...
"""
import json
import re
import statistics
import sys
import time
from pathlib import Path

import serial

PORT = "/dev/ttyACM0"
CAL_FILE = Path(__file__).resolve().parent / "scale_cal" / "calibration.json"
LINE = re.compile(r"AVG t=\d+ n=(\d+) mean=(-?[\d.]+) sd=([\d.]+)")


def capture(seconds: float) -> tuple[float, float, int]:
    """Return (mean of per-second means, sd of those means, total samples)."""
    means, total = [], 0
    with serial.Serial(PORT, 115200, timeout=0.5) as s:
        s.reset_input_buffer()
        s.readline()  # drop a possibly partial line
        end = time.time() + seconds
        while time.time() < end:
            m = LINE.search(s.readline().decode("utf-8", "replace"))
            if m and int(m.group(1)) > 0:
                means.append(float(m.group(2)))
                total += int(m.group(1))
                print(f"  {len(means):3d}s  mean={means[-1]:12.1f}", flush=True)
    if len(means) < 3:
        sys.exit("Too few readings: is the scale_cal firmware running?")
    # Keep only the settled part: readings close to the median of the last half.
    # Drops seconds captured while the weight was still being placed or removed.
    ref = statistics.median(means[len(means) // 2:])
    spread = statistics.median(abs(m - ref) for m in means[len(means) // 2:]) or 1.0
    settled = [m for m in means if abs(m - ref) <= max(10 * spread, 200)]
    if len(settled) < len(means):
        print(f"  (ignored {len(means) - len(settled)} unsettled second(s))")
    if len(settled) < 3:
        sys.exit("Reading never settled: keep the scale still and try again.")
    return statistics.fmean(settled), statistics.stdev(settled), total


def load() -> dict:
    return json.loads(CAL_FILE.read_text()) if CAL_FILE.exists() else {}


def save(cal: dict) -> None:
    CAL_FILE.write_text(json.dumps(cal, indent=2) + "\n")


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    cal = load()

    if cmd == "tare":
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10
        print(f"Capturing tare for {secs:.0f} s, keep the scale empty and still...")
        mean, sd, n = capture(secs)
        cal.update(tare_counts=round(mean, 1), tare_sd=round(sd, 1), tare_samples=n,
                   tare_time=time.strftime("%Y-%m-%d %H:%M:%S"))
        save(cal)
        print(f"TARE  {mean:.1f} counts (sd of 1 s means {sd:.1f}, {n} samples)")

    elif cmd == "weigh":
        if "tare_counts" not in cal:
            sys.exit("Run 'tare' first.")
        grams = float(sys.argv[2])
        secs = float(sys.argv[3]) if len(sys.argv) > 3 else 10
        print(f"Capturing {grams:g} g reference for {secs:.0f} s, keep it still...")
        mean, sd, n = capture(secs)
        delta = mean - cal["tare_counts"]
        if abs(delta) < 20 * max(sd, cal.get("tare_sd", 1), 1):
            sys.exit(f"Change of only {delta:.1f} counts: is the weight on the cell?")
        factor = delta / grams  # counts per gram (sign shows A+/A- orientation)
        resolution = max(sd, cal.get("tare_sd", 0)) / abs(factor)
        cal.update(ref_grams=grams, ref_counts=round(mean, 1), ref_sd=round(sd, 1),
                   counts_per_gram=round(factor, 4), resolution_g=round(resolution, 3),
                   ref_time=time.strftime("%Y-%m-%d %H:%M:%S"))
        save(cal)
        print(f"REF   {mean:.1f} counts, delta {delta:.1f}")
        print(f"FACTOR {factor:.4f} counts/g  ({'inverted' if factor < 0 else 'normal'} sign)")
        print(f"Noise-limited resolution about {resolution:.2f} g (1 s average)")

    elif cmd == "check":
        if "counts_per_gram" not in cal:
            sys.exit("Run 'tare' and 'weigh' first.")
        secs = float(sys.argv[2]) if len(sys.argv) > 2 else 5
        mean, sd, _ = capture(secs)
        grams = (mean - cal["tare_counts"]) / cal["counts_per_gram"]
        print(f"WEIGHT {grams:.1f} g  (+/- {sd / abs(cal['counts_per_gram']):.2f} g)")

    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
