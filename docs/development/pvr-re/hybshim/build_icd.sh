set -e
# Build the glibc Vulkan ICD that drives the bionic PowerVR HAL through libhybris.
#
# This script lived only at /mnt/d/GUH/build_icd.sh (loose in the workspace, never committed)
# while package_asset.py claimed to read "what build_icd.sh produced". Committed here so the
# ICD has a reproducible, in-tree build like the shim does. Output lands in icd/, which is
# where package_asset.py looks for it.
#
# External inputs (all under /mnt/d/GUH/, produced once and stable):
#   $X11INC/$X11LIB   xcb/X11 headers + aarch64 libs   (x11build)
#   $WL/...           wayland-client headers + lib     (wayland-wsi)
#   $LINKS            libhybris.so for the DT_NEEDED   (links)
#   $MESAINC          Vulkan headers                   (mesa-build/mesa/include)
ZIG=$HOME/zig-linux-x86_64-0.13.0/zig
R=$(cd "$(dirname "$0")" && pwd)
X=${X11BUILD:-/mnt/d/GUH/x11build}
W=${WAYLAND_WSI:-/mnt/d/GUH/wayland-wsi}
L=${HYBRIS_LINKS:-/mnt/d/GUH/links}
V=${MESA_INCLUDE:-/mnt/d/GUH/mesa-build/mesa/include}
cd "$R/icd"
"$ZIG" cc -target aarch64-linux-gnu.2.38 -fPIC -shared -O2 \
  -Wl,-rpath,'$ORIGIN' -Wl,--allow-shlib-undefined \
  -I"$V" -I"$W/wlinc" -I"$W/gen" -I"$R/icd/protocols" -I"$X/inc" \
  -o libvulkan_powervr.so pvr_icd.c pvr_wsi.c protocols/wayland-protocol.c protocols/linux-dmabuf-v1-protocol.c \
  -L"$L" -l:libhybris.so \
  -L"$X/lib" -lxcb -lxcb-dri3 -lxcb-present -lX11 -lX11-xcb \
  -L"$W/wllib" -l:libwayland-client.so.0 -lpthread
echo "BUILT:"; ls -la libvulkan_powervr.so
