/* wsi_compositor.c — a minimal Wayland compositor, its own process, for the WSI test.
 * Advertises wl_compositor + zwp_linux_dmabuf_v1 and records what a presenting client sends. */
#include <vulkan/vulkan.h>
#include <wayland-server.h>
#include <linux-dmabuf-v1-server-protocol.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

static int g_commits = 0, g_attaches = 0, g_buffers = 0, g_params = 0;
static int g_last_fd = -1;

static void s_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf, int32_t x, int32_t y) {
    (void)c; (void)r; (void)x; (void)y; if (buf) g_attaches++;
}
static void s_damage(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b, int32_t d, int32_t e) { (void)c;(void)r;(void)a;(void)b;(void)d;(void)e; }
static void s_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb) { (void)c;(void)r;(void)cb; }
static void s_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg) { (void)c;(void)r;(void)reg; }
static void s_commit(struct wl_client *c, struct wl_resource *r) { (void)c;(void)r; g_commits++; }
static void s_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { (void)c;(void)r;(void)t; }
static void s_scale(struct wl_client *c, struct wl_resource *r, int32_t s) { (void)c;(void)r;(void)s; }
static void s_damage_buffer(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b, int32_t d, int32_t e) { (void)c;(void)r;(void)a;(void)b;(void)d;(void)e; }
static void s_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y) { (void)c;(void)r;(void)x;(void)y; }
static void s_destroy_surface(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static const struct wl_surface_interface surface_impl = {
    s_destroy_surface, s_attach, s_damage, s_frame,
    s_region, s_region,            /* set_opaque_region, set_input_region */
    s_commit, s_transform, s_scale, s_damage_buffer, s_offset
};

static void region_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b, int32_t d, int32_t e) { (void)c;(void)r;(void)a;(void)b;(void)d;(void)e; }
static void region_sub(struct wl_client *c, struct wl_resource *r, int32_t a, int32_t b, int32_t d, int32_t e) { (void)c;(void)r;(void)a;(void)b;(void)d;(void)e; }
static const struct wl_region_interface region_impl = { region_destroy, region_add, region_sub };

static void regist_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *s = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s, &surface_impl, NULL, NULL);
    fprintf(stderr, "comp: create_surface\n");
}
static void regist_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *reg = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(reg, &region_impl, NULL, NULL);
}
static const struct wl_compositor_interface compositor_impl = { regist_create_surface, regist_create_region };
static void bind_compositor(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    fprintf(stderr, "comp: bind_compositor v%u\n", ver);
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, ver, id);
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

static void p_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void p_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t p0, uint32_t off, uint32_t stride, uint32_t mod_hi, uint32_t mod_lo) {
    (void)c;(void)r;(void)p0;(void)off;(void)stride;(void)mod_hi;(void)mod_lo; g_params++; g_last_fd = fd;
    fprintf(stderr, "comp: buffer params add fd=%d\n", fd);
}
static void p_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t fmt, uint32_t flags) {
    (void)r;(void)w;(void)h;(void)fmt;(void)flags;
    struct wl_resource *buf = wl_resource_create(c, &wl_buffer_interface, 1, 1);
    (void)buf; g_buffers++;
}
static void p_create_immed(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t w, int32_t h, uint32_t fmt, uint32_t flags) {
    (void)r;(void)w;(void)h;(void)fmt;(void)flags;
    struct wl_resource *buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
    wl_resource_set_implementation(buf, NULL, NULL, NULL);
    zwp_linux_buffer_params_v1_send_created(r, buf);
    g_buffers++;
    fprintf(stderr, "comp: buffer created immed %ux%u fmt=%#x\n", w, h, fmt);
}
static const struct zwp_linux_buffer_params_v1_interface params_impl = { p_destroy, p_add, p_create, p_create_immed };

