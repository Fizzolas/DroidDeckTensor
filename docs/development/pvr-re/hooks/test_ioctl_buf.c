/* Compile the real hook with only its external boundaries redirected.
 * Run via ../test_ioctl_buf.py; no ioctl reaches a device or the host kernel. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bridge_abi.h"

static void *test_dlsym(void *handle, const char *symbol);
static FILE *test_fopen(const char *path, const char *mode);
static int test_fclose(FILE *stream);
static void *test_malloc(size_t size);
static void test_free(void *ptr);
static int test_fprintf(FILE *stream, const char *format, ...);
static int test_fputc(int ch, FILE *stream);
static int test_ferror(FILE *stream);

#define ioctl captured_ioctl
#define dlsym test_dlsym
#define fopen test_fopen
#define fclose test_fclose
#define malloc test_malloc
#define free test_free
#define fprintf test_fprintf
#define fputc test_fputc
#define ferror test_ferror
#include "ioctl_buf.c"
#undef ioctl
#undef dlsym
#undef fopen
#undef fclose
#undef malloc
#undef free
#undef fprintf
#undef fputc
#undef ferror

static FILE *log_file;
static int open_fails, allocation_fails, boundary_ret, boundary_errno, keep_errno;
static int boundary_calls, allocations, frees, live_allocations;
static int write_fails, close_fails, emit_record, readonly_log;
static int inject_at, inject_return, log_writes, stream_error;
static unsigned expected_request;
static void *expected_argument;
static const uint8_t original_input[] = {0xaa, 0xbb, 0xcc};
static const uint8_t changed_input[] = {0x44, 0x55, 0x66};
static const uint8_t changed_output[] = {0x77, 0x88, 0x99};

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "CHECK failed at line %d: %s\n", __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static int controlled_ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    uint8_t *arg = va_arg(ap, void *);
    va_end(ap);
    CHECK(fd == 42);
    CHECK((unsigned)request == expected_request);
    CHECK(arg == expected_argument);
    boundary_calls++;
    if ((unsigned)request == BRIDGE_IOCTL_REQ && arg) {
        uint64_t in_ptr, out_ptr;
        uint32_t in_size, out_size;
        memcpy(&in_ptr, arg + BRIDGE_ARG_IN_PTR, 8);
        memcpy(&out_ptr, arg + BRIDGE_ARG_OUT_PTR, 8);
        memcpy(&in_size, arg + BRIDGE_ARG_IN_SIZE, 4);
        memcpy(&out_size, arg + BRIDGE_ARG_OUT_SIZE, 4);
        uint8_t *in = (uint8_t *)(uintptr_t)in_ptr;
        uint8_t *out = (uint8_t *)(uintptr_t)out_ptr;
        if (in && in_size) {
            CHECK(in_size == sizeof original_input);
            CHECK(memcmp(in, original_input, in_size) == 0);
            memcpy(in, changed_input, in_size);
        }
        if (out && out_size) {
            CHECK(out_size == sizeof changed_output);
            memcpy(out, changed_output, out_size);
        }
    }
    if (!keep_errno) errno = boundary_errno;
    return boundary_ret;
}

static void *test_dlsym(void *handle, const char *symbol) {
    CHECK(handle == RTLD_NEXT);
    CHECK(strcmp(symbol, "ioctl") == 0);
    return (void *)controlled_ioctl;
}

static FILE *test_fopen(const char *path, const char *mode) {
    CHECK(strcmp(path, "/data/local/tmp/bridge.log") == 0);
    CHECK(strcmp(mode, "a") == 0);
    if (readonly_log) {
        CHECK(fclose(log_file) == 0);
        log_file = fopen("readonly.log", "r");
        CHECK(log_file != NULL);
    }
    errno = EBUSY;
    return open_fails ? NULL : log_file;
}

static int test_fclose(FILE *stream) {
    CHECK(stream == log_file);
    if (!readonly_log) CHECK(fflush(stream) == 0);
    errno = close_fails ? EIO : ERANGE; /* Cleanup must not leak its errno to the caller. */
    return close_fails ? -1 : 0; /* Keep the file open for the assertion below. */
}

static int test_fprintf(FILE *stream, const char *format, ...) {
    if (stream == log_file && write_fails) {
        errno = ENOSPC;
        return -1;
    }
    if (stream == log_file && inject_at) {
        /* One injected write failure, which sets the stream's error indicator as stdio does;
         * every later write succeeds but leaves a different errno behind. */
        if (++log_writes == inject_at) {
            errno = ENOSPC;
            stream_error = 1;
            return inject_return;
        }
        errno = ERANGE;
    }
    va_list ap;
    va_start(ap, format);
    int result = vfprintf(stream, format, ap);
    va_end(ap);
    return result;
}

