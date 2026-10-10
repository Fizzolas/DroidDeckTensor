set -e
ZIG=$HOME/zig-linux-x86_64-0.13.0/zig
CO=/mnt/d/GUH/libhybris/hybris/common
OUT=/tmp/hyb_obj
rm -rf "$OUT"; mkdir -p "$OUT/q" "$OUT/hk/lp"
CF="-target aarch64-linux-gnu.2.38 -fPIC -O1 -w -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -DANDROID -D_FORTIFY_SOURCE=1 -D_USING_LIBCXX -I$CO/include -I$CO -I$CO/../include -I$HOME/zig-linux-x86_64-0.13.0/lib/libc/include/aarch64-linux-any -I$HOME/zig-linux-x86_64-0.13.0/lib/libc/include/any-linux-any -I$CO/q -I$CO/q/bionic/libc -I$CO/q/bionic/libc/include"
LIBP="-DDEFAULT_HYBRIS_LD_LIBRARY_PATH=\"/system/lib64:/vendor/lib64:/system/lib64/hw:/vendor/lib64/hw\""
QSRC="hybris_compat dlfcn linker_allocator linker_block_allocator linker linker_cfi linker_config linker_dlwarning linker_gdb_support linker_globals linker_logger linker_main linker_mapped_file_fragment linker_memory linker_namespaces linker_phdr linker_sdk_versions linker_soinfo linker_tls linker_utils rt"
for f in $QSRC; do $ZIG c++ $CF $LIBP -std=c++17 -c "$CO/q/$f.cpp" -o "$OUT/q/$f.o"; done
for f in bionic_call_ifunc_resolver bionic_elf_tls bionic_allocator; do $ZIG c++ $CF $LIBP -std=c++17 -c "$CO/q/bionic/libc/bionic/$f.cpp" -o "$OUT/q/$f.o"; done
$ZIG cc $CF $LIBP -c "$CO/strlcpy.c" -o "$OUT/q/strlcpy.o"; $ZIG cc $CF $LIBP -c "$CO/strlcat.c" -o "$OUT/q/strlcat.o"
$ZIG cc $CF $LIBP -c "$CO/q/tlsdesc_resolver.S" -o "$OUT/q/tlsdesc_resolver.o"
$ZIG c++ -target aarch64-linux-gnu.2.38 -shared -O1 "$OUT/q"/*.o -o /tmp/q.so
echo "built q.so"
HF="-target aarch64-linux-gnu.2.38 -fPIC -O1 -w -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -DANDROID -D_FORTIFY_SOURCE=1 -DWANT_LINKER_Q -I$CO/include -I$CO -I$CO/../include -I$CO/mm/bionic/libc -I$CO/mm/bionic/libc/include"
$ZIG c++ $HF $LIBP -std=c++17 -c "$CO/dso_handle_counters.cpp" -o "$OUT/hk/dso_handle_counters.o"
for f in hooks wrappers wrapper_code_generic_arm logging native_handle hooks_shm sysconf strlcpy strlcat; do $ZIG cc $HF $LIBP -c "$CO/$f.c" -o "$OUT/hk/$f.o"; done
for f in properties cache runtime_cache; do $ZIG cc $HF $LIBP -c "$CO/legacy_properties/$f.c" -o "$OUT/hk/lp/$f.o"; done
$ZIG c++ -target aarch64-linux-gnu.2.38 -shared -O1 "$OUT/hk"/*.o "$OUT/hk/lp"/*.o -o /tmp/libhybris.so
echo "built libhybris.so (Q)"
cp /tmp/q.so /tmp/libhybris.so /mnt/d/GUH/
