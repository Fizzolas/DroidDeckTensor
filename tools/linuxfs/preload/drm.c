/*
 * GEM handle emulation for a DRM render node that has no GEM behind it.
 *
 * Compositors and Mesa validate or convert dma-bufs with PRIME ioctls on the render node, and these
 * devices have no GEM there. Handles only serve as tokens to those callers, so a table of duplicated
 * dma-buf descriptors stands in for the kernel's, and every other device is left to libdrm.
 *
 * Two kernel drivers register such a node, and a session runs on one or the other:
 *   - KGSL, which is not a DRM node at all and is presented as one at /dev/kgsl-3d0 on an Adreno;
 *   - the PowerVR kernel driver's render node (pvrsrvkm, OF_COMPATIBLE "img,gpu") on a Tensor G5.
 * This was keyed to KGSL alone, so on a PowerVR node the guard never matched and the emulation never
 * ran: Steam's client reaches Xwayland through Mesa Zink, whose DRI3/GBM path asks for a handle per
 * buffer (drmPrimeFDToHandle <- zink_bo_get_kms_handle <- dri2_query_image <- dri_gbm), and the real
 * ioctl fails on a driver with no GEM (drm-gem(7): "a driver that does not support gem will return
 * ENODEV for all these ioctls"). The client then dies bringing its window up, without the X11 display
 * setup ever getting the token it asked for.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MAX_HANDLES 4096

/* DRM_IOCTL_GEM_CLOSE from drm.h: _IOW('d', 0x09, struct drm_gem_close { u32 handle, pad; }). */
struct gem_close {
  uint32_t handle;
  uint32_t pad;
};
#define GEM_CLOSE _IOW('d', 0x09, struct gem_close)

struct handle_entry {
  uint32_t handle;
  int dma_fd;
  int drm_fd;
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct handle_entry handle_entries[MAX_HANDLES];
static uint32_t next_handle = 1;
static dev_t kgsl_dev;
static int kgsl_state; /* 0 unknown, 1 known, -1 absent */

/*
 * KGSL, by its fixed path. Only this is decided by name; every other node is decided by asking it,
 * which is what covers the PowerVR render node without assuming a node number or a sysfs path (the
 * guest's /sys is a stand-in the app builds, so a path here would be a guess).
 */
static int is_kgsl(int fd) {
  struct stat st;
  pthread_mutex_lock(&lock);
  if (kgsl_state == 0) {
    kgsl_state = stat("/dev/kgsl-3d0", &st) == 0 ? 1 : -1;
    kgsl_dev = st.st_rdev;
  }
  pthread_mutex_unlock(&lock);
  return kgsl_state > 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == kgsl_dev;
}

/*
 * Whether the driver said it cannot do this at all, as opposed to refusing this particular request.
 * drm-gem(7): a driver without GEM answers ENODEV to every GEM ioctl; a node that has one answers
 * EINVAL for a handle it does not know. Both mean the handle space is not there to use, and both are
 * safe to stand in for, because a handle is only a token to these callers.
 */
static int no_gem_errno(int e) {
  return e == ENODEV || e == EINVAL || e == ENOTTY || e == EOPNOTSUPP;
}

static void *real(const char *name) {
  void *fn = dlsym(RTLD_NEXT, name);
  if (!fn) {
    errno = ENOSYS;
  }
  return fn;
}

static int same_file_description(int a, int b) {
  if (a == b) {
    return 1;
  }
#ifdef SYS_kcmp
  int saved_errno = errno;
  long ret = syscall(SYS_kcmp, getpid(), getpid(), 0 /* KCMP_FILE */, a, b);
  errno = saved_errno;
  return ret == 0;
#else
  return 0;
#endif
}

static int find_handle_locked(int fd, uint32_t handle) {
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (handle_entries[i].handle == handle &&
        same_file_description(handle_entries[i].drm_fd, fd)) {
      return i;
    }
  }
  return -1;
}

/*
 * The old table used its slot index as the public handle. Once #275 started freeing slots,
 * rapidly recreated swapchains immediately reused those numbers. Keep storage slots reusable but
 * make the public token monotonic so a stale handle can fail instead of aliasing a newer buffer.
 */
static uint32_t alloc_handle_locked(void) {
  for (int tries = 0; tries <= MAX_HANDLES; tries++) {
    uint32_t candidate = next_handle++;
    if (next_handle == 0) {
      next_handle = 1;
    }
    if (candidate == 0) {
      continue;
    }
    int used = 0;
    for (int i = 0; i < MAX_HANDLES; i++) {
      if (handle_entries[i].handle == candidate) {
        used = 1;
        break;
      }
    }
    if (!used) {
      return candidate;
    }
  }
  return 0;
}

