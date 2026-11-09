#!/usr/bin/env python3
# /// script
# dependencies = ["pyserial", "pillow"]
# ///
"""Saves what the T-Deck's screen shows as a PNG, using HuckleBerry's "S" serial command.

Usage: uv run tools/screenshot.py --port /dev/ttyACM0 --out screenshot.png
(uv installs pyserial and Pillow; plain python3 works where they are installed.)
"""
import argparse
import sys
import time

import serial
from PIL import Image


def capture(port, timeout=15):
    # Leave DTR and RTS as opening sets them: changing them resets the ESP32-S3.
    with serial.Serial(port, 115200, timeout=1) as ser:
        ser.reset_input_buffer()
        ser.write(b"S\n")
        deadline = time.time() + timeout
        while True:
            if time.time() > deadline:
                sys.exit("No screenshot from the T-Deck (is HuckleBerry running?)")
            line = ser.readline().decode(errors="replace").strip()
            if line.startswith("SCREENSHOT FAILED"):
                sys.exit("The T-Deck couldn't capture the screen")
            if line.startswith("SCREENSHOT "):
                width, height, swapped = (int(v) for v in line.split()[1:4])
                break
        size = width * height * 2
        data = bytearray()
        while len(data) < size:
            if time.time() > deadline:
                sys.exit(f"Screenshot cut off after {len(data)} of {size} bytes")
            data += ser.read(size - len(data))
    order = "big" if swapped else "little"
    rgb = bytearray(width * height * 3)
    for i in range(width * height):
        v = int.from_bytes(data[2 * i:2 * i + 2], order)
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        rgb[3 * i:3 * i + 3] = bytes(((r * 527 + 23) >> 6, (g * 259 + 33) >> 6, (b * 527 + 23) >> 6))
    return Image.frombytes("RGB", (width, height), bytes(rgb))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", required=True)
    parser.add_argument("--out", default="screenshot.png")
    args = parser.parse_args()
    capture(args.port).save(args.out)
    print(f"Saved {args.out}")


if __name__ == "__main__":
    main()
