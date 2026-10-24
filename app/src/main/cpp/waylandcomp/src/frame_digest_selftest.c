/* Host self-test: read a raw RGBA file and print the digest as JSON, field for field the same as
 * tools/diagkit/frames.digest_rgba. The parity test is what keeps the two implementations honest.
 * usage: fdselftest <file.raw> <w> <h> <pitch> [--ascii] */
#include "frame_digest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    int w, h, pitch, want_ascii, i, r, c;
    size_t need, got;
    unsigned char *px;
    struct fd_digest d;
    FILE *f;

    if (argc < 5) {
        fprintf(stderr, "usage: %s <raw> <w> <h> <pitch> [--ascii]\n", argv[0]);
        return 2;
    }
    w = atoi(argv[2]); h = atoi(argv[3]); pitch = atoi(argv[4]);
    want_ascii = argc > 5 && !strcmp(argv[5], "--ascii");
    if (w <= 0 || h <= 0 || pitch < w * 4) { fprintf(stderr, "bad geometry\n"); return 2; }

    need = (size_t)pitch * (size_t)(h - 1) + (size_t)w * 4;
    px = (unsigned char *)malloc(need + 1);
    if (!px) return 1;
    f = fopen(argv[1], "rb");
    if (!f) { perror("open"); free(px); return 1; }
    got = fread(px, 1, need, f);
    fclose(f);
    if (got < need) { fprintf(stderr, "short buffer: %lu of %lu\n",
                              (unsigned long)got, (unsigned long)need); free(px); return 1; }

    if (fd_digest_rgba(px, w, h, pitch, &d) != 0) {
        fprintf(stderr, "digest failed\n");
        free(px);
        return 1;
    }

    printf("{\"w\":%d,\"h\":%d,\"bands\":[", d.w, d.h);
    for (i = 0; i < FD_BANDS; i++)
        printf("%s[%d,%d,%d]", i ? "," : "", d.bands[i][0], d.bands[i][1], d.bands[i][2]);
    printf("],\"zero_rows\":%d,\"zero_runs\":[", d.zero_rows);
    for (i = 0; i < d.zero_run_count; i++)
        printf("%s[%d,%d]", i ? "," : "", d.zero_runs[i][0], d.zero_runs[i][1]);
    printf("],\"rl\":[");
    for (i = 0; i < FD_RL; i++) printf("%s%d", i ? "," : "", d.rl[i]);
    printf("],\"grid\":[");
    for (r = 0; r < FD_GRID; r++) {
        printf("%s[", r ? "," : "");
        for (c = 0; c < FD_GRID; c++) printf("%s%d", c ? "," : "", d.grid[r][c]);
        printf("]");
    }
    printf("],\"hash\":\"sha256:%s\"", d.hash_hex);
    if (want_ascii) {
        char *buf = (char *)malloc((size_t)48 * 20 + 32);
        if (!buf) { free(px); return 1; }
        if (fd_ascii(px, w, h, pitch, 48, 20, buf, (int)((size_t)48 * 20 + 32)) < 0) {
            free(buf); free(px); return 1;
        }
        printf(",\"ascii\":\"");
        for (const char *p = buf; *p; p++) {
            if (*p == '\n') printf("\\n");
            else if (*p == '"' || *p == '\\') printf("\\%c", *p);
            else putchar(*p);
        }
        printf("\"");
        free(buf);
    }
    printf("}\n");
    free(px);
    return 0;
}
