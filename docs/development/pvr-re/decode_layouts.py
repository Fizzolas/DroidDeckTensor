"""Derive input/output struct field maps for PVR bridge wrappers in libsrv_um.so.

Each Bridge<Name> wrapper builds its input struct on the stack, calls
PVRSRVBridgeCall(w1=bridge, w2=func, x3=in, w4=in_size, x5=out, w6=out_size),
then copies fields out of the output buffer. This script emulates the
straight-line stack traffic around that call (no branch following) and
reports every store into the input buffer and every load from the output
buffer. Results are checked against the live capture in bringup_layouts.csv.
"""
import csv
import re
from pathlib import Path

import os

ROOT = Path(__file__).resolve().parent
# Directory holding srv_um.dis and syms.txt generated from the device's own libsrv_um.so.
# The vendor binaries are not part of the repo; set PVR_VENDOR to point at them.
VENDOR = Path(os.environ.get("PVR_VENDOR", ROOT / "vendor"))
BRIDGE_CALL = 0x37A70
# Bring-up (device open, buffer alloc, connect) and the submit/sync run (buffer fill + TDM submit + fence wait).
CAPTURES = ["bringup_layouts.csv", "submit_layouts.csv"]

LINE = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
CALL = re.compile(r"bl\t0x([0-9a-f]+) <PVRSRVBridgeCall>")
MEM = re.compile(r"\[(\w+)(?:,\s*#(-?0x[0-9a-f]+|-?\d+))?\]")
IMM = re.compile(r"#(-?0x[0-9a-f]+|-?\d+)")


def load_insns():
    insns = {}
    for line in (VENDOR / "srv_um.dis").read_text(errors="replace").splitlines():
        m = LINE.match(line)
        if m:
            ops = [o.strip() for o in m.group(3).split("//")[0].split(",")] if m.group(3) else []
            insns[int(m.group(1), 16)] = (m.group(2), [o for o in ops if o])
    return insns


def load_syms():
    syms = []
    for line in (VENDOR / "syms.txt").read_text().splitlines():
        a, name = line.split(maxsplit=1)
        syms.append((int(a, 16), name.strip()))
    syms.sort()
    return syms


def describe(val):
    kind = val[0]
    if kind == "arg":
        return f"incoming arg reg {val[1]}"
    if kind == "stk":
        return f"caller stack +{val[1]:#x}"
    if kind == "const":
        return f"const {val[1]}"
    if kind == "out":
        return f"out+{val[1]}"
    return kind


def sample(hex_text, off, width):
    raw = bytes.fromhex(hex_text) if hex_text else b""
    if off + width > len(raw):
        return "n/a"
    return hex(int.from_bytes(raw[off:off + width], "little"))


def regnum(name):
    return int(name[1:]) if name[0] in "wx" and name[1:].isdigit() else None


def is_x(name):
    return name.startswith("x")


def imm(text):
    return int(text.lstrip("#"), 0)


class Ctx:
    """Symbolic register file: value kinds are ('const', n), ('sp', off), ('arg', n), ('stk', off), ('out', off)."""

    def __init__(self):
        self.reg = {n: ("arg", n) for n in range(31)}

    def get(self, name):
        if name in ("wzr", "xzr"):
            return ("const", 0)
        if name == "sp":
            return ("sp", 0)
        n = regnum(name)
        return self.reg.get(n, ("?",))

    def set(self, name, val):
        n = regnum(name)
        if n is not None:
            self.reg[n] = val


def addr_of(ctx, mem_text):
    m = MEM.search(mem_text)
    base, off = m.group(1), (imm(m.group(2)) if m.group(2) else 0)
    b = ctx.get(base)
    return b, off


def widths(mn, ops):
    """Return list of (width, src_reg_name) for a store instruction."""
    if mn in ("strb", "sturb"):
        return [(1, ops[0])]
    if mn in ("strh", "sturh"):
        return [(2, ops[0])]
    if mn in ("str", "stur"):
        return [(8 if is_x(ops[0]) else 4, ops[0])]
    if mn == "stp":
        w = 8 if is_x(ops[0]) else 4
        return [(w, ops[0]), (w, ops[1])]
    return []


