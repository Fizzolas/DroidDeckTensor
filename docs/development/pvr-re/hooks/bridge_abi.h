/* PowerVR bridge ABI and capture log format: the single definition shared by
 *   hooks/ioctl_buf.c   writes the log with the BRIDGE_LOG_* formats below
 *   decode_layouts.py   parses the log with the same BRIDGE_LOG_* formats
 * Change the layout or the log format here only.
 *
 * The bridge ioctl takes a 32-byte argument:
 *   { u32 bridge; u32 func; u64 in_ptr; u64 out_ptr; u32 in_size; u32 out_size }.
 *
 * Build the hook (NDK clang, device ABI):
 *   $NDK/aarch64-linux-android33-clang -shared -fPIC -Wall -o ioctl_buf.so ioctl_buf.c -ldl
 */
#ifndef BRIDGE_ABI_H
#define BRIDGE_ABI_H

#define BRIDGE_IOCTL_REQ 0xc0206440u

/* stderr diagnostic consumed by run_pipeline.sh, never part of a B/D log record. */
#define BRIDGE_CAPTURE_LOSS_MARKER "pvr-capture-loss:"

#define BRIDGE_ARG_BRIDGE 0   /* u32 bridge id */
#define BRIDGE_ARG_FUNC 4     /* u32 function id */
#define BRIDGE_ARG_IN_PTR 8   /* u64 input buffer address */
#define BRIDGE_ARG_OUT_PTR 16 /* u64 output buffer address */
#define BRIDGE_ARG_IN_SIZE 24 /* u32 input bytes */
#define BRIDGE_ARG_OUT_SIZE 28 /* u32 output bytes */

/* Log record formats. decode_layouts.py compiles BRIDGE_LOG_CALL, BRIDGE_LOG_DATA_HEAD,
 * BRIDGE_LOG_BYTE and BRIDGE_LOG_NULL into regexes, so those may use only %u, %d, %s and %02x.
 * A B record is CALL, then DATA_HEAD + BYTE* or NULL for the input, then the same for the output.
 * A D record is every other ioctl. The decoder ignores D records. */
#define BRIDGE_LOG_CALL "B bridge=%u func=%u ret=%d errno=%d"
#define BRIDGE_LOG_DATA_HEAD " %s[%u]="  /* label (in|out), byte count */
#define BRIDGE_LOG_BYTE "%02x"
#define BRIDGE_LOG_NULL " %s=NULL"       /* label */
#define BRIDGE_LOG_OTHER "D req=0x%08x ret=%d errno=%d"

#endif
