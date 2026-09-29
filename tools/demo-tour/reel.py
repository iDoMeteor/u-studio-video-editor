#!/usr/bin/env python3
"""Cut a highlight reel from earlier runs' raw recordings (screen.mkv +
audio.raw), then post it like a part (captions, narration, cards).

    reel.py <segments.json> <out dir>
    TOUR_PART=<part> python3 post.py <out dir> <out dir>/demo.mp4

segments.json: [[run dir, start s, length s, chapter title, caption], ...]
The segments are joined by re-encoding (the concat filter): a stream-copy
join of recordings from different encoders left one segment's timestamps
out of line, and post.py's trim then dropped it (2026-09-29).
"""
import json, os, subprocess, sys

segs = json.load(open(sys.argv[1]))
out = sys.argv[2]
os.makedirs(out, exist_ok=True)
ins, chapters, t = [], [], 0.0
for i, (src, a, n, title, sub) in enumerate(segs):
    ins += ['-ss', str(a), '-t', str(n), '-i', f'{src}/screen.mkv',
            '-ss', str(a), '-t', str(n), '-f', 's16le', '-ar', '48000', '-ac', '2', '-i', f'{src}/audio.raw']
    chapters.append([t, title, sub, title]); t += n
chapters.append([t, '(end)', '', '(end)'])
fc = ''.join(f'[{2 * i}:v]fps=30,scale=1920:1080,setsar=1[v{i}];' for i in range(len(segs)))
fc += ''.join(f'[v{i}][{2 * i + 1}:a]' for i in range(len(segs))) + f'concat=n={len(segs)}:v=1:a=1[v][a]'
joined = os.path.join(out, 'joined.mkv')
subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + ins + ['-filter_complex', fc, '-map', '[v]', '-map', '[a]',
                '-c:v', 'libx264', '-preset', 'veryfast', '-crf', '16', '-c:a', 'pcm_s16le', joined], check=True)
subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', joined, '-map', '0:v', '-c', 'copy',
                os.path.join(out, 'screen.mkv')], check=True)
subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', joined, '-map', '0:a', '-f', 's16le', '-ar', '48000',
                '-ac', '2', os.path.join(out, 'audio.raw')], check=True)
json.dump(chapters, open(os.path.join(out, 'chapters.json'), 'w'))
print(f'{len(segs)} segments, {t:.1f} s')
