#!/usr/bin/env python3
# CDP observer v4: HTTP-only lifecycle diffing (no browser-ws autoattach - it collides with manual
# page attach in CEF). Attach to every page/other target with a ws URL; read-only: frame tree,
# geometry+resize recorder, SteamClient / BrowserAndBackstackInstances bridge snapshot, console &
# exceptions during a bounded hold. No navigation/reload/resize/input; stdlib only.
import json, socket, base64, os, time, urllib.request, sys, re

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 0
DEADLINE = time.time() + (float(sys.argv[2]) if len(sys.argv) > 2 else 70.0)
def p(*a): print(int(time.time()), *a, flush=True)
def san(u): return str(u).split("?")[0].split("#")[0]
def red(s): return re.sub(r"(token|password|cookie|sessionid|key|auth)=[^&\s\"']*", r"\1=<redacted>", str(s), flags=re.I)
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
    ws.send({"id": mid, "method": method, "params": params or {}})
    end = time.time() + timeout
    while time.time() < end:
        try: o = ws.recv(end)
        except Exception: return ("ERR", method, "io")
        if o is None: continue
        if o.get("id") == mid:
            if "error" in o: return ("ERR", method, o["error"].get("code"), str(o["error"].get("message", ""))[:60])
            return ("OK", o.get("result", {}))
        if o.get("method") == "Runtime.consoleAPICalled":
            pr = o.get("params", {})
            txt = " ".join(str(a.get("value", a.get("description", ""))) for a in (pr.get("args") or [])[:4])
            p("CONSOLE", pr.get("type"), red(txt.replace("\n", " | "))[:180])
        elif o.get("method") == "Runtime.exceptionThrown":
            d = o.get("params", {}).get("exceptionDetails", {})
            p("EXC", (d.get("exception", {}).get("description", "") or d.get("text", ""))[:180].replace("\n", " | "))
    return ("ERR", method, "timeout")

GEO = ("JSON.stringify({iw:innerWidth,ih:innerHeight,ow:outerWidth,oh:outerHeight,"
  "cw:document.documentElement.clientWidth,ch:document.documentElement.clientHeight,"
  "sw:screen.width,sh:screen.height,dpr:devicePixelRatio,t:document.title,"
  "u:location.href.split('?')[0],r:document.readyState,v:document.visibilityState,"
  "kids:document.body?document.body.childElementCount:-1,"
  "win:typeof window!=='undefined'&&window===window.top})")
BRIDGE = ("(function(){try{var o={};var bbi=window.BrowserAndBackstackInstances;"
  "if(bbi){o.bbi=Object.keys(bbi).slice(0,12).map(function(k){var v=bbi[k];"
  "if(v&&typeof v==='object')return [k,Object.keys(v).slice(0,10)];return [k,String(v).slice(0,40)]});}"
  "var sc=window.SteamClient;if(sc){o.sc=Object.keys(sc).slice(0,40);}"
  "if(window.SharedJSContext)o.sjc=Object.keys(window.SharedJSContext).slice(0,20);"
  "if(window.JSAccessor)o.jsa=Object.keys(window.JSAccessor).slice(0,30);"
  "return JSON.stringify(o);}catch(e){return 'err:'+e}})()")

def inspect(t):
    wsid = t.get("webSocketDebuggerUrl"); tid = t.get("targetId", "?")[:10]
    ty = t.get("type")
    if not wsid: p("T-skip", tid, ty, "(no ws url)"); return
    s=None
    for k in range(6):
        try: s = WS(wsid); break
        except Exception as e:
            if k==5: p("attach-fail", tid, ty, repr(e)[:50]); return
            time.sleep(0.4)
    p("attach-ok", tid, ty, str(t.get("title", ""))[:30], san(t.get("url", ""))[:50])
    call(s, "Runtime.enable", timeout=2)
    r = call(s, "Runtime.evaluate", {"expression": "window.__rr=[];addEventListener('resize',()=>__rr.push([innerWidth,innerHeight]))", "returnByValue": True}, timeout=2)
    r = call(s, "Runtime.evaluate", {"expression": GEO, "returnByValue": True}, timeout=3)
    if r[0] == "OK": p("GEO", tid, red(str(r[1].get("result", {}).get("value"))[:300].replace('"', "'")))
    r = call(s, "Runtime.evaluate", {"expression": BRIDGE, "returnByValue": True}, timeout=3)
    if r[0] == "OK": p("BRIDGE", tid, red(str(r[1].get("result", {}).get("value"))[:600]))
    end = min(time.time() + 2.2, DEADLINE)
    while time.time() < end: time.sleep(1)
    r = call(s, "Runtime.evaluate", {"expression": "JSON.stringify(window.__rr||[])", "returnByValue": True}, timeout=2)
    if r[0] == "OK": p("RESIZE", tid, str(r[1].get("result", {}).get("value"))[:120])
    try: s.s.close()
    except Exception: pass

def main():
    known = {}; up = False
    while time.time() < DEADLINE:
        ver = http("/json/version", 2); lst = http("/json/list", 3) if ver else None
        now_up = bool(ver)
        if now_up and not up: p("CDP-UP", (ver or {}).get("Browser", "")[:24])
        if not now_up and up: p("CDP-DOWN")
        up = now_up
        if now_up:
            if lst and not known:
                for t0 in (lst if isinstance(lst, list) else []):
                    tt0 = dict(t0)
                    tt0.pop("webSocketDebuggerUrl", None); tt0.pop("faviconUrl", None)
                    tt0["url"] = san(tt0.get("url",""))
                    p("RAWLIST", red(json.dumps(tt0))[:520])
            cur = {}
            for t in (lst if isinstance(lst, list) else []):
                tid = t.get("targetId", "?"); cur[tid] = 1
                if tid not in known:
                    known[tid] = t
                    p("T+NEW", tid[:10], t.get("type"), str(t.get("title", ""))[:40], san(t.get("url", ""))[:70])
                    if t.get("type") in ("page", "other", "background_page", "webview", "app"):
                        try: inspect(t)
                        except Exception as e: p("inspect-exc", tid[:10], repr(e)[:60])
            for tid in list(known):
                if tid not in cur: p("T-GONE", tid[:10]); known.pop(tid)
        time.sleep(0.7)
    p("done")

if __name__ == "__main__": main()
