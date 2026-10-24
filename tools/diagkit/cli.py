"""The front door. Every command prints JSON on stdout and a human line on stderr."""
from __future__ import annotations

import argparse
import importlib
import importlib.util
import json
import sys
from pathlib import Path


def _module(name):
    """Import a sibling module whether we are a package or loaded by file path.

    The tests load this file directly (so they can assert on the parser without a package
    install), and tools/diag imports it as `diagkit.cli`. Both have to work.
    """
    if __package__:
        return importlib.import_module(f'{__package__}.{name}')
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(f'{name}.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _arm(pairs):
    out = {}
    for pair in pairs or []:
        if '=' not in pair:
            raise argparse.ArgumentTypeError(f'--arm wants name=value, got {pair!r}')
        k, v = pair.split('=', 1)
        out[k] = v
    return out


def build_parser():
    p = argparse.ArgumentParser(prog='diag', description='DroidDeck diagnostic harness')
    p.add_argument('--adb')
    p.add_argument('--serial')
    sub = p.add_subparsers(dest='command', required=True)

    sub.add_parser('doctor', help='is the device clean enough to measure on')

    r = sub.add_parser('run', help='run one labelled experiment')
    r.add_argument('--label', required=True)
    r.add_argument('--hypothesis', required=True)
    r.add_argument('--arm', action='append', type=str, required=True,
                   help='name=value for the ONE thing this run varies')
    r.add_argument('--root', default='runs')

    pr = sub.add_parser('prior', help='search every existing run before forming a hypothesis')
    pr.add_argument('question')
    pr.add_argument('--roots', nargs='*', default=[])

    a = sub.add_parser('analyze', help='timeline, symbolization and cited findings')
    a.add_argument('run')
    a.add_argument('--symbolize', action='store_true')

    c = sub.add_parser('compare', help='diff two runs, refusing arms that vary more than one thing')
    c.add_argument('a')
    c.add_argument('b')

    f = sub.add_parser('frame', help='digest a frame file (raw RGBA) or the live panel')
    f.add_argument('--raw', help='path to a raw RGBA frame')
    f.add_argument('--width', type=int)
    f.add_argument('--height', type=int)
    f.add_argument('--pitch', type=int, help='row pitch in bytes; defaults to width*4')
    f.add_argument('--panel', action='store_true', help='screencap the device (explicit request only)')
    f.add_argument('--ascii', action='store_true')

    e = sub.add_parser('export', help='stage a text-only bundle of a run')
    e.add_argument('run')
    e.add_argument('dest', nargs='?')
    return p


def main(argv=None):
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return 2 if exc.code not in (0, None) else 0
    try:
        return _dispatch(args)
    except Exception as exc:  # the CLI's contract is an exit code, not a traceback
        print(json.dumps({'ok': False, 'error': str(exc)}))
        print(f'diag: {exc}', file=sys.stderr)
        return getattr(exc, 'exit_code', 2)


def _adb(args):
    return _module('device').Adb(args.adb, args.serial)


def _dispatch(args):
    if args.command == 'doctor':
        doctor_mod = _module('doctor')
        inv = doctor_mod.inventory(_adb(args))
        state, reasons = doctor_mod.verdict(inv)
        print(json.dumps({'ok': state == 'clean', 'state': state,
                          'reasons': reasons, 'inventory': inv}))
        return 0 if state == 'clean' else 4

    if args.command == 'frame':
        frames = _module('frames')
        if args.panel:
            pixels, w, h, _fmt = frames.parse_screencap(_adb(args).screencap_raw())
            out = frames.digest_rgba(pixels, w, h, want_ascii=args.ascii)
            out['source'] = 'panel'
        elif args.raw:
            if args.width is None or args.height is None:
                raise SystemExit(2)
            data = Path(args.raw).read_bytes()
            out = frames.digest_rgba(data, args.width, args.height,
                                     pitch=args.pitch or args.width * 4,
                                     want_ascii=args.ascii)
            out['source'] = 'file'
        else:
            raise SystemExit(2)
        print(json.dumps(out, sort_keys=True))
        return 0

    if args.command == 'run':
        return _run(args)

    if args.command == 'prior':
        prior = _module('prior')
        roots = [Path(r) for r in (args.roots or ['.', '../audit'])]
        hits = prior.search(roots, args.question)
        print(json.dumps({'ok': True, 'hits': hits}, sort_keys=True))
        print(f'{len(hits)} hit(s) - cite these before writing the hypothesis', file=sys.stderr)
        return 0

    if args.command == 'analyze':
        analyze = _module('analyze')
        found = analyze.findings_from(Path(args.run))
        print(json.dumps({'ok': True, 'findings': found}, sort_keys=True))
        print(analyze.render(found), file=sys.stderr)
        return 0

    if args.command == 'compare':
        compare = _module('compare')
        reasons = compare.refusal(Path(args.a), Path(args.b))
        print(json.dumps({'ok': not reasons, 'refusals': reasons}, sort_keys=True))
        return 0 if not reasons else 4

    if args.command == 'export':
        analyze = _module('analyze')
        run_dir = Path(args.run)
        dest = Path(args.dest) if args.dest else run_dir.with_name(run_dir.name + '-export')
        dest.mkdir(parents=True, exist_ok=True)
        copied = []
        for path in run_dir.rglob('*'):
            if not path.is_file():
                continue
            if path.suffix.lower() in {'.raw', '.png', '.jpg', '.bin', '.so'}:
                continue
            target = dest / path.relative_to(run_dir)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(path.read_bytes())
            copied.append(str(path.relative_to(run_dir)))
        (dest / 'findings.txt').write_text(analyze.render(analyze.findings_from(run_dir)))
        print(json.dumps({'ok': True, 'dest': str(dest), 'files': sorted(copied)}, sort_keys=True))
        return 0

    raise SystemExit(2)


def _git_commit():
    import subprocess
    try:
        return subprocess.run(['git', 'rev-parse', 'HEAD'], capture_output=True, text=True,
                              check=False, timeout=10).stdout.strip() or 'unknown'
    except Exception:
        return 'unknown'


def _run(args):
    doctor_mod = _module('doctor')
    runs_mod = _module('runs')
    adb = _adb(args)
    state, reasons = doctor_mod.verdict(doctor_mod.inventory(adb))
    if state != 'clean':
        print(json.dumps({'ok': False, 'state': state, 'reasons': reasons}))
        print('diag: the device is not clean; run `diag doctor` for the list', file=sys.stderr)
        return 4
    try:
        version_code = int(adb.provider('state')['appVersion'].split('.')[-1])
    except Exception:
        version_code = 0
    run = runs_mod.Run(Path(args.root), args.label, args.hypothesis,
                       _arm(args.arm), adb, _git_commit(), version_code)
    run.begin()
    print(json.dumps({'ok': True, 'run': str(run.dir), 'arm': run.arm}))
    return 0