def analyse(start, end, insns, bridge_hint=None):
    ctx = Ctx()
    addrs = sorted(a for a in insns if start <= a < end)
    call_at = None
    call_info = {}
    stores = []   # (abs_off, width, src_value)
    pre_loads = []
    out_src = {}  # reg -> ('out', off)
    for a in addrs:
        mn, ops = insns[a]
        if mn == "bl" and CALL.search(f"{mn}\t{ops[0]}" if ops else mn):
            call_at = a
            call_info = {
                "bridge": ctx.get("w1"),
                "func": ctx.get("w2"),
                "in_base": ctx.get("x3"),
                "in_size": ctx.get("w4"),
                "out_base": ctx.get("x5"),
                "out_size": ctx.get("w6"),
            }
            break
        if mn in ("mov", "movz") and len(ops) == 2:
            ctx.set(ops[0], ctx.get(ops[1]) if not ops[1].startswith("#") else ("const", imm(ops[1])))
        elif mn in ("add", "sub") and len(ops) == 3:
            base = ctx.get(ops[1])
            if ops[2].startswith("#") and base[0] == "sp":
                k = imm(ops[2])
                ctx.set(ops[0], ("sp", base[1] + (k if mn == "add" else -k)))
            elif ops[2].startswith("#") and base[0] == "const":
                ctx.set(ops[0], ("const", base[1] + (imm(ops[2]) if mn == "add" else -imm(ops[2]))))
        elif mn.startswith("str") or mn.startswith("stp") or mn.startswith("stur") or mn.startswith("sturb"):
            # memory operand is the first bracketed operand
            mem_idx = next(i for i, o in enumerate(ops) if "[" in o)
            base, off = addr_of(ctx, ",".join(ops[mem_idx:]))
            src_ops = ops[:mem_idx]
            if base[0] == "sp":
                pos = base[1] + off
                w = 0
                for width, src in zip([x[0] for x in widths(mn, src_ops)], src_ops):
                    stores.append((pos + w, width, ctx.get(src), src))
                    w += width
            elif base[0] == "stk" or base[0] == "arg":
                # store through a pointer held in a register (caller output pointer)
                for src in src_ops:
                    val = ctx.get(src)
                    if val[0] == "out":
                        out_src.setdefault(("dest", base, off), []).append(val)
        elif mn.startswith("ldr") or mn.startswith("ldur") or mn.startswith("ldp"):
            mem_idx = next((i for i, o in enumerate(ops) if "[" in o), None)
            if mem_idx is None:
                continue
            base, off = addr_of(ctx, ",".join(ops[mem_idx:]))
            dst = ops[:mem_idx]
            if "x29" in ops[mem_idx] and off >= 0:
                # x29 is the frame pointer: non-negative offsets address the caller's stack arguments
                for d in dst:
                    ctx.set(d, ("stk", off))
            elif base[0] == "sp":
                pre_loads.append((base[1] + off, a, dst))
                for d in dst:
                    ctx.set(d, ("mem", base[1] + off))
            elif base[0] == "sp" or base[0] == "arg":
                for d in dst:
                    ctx.set(d, ("stk", off))
            else:
                for d in dst:
                    ctx.set(d, ("?",))
        elif mn in ("cbz", "cbnz", "b", "b.ne", "b.eq", "tbz", "tbnz", "ret", "retaa", "adr", "adrp", "nop"):
            if mn in ("adr", "adrp") and ops:
                ctx.set(ops[0], ("?",))
            if mn == "ret" or mn == "retaa":
                break
        else:
            # any other writer clobbers its destination
            if ops and regnum(ops[0]) is not None:
                ctx.set(ops[0], ("?",))
    # Output-side pass: from call site onward, walk loads from out buffer.
    out_fields = []
    if call_at is not None:
        ctx2 = Ctx()
        post = [a for a in addrs if a > call_at]
        base_out = call_info["out_base"]
        out_sp = base_out[1] if base_out[0] == "sp" else None
        out_regs = {}
        for a in post:
            mn, ops = insns[a]
            if mn == "ret" or mn == "retaa":
                break
            if mn.startswith("ldr") and any("[" in o for o in ops):
                mem_idx = next(i for i, o in enumerate(ops) if "[" in o)
                base, off = addr_of(ctx2, ",".join(ops[mem_idx:]))
                if base[0] == "sp" and out_sp is not None:
                    rel = base[1] + off - out_sp
                    width = 8 if is_x(ops[0]) else (1 if mn == "ldrb" else 4)
                    for d in ops[:mem_idx]:
                        out_regs[regnum(d)] = (rel, width)
                        ctx2.set(d, ("out", rel))
                    continue
                for d in ops[:mem_idx]:
                    ctx2.set(d, ("?",))
            if mn.startswith("str") and any("[" in o for o in ops):
                src = ops[0]
                n = regnum(src)
                if n in out_regs:
                    rel, width = out_regs[n]
                    out_fields.append((rel, width, "store-to-caller-pointer"))
                continue
            if mn == "mov" and len(ops) == 2 and regnum(ops[1]) in out_regs:
                ctx2.set(ops[0], ("out", out_regs[regnum(ops[1])][0]))
                out_regs[regnum(ops[0])] = out_regs[regnum(ops[1])]
    return call_info, stores, out_fields