int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) {
  int (*fn)(int, int, uint32_t *) = (int (*)(int, int, uint32_t *)) real("drmPrimeFDToHandle");
  if (!is_kgsl(fd)) {
    /* Ask the node first. If it has no GEM it says so for every request, and the handle is only a
     * token to the caller, so it is answered here instead. */
    int rc = fn ? fn(fd, prime_fd, handle) : -ENOSYS;
    if (!fn || !no_gem_errno(errno)) {
      return rc;
    }
  }
  int dup_fd = fcntl(prime_fd, F_DUPFD_CLOEXEC, 0);
  if (dup_fd < 0) {
    return -errno;
  }
  int ret = -ENOMEM;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_HANDLES; i++) {
    if (handle_entries[i].handle == 0) {
      uint32_t token = alloc_handle_locked();
      if (!token) {
        break;
      }
      handle_entries[i].handle = token;
      handle_entries[i].dma_fd = dup_fd;
      handle_entries[i].drm_fd = fd;
      *handle = token;
      ret = 0;
      break;
    }
  }
  pthread_mutex_unlock(&lock);
  if (ret) {
    close(dup_fd);
  }
  return ret;
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) {
  int (*fn)(int, uint32_t, uint32_t, int *) = (int (*)(int, uint32_t, uint32_t, int *)) real("drmPrimeHandleToFD");
  int ret = -EINVAL;
  int emulated = 1;
  if (!is_kgsl(fd)) {
    ret = fn ? fn(fd, handle, flags, prime_fd) : -ENOSYS;
    /* A handle this table handed out is not one the kernel knows, so a real node answers EINVAL for
     * it; anything else is the kernel's own answer and is passed straight back. */
    emulated = !fn || no_gem_errno(errno);
  }
  if (emulated) {
    ret = -EINVAL;
    pthread_mutex_lock(&lock);
    int index = find_handle_locked(fd, handle);
    if (index >= 0) {
      *prime_fd = fcntl(handle_entries[index].dma_fd, F_DUPFD_CLOEXEC, 0);
      ret = *prime_fd < 0 ? -errno : 0;
    }
    pthread_mutex_unlock(&lock);
  }
  return ret;
}

/* Drops only a handle belonging to this KGSL file description. */
static int release_handle(int fd, uint32_t handle) {
  int found = 0;
  pthread_mutex_lock(&lock);
  int index = find_handle_locked(fd, handle);
  if (index >= 0) {
    close(handle_entries[index].dma_fd);
    handle_entries[index] = (struct handle_entry){0};
    found = 1;
  }
  pthread_mutex_unlock(&lock);
  return found;
}

int drmCloseBufferHandle(int fd, uint32_t handle) {
  int (*fn)(int, uint32_t) = (int (*)(int, uint32_t)) real("drmCloseBufferHandle");
  if (!is_kgsl(fd)) {
    int rc = fn ? fn(fd, handle) : -ENOSYS;
    if (!fn || !no_gem_errno(errno)) {
      return rc;
    }
  }
  return release_handle(fd, handle) ? 0 : -EINVAL;
}

/*
 * Zink gives its handles back with the GEM_CLOSE ioctl itself, not drmCloseBufferHandle
 * (zink_bo.c, bo_destroy). KGSL refuses the ioctl, so the duplicated descriptor stayed open and
 * kept the whole buffer alive. Preserve #275's reclamation without letting another KGSL file
 * description release the token or making a new buffer immediately inherit a retired token.
 *
 * Caught at ioctl() (netif.c), which libdrm's drmIoctl calls, rather than by exporting drmIoctl:
 * every session process preloads this library, and exporting drmIoctl from it brought on a
 * judder replaying old frames in No Man's Sky on an Adreno 840 even with no handle released.
 */
__attribute__((visibility("hidden"))) int bl_drm_gem_close(int fd, unsigned long request, void *arg, int *rc) {
  if (request != GEM_CLOSE || !arg) {
    return 0;
  }
  /* Only an emulated handle is ours to release. A node with a real GEM keeps its own, and one with
   * none refuses the ioctl (KGSL, by name; a PowerVR node, by the same ENODEV/EINVAL answer as the
   * PRIME calls above). */
  if (!is_kgsl(fd)) {
    int (*fn)(int, unsigned long, void *) = (int (*)(int, unsigned long, void *)) real("ioctl");
    if (!fn || fn(fd, request, arg) != 0 || !no_gem_errno(errno)) {
      return 0;
    }
  }
  if (release_handle(fd, ((struct gem_close *) arg)->handle)) {
    *rc = 0;
  } else {
    errno = EINVAL;
    *rc = -1;
  }
  return 1;
}

/*
 * fork() carries over only the calling thread, so a lock another thread was holding at that
 * instant stays held in the child by a thread that is not there to release it. Taking it before
 * the fork makes the copy consistent; the parent then unlocks it and the child, whose one thread
 * never locked it, gets a fresh one. (WinNative 79aa7f68.)
 */
static void drm_lock_before_fork(void) { pthread_mutex_lock(&lock); }
static void drm_unlock_after_fork(void) { pthread_mutex_unlock(&lock); }
static void drm_reset_after_fork(void) { pthread_mutex_init(&lock, NULL); }

__attribute__((constructor)) static void install_drm_fork_handlers(void) {
  pthread_atfork(drm_lock_before_fork, drm_unlock_after_fork, drm_reset_after_fork);
}