static int test_fputc(int ch, FILE *stream) {
    if (stream == log_file && write_fails) {
        errno = ENOSPC;
        return EOF;
    }
    if (stream == log_file && inject_at) errno = ERANGE;
    return fputc(ch, stream);
}

static int test_ferror(FILE *stream) {
    return stream == log_file && (write_fails || stream_error) ? 1 : ferror(stream);
}

static void *test_malloc(size_t size) {
    allocations++;
    errno = ENOMEM;
    if (allocation_fails) return NULL;
    void *ptr = malloc(size);
    CHECK(ptr != NULL);
    live_allocations++;
    return ptr;
}

static void test_free(void *ptr) {
    if (ptr) {
        frees++;
        live_allocations--;
    }
    free(ptr);
    errno = ERANGE;
}

/* The 32-byte bridge envelope the hook reads; bridge_abi.h owns its layout. */
static void envelope(uint8_t arg[32], uint32_t bridge, uint32_t func, const uint8_t *in,
                     uint32_t in_size, const uint8_t *out, uint32_t out_size) {
    uint64_t in_ptr = (uint64_t)(uintptr_t)in, out_ptr = (uint64_t)(uintptr_t)out;
    memcpy(arg + BRIDGE_ARG_BRIDGE, &bridge, 4);
    memcpy(arg + BRIDGE_ARG_FUNC, &func, 4);
    memcpy(arg + BRIDGE_ARG_IN_PTR, &in_ptr, 8);
    memcpy(arg + BRIDGE_ARG_OUT_PTR, &out_ptr, 8);
    memcpy(arg + BRIDGE_ARG_IN_SIZE, &in_size, 4);
    memcpy(arg + BRIDGE_ARG_OUT_SIZE, &out_size, 4);
}

static void run_case(const char *name, int null_in, int null_out, int alias,
                     int zero_size, int null_arg, unsigned request,
                     int ret, int err, int stale, int no_log, int no_memory) {
    uint8_t input[3], output[3] = {0x11, 0x22, 0x33}, arg[32] = {0};
    memcpy(input, original_input, sizeof input);
    uint32_t bridge = 1, func = 0, in_size = zero_size ? 0 : 3, out_size = in_size;
    envelope(arg, bridge, func, null_in ? NULL : input, in_size,
             null_out ? NULL : (alias ? input : output), out_size);
    expected_request = request;
    expected_argument = null_arg ? NULL : arg;
    open_fails = no_log;
    allocation_fails = no_memory;
    boundary_ret = ret;
    boundary_errno = err;
    keep_errno = stale;
    boundary_calls = allocations = frees = live_allocations = 0;
    log_file = tmpfile();
    CHECK(log_file != NULL);
    errno = 2;
    int result = captured_ioctl(42, (int)request, expected_argument);
    int result_errno = errno;
    int expected_errno = stale ? EBUSY : err;
    CHECK(result == ret);
    CHECK(result_errno == expected_errno);
    CHECK(boundary_calls == 1);
    CHECK(live_allocations == 0);
    if (!null_arg && request == BRIDGE_IOCTL_REQ && !zero_size) {
        if (!null_in) CHECK(memcmp(input, alias && !null_out ? changed_output : changed_input, 3) == 0);
        if (!null_out && !alias) CHECK(memcmp(output, changed_output, 3) == 0);
    }
    char actual[512] = {0}, expected[512] = {0};
    rewind(log_file);
    size_t bytes = fread(actual, 1, sizeof actual - 1, log_file);
    if (!readonly_log) CHECK(!ferror(log_file));
    CHECK(bytes < sizeof actual - 1);
    if (!no_log && !write_fails && !readonly_log && !(no_memory && !null_in && !zero_size && !null_arg && request == BRIDGE_IOCTL_REQ)) {
        if (request != BRIDGE_IOCTL_REQ || null_arg) {
            snprintf(expected, sizeof expected, BRIDGE_LOG_OTHER "\n", request, ret, expected_errno);
        } else {
            int n = snprintf(expected, sizeof expected, BRIDGE_LOG_CALL, bridge, func, ret, expected_errno);
            if (null_in) n += snprintf(expected + n, sizeof expected - n, BRIDGE_LOG_NULL, "in");
            else n += snprintf(expected + n, sizeof expected - n, BRIDGE_LOG_DATA_HEAD "%s", "in", in_size,
                               zero_size ? "" : "aabbcc");
            if (null_out) n += snprintf(expected + n, sizeof expected - n, BRIDGE_LOG_NULL, "out");
            else n += snprintf(expected + n, sizeof expected - n, BRIDGE_LOG_DATA_HEAD "%s", "out", out_size,
                               zero_size ? "" : "778899");
            snprintf(expected + n, sizeof expected - n, "\n");
        }
    }
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "%s: wanted [%s], got [%s]\n", name, expected, actual);
        exit(1);
    }
    if (no_memory && !null_in && !zero_size && !no_log) CHECK(allocations == 1 && frees == 0);
    if (!no_memory && !null_in && !zero_size && !no_log && !null_arg && request == BRIDGE_IOCTL_REQ)
        CHECK(allocations == 1 && frees == 1);
    if (no_log) CHECK(allocations == 0);
    CHECK(fclose(log_file) == 0);
    if (emit_record) fputs(actual, stdout);
    else printf("PASS %s\n", name);
}

