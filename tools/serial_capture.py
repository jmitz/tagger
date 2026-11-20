#!/usr/bin/env python3
"""Reset the ESP32-S3 over USB-Serial/JTAG and capture its serial output.

Usage: serial_capture.py SECONDS [PORT] [--noreset]

Output is streamed to stdout line by line, so it can be tee'd or tailed.
"""
import sys
import time

import serial


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    seconds = float(args[0]) if args else 10.0
    port = args[1] if len(args) > 1 else "/dev/ttyACM0"
    s = serial.Serial(port, 115200, timeout=0.2)
    if "--noreset" not in sys.argv:
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
    end = time.time() + seconds
    while time.time() < end:
        data = s.read(4096)
        if data:
            sys.stdout.write(data.decode("utf-8", "replace"))
            sys.stdout.flush()


if __name__ == "__main__":
    main()
