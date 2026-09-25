# Post-process a recorded tour: captions (ASS, burned in), audio mux, fast-forward spans.
import json, os, subprocess, sys
OUT = sys.argv[1]; FINAL = sys.argv[2]
ch = json.load(open(os.path.join(OUT, 'chapters.json')))
ff = json.load(open(os.path.join(OUT, 'ff.json'))) if os.path.exists(os.path.join(OUT, 'ff.json')) else []
SPEED = 8.0
dur = float(subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0',
                            os.path.join(OUT, 'screen.mkv')], capture_output=True, text=True).stdout.strip())

def remap(t):
    """Time in the source -> time in the output, after fast-forward spans."""
    out = t
    for a, b in ff:
        if t > b: out -= (b - a) * (1 - 1 / SPEED)
        elif t > a: out -= (t - a) * (1 - 1 / SPEED)
    return out

def ts(t):
    t = max(0.0, t); h = int(t // 3600); m = int(t % 3600 // 60); s = t % 60
    return f"{h}:{m:02d}:{s:05.2f}"

def esc(s):
    return s.replace('{', '(').replace('}', ')').replace('\n', ' ')

ass = ["[Script Info]", "ScriptType: v4.00+", "PlayResX: 1920", "PlayResY: 1080", "",
       "[V4+ Styles]",
       "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding",
       # ASS colours are &HAABBGGRR; magenta #ff2bd6 -> &H00D62BFF ; box ink #0e0b1a @ ~20% transparent
       "Style: Title,Space Grotesk,46,&H00D62BFF,&H00FFFFFF,&H330E0B1A,&H330E0B1A,1,0,0,0,100,100,0,0,3,16,0,2,80,80,380,1",
       "Style: Sub,Space Grotesk,30,&H00FFFFFF,&H00FFFFFF,&H330E0B1A,&H330E0B1A,0,0,0,0,100,100,0,0,3,12,0,2,80,80,372,1",
       "", "[Events]", "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text"]
for i, (t, title, sub) in enumerate(ch):
    nxt = ch[i + 1][0] if i + 1 < len(ch) else dur
    a, b = remap(t + 0.2), remap(min(nxt, t + 5.5)) - 0.1
    if b - a < 0.8: b = a + 0.8
    text = "{\\fad(250,250)}" + esc(title)
    if sub:
        text += "\\N{\\fs30\\b0\\c&H00FFFFFF&}" + esc(sub)
    ass.append(f"Dialogue: 0,{ts(a)},{ts(b)},Title,,0,0,0,,{text}")
open(os.path.join(OUT, 'captions.ass'), 'w').write('\n'.join(ass) + '\n')

# Segments: normal / fast / normal ...
cuts = [0.0]
for a, b in ff: cuts += [a, b]
cuts.append(dur)
parts, labels = [], []
fast = {i for i in range(1, len(cuts) - 1, 2)}
fc = []
for i in range(len(cuts) - 1):
    a, b = cuts[i], cuts[i + 1]
    if b - a < 0.05: continue
    k = len(labels)
    if i in fast:
        fc.append(f"[0:v]trim={a}:{b},setpts=(PTS-STARTPTS)/{SPEED}[v{k}]")
        fc.append(f"anullsrc=r=48000:cl=stereo,atrim=0:{(b - a) / SPEED}[a{k}]")
    else:
        fc.append(f"[0:v]trim={a}:{b},setpts=PTS-STARTPTS[v{k}]")
        fc.append(f"[1:a]atrim={a}:{b},asetpts=PTS-STARTPTS[a{k}]")
    labels.append(k)
fc.append(''.join(f"[v{k}][a{k}]" for k in labels) + f"concat=n={len(labels)}:v=1:a=1[vc][ac]")
fc.append(f"[vc]subtitles={os.path.join(OUT, 'captions.ass')}[vo]")
cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', os.path.join(OUT, 'screen.mkv'),
       '-f', 's16le', '-ar', '48000', '-ac', '2', '-i', os.path.join(OUT, 'audio.raw'),
       '-filter_complex', ';'.join(fc), '-map', '[vo]', '-map', '[ac]',
       '-c:v', 'libx264', '-preset', 'medium', '-crf', '20', '-pix_fmt', 'yuv420p',
       '-c:a', 'aac', '-b:a', '160k', '-movflags', '+faststart', FINAL]
r = subprocess.run(cmd, capture_output=True, text=True)
print(r.returncode, r.stderr[-800:])
