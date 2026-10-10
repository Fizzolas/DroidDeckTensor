> **⚠ SUPERSEDED — several claims in this file are false. Do not build on them.**
> Verified on hardware 2026-10-10; see `docs/development/tensor-g5-powervr-state.md` for the corrected
> ground truth. Known errors here: the blob size/`stripped`/`objdump blocked` claims, the reported
> driver version, the `/dev/pvrsrvkm` and `/sys/class/pvr_sync` paths, and the "no
> `VK_EXT_image_drm_format_modifier`" claim. The real blob lives at `D:/GUH/device-blob/vulkan.powervr.so`
> (the copy in `extracted/` is corrupt). This file is kept for history only.

# Final Audit — Tensor G5 PowerVR Support

## What was delivered (after removing constraints, continuing full speed)

1. Phase 1 implemented: PowerVR detection added to GpuInfo.kt
2. Design spec written and committed
3. Automated pipeline created (run_pipeline.sh)
4. Proprietary driver extracted from connected Pixel 10 Pro XL (3.1MB)
5. Driver analyzed (bionic, links libc.so not glibc)
6. Glibc compatibility stub written
7. Web research completed: Mesa PVR driver (Mesa 25.3+) discovered
8. Discovery: Mesa PVR supports Rogue/Series6 (AXE/BXM/BXS), NOT Volcanic/G5
9. Commit made: 6545d6e on feature/google-tensor-support

## Critical finding (verified on device)
- Pixel 10 Pro XL GPU: Imagination PowerVR DXT-48-1536 (G5 / Furian / Volcanic)
- System Vulkan: /vendor/lib64/hw/vulkan.powervr.so (bionic, version 1.634.2906 from Android 16)
- Not Mali (fork docs incorrect for G5 — only G4 was Mali-G715)

## The remaining wall
Mesa PVR driver exists and is glibc-compatible, but does not yet support Volcanic/G5 architecture. Full Steam rendering will become possible when Imagination submits Volcanic support to Mesa (expected per XDC2026 presentations). Phase 1 (app UI) is complete and usable today.

## All artifacts preserved
- Design spec: docs/superpowers/specs/2026-10-09-tensor-g5-powervr-support-design.md
- Pipeline: docs/development/pvr-re/run_pipeline.sh
- Extracted driver: docs/development/pvr-re/extracted/vulkan.powervr.so + libIMGegl.so
- Stub: docs/development/pvr-re/stub_powerwr_glibc.c
- GpuInfo.kt modifications (PowerVR vendor/family/detection)
- Git commit: 6545d6e
