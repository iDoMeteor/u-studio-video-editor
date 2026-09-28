#!/usr/bin/env python3
"""A local stand-in for the template sharing service (doc 21), for tests.

    tools/share_mock.py [--port 0] [--seed DIR] [--log FILE] [--corrupt]

Serves doc 21's first-version API on 127.0.0.1, in memory:

  GET  /v1/packs?q=&tag=                          catalogue
  GET  /v1/packs/{id}                             one pack (id URL-encoded)
  POST /v1/packs/{id}/versions/{v}/download       a signed URL (expires in 5 min), sha256, size
  GET  /files/{token}                             the signed URL's target
  POST /v1/uploads                                (signed in) a presigned PUT URL and an upload id
  PUT  /upload/{token}                            the presigned target
  GET  /v1/uploads/{id}                           (signed in) pipeline status
  GET  /v1/me                                     (signed in) the publisher
  GET  /oauth2/authorize                          OAuth 2.0 code + PKCE: redirects at once, approving
  POST /oauth2/token                              code (checked against the S256 challenge) or refresh

--seed DIR publishes every .zip / .tar.gz pack in DIR. --log FILE appends
one line per request ("METHOD /path"): the test for "nothing reaches the
network without a user action" reads it. --corrupt serves downloads with
one byte changed, for the client's re-verification. It prints
"listening on http://127.0.0.1:PORT" once ready. No TLS: tests only.
"""
import argparse
import base64
import hashlib
import hmac
import io
import json
import os
import secrets
import sys
import tarfile
import threading
import time
import urllib.parse
import xml.etree.ElementTree as ET
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEY = secrets.token_bytes(32)
LOCK = threading.Lock()
PACKS = {}      # id -> {"manifest": {...}, "versions": {version: bytes}}
UPLOADS = {}    # upload id -> {"id", "version", "status", "reasons", "publisher"}
CODES = {}      # authorization code -> {"challenge", "redirect_uri"}
TOKENS = {}     # access token -> publisher slug
REFRESH = {}    # refresh token -> publisher slug
ARGS = None


def sign(payload):
    raw = base64.urlsafe_b64encode(json.dumps(payload).encode()).decode().rstrip("=")
    mac = hmac.new(KEY, raw.encode(), hashlib.sha256).hexdigest()
    return raw + "." + mac


def unsign(token):
    try:
        raw, mac = token.rsplit(".", 1)
    except ValueError:
        return None
    if not hmac.compare_digest(mac, hmac.new(KEY, raw.encode(), hashlib.sha256).hexdigest()):
        return None
    payload = json.loads(base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4)))
    return payload if payload.get("exp", 0) >= time.time() else None


def manifest_of(data):
    """pack.xml from a zip or tar.gz, as a dict."""
    try:
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            xml = z.read("pack.xml")
    except zipfile.BadZipFile:
        with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as t:
            xml = t.extractfile("pack.xml").read()
    root = ET.fromstring(xml)
    text = lambda tag: (root.findtext(tag) or "")
    return {"id": root.get("id"), "version": root.get("version"), "title": text("title"),
            "description": text("description"), "author": text("author"), "licence": text("licence"),
            "tags": [t.text or "" for t in root.findall("tag")],
            "templates": [f.get("path") for f in root.findall("file") if f.get("path", "").startswith("templates/")]}


def publish(data, publisher="seed"):
    m = manifest_of(data)
    with LOCK:
        entry = PACKS.setdefault(m["id"], {"manifest": m, "versions": {}, "downloads": 0, "publisher": publisher})
        entry["manifest"] = m
        entry["versions"][m["version"]] = data
    return m


