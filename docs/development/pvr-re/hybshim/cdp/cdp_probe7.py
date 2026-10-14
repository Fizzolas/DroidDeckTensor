# CDP observer v7.1: same-run X11 + web correlation for CEF/Steam. Read-only.
# - HTTP lifecycle diff (/json/version + /json/list); classifies page targets: controller (SharedJSContext) vs BP.
# - Attaches to the BP page (the second page, not SharedJSContext) with retry (handshake-closed races).
# - Installs ADDITIVE listeners (visibilitychange/focus/blur/pagehide/pageshow/resize) into window.__v7,
#   reads document.hidden/visibilityState/hasFocus + geometry + frame tree + DOM/login-content markers
#   (querySelectorAll, never null-unsafe), holds bounded time, then reads back transitions.
# - On each successful BP attach, spawns /tmp/xinspect2 --events N (native Create/Map/Unmap/Reparent/
#   Configure/Property tail + separate active-window/input-focus observations) into the SAME log, so the
#   web observation and the native tail share one timeline. No app API calls, no reload/nav/input.
# - Every line carries epoch + CLOCK_MONOTONIC (xinspect2 uses the same clock; guest wall clock is faked).
import json, socket, base64, os, time, subprocess, urllib.request, sys, re

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 0
DEADLINE = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 70.0)

def stamp():
    return int(time.time()), round(time.clock_gettime(time.CLOCK_MONOTONIC), 2)
def p(*a):
    e, m = stamp(); print(e, "mono=%s" % m, *a, flush=True)
def san(u): return str(u).split("?")[0].split("#")[0]
def red(s): return re.sub(r"(token|password|cookie|sessionid|key|auth|code)=[^&\s\"'&]*", r"\1=<redacted>", str(s), flags=re.I)

def http(path, timeout=3):
    try: return json.load(urllib.request.urlopen("http://127.0.0.1:%d%s" % (PORT, path), timeout=timeout))
    except Exception: return None

class WS:
    def __init__(self, url, timeout=3):
        u = url.split("://", 1)[1]; hp, path = u.split("/", 1); host, port = hp.split(":")
        self.s = socket.create_connection((host, int(port)), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /%s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key)).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            c = self.s.recv(4096)
            if not c: raise RuntimeError("handshake-closed")
            buf += c
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
    def _exact(self, n, end):
        b = self.buf
        while len(b) < n:
            self.s.settimeout(max(0.1, end - time.time())); c = self.s.recv(65536)
            if not c: raise RuntimeError("closed")
            b += c
        self.buf = b[n:]; return b[:n]
    def send(self, obj):
        data = json.dumps(obj).encode(); hdr = bytearray([0x81]); n = len(data)
        if n < 126: hdr.append(0x80 | n)
        elif n < 65536: hdr.extend([0x80 | 126, n >> 8, n & 0xFF])
        else: hdr.extend([0x80 | 127] + [(n >> (8*i)) & 0xFF for i in range(7, -1, -1)])
        mask = os.urandom(4)
        self.s.sendall(bytes(hdr) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))
    def recv(self, end):
        while True:
            b = self._exact(2, end); ln = b[1] & 0x7F
            if ln == 126: ln = int.from_bytes(self._exact(2, end), "big")
            elif ln == 127: ln = int.from_bytes(self._exact(8, end), "big")
            if ln > 12_000_000: return None
            fr = self._exact(ln, end)
            try: return json.loads(fr.decode("utf-8", "replace"))
            except Exception: return None

_id = [0]
def call(ws, method, params=None, timeout=3):
    _id[0] += 1; mid = _id[0]
    try: ws.send({"id": mid, "method": method, "params": params or {}})
    except Exception: return ("ERR", method, "send-io")
    end = time.time() + timeout
    while time.time() < end:
        try: o = ws.recv(end)
        except Exception: return ("ERR", method, "io")
        if o is None: continue
        if o.get("id") == mid:
            if "error" in o: return ("ERR", method, o["error"].get("code"), str(o["error"].get("message", ""))[:60])
            return ("OK", o.get("result", {}))
    return ("ERR", method, "timeout")

INSTALL = ("window.__v7=window.__v7||[];"
  "['visibilitychange','focus','blur','pagehide','pageshow','resize'].forEach(function(t){"
  "addEventListener(t,function(){__v7.push([t,Date.now(),document.visibilityState,document.hidden,document.hasFocus(),innerWidth,innerHeight])},true);});"
  "'installed'")
READ = ("JSON.stringify({"
  "hidden:document.hidden,vis:document.visibilityState,focus:document.hasFocus(),"
  "iw:innerWidth,ih:innerHeight,ow:outerWidth,oh:outerHeight,dpr:devicePixelRatio,"
  "cw:document.documentElement.clientWidth,ch:document.documentElement.clientHeight,"
  "t:document.title,u:location.href.split('?')[0],rs:document.readyState,"
  "kids:document.body?document.body.childElementCount:-1,"
  "childTags:document.body?[...document.body.children].slice(0,10).map(function(e){return e.tagName+(e.id?'#'+e.id:'')}):[],"
  "marks:['login','password','qrcode','signin','remember'].map(function(s){"
  "return [s,document.querySelectorAll('[class*=\"'+s+'\"]').length]})"
  "})")
TXT = ("(function(){var d=document.body&&document.body.innerText||'';"
  "return JSON.stringify({len:d.length,head:d.replace(/\\s+/g,' ').slice(0,200)})})()")
EVS = "JSON.stringify(window.__v7||[])"

