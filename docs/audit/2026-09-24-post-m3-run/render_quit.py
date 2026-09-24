exec(open(__file__.replace('render_quit.py', 'drive_base.py')).read())
import glob
click('Recover'); wait(6)
target = os.path.join(os.path.dirname(outdir), 'export.mp4')
for f in glob.glob(target + '*'): os.remove(f)
click('Render…'); wait(3, 'file dialog')
entry = None
def txt(n):
    try:
        t = n.get_text_iface(); return t.get_text(0, t.get_character_count()) if t else ''
    except Exception as e: return ''
for n, _ in walk(app_node()):
    nm, ds, rl = label(n)
    if rl in ('text', 'entry'):
        log(f"text node role={rl} name={nm!r} text={txt(n)!r}")
        if nm == 'Name:' and entry is None:
            entry = n
log(f"entry found: {entry is not None}")
if entry:
    entry.get_editable_text_iface().set_text_contents(target); time.sleep(0.5)
shot('dialog')
btn = None
for n, _ in walk(app_node()):
    nm, ds, rl = label(n)
    if rl == 'button' and nm == 'Save': btn = n
log(f"dialog Save button found: {btn is not None}")
if btn:
    ai = btn.get_action_iface(); ai.do_action(0); wait(float(os.environ.get('RENDER_WAIT', '2.5')), 'render running')
log(f"part file exists: {os.path.exists(target + '.part')} final: {os.path.exists(target)}")
click('Close'); wait(1); click('Discard', timeout=5)
for _ in range(120):
    if not alive(): break
    time.sleep(0.5)
log(f"app exit code {proc.poll()}; part={os.path.exists(target + '.part')} final={os.path.exists(target)}")
