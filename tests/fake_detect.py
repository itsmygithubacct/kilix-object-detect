#!/usr/bin/env python3
"""A detector that answers without a model, for the coordinate tests.

Always the same box in its own normalised space - 0.25 to 0.75 - so where
it lands in the frame is entirely the library's arithmetic and nothing
else's.
"""
import argparse
import os
import struct
import sys
import time

ROWS, COLUMNS = 20, 6


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--geometry", default="320x320")
    parser.add_argument("--model", default=None)
    parser.add_argument("--device", default=None)
    parser.add_argument("--conf", type=float, default=0.25)
    options = parser.parse_args()

    width, _, height = options.geometry.partition("x")
    frame_bytes = int(width) * int(height) * 4
    class_id = float(os.environ.get("FAKE_CLASS", "0"))
    score = float(os.environ.get("FAKE_SCORE", "0.9"))
    # "chunks:gap_ms" - dribble the reply out slowly, a slice at a time,
    # the way a dying connection to a remote detector would.
    drip = os.environ.get("FAKE_DRIP")

    while True:
        remaining = frame_bytes
        while remaining > 0:
            chunk = sys.stdin.buffer.read(remaining)
            if not chunk:
                return 0
            remaining -= len(chunk)
        rows = [0.0] * (ROWS * COLUMNS)
        rows[0], rows[1] = class_id, score
        box = os.environ.get("FAKE_BOX", "0.25,0.25,0.75,0.75")
        y0, x0, y1, x1 = (float(v) for v in box.split(","))
        rows[2], rows[3], rows[4], rows[5] = y0, x0, y1, x1
        payload = struct.pack("<%df" % (ROWS * COLUMNS), *rows)
        if drip:
            chunks, gap_ms = (int(v) for v in drip.split(":"))
            step = max(1, len(payload) // chunks)
            for at in range(0, len(payload), step):
                time.sleep(gap_ms / 1000.0)
                sys.stdout.buffer.write(payload[at:at + step])
                sys.stdout.buffer.flush()
        else:
            sys.stdout.buffer.write(payload)
            sys.stdout.buffer.flush()


if __name__ == "__main__":
    sys.exit(main())
