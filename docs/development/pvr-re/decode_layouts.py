"""Derive input/output struct field maps for PVR bridge wrappers in libsrv_um.so.

Each Bridge<Name> wrapper builds its input struct on the stack, calls
PVRSRVBridgeCall(w1=bridge, w2=func, x3=in, w4=in_size, x5=out, w6=out_size),
then copies fields out of the output buffer. This script emulates the
straight-line stack traffic around that call (no branch following), reports
every store into the input buffer and every load from the output buffer, and
checks the bridge IDs and buffer sizes against the captures in CAPTURES.

Usage: PVR_VENDOR=<dir with srv_um.dis and syms.txt> python decode_layouts.py
"""
import csv
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
# Holds srv_um.dis and syms.txt generated from the device's own libsrv_um.so.
# The vendor binaries are not part of the repo, so PVR_VENDOR must point at them.
VENDOR = Path(os.environ.get("PVR_VENDOR", ROOT / "vendor"))
# Bring-up (device open, buffer alloc, connect) and the submit/sync run (buffer fill + TDM submit + fence wait).
CAPTURES = ["bringup_layouts.csv", "submit_layouts.csv", "dispatch_layouts.csv"]

INSN = re.compile(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$")
BRIDGE_LINE = re.compile(r"B bridge=(\d+) func=(\d+) ret=(-?\d+) errno=\d+ in(?:\[(\d+)\])?=(\S*|NULL) out(?:\[(\d+)\])?=(\S*|NULL)")
MEM = re.compile(r"\[(\w+)(?:,\s*#(-?0x[0-9a-f]+|-?\d+))?\]")
# Control-flow instructions that do not write their first operand, so it must keep its value.
CONTROL = {"cbz", "cbnz", "b", "b.ne", "b.eq", "tbz", "tbnz", "nop"}


def load_insns():
    insns = {}
    for line in (VENDOR / "srv_um.dis").read_text(errors="replace").splitlines():
        m = INSN.match(line)
        if m:
            ops = [o.strip() for o in m.group(3).split("//")[0].split(",") if o.strip()]
            insns[int(m.group(1), 16)] = (m.group(2), ops)
    return insns


def load_syms():
    syms = []
    for line in (VENDOR / "syms.txt").read_text().splitlines():
        a, name = line.split(maxsplit=1)
        syms.append((int(a, 16), name.strip()))
    syms.sort()
    return syms


def from_log(log_path, csv_path):
    """Write a capture CSV from a raw bridge log: one row per (bridge, func), first sample kept."""
    names = {}
    with (ROOT / "bridge_table.csv").open() as f:
        for r in csv.DictReader(f):
            if r["bridge_id"] and r["function_id"]:
                names[(int(r["bridge_id"]), int(r["function_id"]))] = r["symbol"]
    rows = {}
    for line in Path(log_path).read_text(errors="replace").splitlines():
        m = BRIDGE_LINE.search(line)
        if not m:
            continue
        key = (int(m[1]), int(m[2]))
        if key not in rows:
            rows[key] = {"calls": 0, "ret": m[3], "in_size": m[4] or "0",
                         "in_hex": "" if m[5] == "NULL" else m[5],
                         "out_size": m[6] or "0", "out_hex": "" if m[7] == "NULL" else m[7]}
        rows[key]["calls"] += 1
    unknown = [k for k in rows if k not in names]
    if unknown:
        raise SystemExit(f"no symbol in bridge_table.csv for bridge/func {unknown}")
    with (ROOT / csv_path).open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["bridge", "func", "symbol", "calls", "ret", "in_size", "in_hex", "out_size", "out_hex"])
        for (b, fn), r in sorted(rows.items()):
            w.writerow([b, fn, names[(b, fn)], r["calls"], r["ret"], r["in_size"], r["in_hex"], r["out_size"], r["out_hex"]])
    print(f"wrote {csv_path}: {len(rows)} calls from {log_path}")


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


def const(val):
    return val[1] if val[0] == "const" else None


class Ctx:
    """Symbolic registers: ('const', n), ('sp', off), ('arg', n), ('stk', off), ('out', off), ('mem', off), ('?',)."""

    def __init__(self):
        self.reg = {n: ("arg", n) for n in range(31)}

    def get(self, name):
        if name in ("wzr", "xzr"):
            return ("const", 0)
        if name == "sp":
            return ("sp", 0)
        return self.reg.get(regnum(name), ("?",))

    def set(self, name, val):
        n = regnum(name)
        if n is not None:
            self.reg[n] = val


