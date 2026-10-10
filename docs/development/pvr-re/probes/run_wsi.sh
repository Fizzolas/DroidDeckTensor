#!/system/bin/sh
cd /data/local/tmp/glibc
export VK_ICD_FILENAMES=/data/local/tmp/glibc/icd/powervr_icd.json
export HYBRIS_LINKER_DIR=/data/local/tmp/glibc/linkerdir
export HYBRIS_LD_LIBRARY_PATH=/system/lib64:/vendor/lib64:/system/lib64/hw:/vendor/lib64/hw
export WSI_SOCK=pvr-$RANDOM
rm -f /tmp/comp.log /tmp/cli.txt
setsid ./lib/ld-linux-aarch64.so.1 --library-path ./lib:./linkerdir ./bin/wsi_compositor >/tmp/comp.log 2>&1 < /dev/null &
CPID=$!
sleep 2
timeout 25 ./lib/ld-linux-aarch64.so.1 --library-path ./lib:./linkerdir ./bin/wsi_client >/tmp/cli.txt 2>&1
echo "client_exit=$?"
kill -9 $CPID 2>/dev/null
