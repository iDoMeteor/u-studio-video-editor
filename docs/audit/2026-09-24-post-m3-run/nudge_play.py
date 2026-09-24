exec(open(__file__.replace('nudge_play.py', 'drive_base.py')).read())
click('Recover'); wait(6)
act('select-all')
for rnd in range(int(os.environ.get('ROUNDS', '6'))):
    act('play-pause')
    t0 = time.time()
    for i in range(40):
        r = subprocess.run(['gdbus', 'call', '--session', '--dest', 'com.ustudio.VideoEditor', '--object-path',
                            '/com/ustudio/VideoEditor/window/1', '--method', 'org.gtk.Actions.Activate',
                            'nudge-right' if i % 2 == 0 else 'nudge-left', '[]', '{}'], capture_output=True, text=True, timeout=20)
        if r.returncode != 0:
            log(f"round {rnd} nudge {i}: FAILED after {time.time()-t0:.1f}s: {r.stderr.strip()[:120]}"); break
    else:
        log(f"round {rnd}: 40 nudges while playing in {time.time()-t0:.1f}s")
        act('play-pause'); continue
    break
log(f"alive={alive()}")
if alive():
    P = proc.pid
    subprocess.run(['timeout', '60', 'gdb', '-batch', '-p', str(P), '-ex', 'set pagination off', '-ex', 'thread apply all bt 12', '-ex', 'detach'],
                   stdout=open(os.path.join(outdir, 'bt.txt'), 'w'), stderr=subprocess.STDOUT)
    proc.kill(); proc.wait(10)