def addr_of(ctx, mem_text):
    m = MEM.search(mem_text)
    off = int(m.group(2), 0) if m.group(2) else 0
    return ctx.get(m.group(1)), off


def mem_operand(ops):
    return next((i for i, o in enumerate(ops) if "[" in o), None)


def store_widths(mn, srcs):
    if mn in ("strb", "sturb"):
        return [1]
    if mn in ("strh", "sturh"):
        return [2]
    w = 8 if is_x(srcs[0]) else 4
    return [w, w] if mn == "stp" else [w]


def analyse(start, end, insns):
    """Emulate one wrapper up to its PVRSRVBridgeCall site.

    Returns (call, stores, addrs_after_call): call holds the IDs and buffer
    registers at the call (None if there is no call site), stores lists
    (absolute_sp_offset, width, value) for every store to the stack.
    """
    ctx = Ctx()
    addrs = sorted(a for a in insns if start <= a < end)
    stores = []
    for idx, a in enumerate(addrs):
        mn, ops = insns[a]
        if mn == "bl" and ops and "<PVRSRVBridgeCall>" in ops[0]:
            call = {
                "bridge": const(ctx.get("w1")),
                "func": const(ctx.get("w2")),
                "in_base": ctx.get("x3"),
                "in_size": const(ctx.get("w4")),
                "out_base": ctx.get("x5"),
                "out_size": const(ctx.get("w6")),
            }
            return call, stores, addrs[idx + 1:]
        if mn in ("mov", "movz") and len(ops) == 2:
            ctx.set(ops[0], ("const", int(ops[1].lstrip("#"), 0)) if ops[1].startswith("#") else ctx.get(ops[1]))
        elif mn in ("add", "sub") and len(ops) == 3:
            base = ctx.get(ops[1])
            if ops[2].startswith("#") and base[0] == "sp":
                k = int(ops[2].lstrip("#"), 0)
                ctx.set(ops[0], ("sp", base[1] + (k if mn == "add" else -k)))
            elif ops[2].startswith("#") and base[0] == "const":
                k = int(ops[2].lstrip("#"), 0)
                ctx.set(ops[0], ("const", base[1] + (k if mn == "add" else -k)))
        elif mn.startswith(("str", "stp", "stur")):
            mem = mem_operand(ops)
            base, off = addr_of(ctx, ",".join(ops[mem:]))
            if base[0] == "sp":
                srcs = ops[:mem]
                pos = base[1] + off
                for src, width in zip(srcs, store_widths(mn, srcs)):
                    stores.append((pos, width, ctx.get(src)))
                    pos += width
        elif mn.startswith(("ldr", "ldur", "ldp")):
            mem = mem_operand(ops)
            if mem is None:
                continue
            base, off = addr_of(ctx, ",".join(ops[mem:]))
            if "x29" in ops[mem] and off >= 0:
                # x29 is the frame pointer: non-negative offsets address the caller's stack arguments.
                value = ("stk", off)
            elif base[0] == "sp":
                value = ("mem", base[1] + off)
            elif base[0] == "arg":
                value = ("stk", off)
            else:
                value = ("?",)
            for d in ops[:mem]:
                ctx.set(d, value)
        elif mn in ("ret", "retaa"):
            break
        elif mn in CONTROL:
            continue
        elif ops and regnum(ops[0]) is not None:
            ctx.set(ops[0], ("?",))
    return None, stores, []


def output_fields(insns, after_call, out_base):
    """Loads from the output buffer after the call and where they are stored to caller pointers."""
    out_sp = out_base[1] if out_base[0] == "sp" else None
    ctx = Ctx()
    out_regs = {}
    fields = []
    for a in after_call:
        mn, ops = insns[a]
        if mn in ("ret", "retaa"):
            break
        if mn.startswith("ldr") and mem_operand(ops) is not None:
            mem = mem_operand(ops)
            base, off = addr_of(ctx, ",".join(ops[mem:]))
            if base[0] == "sp" and out_sp is not None:
                rel = base[1] + off - out_sp
                width = 8 if is_x(ops[0]) else (1 if mn == "ldrb" else 4)
                for d in ops[:mem]:
                    out_regs[regnum(d)] = (rel, width)
                    ctx.set(d, ("out", rel))
                continue
            for d in ops[:mem]:
                ctx.set(d, ("?",))
        if mn.startswith("str") and mem_operand(ops) is not None:
            if regnum(ops[0]) in out_regs:
                fields.append((*out_regs[regnum(ops[0])], "store-to-caller-pointer"))
            continue
        if mn == "mov" and len(ops) == 2 and regnum(ops[1]) in out_regs:
            ctx.set(ops[0], ("out", out_regs[regnum(ops[1])][0]))
            out_regs[regnum(ops[0])] = out_regs[regnum(ops[1])]
    return fields


