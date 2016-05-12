/*
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <stdlib.h>
#include <sys/mman.h>
#include <assert.h>
#include <errno.h>
#include <xf86.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>
#include <libdrm/exynos_drmif.h>

#include "omap_dumb.h"
#include "omap_msg.h"

enum {
	DRM_EXYNOS_GEM_CPU_ACQUIRE_SHARED = 0x0,
	DRM_EXYNOS_GEM_CPU_ACQUIRE_EXCLUSIVE = 0x1,
};

struct drm_exynos_gem_cpu_acquire {
	unsigned int handle;
	unsigned int flags;
};

struct drm_exynos_gem_cpu_release {
	unsigned int handle;
};

/* TODO: use exynos_drm.h kernel headers (http://crosbug.com/37294) */
#define DRM_EXYNOS_GEM_CPU_ACQUIRE     0x08
#define DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE       DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_EXYNOS_GEM_CPU_ACQUIRE, struct drm_exynos_gem_cpu_acquire)
#define DRM_EXYNOS_GEM_CPU_RELEASE     0x09
#define DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE       DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_EXYNOS_GEM_CPU_RELEASE, struct drm_exynos_gem_cpu_release)

static void *bo_exynos_create(struct omap_device *dev, uint32_t width,
			uint32_t height, uint32_t flags, uint32_t *handle,
			uint32_t *pitch)
{
	struct exynos_bo *exynos_bo;
	size_t size;

	/* align to 64 bytes since Mali requires it.
	 */
	*pitch = ((((width * bpp + 7) / 8) + 63) / 64) * 64;
	size = height * (*pitch);

	flags |= EXYNOS_BO_NONCONTIG;

	exynos_bo = exynos_bo_create(dev->bo_dev, size, flags);
	*handle = exynos_bo_handle(exynos_bo);

	return exynos_bo;
}

static void bo_exynos_destroy(struct omap_bo *bo)
{
	exynos_bo_destroy(bo->priv_bo);
}

static int bo_exynos_get_name(struct omap_bo *bo, uint32_t *name)
{
	return exynos_bo_get_name(bo->priv_bo, name);
}

static void *bo_exynos_map(struct omap_bo *bo)
{
	struct exynos_bo *exynos_bo = bo->priv_bo;
	if (exynos_bo->vaddr)
		return exynos_bo->vaddr;
	return exynos_bo_map(bo->priv_bo);
}

static int bo_exynos_cpu_prep(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_exynos_gem_cpu_acquire acquire;
	int ret;

	acquire.handle = bo->handle;
	acquire.flags = (op & OMAP_GEM_WRITE)
		? DRM_EXYNOS_GEM_CPU_ACQUIRE_EXCLUSIVE
		: DRM_EXYNOS_GEM_CPU_ACQUIRE_SHARED;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE,
			&acquire);
	if (ret)
		ERROR_MSG("DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE failed: %s",
				strerror(errno));
	return ret;
}

static int bo_exynos_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_exynos_gem_cpu_release release;
	int ret;

	release.handle = bo->handle;
	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE,
			&release);
	if (ret)
		ERROR_MSG("DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE failed: %s",
				strerror(errno));
	return ret;
}

static const struct bo_ops bo_exynos_ops = {
	.bo_create = bo_exynos_create,
	.bo_destroy = bo_exynos_destroy,
	.bo_get_name = bo_exynos_get_name,
	.bo_map = bo_exynos_map,
	.bo_cpu_prep = bo_exynos_cpu_prep,
	.bo_cpu_fini = bo_exynos_cpu_fini,
};

int bo_device_init(struct omap_device *dev)
{
	struct exynos_device *new_exynos_dev;

	new_exynos_dev = exynos_device_create(dev->fd);
	if (!new_exynos_dev)
		return FALSE;

	dev->bo_dev = new_exynos_dev;
	dev->ops = &bo_exynos_ops;

	return TRUE;
}

void bo_device_deinit(struct omap_device *dev)
{
	if (dev->bo_dev)
		exynos_device_destroy(dev->bo_dev);
}