def latest(entry):
    def key(v):
        return [int(x) if x.isdigit() else 0 for x in v.split("-")[0].split(".")]
    return max(entry["versions"], key=key)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def log(self):
        if ARGS.log:
            with LOCK, open(ARGS.log, "a") as f:
                f.write(f"{self.command} {urllib.parse.urlsplit(self.path).path}\n")

    def reply(self, status, body=b"", content_type="application/json", headers=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def publisher(self):
        auth = self.headers.get("Authorization", "")
        return TOKENS.get(auth[7:]) if auth.startswith("Bearer ") else None

    def base(self):
        return f"http://{self.headers.get('Host')}"

    def do_GET(self):
        self.log()
        url = urllib.parse.urlsplit(self.path)
        parts = [urllib.parse.unquote(p) for p in url.path.split("/")[1:]]
        query = urllib.parse.parse_qs(url.query)
        if parts[:2] == ["v1", "packs"] and len(parts) == 2:
            q = (query.get("q") or [""])[0].lower()
            tag = (query.get("tag") or [""])[0]
            out = []
            with LOCK:
                for pid, e in sorted(PACKS.items()):
                    m = e["manifest"]
                    if q and q not in m["title"].lower() and q not in m["description"].lower():
                        continue
                    if tag and tag not in m["tags"]:
                        continue
                    out.append({"id": pid, "title": m["title"], "author": m["author"], "tags": m["tags"],
                                "licence": m["licence"], "latestVersion": latest(e), "downloads": e["downloads"]})
            return self.reply(200, {"packs": out, "cursor": None})
        if parts[:2] == ["v1", "packs"] and len(parts) == 3:
            with LOCK:
                e = PACKS.get(parts[2])
                if not e:
                    return self.reply(404, {"error": "no such pack"})
                m = e["manifest"]
                versions = [{"version": v, "sha256": hashlib.sha256(d).hexdigest(), "size": len(d)}
                            for v, d in e["versions"].items()]
            return self.reply(200, {"id": m["id"], "title": m["title"], "description": m["description"],
                                    "author": m["author"], "licence": m["licence"], "tags": m["tags"],
                                    "versions": versions,
                                    "templates": [os.path.basename(t) for t in m["templates"]]})
        if parts[:1] == ["files"] and len(parts) == 2:
            payload = unsign(parts[1])
            if not payload:
                return self.reply(403, {"error": "expired or invalid"})
            with LOCK:
                data = PACKS[payload["id"]]["versions"][payload["version"]]
            if ARGS.corrupt:
                data = data[:-10] + bytes([data[-10] ^ 0xFF]) + data[-9:]
            return self.reply(200, data, "application/octet-stream")
        if parts[:2] == ["v1", "uploads"] and len(parts) == 3:
            if not self.publisher():
                return self.reply(401, {"error": "sign in"})
            with LOCK:
                u = UPLOADS.get(parts[2])
            if not u:
                return self.reply(404, {"error": "no such upload"})
            return self.reply(200, {"status": u["status"], "reasons": u["reasons"]})
        if parts == ["v1", "me"]:
            who = self.publisher()
            if not who:
                return self.reply(401, {"error": "sign in"})
            return self.reply(200, {"displayName": who.title(), "slug": who})
        if parts == ["oauth2", "authorize"]:
            redirect = (query.get("redirect_uri") or [""])[0]
            challenge = (query.get("code_challenge") or [""])[0]
            if (query.get("code_challenge_method") or [""])[0] != "S256" or not challenge or \
                    not redirect.startswith("http://127.0.0.1:"):
                return self.reply(400, {"error": "PKCE S256 and a loopback redirect are required"})
            code = secrets.token_urlsafe(16)
            with LOCK:
                CODES[code] = {"challenge": challenge, "redirect_uri": redirect}
            state = (query.get("state") or [""])[0]
            location = redirect + "?" + urllib.parse.urlencode({"code": code, "state": state})
            return self.reply(302, b"", "text/plain", {"Location": location})
        return self.reply(404, {"error": "not found"})

    def do_POST(self):
        self.log()
        url = urllib.parse.urlsplit(self.path)
        parts = [urllib.parse.unquote(p) for p in url.path.split("/")[1:]]
        body = self.body()
        if len(parts) == 6 and parts[:2] == ["v1", "packs"] and parts[3] == "versions" and parts[5] == "download":
            with LOCK:
                e = PACKS.get(parts[2])
                data = e and e["versions"].get(parts[4])
                if not data:
                    return self.reply(404, {"error": "no such version"})
                e["downloads"] += 1
            token = sign({"id": parts[2], "version": parts[4], "exp": time.time() + 300})
            return self.reply(200, {"url": f"{self.base()}/files/{token}",
                                    "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)})
        if parts == ["v1", "uploads"]:
            who = self.publisher()
            if not who:
                return self.reply(401, {"error": "sign in"})
            request = json.loads(body or b"{}")
            upload = secrets.token_hex(8)
            with LOCK:
                UPLOADS[upload] = {"id": request.get("id"), "version": request.get("version"),
                                   "status": "pending", "reasons": [], "publisher": who}
            token = sign({"upload": upload, "exp": time.time() + 300})
            return self.reply(200, {"uploadId": upload, "url": f"{self.base()}/upload/{token}"})
        if parts == ["oauth2", "token"]:
            form = urllib.parse.parse_qs(body.decode())
            grant = (form.get("grant_type") or [""])[0]
            if grant == "authorization_code":
                code = (form.get("code") or [""])[0]
                verifier = (form.get("code_verifier") or [""])[0]
                with LOCK:
                    pending = CODES.pop(code, None)
                expected = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip("=")
                if not pending or pending["challenge"] != expected or \
                        (form.get("redirect_uri") or [""])[0] != pending["redirect_uri"]:
                    return self.reply(400, {"error": "invalid_grant"})
                who = "tester"
            elif grant == "refresh_token":
                who = REFRESH.pop((form.get("refresh_token") or [""])[0], None)
                if not who:
                    return self.reply(400, {"error": "invalid_grant"})
            else:
                return self.reply(400, {"error": "unsupported_grant_type"})
            access, refresh = secrets.token_urlsafe(24), secrets.token_urlsafe(24)
            with LOCK:
                TOKENS[access] = who
                REFRESH[refresh] = who
            return self.reply(200, {"access_token": access, "refresh_token": refresh, "token_type": "Bearer",
                                    "expires_in": 3600})
        return self.reply(404, {"error": "not found"})

    def do_PUT(self):
        self.log()
        parts = urllib.parse.urlsplit(self.path).path.split("/")[1:]
        if parts[:1] == ["upload"] and len(parts) == 2:
            payload = unsign(parts[1])
            if not payload:
                return self.reply(403, {"error": "expired or invalid"})
            data = self.body()
            with LOCK:
                u = UPLOADS.get(payload["upload"])
            if len(data) > 25 * 1024 * 1024:
                return self.reply(413, {"error": "too big"})
            try:
                m = manifest_of(data)
                if (m["id"], m["version"]) != (u["id"], u["version"]):
                    raise ValueError("the pack isn't the one announced")
                publish(data, u["publisher"])
                u["status"] = "published"
            except Exception as e:  # the service never trusts the upload
                u["status"] = "rejected"
                u["reasons"] = [str(e)]
            return self.reply(200, b"", "text/plain")
        return self.reply(404, {"error": "not found"})


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--seed")
    parser.add_argument("--log")
    parser.add_argument("--corrupt", action="store_true")
    ARGS = parser.parse_args()
    if ARGS.seed:
        for name in sorted(os.listdir(ARGS.seed)):
            if name.endswith((".zip", ".tar.gz", ".tgz")):
                with open(os.path.join(ARGS.seed, name), "rb") as f:
                    publish(f.read())
    server = ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler)
    print(f"listening on http://127.0.0.1:{server.server_address[1]}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
