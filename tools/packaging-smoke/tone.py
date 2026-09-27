#!/usr/bin/env python3
"""Report the pitch of raw audio and any dropouts: tone.py <s16le stereo 48 kHz file>.

The test media carries a 440 Hz sine. Pitch comes from the period between
positive-going zero crossings, located by interpolation, per audible 0.1 s
block (median). Counting crossings instead reads low whenever the audio
has dropouts: 0.50.0-beta.2's start-of-playback starvation (gaps of 3-47 ms
in the first 0.6 s) read as 150-225 Hz that way, though every audible
cycle was at 440 Hz. Gaps are runs of near-silence (|s| <= 2) of 1 ms or
more inside the audible span.
"""
import array
import statistics
import sys

RATE, BLOCK, GAP = 48000, 4800, 48
left = array.array("h", open(sys.argv[1], "rb").read())[0::2]
if not left:
    sys.exit("no samples")
loud = [i for i in range(0, len(left) - BLOCK, BLOCK) if max(abs(x) for x in left[i:i + BLOCK]) > 300]
pitches = []
for start in loud:
    b = left[start:start + BLOCK]
    ups = [i - 1 + -b[i - 1] / (b[i] - b[i - 1]) for i in range(1, BLOCK) if b[i - 1] < 0 <= b[i] and b[i] - b[i - 1] > 50]
    periods = [y - x for x, y in zip(ups, ups[1:])]
    if len(periods) >= 3:
        pitches.append(RATE / statistics.median(periods))
gaps, run = [], 0
# From the first audible sample to the last: the silence before playback
# starts and after it pauses isn't a dropout.
audible = [i for i, s in enumerate(left) if abs(s) > 300]
span = left[audible[0]:audible[-1] + 1] if audible else []
for s in span:
    if abs(s) <= 2:
        run += 1
    else:
        if run >= GAP:
            gaps.append(run * 1000 / RATE)
        run = 0
pitch = statistics.median(pitches) if pitches else 0.0
print(f"audio: {len(left) / RATE:.1f}s, audible {len(loud) * 0.1:.1f}s, ~{pitch:.0f} Hz, "
      f"{len(gaps)} gap(s){' up to ' + format(max(gaps), '.0f') + ' ms' if gaps else ''}")
