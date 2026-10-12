# Rollback baseline — recorded 2026-10-12 before Phase-1 changes

## Repo
- branch `feature/google-tensor-support`, HEAD `e70e6f5`, working tree clean.

## Shipped artifacts (repo `docs/development/pvr-re/hybshim/dist/` == installed guest)
- `libhybris.so` sha256 `058bdaed24d4c6499297731da2d1442821f80a213a66e9302f8d3191aa471775`
- `q.so`         sha256 `ba9b7904f39053a676abbc62f278d67e48b04d1389335f7022bb3d23bc11f2ed`
- `libvulkan_powervr.so` sha256 `68f90ad851da57665de211f6f88772fa4ddb38c0c65c44d18eadca0e2dd1cf96`

## Helper script (the one staged every session)
- `tools/linuxfs/overlay/usr/local/bin/droiddeck-session` sha256
  `1c63462d22d2112b42e55b6d505fc364a20d2816c334b3d599677bdd1e4bbc63`
  (identical in repo, apk asset, and guest `files/linuxfs/usr/local/bin/`).

## Guest graphics libs (STOCK at baseline)
- `files/linuxfs/usr/lib/libgbm.so.1.0.0` sha256 `e28a74e111cf6f9e2c0f5e1aa3edb0c431dfe088a24cd37da4a0f974eaa42dd1` (67616 B)
  (`.real` backup also present, identical)
- `files/linuxfs/root/.local/share/Steam/steamrtarm64/libminigbm.so` sha256
  `ea1201f47c0854f4810ea850919fd0212b28cf92088b87fa1816e0050e4e123c` (2046456 B) (`.real` present)
- NOTE: the guest `files/linux_vulkan_drivers/powervr-g5x/build_id.json` *file content* is stale
  (says `037c8e87`/`b31d4d2d`) but the actual `.so` files there match `dist/` — the file is metadata only.

## Rollback procedure (restore stock, no shim)
1. Remove the Phase-1 shim (a NEW file; stock libgbm resolves via `/usr/lib`):
   `run-as com.droiddeck.launcher rm -f files/linuxfs/root/.local/share/Steam/steamrtarm64/libgbm.so.1`
2. Remove the libhybris copies placed for the shim:
   `rm -f files/linuxfs/tmp/libhybris.so files/linuxfs/tmp/q.so`
   (and from `steamrtarm64/` if copied there).
3. Clear the helper env: `rm -f /sdcard/Download/droiddeck-env`.
4. `am force-stop com.droiddeck.launcher`.
Stock `libgbm`/`libminigbm` are untouched by Phase 1 (the shim is a *new* file beside them, not an
overwrite), so no restore of the stock libs is needed.
