/*
 * Copyright © 2014 ROCKCHIP, Inc.
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
#include <xorg-server.h>
#include <xf86.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libkms/libkms.h>
#include "omap_dumb.h"
#include "omap_msg.h"

static void *bo_rockchip_create(struct omap_device *dev,
				size_t width, size_t height, uint32_t flags,
				uint32_t *handle, uint32_t *pitch)
{
	struct kms_driver *kms = dev->bo_dev;
	struct kms_bo *kms_bo;
	unsigned attr[7];

	attr[0] = KMS_WIDTH;
	attr[1] = width;
	attr[2] = KMS_HEIGHT;
	attr[3] = height;
	attr[4] = KMS_BO_TYPE;
	attr[5] = KMS_BO_TYPE_SCANOUT_X8R8G8B8;
	attr[6] = 0;

	if (kms_bo_create(kms, attr, &kms_bo))
		return NULL;

	if (kms_bo_get_prop(kms_bo, KMS_HANDLE, handle))
		return NULL;

	if (kms_bo_get_prop(kms_bo, KMS_PITCH, pitch))
		return NULL;

	return kms_bo;
}

static void bo_rockchip_destroy(struct omap_bo *bo)
{
	kms_bo_destroy((struct kms_bo **)&bo->priv_bo);
}

static int bo_rockchip_get_name(struct omap_bo *bo, uint32_t *name)
{
	struct drm_gem_flink req = {
		.handle = bo->handle,
	};
	int ret;

	ret = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_FLINK, &req);
	if (ret) {
		return ret;
	}

	*name = req.name;

	return 0;
}

static void *bo_rockchip_map(struct omap_bo *bo)
{
	void *map_addr;

	if (kms_bo_map(bo->priv_bo, &map_addr))
		return NULL;

	return map_addr;
}

static int bo_rockchip_cpu_prep(struct omap_bo *bo, enum omap_gem_op op)
{
	return 0;
}

static int bo_rockchip_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	return 0;
}

static const struct bo_ops bo_rockchip_ops = {
	.bo_create = bo_rockchip_create,
	.bo_destroy = bo_rockchip_destroy,
	.bo_get_name = bo_rockchip_get_name,
	.bo_map = bo_rockchip_map,
	.bo_cpu_prep = bo_rockchip_cpu_prep,
	.bo_cpu_fini = bo_rockchip_cpu_fini,
};

int bo_device_init(struct omap_device *dev)
{
	struct kms_driver *kms;
	int ret;

	ret = kms_create(dev->fd, &kms);
	if (ret || !kms)
		return FALSE;

	dev->bo_dev = kms;
	dev->ops = &bo_rockchip_ops;

	return TRUE;
}

void bo_device_deinit(struct omap_device *dev)
{
	if (dev->bo_dev)
		kms_destroy(dev->bo_dev);
}
