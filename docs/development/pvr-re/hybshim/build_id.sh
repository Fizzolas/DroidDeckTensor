#!/bin/sh
# Stamp dist/build_id.json with the identity of what was just built.
#
# Why this exists: the Tensor G5 PowerVR debugging lost a day to measuring one
# libhybris.so while shipping another. `_hybris_hook_strcmp` was at 0x3bf94, 0x3c080,
# 0x3bfd8 and 0x3bd9c across four sessions that were all narrated as one thread, because
# every `build_hybris.sh` wrote its output to /mnt/d/GUH/ (a scratch drop) while the app
# shipped docs/.../hybshim/dist/ - a directory nothing rebuilt. So the artifact under test
# was never the artifact under deployment, and no conclusion could compound.
#
# The fix has two halves, and this is the second:
#   1. the build scripts write into dist/ (so "built" and "shipped" are the same files);
#   2. this records the sha256 of those files plus the source commit they came from, so a
#      hash taken at measurement time can be compared with the hash inside a shipped asset.
#
# Run it after the build scripts, or use build_all.sh which does both. Reads whatever is
# present in dist/; missing artifacts are recorded as null rather than guessed.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
DIST="$HERE/dist"
# The libhybris checkout the shim is built from. Overridable for a different tree.
HYBRIS_CO=${HYBRIS_CO:-/mnt/d/GUH/libhybris}

sha() { [ -f "$1" ] && sha256sum "$1" | cut -d' ' -f1 || echo null; }
jstr() { [ -n "$1" ] && printf '"%s"' "$1" || printf 'null'; }

LIBHYBRIS=$(sha "$DIST/libhybris.so")
QSO=$(sha "$DIST/q.so")
# The ICD is built into icd/, not dist/ (build_icd.sh writes it there and package_asset.py reads
# it from there). Hashing dist/libvulkan_powervr.so would read a stale leftover and record a
# build identity for a file that is not the one shipped - the very mistake this script exists to
# prevent, caught here by package_asset.py's check on the first run.
ICD=$(sha "$HERE/icd/libvulkan_powervr.so")

# Source provenance. The shim is built from a checkout that is not this repo, so pin what
# was in it at build time - branch and commit - or the artifact has no traceable origin.
CO_COMMIT=$(git -C "$HYBRIS_CO" rev-parse HEAD 2>/dev/null || echo "")
CO_BRANCH=$(git -C "$HYBRIS_CO" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "")
CO_DIRTY=""
if [ -n "$CO_COMMIT" ] && ! git -C "$HYBRIS_CO" diff --quiet 2>/dev/null; then CO_DIRTY="+dirty"; fi

BUILT_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)

cat > "$DIST/build_id.json" <<EOF
{
  "schemaVersion": 1,
  "builtAt": "$BUILT_AT",
  "artifacts": {
    "libhybris.so": $(jstr "$LIBHYBRIS"),
    "q.so": $(jstr "$QSO"),
    "libvulkan_powervr.so": $(jstr "$ICD")
  },
  "libhybrisSource": {
    "commit": $(jstr "${CO_COMMIT}${CO_DIRTY}"),
    "branch": $(jstr "$CO_BRANCH"),
    "checkout": $(jstr "$HYBRIS_CO")
  }
}
EOF

echo "build_id: libhybris.so=$LIBHYBRIS"
echo "          q.so=$QSO"
echo "          libvulkan_powervr.so=$ICD"
echo "          source=${CO_COMMIT}${CO_DIRTY} ($CO_BRANCH)"
echo "wrote $DIST/build_id.json"
