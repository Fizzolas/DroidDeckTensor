#include "frame_digest.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* SHA-256, written out rather than linked: the compositor may not depend on libcrypto, and the
 * Python side uses hashlib. Both must hash the same bytes in the same order - each row up to
 * width*4, skipping the pitch gap - or the parity test fails. */
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

struct sha { uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t used; };

static void sha_init(struct sha *s) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    memcpy(s->h, iv, sizeof iv); s->len = 0; s->used = 0;
}

static void sha_block(struct sha *s, const unsigned char *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3]; e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

/* `bump` exists because the length field must not count the padding itself. */
static void sha_update(struct sha *s, const unsigned char *p, size_t n, int bump) {
    if (bump) s->len += n;
    while (n) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { sha_block(s, s->buf); s->used = 0; }
    }
}

static void sha_final(struct sha *s, char hex[65]) {
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80, zero = 0;
    sha_update(s, &pad, 1, 0);
    while (s->used != 56) sha_update(s, &zero, 1, 0);
    for (int i = 7; i >= 0; i--) {
        unsigned char b = (unsigned char)(bits >> (i * 8));
        sha_update(s, &b, 1, 0);
    }
    static const char *hx = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 4; j++) {
            unsigned char hi = (unsigned char)((s->h[i] >> ((3 - j) * 8 + 4)) & 0xf);
            unsigned char lo = (unsigned char)((s->h[i] >> ((3 - j) * 8)) & 0xf);
            hex[i*8 + j*2] = hx[hi];
            hex[i*8 + j*2 + 1] = hx[lo];
        }
    }
    hex[64] = 0;
}

static int bucket(int n) {
    static const int caps[FD_RL] = {1,2,4,8,16,32,64,128,256,512,1024,2048,4096};
    for (int i = 0; i < FD_RL; i++) if (n <= caps[i]) return i;
    return FD_RL - 1;
}

/* Row i's mean luminance and whether it is wholly zero. Both go through the same fixed-point
 * integer arithmetic as tools/diagkit/frames.py so the two producers agree exactly. */
static void row_stats(const unsigned char *px, int w, int pitch, int y, int *mean, int *zero) {
    const unsigned char *row = px + (size_t)y * pitch;
    long sr = 0, sg = 0, sb = 0;
    int z = 1, x;
    for (x = 0; x < w; x++) {
        const unsigned char *p = row + x * 4;
        sr += p[0]; sg += p[1]; sb += p[2];
        if (p[0] || p[1] || p[2] || p[3]) z = 0;
    }
    *mean = (int)((77 * sr + 150 * sg + 29 * sb) / (256L * w));
    *zero = z;
}

static int cell_mean(const unsigned char *px, int pitch, int x0, int x1, int y0, int y1) {
    long total = 0;
    long n;
    for (int y = y0; y < y1; y++) {
        const unsigned char *p = px + (size_t)y * pitch;
        for (int x = x0; x < x1; x++) {
            const unsigned char *q = p + x * 4;
            total += 77 * q[0] + 150 * q[1] + 29 * q[2];
        }
    }
    n = (long)(y1 - y0) * (x1 - x0);
    if (n < 1) n = 1;
    return (int)(total / (256L * n));
}

int fd_digest_rgba(const unsigned char *px, int w, int h, int pitch, struct fd_digest *out) {
    struct sha s;
    int b, r, c, y;

    if (!px || !out || w <= 0 || h <= 0 || pitch < w * 4) return -1;
    memset(out, 0, sizeof *out);
    out->w = w; out->h = h;

    /* The hash covers every row's w*4 bytes and skips the pitch gap. */
    sha_init(&s);
    for (y = 0; y < h; y++)
        sha_update(&s, px + (size_t)y * pitch, (size_t)w * 4, 1);
    sha_final(&s, out->hash_hex);

    /* Bands: recompute the row stats inside each band rather than store h of them, so there is
     * no allocation and no fixed limit on the frame height. */
    for (b = 0; b < FD_BANDS; b++) {
        int y0 = b * h / FD_BANDS, y1 = (b + 1) * h / FD_BANDS;
        long total = 0;
        int mn = 255, mx = 0, zc = 0, n;
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > h) y1 = h;
        for (y = y0; y < y1; y++) {
            int mean, zero;
            row_stats(px, w, pitch, y, &mean, &zero);
            total += mean;
            if (mean < mn) mn = mean;
            if (mean > mx) mx = mean;
            zc += zero;
        }
        n = y1 - y0;
        if (n < 1) n = 1;
        out->bands[b][0] = (int)(total / n);
        out->bands[b][1] = mx - mn;
        out->bands[b][2] = zc;
    }

    /* Wholly-zero rows, and the runs they form. */
    for (y = 0; y < h; y++) {
        int mean, zero;
        row_stats(px, w, pitch, y, &mean, &zero);
        if (!zero) continue;
        out->zero_rows++;
        if (out->zero_run_count &&
            out->zero_runs[out->zero_run_count-1][0] + out->zero_runs[out->zero_run_count-1][1] == y)
            out->zero_runs[out->zero_run_count-1][1]++;
        else if (out->zero_run_count < FD_MAX_RUNS) {
            out->zero_runs[out->zero_run_count][0] = y;
            out->zero_runs[out->zero_run_count][1] = 1;
            out->zero_run_count++;
        }
    }

    /* The run-length profile of the first row, matching frames._run_lengths. */
    {
        const unsigned char *prev = 0;
        int run = 0;
        for (int x = 0; x < w; x++) {
            const unsigned char *p = px + x * 4;
            if (prev && !memcmp(prev, p, 4)) { run++; continue; }
            if (prev) out->rl[bucket(run)]++;
            prev = p; run = 1;
        }
        if (prev) out->rl[bucket(run)]++;
    }

    for (r = 0; r < FD_GRID; r++) {
        int y0 = r * h / FD_GRID, y1 = (r + 1) * h / FD_GRID;
        for (c = 0; c < FD_GRID; c++) {
            int x0 = c * w / FD_GRID, x1 = (c + 1) * w / FD_GRID;
            out->grid[r][c] = cell_mean(px, pitch, x0, x1, y0, y1);
        }
    }
    return 0;
}

int fd_ascii(const unsigned char *px, int w, int h, int pitch, int cols, int rows,
             char *out, int out_len) {
    static const char *ramp = FD_RAMP;
    int ramp_n = 10, n = 0, r, c;

    if (!px || !out || w <= 0 || h <= 0 || cols <= 0 || rows <= 0) return -1;
    for (r = 0; r < rows; r++) {
        int y0 = r * h / rows, y1 = (r + 1) * h / rows;
        for (c = 0; c < cols; c++) {
            int x0 = c * w / cols, x1 = (c + 1) * w / cols;
            int v = cell_mean(px, pitch, x0, x1, y0, y1);
            int idx = v * ramp_n / 256;
            if (idx < 0) idx = 0;
            if (idx >= ramp_n) idx = ramp_n - 1;
            if (n + 2 >= out_len) return -1;
            out[n++] = ramp[idx];
        }
        if (n + 2 >= out_len) return -1;
        out[n++] = '\n';
    }
    if (n > 0) n--;              /* drop the trailing newline, as Python's join does */
    out[n] = 0;
    return n;
}
