Bridge approach: use libhybris method
The extracted driver is bionic. To load it in glibc:
1. Create fake libdl.so (glibc interface) that calls bionic symbols
2. Use LD_PRELOAD to redirect dlopen() to load bionic .so
3. Resolve symbol differences (bionic libc.so vs glibc libc.so.6)
4. Build glibc-linked wrapper that opens bionic driver
