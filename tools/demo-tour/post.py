# Post-process a recorded tour: keep a part's chapters (TOUR_PART, parts.json),
# fast-forward the waits, burn in the captions, lay the narration over its
# chapters with the app's own audio ducked under it, add the logo cards, and
# loudness-normalise the result.
#   post.py <run dir> <out.mp4>        env: TOUR_PART, TOUR_VERSION, TOUR_LOGO
import json, os, subprocess, sys
OUT = sys.argv[1]; FINAL = sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
SPEED = 8.0
NARR_LEAD = 0.4          # as tourlib.NARR_LEAD
LOUDNESS = 'loudnorm=I=-16:TP=-1.5:LRA=11'

def ffprobe_dur(path):
    return float(subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0',
                                 path], capture_output=True, text=True).stdout.strip())

def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(what, r.returncode, r.stderr[-800:], flush=True)
    return r

ch = [list(c) + [None] * (4 - len(c)) for c in json.load(open(os.path.join(OUT, 'chapters.json')))]
for c in ch:
    c[3] = c[3] or c[1]      # runs from before narration keys: the title is the key
ffp = os.path.join(OUT, 'ff.json')
ff = json.load(open(ffp)) if os.path.exists(ffp) else []
np_ = os.path.join(OUT, 'narration.json')
NARR = json.load(open(np_)) if os.path.exists(np_) else {}
dur = ffprobe_dur(os.path.join(OUT, 'screen.mkv'))
PART = os.environ.get('TOUR_PART', '')
part = json.load(open(os.path.join(HERE, 'parts.json')))[PART] if PART else None

# ---- the source spans to keep: a part's chapters, or everything
def chapter_end(i):
    return ch[i + 1][0] if i + 1 < len(ch) else dur

if part:
    # A part names chapters by title; a title the tour uses twice would pull in
    # the wrong footage (2026-09-28: the titles run's own "Save"). Refuse.
    dupes = sorted({c[1] for c in ch if c[1] in part['chapters'] and sum(d[1] == c[1] for d in ch) > 1})
    if dupes:
        sys.exit(f"post: part chapters used more than once in the tour: {dupes}; make their titles unique")
    keep = []
    for i, (t, title, sub, key) in enumerate(ch):
        if title in part['chapters']:
            a, b = max(0.0, t - 0.3), chapter_end(i) - 0.3
            if keep and a - keep[-1][1] < 0.5:
                keep[-1][1] = b
            else:
                keep.append([a, b])
else:
    keep = [[0.0, dur]]
kept = lambda t: any(a <= t < b for a, b in keep)

# ---- pieces: kept spans split at the fast-forwards; out() maps source -> output time
pieces = []
for a, b in keep:
    cuts = [a]
    for fa, fb in ff:
        if a < fa < b: cuts.append(fa)
        if a < fb < b: cuts.append(fb)
    cuts.append(b)
    for x, y in zip(cuts, cuts[1:]):
        if y - x > 0.05:
            fast = any(fa <= x and y <= fb for fa, fb in ff)
            pieces.append((x, y, fast))

def out(t):
    o = 0.0
    for x, y, fast in pieces:
        span = (y - x) / (SPEED if fast else 1)
        if t >= y:
            o += span
        elif t >= x:
            return o + (t - x) / (SPEED if fast else 1)
        else:
            return o
    return o

def in_ff(t):
    for fa, fb in ff:
        if fa <= t < fb: return fb
    return None

