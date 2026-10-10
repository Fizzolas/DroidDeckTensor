/* pvr_drm_gate_probe.c — the steps gamescope's createDevice() takes AFTER the primary-node gate.
 *
 * gamescope 3.16.29, CVulkanDevice::createDevice() (src/rendervulkan.cpp):
 *
 *   if ( !GetBackend()->UsesVulkanSwapchain() && !drmProps.hasPrimary ) { ...; return false; }
 *   if ( !drmProps.hasRender ) { ...; return false; }
 *   dev_t renderDevId = makedev( drmProps.renderMajor, drmProps.renderMinor );
 *   drmGetDeviceFromDevId( renderDevId, 0, &drmDev );
 *   const char *drmRenderName = drmDev->nodes[DRM_NODE_RENDER];
 *   m_drmRendererFd = open( drmRenderName, O_RDWR | O_CLOEXEC );
 *
 * The gate is only half the story: if libdrm cannot describe the render node the HAL names, or the
 * node will not open, gamescope dies with "drmGetDeviceFromDevId() failed" or "failed to open DRM
 * render node" instead. This probe runs those two steps with the runtime's own libdrm and the
 * device numbers the ICD reports, so a red result names the next failure before a session is spent
 * finding it.
 *
 * Build (WSL, zig) against the runtime's libdrm:
 *   zig cc -target aarch64-linux-gnu.2.38 -O1 -o pvr_drm_gate_probe pvr_drm_gate_probe.c \
 *     -I<runtime>/usr/include -L<runtime>/usr/lib -l:libdrm.so.2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sysmacros.h>
#include <xf86drm.h>

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Defaults are what the PowerVR HAL's render node is on a Tensor G5; override on the command
     * line to check another device. */
    unsigned rmaj = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 226;
    unsigned rmin = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 128;
    printf("== gamescope's post-gate DRM steps, render=%u:%u ==\n", rmaj, rmin);

    dev_t renderDevId = makedev(rmaj, rmin);
    printf("drmGetNodeTypeFromDevId(%u:%u) = ", rmaj, rmin);
    int type = drmGetNodeTypeFromDevId(renderDevId);
    printf("%d (%s)\n", type, type == DRM_NODE_RENDER ? "render" : type == DRM_NODE_PRIMARY ? "primary" : "?");

    drmDevice *drmDev = NULL;
    int r = drmGetDeviceFromDevId(renderDevId, 0, &drmDev);
    if (r != 0 || !drmDev) {
        printf("RESULT: drmGetDeviceFromDevId() -> %d. gamescope would log 'drmGetDeviceFromDevId() failed'.\n", r);
        return 1;
    }
    printf("drmGetDeviceFromDevId() -> 0, bustype=%d\n", drmDev->bustype);
    printf("available_nodes:");
    for (int i = 0; i < DRM_NODE_MAX; i++)
        if (drmDev->available_nodes & (1 << i)) printf(" %s=%s", i == DRM_NODE_PRIMARY ? "primary" : i == DRM_NODE_RENDER ? "render" : "ctrl", drmDev->nodes[i]);
    printf("\n");
    if (!(drmDev->available_nodes & (1 << DRM_NODE_RENDER))) {
        printf("RESULT: libdrm did not report a render node - gamescope asserts here.\n");
        return 1;
    }
    const char *name = drmDev->nodes[DRM_NODE_RENDER];

    int fd = open(name, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("RESULT: open(\"%s\") failed. gamescope would log 'failed to open DRM render node'.\n", name);
        drmFreeDevice(&drmDev);
        return 1;
    }
    printf("open(\"%s\", O_RDWR|O_CLOEXEC) -> fd %d\n", name, fd);
    close(fd);
    drmFreeDevice(&drmDev);
    printf("RESULT: PASS - the gate's follow-on steps succeed; the Wayland backend's createDevice() completes.\n");
    return 0;
}
