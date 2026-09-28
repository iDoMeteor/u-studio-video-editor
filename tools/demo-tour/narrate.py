#!/usr/bin/env python3
"""Prepare a demo's narration: speak every line (tts.py, cached) and write
narration.json for the tour and post.py.

    narrate.py <narration.txt> <out/narration.json>

A narration file has "## <chapter title>" headers, each followed by the line
spoken over that chapter; "#2" after a title means its second occurrence.
"## intro" and "## outro" play over the title cards. Other "#" lines are
comments. Runs outside the tour's private session, which has its own HOME
(the key and the TTS cache live in the real one).
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tts


def parse(path):
    lines, cur = {}, None
    for raw in open(path, encoding='utf-8'):
        line = raw.strip()
        if line.startswith('## '):
            cur = line[3:].strip()
            lines[cur] = []
        elif line.startswith('#') or not line:
            continue
        elif cur is not None:
            lines[cur].append(line)
    return {k: ' '.join(v) for k, v in lines.items() if v}


def key(title, occurrence):
    return title if occurrence == 1 else f'{title} #{occurrence}'


def main():
    src, out = sys.argv[1], sys.argv[2]
    result = {}
    for k, text in parse(src).items():
        wav = tts.speak(text)
        result[k] = {'text': text, 'wav': wav, 'dur': round(tts.duration(wav), 3)}
        print(f'{result[k]["dur"]:6.2f}s  {k}', flush=True)
    json.dump(result, open(out, 'w'), indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
