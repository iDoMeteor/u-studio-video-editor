#!/usr/bin/env python3
"""Record the real desktop through the ScreenCast portal (Wayland), for demo
parts Xvfb can't show (GPU acceleration). The portal shows its consent
dialog: the owner picks a monitor or window and clicks Share.

    screencast.py <out.mkv> <ready-file> <stop-file>

Writes <ready-file> (containing the start time) once frames are flowing, and
stops cleanly when <stop-file> appears. Video only, H.264 in Matroska.
Talks to the owner's real session bus: run it outside the tour's private one.
"""
import os, sys, time
import dbus
from dbus.mainloop.glib import DBusGMainLoop
import gi
gi.require_version('Gst', '1.0')
from gi.repository import GLib, Gst

OUT, READY, STOP = sys.argv[1], sys.argv[2], sys.argv[3]
DBusGMainLoop(set_as_default=True)
Gst.init(None)
bus = dbus.SessionBus()
portal = bus.get_object('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop')
sc = dbus.Interface(portal, 'org.freedesktop.portal.ScreenCast')
loop = GLib.MainLoop()
sender = bus.get_unique_name()[1:].replace('.', '_')
state = {'session': None, 'n': 0, 'pipeline': None}

def token():
    state['n'] += 1
    return f'ustudemo{os.getpid()}_{state["n"]}'

def request(method, *args, options):
    t = token()
    path = f'/org/freedesktop/portal/desktop/request/{sender}/{t}'
    options['handle_token'] = t
    bus.add_signal_receiver(lambda code, results: on_response(method.__name__ if hasattr(method, '__name__') else '', code, results, path),
                            'Response', 'org.freedesktop.portal.Request', 'org.freedesktop.portal.Desktop', path)
    method(*args, options)

steps = []
def on_response(_name, code, results, path):
    if code != 0:
        print(f'portal step refused or cancelled (code {code})', flush=True)
        loop.quit(); return
    steps.pop(0)(results)

def created(results):
    state['session'] = results['session_handle']
    steps.append(selected)
    # types: 1 monitor, 2 window; cursor_mode 2 embedded
    request(sc.SelectSources, state['session'], options={'types': dbus.UInt32(int(os.environ.get('SC_TYPES', '3'))), 'multiple': False,
                                                          'cursor_mode': dbus.UInt32(2)})

def selected(_results):
    steps.append(started)
    print('CONSENT: the portal dialog is up now', flush=True)
    request(sc.Start, state['session'], '', options={})

def started(results):
    node = int(results['streams'][0][0])
    fd = sc.OpenPipeWireRemote(state['session'], dbus.Dictionary({}, signature='sv')).take()
    desc = (f'pipewiresrc fd={fd} path={node} do-timestamp=true keepalive-time=1000 ! videorate ! '
            'video/x-raw,framerate=30/1 ! videoconvert ! queue ! '
            'x264enc speed-preset=veryfast tune=zerolatency bitrate=12000 key-int-max=60 ! '
            f'h264parse ! matroskamux ! filesink location={OUT}')
    p = Gst.parse_launch(desc)
    state['pipeline'] = p
    p.set_state(Gst.State.PLAYING)
    open(READY, 'w').write(f'{time.time()}\n')
    print('RECORDING', flush=True)
    GLib.timeout_add(250, check_stop)

def check_stop():
    if not os.path.exists(STOP):
        return True
    p = state['pipeline']
    p.send_event(Gst.Event.new_eos())
    p.get_bus().timed_pop_filtered(10 * Gst.SECOND, Gst.MessageType.EOS | Gst.MessageType.ERROR)
    p.set_state(Gst.State.NULL)
    try:
        dbus.Interface(bus.get_object('org.freedesktop.portal.Desktop', state['session']),
                       'org.freedesktop.portal.Session').Close()
    except Exception:
        pass
    print('STOPPED', flush=True)
    loop.quit()
    return False

steps.append(created)
request(sc.CreateSession, options={'session_handle_token': token()})
loop.run()