# ---- captions (ASS, burned in)
def ts(t):
    t = max(0.0, t); h = int(t // 3600); m = int(t % 3600 // 60); s = t % 60
    return f"{h}:{m:02d}:{s:05.2f}"

def esc(s):
    return s.replace('{', '(').replace('}', ')').replace('\n', ' ')

ass = ["[Script Info]", "ScriptType: v4.00+", "PlayResX: 1920", "PlayResY: 1080", "",
       "[V4+ Styles]",
       "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding",
       # ASS colours are &HAABBGGRR; magenta #ff2bd6 -> &H00D62BFF ; box ink #0e0b1a @ ~20% transparent
       # MarginV 380 sits on the preview's lower edge; a part about on-screen
       # text (captions) moves ours down over the timeline ("caption_margin_v").
       f"Style: Title,Space Grotesk,46,&H00D62BFF,&H00FFFFFF,&H330E0B1A,&H330E0B1A,1,0,0,0,100,100,0,0,3,16,0,2,80,80,{(part or {}).get('caption_margin_v', 380)},1",
       "", "[Events]", "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text"]
for i, (t, title, sub, key) in enumerate(ch):
    if title == '(end)' or not kept(t + 0.2):
        continue
    a, b = out(t + 0.2), out(min(chapter_end(i), t + 5.5)) - 0.1
    if b - a < 0.8: b = a + 0.8
    text = "{\\fad(250,250)}" + esc(title)
    if sub:
        text += "\\N{\\fs30\\b0\\c&H00FFFFFF&}" + esc(sub)
    ass.append(f"Dialogue: 0,{ts(a)},{ts(b)},Title,,0,0,0,,{text}")
open(os.path.join(OUT, 'captions.ass'), 'w').write('\n'.join(ass) + '\n')

# ---- narration placement (output time); never inside a fast-forward, never overlapping
lines = []
prev_end = 0.0
for t, title, sub, key in ch:
    line = NARR.get(key) if key else None
    if not line or not kept(t + 0.2):
        continue
    src = t + NARR_LEAD
    at = out(src) if in_ff(src) is None else out(in_ff(src)) + 0.2
    at = max(at, prev_end + 0.25)
    lines.append((at, line['wav']))
    prev_end = at + line['dur']
print(f"narration: {len(lines)} line(s)", flush=True)

# ---- the body: video + app audio, fast-forwards, captions, narration ducking the app
fc, labels = [], []
for k, (x, y, fast) in enumerate(pieces):
    if fast:
        fc.append(f"[0:v]trim={x}:{y},setpts=(PTS-STARTPTS)/{SPEED}[v{k}]")
        fc.append(f"anullsrc=r=48000:cl=stereo,atrim=0:{(y - x) / SPEED}[a{k}]")
    else:
        fc.append(f"[0:v]trim={x}:{y},setpts=PTS-STARTPTS[v{k}]")
        fc.append(f"[1:a]atrim={x}:{y},asetpts=PTS-STARTPTS[a{k}]")
    labels.append(k)
fc.append(''.join(f"[v{k}][a{k}]" for k in labels) + f"concat=n={len(labels)}:v=1:a=1[vc][ac]")
fc.append(f"[vc]subtitles={os.path.join(OUT, 'captions.ass')}[vo]")
inputs = ['-i', os.path.join(OUT, 'screen.mkv'),
          '-f', 's16le', '-ar', '48000', '-ac', '2', '-i', os.path.join(OUT, 'audio.raw')]
if lines:
    for j, (at, wav) in enumerate(lines):
        inputs += ['-i', wav]
        ms = int(at * 1000)
        fc.append(f"[{j + 2}:a]aresample=48000,aformat=channel_layouts=stereo,adelay={ms}|{ms}[n{j}]")
    fc.append(''.join(f"[n{j}]" for j in range(len(lines))) +
              f"amix=inputs={len(lines)}:normalize=0:dropout_transition=0,asplit=2[nar][key]")
    # The app's sound dips under the voice and comes back between lines.
    fc.append("[ac][key]sidechaincompress=threshold=0.015:ratio=10:attack=15:release=350:makeup=1[duck]")
    fc.append("[duck]volume=0.8[bed];[bed][nar]amix=inputs=2:normalize=0:duration=first[am]")
else:
    fc.append("[ac]anull[am]")
cmd = ['ffmpeg', '-y', '-loglevel', 'error'] + inputs + [
       '-filter_complex', ';'.join(fc), '-map', '[vo]', '-map', '[am]',
       '-c:v', 'libx264', '-preset', 'medium', '-crf', '20', '-pix_fmt', 'yuv420p',
       '-c:a', 'pcm_s16le', FINAL + '.body.mkv']
r = run(cmd, 'body')
if r.returncode != 0:
    sys.exit(1)

# ---- intro / outro cards with the owner's logo, narrated when the script has lines for them
LOGO = os.environ.get('TOUR_LOGO', '/home/jj/projects/unicorn-tears/software assets/video editor/images/logos/'
                      'U-Stu-Video-Editor-Logo-01-chatgpt/u-stu-video-editor-full.png')
ver = os.environ.get('TOUR_VERSION', '')
font = '/usr/share/fonts/google-noto/NotoSans-Regular.ttf'
if not os.path.exists(font):
    font = subprocess.run(['fc-match', '-f', '%{file}', 'sans'], capture_output=True, text=True).stdout.strip()

def dt_escape(s):
    return s.replace('\\', '\\\\').replace(':', '\\:').replace("'", "’").replace('%', '\\%')

def card(path, secs, lines_, voice=None):
    dt = ''.join(
        f",drawtext=fontfile='{font}':text='{dt_escape(t)}':fontcolor={c}:fontsize={fs}:x=(w-text_w)/2:y={y}"
        for t, c, fs, y in lines_)
    fc_ = (f"color=c=0x080914:s=1920x1080:r=30:d={secs}[bg];[1:v]scale=1400:-1[lg];"
           f"[bg][lg]overlay=(W-w)/2:250{dt},fade=in:st=0:d=0.7,fade=out:st={secs - 0.7}:d=0.7[v]")
    audio_in = ['-i', voice] if voice else ['-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=stereo']
    if voice:
        fc_ += ";[2:a]aresample=48000,aformat=channel_layouts=stereo,adelay=600|600,apad[a]"
    else:
        fc_ += ";[2:a]anull[a]"
    cmd_ = ['ffmpeg', '-y', '-loglevel', 'error', '-f', 'lavfi', '-i', 'anullsrc=r=48000:cl=stereo',
            '-loop', '1', '-t', str(secs), '-i', LOGO] + audio_in + [
            '-filter_complex', fc_, '-map', '[v]', '-map', '[a]', '-t', str(secs),
            '-c:v', 'libx264', '-crf', '18', '-pix_fmt', 'yuv420p', '-c:a', 'pcm_s16le', path]
    return run(cmd_, 'card ' + os.path.basename(path))

parts_ = [FINAL + '.body.mkv']
if os.path.exists(LOGO):
    intro = os.path.join(OUT, 'intro.mkv'); outro = os.path.join(OUT, 'outro.mkv')
    iv, ov = NARR.get('intro'), NARR.get('outro')
    head = f"{part['title']}  ·  {part['subtitle']}" if part else 'Demo tour'
    ok_i = card(intro, max(4.5, (iv['dur'] + 1.4) if iv else 0),
                [(head, '0xF3F8FF' if part else '0xA04BFA', 40, 740),
                 (f'v{ver}' if ver else '', '0xA04BFA', 30, 810)], iv and iv['wav']).returncode == 0
    ok_o = card(outro, max(6, (ov['dur'] + 1.4) if ov else 0),
                [('Part of the Unicorn Tears Project  ·  djunicorntears.com', '0xF3F8FF', 38, 760),
                 ('Source  ·  github.com/idometeor/u-studio-video-editor', '0x23DDF2', 30, 830),
                 ('MIT licence', '0x9A93B8', 26, 890)], ov and ov['wav']).returncode == 0
    if ok_i and ok_o:
        parts_ = [intro, FINAL + '.body.mkv', outro]

# ---- join, loudness-normalise, encode
ins = sum((['-i', p] for p in parts_), [])
join = ''.join(f"[{k}:v][{k}:a]" for k in range(len(parts_))) + f"concat=n={len(parts_)}:v=1:a=1[v][a0];[a0]{LOUDNESS}[a]"
r = run(['ffmpeg', '-y', '-loglevel', 'error'] + ins + ['-filter_complex', join, '-map', '[v]', '-map', '[a]',
         '-c:v', 'libx264', '-preset', 'medium', '-crf', '20', '-pix_fmt', 'yuv420p',
         '-c:a', 'aac', '-b:a', '192k', '-ar', '48000', '-movflags', '+faststart', FINAL], 'final')
if r.returncode == 0:
    os.remove(FINAL + '.body.mkv')
sys.exit(r.returncode)
