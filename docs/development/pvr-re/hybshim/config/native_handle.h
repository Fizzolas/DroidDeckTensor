/* Minimal Android cutils/native_handle.h for the hand-built shim (normally from android-headers). */
#ifndef CUTILS_NATIVE_HANDLE_H
#define CUTILS_NATIVE_HANDLE_H
#include <sys/cdefs.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[];
} native_handle_t;
typedef const native_handle_t *buffer_handle_t;
#ifdef __cplusplus
}
#endif
#endif
