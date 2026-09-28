#!/usr/bin/env python3
"""Writes the demo's template pack: a few built-in stream templates under a
pack name that reads well on screen. Same format as tools/make_test_pack.py
(doc 20), whose PNG writer it borrows; generated at runtime, never committed.

    make_demo_pack.py OUT.zip
"""
import hashlib, importlib.util, os, sys, zipfile
from xml.sax.saxutils import quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, "..")
spec = importlib.util.spec_from_file_location("make_test_pack", os.path.join(TOOLS, "make_test_pack.py"))
mtp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mtp)

TEMPLATES = ("starting-soon", "be-right-back", "up-next")


def main():
    out = sys.argv[1]
    files = {}
    for name in TEMPLATES:
        with open(os.path.join(mtp.TEMPLATES, name + ".ustitle"), "rb") as f:
            files[f"templates/{name}.ustitle"] = f.read()
        files[f"previews/{name}.png"] = mtp.png()
    manifest = ('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<pack format="1" id="unicorn-tears/stream-kit" version="1.0.0">\n'
                "  <title>Unicorn Tears stream kit</title>\n  <author>Unicorn Tears Project</author>\n"
                "  <licence>CC0-1.0</licence>\n")
    for path, data in files.items():
        manifest += (f"  <file path={quoteattr(path)} size=\"{len(data)}\" "
                     f"sha256=\"{hashlib.sha256(data).hexdigest()}\"/>\n")
    manifest += "</pack>\n"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("pack.xml", manifest)
        for path, data in files.items():
            z.writestr(path, data)
    return 0


if __name__ == "__main__":
    sys.exit(main())
