#!/usr/bin/env python3
"""Report the loudness and pitch of raw audio: tone.py <s16le stereo 48 kHz file>.

The test media carries a 440 Hz sine. The pitch is the median over audible
0.1 s blocks (zero crossings per block): the first ~0.6 s after pressing
play reads lower (150-225 Hz, seen 2026-09-27), which an average over the
whole span would be dragged down by.
"""
import array
import statistics
import sys

samples = array.array("h", open(sys.argv[1], "rb").read())
left = samples[0::2]
n = len(left)
if n == 0:
    sys.exit("no samples")
block = 4800  # 0.1 s
freqs = []
for start in range(0, n - block, block):
    seg = left[start:start + block]
    if max(abs(x) for x in seg) > 300:
        crossings = sum(1 for i in range(1, block) if (seg[i - 1] < 0) != (seg[i] < 0))
        freqs.append(crossings / 2 / (block / 48000))
median = statistics.median(freqs) if freqs else 0.0
print(f"audio: {n / 48000:.1f}s, audible {len(freqs) * 0.1:.1f}s, ~{median:.0f} Hz")
