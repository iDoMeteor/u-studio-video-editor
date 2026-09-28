#!/usr/bin/env python3
"""Narration for the demo videos: OpenAI's speech API, one cached WAV per line.

    tts.py "Some text" [out.wav]     speak one line (prints the cached WAV's path)

The key is read at call time from OPENAI_API_KEY, or else from the owner's
~/Repos/ai-animated-video/config.env (read, never copied). It is never
printed, logged or written anywhere. Network use by the demo tooling only;
the editor itself never goes online.

WAVs are cached in ~/.cache/ustudio-demo-tts by a hash of every request
parameter (text, voice, model, speed, instructions), so reruns are instant
and a changed line or voice makes a new file.
"""
import hashlib, json, os, re, shutil, subprocess, sys, urllib.error, urllib.request

VOICE = os.environ.get('TOUR_VOICE', 'cedar')
MODEL = os.environ.get('TOUR_VOICE_MODEL', 'gpt-4o-mini-tts')
SPEED = float(os.environ.get('TOUR_VOICE_SPEED', '0.9'))
INSTRUCTIONS = ("You are narrating a screen recording of a video editor for its users. Speak warmly and clearly, "
                "like a friendly creator showing a tool they enjoy: relaxed, natural, conversational; "
                "not salesy, not rushed.")
CACHE = os.path.join(os.path.expanduser('~'), '.cache', 'ustudio-demo-tts')
CONFIG = os.path.join(os.path.expanduser('~'), 'Repos', 'ai-animated-video', 'config.env')


def _key():
    key = os.environ.get('OPENAI_API_KEY', '').strip()
    if key:
        return key
    try:
        with open(CONFIG) as f:
            for line in f:
                m = re.match(r'\s*(?:export\s+)?OPENAI_API_KEY\s*=\s*(.*?)\s*$', line)
                if m:
                    return m.group(1).strip('"\'')
    except OSError:
        pass
    raise SystemExit('tts: no OPENAI_API_KEY in the environment or ' + CONFIG)


def params(text):
    return {'model': MODEL, 'voice': VOICE, 'speed': SPEED, 'instructions': INSTRUCTIONS,
            'input': text, 'response_format': 'wav'}


def cached(text):
    """The cache path for this line (it may not exist yet)."""
    h = hashlib.sha256(json.dumps(params(text), sort_keys=True).encode()).hexdigest()[:32]
    return os.path.join(CACHE, h + '.wav')


def speak(text):
    """A WAV of `text`, from the cache or the API."""
    path = cached(text)
    if os.path.exists(path):
        return path
    os.makedirs(CACHE, exist_ok=True)
    req = urllib.request.Request('https://api.openai.com/v1/audio/speech',
                                 data=json.dumps(params(text)).encode(),
                                 headers={'Authorization': 'Bearer ' + _key(), 'Content-Type': 'application/json'})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                data = r.read()
            break
        except urllib.error.HTTPError as e:
            body = e.read()[:300].decode(errors='replace')
            if attempt == 2 or e.code < 500 and e.code != 429:
                raise SystemExit(f'tts: HTTP {e.code}: {body}')
        except urllib.error.URLError as e:
            if attempt == 2:
                raise SystemExit(f'tts: {e.reason}')
    tmp = path + '.part'
    with open(tmp, 'wb') as f:
        f.write(data)
    # The API's streamed WAV header can carry a placeholder length; rewrite it
    # so ffprobe and ffmpeg read the duration right.
    fixed = path + '.fix.wav'
    r = subprocess.run(['ffmpeg', '-y', '-loglevel', 'error', '-i', tmp, '-c:a', 'pcm_s16le', fixed],
                       capture_output=True)
    if r.returncode == 0:
        os.replace(fixed, path)
        os.remove(tmp)
    else:
        os.replace(tmp, path)
    return path


def duration(path):
    out = subprocess.run(['ffprobe', '-v', 'error', '-show_entries', 'format=duration', '-of', 'csv=p=0', path],
                         capture_output=True, text=True).stdout.strip()
    return float(out or 0)


if __name__ == '__main__':
    wav = speak(sys.argv[1])
    if len(sys.argv) > 2:
        shutil.copy(wav, sys.argv[2])
    print(wav, f'{duration(wav):.2f}s')
