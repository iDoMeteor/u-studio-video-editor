#!/usr/bin/env python3
"""Cut the showreel's section segments from raw recording runs and write the
build plan (plan.json) and the music video for showreel_build.py.

    showreel_plan.py <runs dir> <build dir>

Each section is a list of picks (run, from chapter, to chapter or +seconds),
joined and sped up, plus `pad` seconds of spare footage at the end (cut off
again in U Stu, so each cut has frames for its transition). Bookend clips come
from the staged demo media. Silent: the music is its own track.
"""
import json, os, subprocess, sys

RUNS, BUILD = sys.argv[1], sys.argv[2]
MEDIA = os.path.expanduser('~/.cache/ustudio-demo-media')
SEG = os.path.join(BUILD, 'segments')
os.makedirs(SEG, exist_ok=True)
PAD = 1.0

SECTIONS = [
    # name, picks, speed, title template, fields
    ('intro', [('media', 'unicorn/chunk_000.mp4', 0, 13)], 1.0,
     'Title card', {'title': 'U Stu Video Editor', 'subtitle': 'Edit. Animate. Ship it.'}),
    ('Edit fast', [('r1-edit-pip', 'Import', 'Import a folder'), ('r1-edit-pip', 'Move', 'Undo and redo')], 2.2,
     'Chapter card', {'number': '01', 'title': 'Edit fast'}),
    ('Picture in picture', [('r1-edit-pip', 'Transform on the preview', 'Edit Transform'),
                            ('r2-keyframes', 'Pin a starting point', 'Curve lanes'),
                            ('r2-keyframes', 'Touch-record', '+26')], 2.2,
     'Chapter card', {'number': '02', 'title': 'Picture in picture'}),
    ('Transitions', [('r3-trans1', 'Wipes', 'Softness and reverse'), ('r4-trans2', 'Zoom', 'Sound: equal power'),
                     ('r5-trans3', 'Blend dissolves', 'Sound: Cut')], 1.3,
     'Chapter card', {'number': '03', 'title': 'Transitions'}),
    ('Effects', [('r6-fx1', 'Search and try', '(done)'), ('r7-fx2', 'Keyframes', '(done)')], 2.6,
     'Chapter card', {'number': '04', 'title': 'Effects'}),
    ('Titles', [('r9-titles', 'Template gallery', 'Export for OBS'), ('r8-anim', 'Add an animation', 'In the editor')], 2.0,
     'Chapter card', {'number': '05', 'title': 'Titles'}),
    ('Captions', [('r10-captions', 'Import captions', '(done)')], 1.6,
     'Chapter card', {'number': '06', 'title': 'Captions'}),
    ('Mixed media', [('r12-media', 'Import a folder', 'Split audio'), ('r12-media', 'Import Image Sequence', '+24'),
                     ('r12-media', 'Missing media', '(end)')], 2.0,
     'Chapter card', {'number': '07', 'title': 'Mixed media'}),
    ('GPU', [('r11-gpu', 'GPU acceleration', '+10'), ('r11-gpu', 'Playing on the graphics card', 'The difference')], 1.6,
     'Big number', {'stat': '26 fps', 'label': 'with GPU acceleration on (20 without)'}),
    ('Render', [('r12-media', 'Render', 'Render finished'), ('r12-media', 'Render finished', 'Missing media')], 1.6,
     'Chapter card', {'number': '08', 'title': 'Render'}),
    ('outro', [('media', 'unicorn/chunk_009.mp4', 0, 14)], 1.0,
     'End card with call to action', {'headline': 'Made with U Stu Video Editor', 'cta': 'Try the beta today'}),
]
TRANSITIONS = ['Slide Up', 'Zoom In', 'Additive Dissolve', 'Push Left', 'Circle Out', 'Spin In',   # visible tiles only: a click on one scrolled out of view misses
               'Lighten Dissolve', 'Screen Dissolve', 'Wipe Down and Right', 'Flash']


def chapters(run):
    return json.load(open(os.path.join(RUNS, run, 'chapters.json')))


def ch_time(run, title, after=None):
    for c in chapters(run):
        if c[1] == title and (after is None or c[0] > after):
            return c[0]
    raise SystemExit(f'{run}: no chapter {title!r}')


