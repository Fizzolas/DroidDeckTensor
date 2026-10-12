# libminigbm.so over AHardwareBuffer

A drop-in replacement for the Steam client's `steamrtarm64/libminigbm.so` whose GBM API is answered
by **real gralloc buffers** (`AHardwareBuffer_*`) reached from the glibc guest through **libhybris** -
the same glibc->bionic bridge the Vulkan ICD uses. Chromium's GPU process allocates its shared-image
buffers through GBM; the PowerVR render node has no GBM/GEM, so its `gbm_bo_get_fd` fails and the GPU
process aborts. This shim gives it a genuine dma-buf instead.

## Why libminigbm.so and not libgbm.so.1

Steam's client ships and loads its **own** `steamrtarm64/libminigbm.so` (Chromium's minigbm); a
`libgbm.so.1` placed in `LD_LIBRARY_PATH` is never consulted. The replacement must be
`steamrtarm64/libminigbm.so`.

## Why it must be padded to exactly 2046456 bytes

Steam verifies its installed files by **size** (`BVerifyInstalledFiles: ...is N bytes, expected M`)
and refuses to start (`rc=42`) when it does not match. ELF ignores trailing bytes, so the shim is
padded to the real library's size.

## Build and deploy (dev procedure, not yet packaged)

```
zig cc -target aarch64-linux-gnu.2.31 -fPIC -shared -O2 -Wl,-soname,libminigbm.so \
    -o libminigbm_shim.so gbm_ahb.c -ldl
# pad to the size of the real library on this device
python3 -c "d=open('libminigbm_shim.so','rb').read(); open('libminigbm.so','wb').write(d+b'\x00'*(2046456-len(d)))"
```

Deploy: back up the real `steamrtarm64/libminigbm.so` as `.real`, put the padded shim in its place,
and place `libhybris.so` + `q.so` (from the PowerVR driver bundle) beside it or where
`HYBRIS_LINKER_DIR` points.

## Measured

With the shim in place (session `2026-10-11-102-steam`): CEF GPU-process crashes ~70 -> **1**,
`Buffer handle is null` ~350 -> **9**, `gbm_wrapper` export errors 80 -> **0**, update-UI presents
21 -> **40**. The main client window is still 1x1 and the client still ends `WaitingForCredentials`;
the last crash and the 1x1 remain. See `docs/development/tensor-g5-powervr-state.md` sections 49-50.
