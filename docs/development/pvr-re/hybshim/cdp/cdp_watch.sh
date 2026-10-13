#!/bin/sh
# Read-only CDP observer: one-line compact outputs. On first endpoint-UP, fire the one-shot page
# inspector (Stage B) once. HTTP + bounded; no reloads, no focus, no resize.
port="$1"
now=$(date +%s 2>/dev/null)
v=$(curl -s --max-time 2 "http://127.0.0.1:$port/json/version" 2>/dev/null | tr -d '\n\r' | cut -c1-160)
l=$(curl -s --max-time 3 "http://127.0.0.1:$port/json/list" 2>/dev/null | tr -d '\n\r')
if [ -z "$v" ]; then echo "CDP $now endpoint=DOWN"; exit 0; fi
echo "CDP $now endpoint=UP ver=$v"
if [ -n "$l" ] && command -v python3 >/dev/null 2>&1; then
  echo "$l" | python3 -c '
import json,sys
try: ts=json.loads(sys.stdin.read())
except Exception as e: print("  TP parse-fail",e); sys.exit()
print("  TP count=%d" % len(ts))
for t in ts:
    u=str(t.get("url","")); u=u.split("?")[0].split("#")[0]
    print("  TP %s|%s|%.60s|%.110s|win=%s,%s xy=%s,%s w=%s h=%s" % (str(t.get("id"))[:10], t.get("type"), t.get("title"), u, t.get("windowId"), t.get("type"), t.get("x"), t.get("y"), t.get("width"), t.get("height")))
' 2>/dev/null
fi
# one-shot page inspection when UP (marker prevents repeats; browser lifetime is ~7-10 s)
if [ ! -e /tmp/cdp_done ] && command -v timeout >/dev/null 2>&1; then
  : > /tmp/cdp_done
  timeout 20 python3 /tmp/cdp_page.py "$port" 2>&1 | head -30
fi
