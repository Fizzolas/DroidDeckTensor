#!/usr/bin/env python3
"""Repack app/src/main/assets/linux_driver/powervr-g5x.tzst from the built shim.

The asset the app unpacks into files/linux_vulkan_drivers/powervr-g5x/ on a PowerVR device: the ICD
plus the emulated bionic linker it drives the HAL through. Deterministic - fixed mtime, uid/gid and
mode, entries in a fixed order - so rebuilding from unchanged inputs is byte-identical, and the ICD
inside the asset can be compared against a fresh build_icd.sh output by sha256.

    python3 -I docs/development/pvr-re/hybshim/package_asset.py

Reads what build_icd.sh (icd/libvulkan_powervr.so) and build_hybris.sh (dist/) produced.

Build identity. The whole reason this script now checks anything: a day was lost measuring one
libhybris.so while shipping another (see build_id.sh). So the asset carries a build_id.json, and
this refuses to package unless dist/build_id.json exists and matches the bytes it is about to ship.
A dist/ that was rebuilt without re-stamping, or edited by hand, is caught here rather than in a
session three hours later. Pass --allow-stale to override (and it prints loudly that it did).
"""
import hashlib
import io
import json
import os
import sys
import tarfile
from compression import zstd

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
OUT = os.path.join(ROOT, 'app/src/main/assets/linux_driver/powervr-g5x.tzst')
DIST = os.path.join(HERE, 'dist')
BUILD_ID = os.path.join(DIST, 'build_id.json')

MEMBERS = [
    (os.path.join(HERE, 'icd/libvulkan_powervr.so'),    'libvulkan_powervr.so'),
    (os.path.join(DIST, 'libhybris.so'),                'libhybris.so'),
    (os.path.join(DIST, 'q.so'),                        'q.so'),
    (os.path.join(DIST, 'libwayland-client.so.0'),      'libwayland-client.so.0'),
    (os.path.join(DIST, 'libffi.so.8'),                 'libffi.so.8'),
    (BUILD_ID,                                          'build_id.json'),
]

# The artifacts build_id.sh records by name, and where each actually lives on disk. Only these
# are hash-checked; the bundled runtime libs (wayland-client, libffi) are not built here. Note
# the ICD is in icd/ while the shim DSOs are in dist/ - the two must not be confused, which is
# the exact mistake that let a stale dist/libvulkan_powervr.so be stamped as if it were built.
CHECKED = {'libvulkan_powervr.so': os.path.join(HERE, 'icd/libvulkan_powervr.so'),
           'libhybris.so':         os.path.join(DIST, 'libhybris.so'),
           'q.so':                 os.path.join(DIST, 'q.so')}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def verify(allow_stale):
    """Return the loaded build_id, or exit if dist/ does not match it."""
    if not os.path.isfile(BUILD_ID):
        print('no dist/build_id.json - run build_all.sh (or build_id.sh) before packaging',
              file=sys.stderr)
        sys.exit(2)
    with open(BUILD_ID) as f:
        bid = json.load(f)
    recorded = bid.get('artifacts', {})
    mismatches = []
    for member, path in CHECKED.items():
        want = recorded.get(member)
        got = sha256(path) if os.path.isfile(path) else None
        if want != got:
            mismatches.append((member, want, got))
    if mismatches:
        print('dist/ does not match dist/build_id.json:', file=sys.stderr)
        for member, want, got in mismatches:
            print('  %-24s recorded=%s actual=%s' % (member, want, got), file=sys.stderr)
        print('the build changed after it was stamped; rebuild and re-stamp (build_all.sh).',
              file=sys.stderr)
        if not allow_stale:
            sys.exit(3)
        print('--allow-stale: shipping anyway, this asset is NOT reproducible.', file=sys.stderr)
    return bid


def main(argv):
    allow_stale = '--allow-stale' in argv
    for path, _ in MEMBERS:
        if not os.path.isfile(path):
            print('missing input: %s' % path, file=sys.stderr)
            return 1
    bid = verify(allow_stale)

    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode='w', format=tarfile.USTAR_FORMAT) as tf:
        for path, name in MEMBERS:
            with open(path, 'rb') as f:
                data = f.read()
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            ti.mtime = 0
            ti.mode = 0o644
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = 'root'
            tf.addfile(ti, io.BytesIO(data))
    with open(OUT, 'wb') as f:
        f.write(zstd.compress(buf.getvalue(), level=19))

    a = bid.get('artifacts', {})
    print('shipped build:')
    print('  libvulkan_powervr.so %s' % a.get('libvulkan_powervr.so'))
    print('  libhybris.so         %s' % a.get('libhybris.so'))
    print('  q.so                 %s' % a.get('q.so'))
    print('  source               %s' % (bid.get('libhybrisSource', {}).get('commit')))
    print('  builtAt              %s' % bid.get('builtAt'))
    print('wrote %s (%d bytes)' % (OUT, os.path.getsize(OUT)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
