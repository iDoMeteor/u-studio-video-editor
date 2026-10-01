#!/usr/bin/env python3
"""Lay the narration over U Stu's render of the showreel: each section's line
at its start (just after its title card), the music ducked under the voice,
loudness-normalised to -16 LUFS. The picture is U Stu's render, untouched.

    showreel_mix.py <plan.json> <narration.json> <render.mp4> <out.mp4>
"""
import json, subprocess, sys

plan = json.load(open(sys.argv[1])); narr = json.load(open(sys.argv[2]))
render, out = sys.argv[3], sys.argv[4]
FPS = 30
starts, t = [], 0
for s in plan['segments']:
    starts.append(t / FPS); t += int(round(s['content'] * FPS))

lines = []
for sec, at in zip(plan['sections'], starts):
    key = sec['name']
    if key not in narr:
        continue
    lead = 1.0 if key in ('intro', 'outro') else 1.8
    lines.append((at + lead, narr[key]['wav'], narr[key]['dur'], key))
prev = 0.0
for k, (a, w, d, key) in enumerate(lines):          # never overlap
    if a < prev + 0.3:
        lines[k] = (prev + 0.3, w, d, key); a = prev + 0.3
    prev = a + d
    print(f'{a:7.1f}s  {key} ({d:.1f}s)')

ins = ['-i', render]
fc = []
for j, (a, w, d, key) in enumerate(lines):
    ins += ['-i', w]
    ms = int(a * 1000)
    fc.append(f'[{j + 1}:a]aresample=48000,aformat=channel_layouts=stereo,adelay={ms}|{ms}[n{j}]')
fc.append(''.join(f'[n{j}]' for j in range(len(lines))) +
          f'amix=inputs={len(lines)}:normalize=0:dropout_transition=0,asplit=2[nar][key]')
fc.append('[0:a]aresample=48000[music]')
fc.append('[music][key]sidechaincompress=threshold=0.02:ratio=8:attack=20:release=500:makeup=1[duck]')
fc.append('[duck]volume=0.85[bed];[bed][nar]amix=inputs=2:normalize=0:duration=first,'
          'loudnorm=I=-16:TP=-1.5:LRA=11[a]')
r = subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + ins + ['-filter_complex', ';'.join(fc),
                    '-map', '0:v', '-map', '[a]', '-c:v', 'copy', '-c:a', 'aac', '-b:a', '256k', '-ar', '48000',
                    '-movflags', '+faststart', out], capture_output=True, text=True)
print('mix', r.returncode, r.stderr[-600:])
sys.exit(r.returncode)