_last_ev = [0.0]
def spawn_events(secs=25):
    now = time.monotonic()
    if now - _last_ev[0] < 30: return "skip"
    _last_ev[0] = now
    try:
        subprocess.Popen(["/tmp/xinspect2", "--events", str(secs)])
        p("EV-SPAWN", "xinspect2 --events %d" % secs); return "ok"
    except Exception as e:
        p("EV-SPAWN-FAIL", repr(e)[:60]); return "fail"

def describe(t):
    # LIST-time facts (run 27 lesson: title can be EMPTY on the first list; the controller then
    # must be told apart by URL): controller url = steamloopback.host; BP url = about:blank?...
    url = t.get("url", ""); title = str(t.get("title", ""))
    if "SharedJSContext" in title: return "controller"
    if url.startswith("about:blank") or "Gamepad" in title or "Big Picture" in title: return "BP"
    if "steamloopback.host" in url: return "controller"
    return "BP"   # anything else non-controller: give it the long hold + native tail

def tid_of(t):
    ws = t.get("webSocketDebuggerUrl") or ""
    return t.get("targetId") or t.get("id") or (ws.rsplit("/", 1)[-1] if ws else "?")

def inspect(t, hold=6.0):
    wsid = t.get("webSocketDebuggerUrl"); tid = tid_of(t)[:10]
    kind = describe(t)
    if not wsid: p("attach-skip", kind, tid, "no-ws-url"); return None
    s = None; tend = time.time() + 4.0 if kind == "BP" else time.time() + 3.0
    while time.time() < tend:
        try: s = WS(wsid); break
        except Exception: time.sleep(0.15)
    if s is None: p("attach-fail", kind, tid); return None
    p("attach-ok", kind, tid, str(t.get("title", ""))[:34], san(t.get("url", ""))[:56])
    if kind == "BP": spawn_events(25)
    call(s, "Runtime.enable", timeout=2)
    call(s, "Runtime.evaluate", {"expression": INSTALL, "returnByValue": True}, timeout=2)
    ft = call(s, "Page.enable", timeout=2)
    if ft[0] == "OK":
        tr = call(s, "Page.getFrameTree", timeout=2)
        if tr[0] == "OK":
            f = (tr[1].get("frameTree") or {}).get("frame") or {}
            p("FRAME", kind, "id=%s url=%s" % (str(f.get("id", ""))[:10], san(f.get("url", ""))[:40]))
    r0 = call(s, "Runtime.evaluate", {"expression": READ, "returnByValue": True}, timeout=3)
    r1 = call(s, "Runtime.evaluate", {"expression": TXT, "returnByValue": True}, timeout=3)
    if r0[0] == "OK": p("V7", kind, red(str(r0[1].get("result", {}).get("value"))[:560]))
    else: p("V7-fail", kind, r0[1:])
    if r1[0] == "OK": p("V7TXT", kind, red(str(r1[1].get("result", {}).get("value"))[:260]))
    # Multi-read (STEP 3): native BP death lands ~0.4-0.9 s after activation (runs 23/27), so read
    # visibility repeatedly on a fast cadence instead of one delayed read. V7M-fail lines are
    # informative: an io error here = target/generation died between reads (NOT "still hidden").
    t0 = time.time()
    next_read = t0 + 0.35
    end = min(t0 + hold, DEADLINE)
    n = 0
    while time.time() < end:
        if time.time() >= next_read:
            n += 1
            rm = call(s, "Runtime.evaluate", {"expression": READ, "returnByValue": True}, timeout=1.5)
            if rm[0] == "OK": p("V7M%d" % n, kind, red(str(rm[1].get("result", {}).get("value"))[:300]))
            else:
                p("V7M%d-fail" % n, kind, str(rm[1:])[:60])
                if str(rm[1:]).startswith("('io'") or "'io'" in str(rm[1:]): break
            next_read = time.time() + 0.35
        else:
            time.sleep(0.1)
    r = call(s, "Runtime.evaluate", {"expression": EVS, "returnByValue": True}, timeout=3)
    if r[0] == "OK": p("V7EV", kind, red(str(r[1].get("result", {}).get("value"))[:400]))
    else: p("V7EV-fail", kind, str(r[1:])[:60])
    r = call(s, "Runtime.evaluate", {"expression": READ, "returnByValue": True}, timeout=3)
    if r[0] == "OK": p("V7B", kind, red(str(r[1].get("result", {}).get("value"))[:300]))
    else: p("V7B-fail", kind, str(r[1:])[:60])
    try: s.s.close()
    except Exception: pass
    return kind

def main():
    known = {}; up = False
    while time.time() < DEADLINE:
        ver = http("/json/version", 2); lst = http("/json/list", 3) if ver else None
        now_up = bool(ver)
        if now_up and not up: p("CDP-UP", (ver or {}).get("Browser", "")[:24])
        if not now_up and up: p("CDP-DOWN")
        up = now_up
        if now_up and isinstance(lst, list):
            pages = [t for t in lst if t.get("type") == "page"]
            kinds = sorted(describe(t) for t in pages)
            cur = set(tid_of(t) for t in pages)
            for tid in list(known):
                if tid not in cur: p("T-GONE", tid[:10], known[tid][0]); known.pop(tid)
            p("LIST", "pages=%d" % len(pages), ",".join(kinds))
            for t in pages:
                tid = tid_of(t)
                if tid not in known:
                    kind = describe(t)
                    known[tid] = (kind, t)
                    # v6's guard, restored: an io exception inside inspect must not kill the whole
                    # observer (sessions 22/23/25 lost all later rounds to an unguarded send).
                    try: inspect(t, hold=(6.0 if kind == "BP" else 1.5))
                    except Exception as e: p("inspect-exc", kind, tid, repr(e)[:70])
        time.sleep(0.5)
    p("done")

if __name__ == "__main__": main()
