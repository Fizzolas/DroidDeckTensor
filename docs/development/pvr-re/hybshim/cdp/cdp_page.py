#!/usr/bin/env python3
# Read-only-ish CDP page inspector (Stage B): attaches to page targets via CDP websocket (stdlib
# only), reads document state + exceptions, and takes ONE bounded screenshot of a non-bootstrap
# page target if present. No navigation, no script injection, no reloads, no resize.
import json, socket, base64, os, sys, time, urllib.request, hashlib

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 0
TS = int(time.time())
def log(*a): print("CDPP %d" % TS, *a, flush=True)

try:
    lst = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json/list" % PORT, timeout=3))
except Exception as e:
    log("endpoint-unreachable", repr(e)); sys.exit(0)

def ws_connect(url, timeout=4):
    # url ws://127.0.0.1:port/devtools/page/ID
    assert url.startswith("ws://")
    hostport, path = url[5:].split("/", 1)
    host, port = hostport.split(":")
    s = socket.create_connection((host, int(port)), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    req = ("GET /%s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key))
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        c = s.recv(4096)
        if not c: raise RuntimeError("ws handshake closed")
        buf += c
    return s

def ws_send(s, obj):
    data = json.dumps(obj).encode()
    hdr = bytearray([0x81]); n = len(data)
    if n < 126: hdr.append(0x80 | n)
    elif n < 65536: hdr.extend([0x80 | 126, n >> 8, n & 0xFF])
    else: hdr.extend([0x80 | 127] + [(n >> (8*i)) & 0xFF for i in range(7, -1, -1)])
    mask = os.urandom(4)
    s.sendall(bytes(hdr) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

def _recv_exact(s, n, deadline):
    b = b""
    while len(b) < n:
        r = deadline - time.time()
        if r <= 0: raise TimeoutError()
        s.settimeout(r)
        c = s.recv(n - len(b))
        if not c: raise RuntimeError("closed")
        b += c
    return b

def ws_recv(s, deadline):
    b = _recv_exact(s, 2, deadline)
    ln = b[1] & 0x7F
    if ln == 126: ln = int.from_bytes(_recv_exact(s, 2, deadline), "big")
    elif ln == 127: ln = int.from_bytes(_recv_exact(s, 8, deadline), "big")
    if ln > 8_000_000: raise RuntimeError("frame too big %d" % ln)
    return _recv_exact(s, ln, deadline)

def cdp(s, mid, method, params=None, wait=True, timeout=5):
    msg = {"id": mid, "method": method, "params": params or {}}
    ws_send(s, msg)
    if not wait: return None
    end = time.time() + timeout
    while time.time() < end:
        try: fr = ws_recv(s, end)
        except (TimeoutError, RuntimeError): return None
        try: o = json.loads(fr.decode("utf-8", "replace"))
        except Exception: continue
        if o.get("id") == mid: return o
        ev = o.get("method", "")
        if ev == "Runtime.exceptionThrown":
            d = o.get("params", {}).get("exceptionDetails", {})
            des = d.get("exception", {}).get("description", "") or d.get("text", "")
            log("EXC", (des[:200]).replace("\n", " | "))
        elif ev == "Log.entryAdded":
            e = o.get("params", {}).get("entry", {})
            if e.get("level") in ("error", "warning"):
                log("LOG", e.get("source"), str(e.get("text", ""))[:160].replace("\n", " | "))
    return None

m = 0
for t in lst:
    if t.get("type") != "page": continue
    title = t.get("title", "")
    url = str(t.get("url", "")).split("?")[0]
    wid = t.get("id")
    try:
        s = ws_connect(t["webSocketDebuggerUrl"]); m += 1
    except Exception as e:
        log("attach-fail", wid, repr(e)); continue
    cdp(s, m + 1000, "Runtime.enable", wait=True, timeout=4); m += 1
    cdp(s, m + 1000, "Log.enable", wait=False); m += 1
    r = cdp(s, m + 1000, "Runtime.evaluate",
            {"expression": "JSON.stringify([document.readyState, location.href.split('?')[0], "
             "document.visibilityState, window.innerWidth, window.innerHeight, devicePixelRatio, "
             "document.body?document.body.childElementCount:-1, document.title])",
             "returnByValue": True}, timeout=5); m += 1
    val = r.get("result", {}).get("result", {}).get("value") if r else None
    log("PAGE", wid, url[:60], "state=", str(val)[:220].replace('"', "'"))
    if True:  # bounded single screenshot of the (only) page target
        r = cdp(s, m + 1000, "Page.captureScreenshot", {"format": "jpeg", "quality": 40}, timeout=8); m += 1
        data = r.get("result", {}).get("data") if r else None
        if data:
            fn = "/root/.local/share/Steam/logs/cdp_shot_%d.jpg" % (TS % 100000)
            open(fn, "wb").write(base64.b64decode(data))
            log("SHOT", fn, len(data), "b64")
        else:
            log("SHOT-fail", wid)
    s.close()
log("done targets=%d pages=%d" % (len(lst), sum(1 for t in lst if t.get("type") == "page")))