def ff_spans(run):
    p = os.path.join(RUNS, run, 'ff.json')
    return json.load(open(p)) if os.path.exists(p) else []


plan = {'segments': [], 'sections': [], 'transitions': TRANSITIONS, 'looks': {},
        'segments_dir': SEG}
narr_at = []
for idx, (name, picks, speed, template, fields) in enumerate(SECTIONS):
    out = os.path.join(SEG, f'{idx:02d}-{name.lower().replace(" ", "-")}.mp4')
    pad = 0.0 if idx == len(SECTIONS) - 1 else PAD
    ranges = []
    for p in picks:
        if p[0] == 'media':
            ranges.append((os.path.join(MEDIA, p[1]), p[2], p[3]))
            continue
        run, a, b = p
        ta = ch_time(run, a)
        tb = ta + float(b[1:]) if b.startswith('+') else ch_time(run, b, ta)
        # skip a fast-forwarded wait (a render) inside the pick: keep both sides
        for fa, fb in ff_spans(run):
            if fa - 1.5 <= ta < fb:
                ta = fb - 0.5             # a pick starting at the wait resumes after it
            elif ta < fa < tb:
                tb = fa
        ranges.append((os.path.join(RUNS, run, 'screen.mkv'), ta + 0.3, tb - 0.3))
    # the pad comes from the footage after the last pick
    src, a, b = ranges[-1]
    ranges[-1] = (src, a, b + pad * speed)
    ins, fc = [], []
    for k, (src, a, b) in enumerate(ranges):
        ins += ['-ss', f'{a:.3f}', '-t', f'{b - a:.3f}', '-i', src]
        fc.append(f'[{k}:v]fps=30,scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2,'
                  f'setsar=1,setpts=(PTS-STARTPTS)/{speed}[v{k}]')
    fc.append(''.join(f'[v{k}]' for k in range(len(ranges))) + f'concat=n={len(ranges)}:v=1:a=0[v]')
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y'] + ins + ['-filter_complex', ';'.join(fc), '-map', '[v]',
                    '-r', '30', '-c:v', 'libx264', '-preset', 'medium', '-crf', '15', '-pix_fmt', 'yuv420p', out],
                   check=True)
    dur = float(subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', out],
                               capture_output=True, text=True).stdout)
    plan['segments'].append({'file': out, 'content': round(dur - pad, 3), 'pad': pad})
    plan['sections'].append({'segment': idx, 'template': template, 'fields': fields, 'name': name})
    print(f'{idx:02d} {name:20s} {dur - pad:6.1f}s  (x{speed})', flush=True)

plan['looks'] = {'0': 'Unicorn Glow', str(len(SECTIONS) - 1): 'Neon Night'}
total = sum(s['content'] for s in plan['segments'])

# Music: Neon Level Up into Rave All Night (3 s crossfade), cut to the reel with a
# 4 s fade, on a black 1080p30 picture so it imports and splits like any clip.
MUSIC = os.path.join(os.path.dirname(BUILD.rstrip('/')), 'music')
music_video = os.path.join(BUILD, 'music.mp4')
subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', os.path.join(MUSIC, 'neon-level-up.mp3'),
                '-i', os.path.join(MUSIC, 'rave-all-night.mp3'), '-f', 'lavfi', '-i', f'color=c=black:s=1920x1080:r=30',
                '-filter_complex', f'[0:a][1:a]acrossfade=d=3[m];[m]atrim=0:{total:.3f},afade=t=out:st={total - 4:.3f}:d=4,'
                                   'aresample=48000[a]',
                '-map', '2:v', '-map', '[a]', '-t', f'{total:.3f}', '-c:v', 'libx264', '-preset', 'ultrafast',
                '-tune', 'stillimage', '-c:a', 'aac', '-b:a', '256k', music_video], check=True)
plan['music_video'] = music_video
plan['total'] = total
json.dump(plan, open(os.path.join(BUILD, 'plan.json'), 'w'), indent=1)
print(f'total {total:.1f} s = {int(total // 60)}:{int(total % 60):02d}')