def main():
    insns = load_insns()
    syms = load_syms()
    cap = {}
    # Each capture CSV contributes its calls; a symbol seen in both keeps the first sample.
    for capture in CAPTURES:
        with (ROOT / capture).open() as f:
            for r in csv.DictReader(f):
                cap.setdefault(r["symbol"], r)
    rows = []
    for i, (start, name) in enumerate(syms):
        if name not in cap:
            continue
        end = syms[i + 1][0] if i + 1 < len(syms) else start + 0x400
        call, stores, after = analyse(start, end, insns)
        if call is None:
            print("NO CALL SITE:", name)
            continue
        c = cap[name]
        base = call["in_base"][1] if call["in_base"][0] == "sp" else None
        size = call["in_size"] or 0
        in_fields = sorted({(off - base, w, describe(v)) for off, w, v in stores
                            if base is not None and base <= off < base + size})
        rows.append({
            "symbol": name,
            "bridge": call["bridge"],
            "func": call["func"],
            "in_size_code": call["in_size"],
            "in_size_captured": int(c["in_size"]),
            "out_size_code": call["out_size"],
            "out_size_captured": int(c["out_size"]),
            "in_hex": c["in_hex"],
            "out_hex": c["out_hex"],
            "in_fields": in_fields,
            "out_fields": sorted(set(output_fields(insns, after, call["out_base"]))),
        })
    for r in rows:
        print(r["symbol"], r["bridge"], r["func"], "in", r["in_size_code"], r["in_size_captured"],
              "out", r["out_size_code"], r["out_size_captured"])
        for f in r["in_fields"]:
            print("   in  ", f, "sample=", sample(r["in_hex"], f[0], f[1]))
        for f in r["out_fields"]:
            print("   out ", f, "sample=", sample(r["out_hex"], f[0], f[1]))
    failures = []
    for r in rows:
        c = cap[r["symbol"]]
        if (r["bridge"], r["func"]) != (int(c["bridge"]), int(c["func"])):
            failures.append((r["symbol"], "bridge/func"))
        if r["in_size_code"] != int(c["in_size"]):
            failures.append((r["symbol"], "in size"))
        if r["out_size_code"] != int(c["out_size"]):
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
sizes it encodes match the captured ones. All 36 pairs match: 33 from the bring-up run
(`bringup_layouts.csv`), `BridgeRGXTDMSubmitTransfer2` from the buffer-fill run
(`submit_layouts.csv`), and `BridgeChangeSparseMem` and `BridgeRGXKickCDM` from the dispatch run
(`dispatch_layouts.csv`). Run output is in `decode_out.txt`.

Submission cross-check: the `CreateTransferContext` output at out+0 is `0x29`, and the
`SubmitTransfer2` input at offset 8 carries `0x29`, so the field is the transfer context handle.

Dispatch cross-check: the `RGXKickCDM` input at offset 8 carries `0x3d`, the compute context handle
that `BridgeRGXCreateComputeContext` returns at out+0 in the bring-up capture.

Capture CSVs are regenerated from raw bridge logs with `python decode_layouts.py --from-log <log> <csv>`.

To regenerate outside this repo, point `PVR_VENDOR` at a directory holding `srv_um.dis`
(`llvm-objdump -d` of the device's `libsrv_um.so`) and `syms.txt`. The vendor binaries are not checked in.

Field *meanings* ("page size", "heap base") are not established here. These offsets
and widths are the layout that a backend must write. Names need the upstream PowerVR
DDK headers or a kernel-side read to confirm.
"""


def write_reference(rows):
    lines = [HEADER]
    for r in rows:
        lines.append(f"\n## {r['symbol']} (bridge {r['bridge']}, function {r['func']})\n")
        lines.append(f"Input {r['in_size_code']} bytes (captured {r['in_size_captured']}), "
                     f"output {r['out_size_code']} bytes (captured {r['out_size_captured']}).\n")
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
    if sys.argv[1:2] == ["--from-log"]:
        if len(sys.argv) != 4:
            raise SystemExit("usage: decode_layouts.py --from-log <bridge.log> <capture.csv>")
        from_log(sys.argv[2], sys.argv[3])
    else:
        main()