def main():
    insns = load_insns()
    syms = load_syms()
    rows = []
    cap = {}
    # Each capture CSV contributes its calls; a symbol seen in both keeps the first sample.
    for capture in CAPTURES:
        with (ROOT / capture).open() as f:
            for r in csv.DictReader(f):
                cap.setdefault(r["symbol"], r)
    names = [n for _, n in syms]
    for i, (start, name) in enumerate(syms):
        if name not in cap:
            continue
        end = syms[i + 1][0] if i + 1 < len(syms) else start + 0x400
        info, stores, outs = analyse(start, end, insns)
        c = cap[name]
        if "in_base" not in info:
            print("NO CALL SITE:", name)
            continue
        base = info["in_base"][1] if info["in_base"][0] == "sp" else None
        size = info["in_size"][1] if info["in_size"][0] == "const" else 0
        in_fields = sorted({(off - base, w, describe(v)) for off, w, v, _ in stores
                            if base is not None and base <= off < base + size})
        rows.append({
            "symbol": name,
            "bridge": info.get("bridge"),
            "func": info.get("func"),
            "in_size_code": info.get("in_size"),
            "in_size_captured": int(c["in_size"]),
            "out_size_code": info.get("out_size"),
            "out_size_captured": int(c["out_size"]),
            "in_hex": c["in_hex"],
            "out_hex": c["out_hex"],
            "in_fields": in_fields,
            "out_fields": sorted(set(outs)),
        })
    for r in rows:
        print(r["symbol"], r["bridge"][1], r["func"][1], "in", r["in_size_code"][1], r["in_size_captured"],
              "out", r["out_size_code"][1], r["out_size_captured"])
        for f in r["in_fields"]:
            print("   in  ", f, "sample=", sample(r["in_hex"], f[0], f[1]))
        for f in r["out_fields"]:
            print("   out ", f, "sample=", sample(r["out_hex"], f[0], f[1]))
    failures = []
    for r in rows:
        c = cap[r["symbol"]]
        if (r["bridge"][1], r["func"][1]) != (int(c["bridge"]), int(c["func"])):
            failures.append((r["symbol"], "bridge/func"))
        if r["in_size_code"][1] != int(c["in_size"]):
            failures.append((r["symbol"], "in size"))
        if r["out_size_code"][1] != int(c["out_size"]):
            failures.append((r["symbol"], "out size"))
    print("checked", len(rows), "functions; failures:", failures or "none")
    write_reference(rows)
    return rows


HEADER = """# Bring-up bridge field map (libsrv_um.so, Pixel 10 Pro XL)

Generated by `decode_layouts.py` from `vendor/srv_um.dis` and checked against
`bringup_layouts.csv` (captured on device, every call returned 0). Regenerate with
`python decode_layouts.py`.

## How to read this

- **Offset / width** are byte positions inside the input or output buffer. Widths come from the
  store (`str`/`stp`/`strb`/`strh`) or load instruction that touches that slot.
- **Origin** says where the stored value came from in the wrapper:
  - `incoming arg reg N`: the wrapper's own argument register `xN`/`wN` at entry (C argument
    position N-1 with `x0` as the first argument). Which C parameter that is needs the upstream
    prototype, which this build does not ship.
  - `caller stack +0xK`: a stack argument of the wrapper, at `x29+K`.
  - `out+K` / `store-to-caller-pointer`: the value is read from the output buffer and written through
    a caller-supplied pointer.
- **sample** is the little-endian value the live capture carried at that offset.
- The wrapper's return path reads `[out+8]` as the PVRSRV error code (`ldr w0, [sp, #8]` after the
  call, with the output buffer based at `sp`). In `BridgeConnect` the captured `out+8` is zero.

## Verification

For every function the script checks that the wrapper's `bl PVRSRVBridgeCall` site
has the bridge/function IDs in `bringup_layouts.csv`, and that the input and output
sizes it encodes match the captured ones. All 34 pairs match: 33 from the bring-up run
(`bringup_layouts.csv`) and `BridgeRGXTDMSubmitTransfer2` from the buffer-fill run
(`submit_layouts.csv`), which is the only bridge call the fill submission added. Run output is in
`decode_out.txt`.

Submission cross-check: the `CreateTransferContext` output at out+0 is `0x29`, and the
`SubmitTransfer2` input at offset 8 carries `0x29`, so the field is the transfer context handle.

To regenerate outside this repo, point `PVR_VENDOR` at a directory holding `srv_um.dis`
(`llvm-objdump -d` of the device's `libsrv_um.so`) and `syms.txt`. The vendor binaries are not checked in.

Field *meanings* ("page size", "heap base") are not established here. These offsets
and widths are the layout that a backend must write. Names need the upstream PowerVR
DDK headers or a kernel-side read to confirm.
"""


def write_reference(rows):
    lines = [HEADER]
    for r in rows:
        lines.append(f"\n## {r['symbol']} (bridge {r['bridge'][1]}, function {r['func'][1]})\n")
        lines.append(f"Input {r['in_size_code'][1]} bytes (captured {r['in_size_captured']}), "
                     f"output {r['out_size_code'][1]} bytes (captured {r['out_size_captured']}).\n")
        if r["in_fields"]:
            lines.append("| in offset | width | origin | sample |\n|---|---|---|---|")
            for off, w, origin in r["in_fields"]:
                lines.append(f"| {off} | {w} | {origin} | {sample(r['in_hex'], off, w)} |")
            lines.append("")
        if r["out_fields"]:
            lines.append("| out offset | width | destination | sample |\n|---|---|---|---|")
            for off, w, dest in sorted(set(r["out_fields"])):
                lines.append(f"| {off} | {w} | {dest} | {sample(r['out_hex'], off, w)} |")
            lines.append("")
    (ROOT / "bridge_field_map.md").write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