/* A bridge call whose log write is injected as either an error return or a short count (the
 * second byte of the input dump), with every write after it succeeding and leaving ERANGE in
 * errno and the stream's error indicator set: the diagnostic must name the errno of the write
 * that failed, not the one a later operation left behind. */
static void run_failed_write_case(int short_write, const char *pass) {
    uint8_t input[3] = {0xaa, 0xbb, 0xcc}, output[3] = {0}, arg[32] = {0};
    /* Record writes: CALL, then the input dump's head and its three bytes, so 4 is a byte write
     * in the middle of the dump, with later writes of the same dump still to come. */
    inject_at = short_write ? 4 : 1;
    inject_return = short_write ? 1 : -1;
    envelope(arg, 1, 0, input, 3, output, 3);
    expected_request = BRIDGE_IOCTL_REQ;
    expected_argument = arg;
    boundary_ret = 0;
    boundary_errno = 2;
    keep_errno = 0;
    boundary_calls = 0;
    log_file = tmpfile();
    CHECK(log_file != NULL);
    errno = 2;
    CHECK(captured_ioctl(42, (int)BRIDGE_IOCTL_REQ, arg) == 0);
    CHECK(errno == 2); /* Loss reporting must not leak its errno to the forwarded call's caller. */
    CHECK(boundary_calls == 1);
    CHECK(fclose(log_file) == 0);
    puts(pass);
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "write-errno") == 0) {
        run_failed_write_case(0, "PASS write errno names the failed write");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "partial-write") == 0) {
        run_failed_write_case(1, "PASS partial write errno names the failed write");
        return 0;
    }
    if (argc > 1) {
        const char *mode = argv[1];
        int ret = argc > 2 && strcmp(argv[2], "failure") == 0 ? -1 : 0;
        int stale = argc > 2 && strcmp(argv[2], "stale") == 0;
        write_fails = strcmp(mode, "write") == 0;
        close_fails = strcmp(mode, "close") == 0;
        emit_record = 1;
        readonly_log = strcmp(mode, "readonly") == 0;
        if (readonly_log) {
            FILE *empty = fopen("readonly.log", "w");
            CHECK(empty != NULL && fclose(empty) == 0);
        }
        unsigned req = argc > 3 && strcmp(argv[3], "other") == 0 ? 0xc0406400u : BRIDGE_IOCTL_REQ;
        run_case(mode, 0, 0, 0, 0, 0, req, ret, ret ? EIO : 2, stale,
                 strcmp(mode, "open") == 0, strcmp(mode, "snapshot") == 0);
        return 0;
    }
    run_case("mutated input/output success", 0, 0, 0, 0, 0, BRIDGE_IOCTL_REQ, 0, 2, 0, 0, 0);
    run_case("mutated input/output failure", 0, 0, 0, 0, 0, BRIDGE_IOCTL_REQ, -1, EIO, 0, 0, 0);
    run_case("aliased input/output", 0, 0, 1, 0, 0, BRIDGE_IOCTL_REQ, 0, 2, 0, 0, 0);
    run_case("NULL input", 1, 0, 0, 0, 0, BRIDGE_IOCTL_REQ, 0, 2, 0, 0, 0);
    run_case("NULL output on failure", 0, 1, 0, 0, 0, BRIDGE_IOCTL_REQ, -1, EIO, 0, 0, 0);
    run_case("both NULL", 1, 1, 0, 0, 0, BRIDGE_IOCTL_REQ, 0, 2, 0, 0, 0);
    run_case("non-NULL zero-length buffers", 0, 0, 0, 1, 0, BRIDGE_IOCTL_REQ, 0, 2, 0, 0, 0);
    run_case("NULL envelope", 0, 0, 0, 0, 1, BRIDGE_IOCTL_REQ, -1, EINVAL, 0, 0, 0);
    run_case("other ioctl", 0, 0, 0, 0, 0, 0xc0406400u, -1, EIO, 0, 0, 0);
    run_case("log open failure", 0, 0, 0, 0, 0, BRIDGE_IOCTL_REQ, -1, EIO, 0, 1, 0);
    run_case("snapshot allocation failure", 0, 0, 0, 0, 0, BRIDGE_IOCTL_REQ, -1, EIO, 0, 0, 1);
    run_case("stale errno on success", 0, 0, 0, 0, 0, BRIDGE_IOCTL_REQ, 0, 0, 1, 0, 0);
    puts("12 controlled ioctl cases passed");
    return 0;
}
