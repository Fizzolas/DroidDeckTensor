#!/usr/bin/env python3
"""Repack app/src/main/assets/linux_driver/powervr-g5x.tzst from the built shim.

The asset the app unpacks into files/linux_vulkan_drivers/powervr-g5x/ on a PowerVR device: the ICD
plus the emulated bionic linker it drives the HAL through. Deterministic - fixed mtime, uid/gid and
mode, entries in a fixed order - so rebuilding from unchanged inputs is byte-identical, and the ICD
inside the asset can be compared against a fresh build_icd.sh output by sha256.

    python3 -I docs/development/pvr-re/hybshim/package_asset.py

Reads what build_icd.sh (icd/libvulkan_powervr.so) and build_hybris.sh (dist/) produced.
"""
import io
import os
import sys
import tarfile
from compression import zstd

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..', '..'))
OUT = os.path.join(ROOT, 'app/src/main/assets/linux_driver/powervr-g5x.tzst')

MEMBERS = [
    (os.path.join(HERE, 'icd/libvulkan_powervr.so'),    'libvulkan_powervr.so'),
    (os.path.join(HERE, 'dist/libhybris.so'),           'libhybris.so'),
    (os.path.join(HERE, 'dist/q.so'),                   'q.so'),
    (os.path.join(HERE, 'dist/libwayland-client.so.0'), 'libwayland-client.so.0'),
    (os.path.join(HERE, 'dist/libffi.so.8'),            'libffi.so.8'),
]


def main():
    for path, _ in MEMBERS:
        if not os.path.isfile(path):
            print('missing input: %s' % path, file=sys.stderr)
            return 1
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
    print('wrote %s (%d bytes)' % (OUT, os.path.getsize(OUT)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
