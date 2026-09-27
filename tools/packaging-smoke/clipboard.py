#!/usr/bin/env python3
"""Print the X CLIPBOARD as UTF-8 text (python-xlib; no xclip needed).

Converts the selection directly: GTK 4's own clipboard reader reported the
app's clipboard as empty on Xvfb even while it held the text.
"""
import time

from Xlib import X, display

d = display.Display()
window = d.screen().root.create_window(0, 0, 1, 1, 0, X.CopyFromParent)
clipboard, prop = d.intern_atom("CLIPBOARD"), d.intern_atom("SMOKE_CLIPBOARD")
window.convert_selection(clipboard, d.intern_atom("UTF8_STRING"), prop, X.CurrentTime)
d.flush()
end = time.time() + 5
while time.time() < end:
    while d.pending_events():
        event = d.next_event()
        if event.type == X.SelectionNotify:
            value = window.get_full_property(prop, X.AnyPropertyType) if event.property != X.NONE else None
            print(value.value.decode(errors="replace") if value else "(clipboard empty)")
            raise SystemExit(0)
    time.sleep(0.05)
print("(clipboard: no answer)")
