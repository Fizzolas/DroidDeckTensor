# libgbm.so.1 over AHardwareBuffer (for CEF)

A drop-in replacement for **`libgbm.so.1`** whose GBM API is answered by real gralloc buffers
(`AHardwareBuffer_*`) reached from the glibc guest through **libhybris** - the same glibc->bionic
bridge the Vulkan ICD uses.

## Why `libgbm.so.1`, and where it goes

**`libcef.so`** (CEF's engine) is the GBM consumer: its `DT_NEEDED` is **`libgbm.so.1`**, and it
references `gbm_create_device` and `gbm_bo_import`. It does **not** reference `libminigbm.so`, and
`steamwebhelper` references neither - so a shim named `libminigbm.so` is loaded by nothing. The
client's rpath is `$ORIGIN` (its own `steamrtarm64/` directory), so the shim goes there as
**`steamrtarm64/libgbm.so.1`**.

## Padding

Steam verifies installed files by size; the shim must be padded to the real `libgbm.so.1.0.0` size
(2046456 bytes on this device). ELF ignores trailing bytes.

## Build

```
zig cc -target aarch64-linux-gnu.2.31 -fPIC -shared -O2 -Wl,-soname,libgbm.so.1 \
    -o libgbm_shim.so gbm_ahb.c -ldl
python3 -c "d=open('libgbm_shim.so','rb').read(); open('libgbm.so.1','wb').write(d+b'\x00'*(2046456-len(d)))"
```

Deploy `libgbm.so.1` into `steamrtarm64/` (back up the real one first), with `libhybris.so` + `q.so`
beside it or where `HYBRIS_LINKER_DIR` points.

## Measured (2026-10-12)

With the shim correctly as `steamrtarm64/libgbm.so.1`: it **loads** (`gbma loaded comm=steamwebhelper`)
and is **called** (`gbm_bo_create ... -> ok`), and CEF's `Buffer handle is null` count drops
**128 -> 0** - the wall that blocked the UI is gone. The GPU process still crashes afterward (when it
uses the handle), with no Chromium-level message; that is the remaining problem. See state doc §53.
