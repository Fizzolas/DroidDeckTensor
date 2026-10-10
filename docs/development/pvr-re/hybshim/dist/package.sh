#!/bin/sh
# Assemble the -Linux driver zip the app imports (LinuxVulkanDriverManager.installDriver).
# Expects ../icd/libvulkan_powervr.so (built with -Wl,-rpath,'$ORIGIN'), q.so and libhybris.so
# from build_hybris.sh, and libwayland-client.so.0 + libffi.so.8.
set -e
d=$(dirname "$0")
cd "$d"
rm -f pvr-powervr-linux.zip
zip -X pvr-powervr-linux.zip \
    libvulkan_powervr.so libhybris.so q.so libwayland-client.so.0 libffi.so.8 meta.json
echo "wrote $(pwd)/pvr-powervr-linux.zip"
