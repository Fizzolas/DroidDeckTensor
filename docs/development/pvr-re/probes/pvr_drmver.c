/* pvr_drmver.c — read the DRM driver name/version of the device's GPU nodes.
 * Decides whether Mesa's pvrsrvkm winsys (which demands name "pvr" and version 1.17)
 * can bind this device's kernel driver. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <drm/drm.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *nodes[] = { "/dev/dri/renderD128", "/dev/dri/card2", "/dev/dri/card0" };
    for (int i = 0; i < 3; i++) {
        int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) { printf("%-22s open failed\n", nodes[i]); continue; }
        char name[64] = {0}, date[64] = {0}, desc[256] = {0};
        struct drm_version v; memset(&v, 0, sizeof(v));
        v.name = name; v.name_len = sizeof(name);
        v.date = date; v.date_len = sizeof(date);
        v.desc = desc; v.desc_len = sizeof(desc);
        int r = ioctl(fd, DRM_IOCTL_VERSION, &v);
        if (r == 0)
            printf("%-22s name='%s' version=%d.%d.%d desc='%s'\n",
                   nodes[i], name, v.version_major, v.version_minor, v.version_patchlevel, desc);
        else
            printf("%-22s DRM_IOCTL_VERSION rc=%d\n", nodes[i], r);
        close(fd);
    }
    return 0;
}
