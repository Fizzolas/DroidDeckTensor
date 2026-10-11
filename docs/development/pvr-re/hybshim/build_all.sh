#!/bin/sh
# Build the shim and the ICD into their real locations, then stamp a build identity.
#
# Run this from WSL. It is the one command that should precede any measurement: after it,
# dist/ and icd/ hold exactly what package_asset.py will ship, and dist/build_id.json holds
# the sha256 of each, so a hash recorded during a session can be checked against the build
# it claims to be. See build_id.sh for why this exists.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)

echo "== shim (q.so, libhybris.so) =="
sh "$HERE/build_hybris.sh"

echo "== ICD (libvulkan_powervr.so) =="
sh "$HERE/build_icd.sh"

echo "== build identity =="
sh "$HERE/build_id.sh"

echo
echo "next: python3 -I $HERE/package_asset.py   (refuses to ship a stale dist/)"