static void fb_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void fb_evt(struct wl_client *c, struct wl_resource *r, uint32_t f) { (void)c;(void)r;(void)f; }
static void fb_dev(struct wl_client *c, struct wl_resource *r, uint32_t d) { (void)c;(void)r;(void)d; }
static void fb_fmt(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t b) { (void)c;(void)r;(void)a;(void)b; }
static void fb_tranche(struct wl_client *c, struct wl_resource *r, uint32_t a, uint32_t b) { (void)c;(void)r;(void)a;(void)b; }
static void fb_flags(struct wl_client *c, struct wl_resource *r, uint32_t f) { (void)c;(void)r;(void)f; }
static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = { fb_destroy, fb_evt, fb_evt, fb_dev, fb_fmt, fb_tranche, fb_flags };

static void d_create_params(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *p = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(p, &params_impl, NULL, NULL);
}
static void d_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; wl_resource_destroy(r); }
static void d_get_default_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id) {
    struct wl_resource *f = wl_resource_create(c, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(f, &feedback_impl, NULL, NULL);
    zwp_linux_dmabuf_feedback_v1_send_main_device(f, 0);
    zwp_linux_dmabuf_feedback_v1_send_done(f);
}
static void d_get_surface_feedback(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *s) { (void)s; d_get_default_feedback(c, r, id); }
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = { d_destroy, d_create_params, d_get_default_feedback, d_get_surface_feedback };
static void bind_dmabuf(struct wl_client *c, void *data, uint32_t ver, uint32_t id) {
    (void)data;
    fprintf(stderr, "comp: bind_dmabuf v%u\n", ver);
    struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, ver, id);
    wl_resource_set_implementation(r, &dmabuf_impl, NULL, NULL);
    zwp_linux_dmabuf_v1_send_modifier(r, 0x34325241, 0, 0);
    zwp_linux_dmabuf_v1_send_modifier(r, 0x34324241, 0, 0);
}

static void client_created(struct wl_listener *l, void *data) { (void)l; (void)data; fprintf(stderr, "comp: accepted a client\n"); }
static struct wl_listener g_cc = { .notify = client_created };

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    const char *name = getenv("WSI_SOCK"); if (!name) name = "pvr-wl-comp";
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 2, "%s", name);
    socklen_t alen = offsetof(struct sockaddr_un, sun_path) + 1 + strlen(addr.sun_path + 1);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&addr, alen) != 0 || listen(lfd, 1) != 0) { printf("comp: listen failed\n"); return 2; }
    struct wl_display *d = wl_display_create();
    struct wl_event_loop *loop = wl_display_get_event_loop(d);
    if (wl_display_add_socket_fd(d, lfd) != 0) { printf("comp: add_socket_fd failed\n"); return 2; }
    fprintf(stderr, "comp: comp global %p\n", (void *)wl_global_create(d, &wl_compositor_interface, 4, NULL, bind_compositor));
    fprintf(stderr, "comp: dmb global %p\n", (void *)wl_global_create(d, &zwp_linux_dmabuf_v1_interface, 4, NULL, bind_dmabuf));
    wl_display_add_client_created_listener(d, &g_cc);
    printf("comp: up\n");
    int last_commits = -1;
    for (int i = 0; i < 300; i++) {
        wl_event_loop_dispatch(loop, 50);
        wl_display_flush_clients(d);          /* push replies out to the client */
        if (g_commits != last_commits) { fprintf(stderr, "comp: commits=%d\n", g_commits); last_commits = g_commits; }
        if (g_commits > 0 && i > 10) break;   /* seen a present; give a moment more */
        if (g_commits > 0) break;
    }
    wl_event_loop_dispatch(loop, 100);
    printf("compositor saw: params=%d buffers=%d attaches=%d commits=%d (last fd=%d)\n",
           g_params, g_buffers, g_attaches, g_commits, g_last_fd);
    int ok = g_commits > 0 && g_attaches > 0 && g_buffers > 0 && g_last_fd >= 0;
    printf("comp: RESULT %s\n", ok ? "OK" : "NONE");
    fflush(NULL); return ok ? 0 : 1;
}
