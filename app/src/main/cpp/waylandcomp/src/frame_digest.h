/* A frame as text, in the compositor. Dependency-free: no Vulkan, no logcat, no allocation.
 * Kept in lockstep with tools/diagkit/frames.py by test_diag_frames_parity.py - if the two ever
 * disagree, two instruments cannot be compared and the comparison is worthless. */
#pragma once
#include <stddef.h>

#define FD_BANDS 32
#define FD_GRID 16
#define FD_RL 13
#define FD_MAX_RUNS 256
#define FD_RAMP " .:-=+*#%@"

struct fd_digest {
    int w, h;
    int zero_rows;
    int zero_run_count;
    int bands[FD_BANDS][3];        /* mean, spread, zero rows */
    int zero_runs[FD_MAX_RUNS][2]; /* start, length */
    int rl[FD_RL];
    int grid[FD_GRID][FD_GRID];
    char hash_hex[65];
};

/* Returns 0 on success, -1 if the buffer is too short for w x h at pitch. */
int fd_digest_rgba(const unsigned char *px, int w, int h, int pitch, struct fd_digest *out);

/* cols x rows mean-luminance map, as characters, newline-separated. Returns the length. */
int fd_ascii(const unsigned char *px, int w, int h, int pitch, int cols, int rows,
             char *out, int out_len);
