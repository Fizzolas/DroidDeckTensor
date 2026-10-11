#!/bin/sh
# Build the guest instrumentation. zig cross-compiles to glibc 2.31 (the runtime's floor); no
# Android SDK and no apt needed. Outputs into this directory's dist/.
set -e
ZIG=${ZIG:-$HOME/zig-linux-x86_64-0.13.0/zig}
D=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D/dist"
for f in mmwatch symwatch crashcatch; do
    [ -f "$D/$f.c" ] || continue
    "$ZIG" cc -target aarch64-linux-gnu.2.31 -fPIC -shared -O1 -o "$D/dist/$f.so" "$D/$f.c" -ldl
    echo "built $f.so"
done
ls -la "$D/dist"
