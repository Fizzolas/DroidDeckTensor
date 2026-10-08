import os
import re, sys
from pathlib import Path
# PVR_VENDOR holds srv_um.dis and syms.txt; bridge_table.csv is written next to this script.
VENDOR = Path(os.environ.get("PVR_VENDOR", Path(__file__).resolve().parent / "vendor"))
dis_lines = (VENDOR / "srv_um.dis").read_text(errors="replace").splitlines()
insns = {}  # addr -> text
pat = re.compile(r"^\s*([0-9a-f]+):\s*\t(.*)$")
for line in dis_lines:
    m = pat.match(line)
    if m:
        insns[int(m.group(1), 16)] = m.group(2).strip()
addrs = sorted(insns)
syms = []
for line in (VENDOR / "syms.txt").read_text().splitlines():
    a, name = line.split(maxsplit=1)
    syms.append((int(a, 16), name))
syms.sort()
bridge_call = 0x37a70  # PVRSRVBridgeCall in libsrv_um.so
imm = re.compile(r"#(0x[0-9a-f]+|\d+)")
def val(text):
    if re.search(r",\s*wzr\s*$", text):
        return 0
    m = imm.search(text)
    return int(m.group(1), 0) if m else None
rows = []
for idx, (start, name) in enumerate(syms):
    if not name.startswith("Bridge") or "BridgeCall" in name or "Bridge" == name:
        continue
    end = syms[idx + 1][0] if idx + 1 < len(syms) else start + 0x2000
    w1 = w2 = None
    found = None
    for a in addrs:
        if a < start or a >= end:
            continue
        t = insns[a]
        if re.match(r"(mov|movz)\s+w1,", t): w1 = val(t)
        elif re.match(r"(mov|movz)\s+w2,", t): w2 = val(t)
        elif re.match(r"movk\s+w1,", t) and w1 is not None: w1 |= (val(t) << 16)
        elif re.match(r"movk\s+w2,", t) and w2 is not None: w2 |= (val(t) << 16)
        if t.startswith("bl") and f"<PVRSRVBridgeCall>" in t:
            found = (w1, w2)
            break
    rows.append((name, found))
out = Path(__file__).resolve().parent / "bridge_table.csv"
with out.open("w") as f:
    f.write("symbol,bridge_id,function_id\n")
    for name, found in rows:
        if found is None:
            f.write(f"{name},,\n")
        else:
            b, fn = found
            f.write(f"{name},{'' if b is None else b},{'' if fn is None else fn}\n")
mapped = sum(1 for _, r in rows if r is not None and r[0] is not None and r[1] is not None)
print(f"bridge symbols: {len(rows)}; with bridge+func ids: {mapped}; wrote {out}")
