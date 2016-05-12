/*
 * Copyright © 2012 ARM Limited
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <sys/mman.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>

#include <xorg-server.h>
#include <xf86.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "omap_dumb.h"
#include "omap_msg.h"

/* device related functions:
 */

struct omap_device *omap_device_new(int fd, ScrnInfoPtr pScrn)
{
	struct omap_device *new_dev = calloc(1, sizeof *new_dev);
	const struct bo_ops *bo_ops;

	if (!new_dev)
		return NULL;

	new_dev->fd = fd;
	new_dev->pScrn = pScrn;

	if (!bo_device_init(new_dev))
		goto err_free_dev;

	bo_ops = new_dev->ops;

	if (!(bo_ops->bo_create && bo_ops->bo_destroy
			&& bo_ops->bo_get_name
			&& bo_ops->bo_map
			&& bo_ops->bo_cpu_prep
			&& bo_ops->bo_cpu_fini)) {
		ERROR_MSG("Omap Dev New Fail: bo_ops setting is Incomplete");
		goto err_deinit_bodev;
	}

	return new_dev;
err_deinit_bodev:
	bo_device_deinit(new_dev);
err_free_dev:
	free(new_dev);
	return NULL;
}

void omap_device_del(struct omap_device *dev)
{
	bo_device_deinit(dev);
	free(dev);
}

/* buffer-object related functions:
 */

static struct omap_bo *omap_bo_new(struct omap_device *dev, uint32_t width,
		uint32_t height, uint8_t depth, uint8_t bpp,
		uint32_t pixel_format)
{
	ScrnInfoPtr pScrn = dev->pScrn;
	const struct bo_ops *bo_ops = dev->ops;
	struct omap_bo *new_buf;
	uint32_t pitch;
	const uint32_t flags = 0;
	int ret;

	new_buf = calloc(1, sizeof(*new_buf));
	if (!new_buf)
		return NULL;

	new_buf->priv_bo = bo_ops->bo_create(dev, width, height, flags,
					     &new_buf->handle, &pitch);
	if (!new_buf->priv_bo) {
		ERROR_MSG("PLATFORM_BO_CREATE(%ux%u flags: 0x%x) failed: %s",
				width, height, flags, strerror(errno));
		goto free_buf;
	}

	if (depth) {
		ret = drmModeAddFB(dev->fd, width, height, depth,
				bpp, pitch, new_buf->handle,
				&new_buf->fb_id);
		if (ret < 0) {
			ERROR_MSG("[BO:%u] add FB {%ux%u depth: %u bpp: %u pitch: %u} failed: %s",
					new_buf->handle, width,
					height, depth, bpp, pitch,
					strerror(errno));
			goto destroy_bo;
		}
		DEBUG_MSG("Created [FB:%u] {%ux%u depth: %u bpp: %u pitch: %u} using [BO:%u]",
				new_buf->fb_id, width, height, depth, bpp,
				pitch, new_buf->handle);
	} else {
		uint32_t handles[4] = { new_buf->handle };
		uint32_t pitches[4] = { pitch };
		uint32_t offsets[4] = { 0 };

		ret = drmModeAddFB2(dev->fd, width, height,
				pixel_format, handles, pitches, offsets,
				&new_buf->fb_id, 0);
		if (ret < 0) {
			ERROR_MSG("[BO:%u] add FB {%ux%u format: %.4s pitch: %u} failed: %s",
					new_buf->handle, width,
					height, (char *)&pixel_format, pitch,
					strerror(errno));
			goto destroy_bo;
		}
		/* print pixel_format as a 'four-cc' ASCII code */
		DEBUG_MSG("[BO:%u] [FB:%u] Added FB: {%ux%u format: %.4s pitch: %u}",
				new_buf->handle, new_buf->fb_id,
				width, height, (char *)&pixel_format, pitch);
	}

	new_buf->dev = dev;
	new_buf->width = width;
	new_buf->height = height;
	new_buf->pitch = pitch;
	new_buf->depth = depth;
	new_buf->bpp = bpp;
	new_buf->pixel_format = pixel_format;
	new_buf->refcnt = 1;
	new_buf->acquired_exclusive = 0;
	new_buf->acquire_cnt = 0;
	new_buf->dirty = TRUE;

	return new_buf;

destroy_bo:
	bo_ops->bo_destroy(new_buf);
free_buf:
	free(new_buf);
	return NULL;
}

struct omap_bo *omap_bo_new_with_depth(struct omap_device *dev, uint32_t width,
		uint32_t height, uint8_t depth, uint8_t bpp)
{
	return omap_bo_new(dev, width, height, depth, bpp, 0);
}

struct omap_bo *omap_bo_new_with_format(struct omap_device *dev, uint32_t width,
		uint32_t height, uint32_t pixel_format, uint8_t bpp)
{
	return omap_bo_new(dev, width, height, 0, bpp, pixel_format);
}

static void omap_bo_del(struct omap_bo *bo)
{
	struct omap_device *dev = bo->dev;
	ScrnInfoPtr pScrn = dev->pScrn;
	int res;

	res = drmModeRmFB(dev->fd, bo->fb_id);
	if (res)
		ERROR_MSG("[BO:%u] Remove [FB:%u] failed: %s",
				bo->handle, bo->fb_id,
				strerror(errno));
	assert(res == 0);
	dev->ops->bo_destroy(bo);
	free(bo);
}

void omap_bo_unreference(struct omap_bo *bo)
{
	if (!bo)
		return;

	assert(bo->refcnt > 0);
	if (--bo->refcnt == 0)
		omap_bo_del(bo);
}

void omap_bo_reference(struct omap_bo *bo)
{
	assert(bo->refcnt > 0);
	bo->refcnt++;
}

uint32_t omap_bo_get_name(struct omap_bo *bo)
{
	struct omap_device *dev = bo->dev;
	ScrnInfoPtr pScrn = dev->pScrn;
	uint32_t name;
	int ret;

	ret = dev->ops->bo_get_name(bo, &name);
	if (ret) {
		ERROR_MSG("[BO:%u] BO_GET_NAME failed: %s",
				bo->handle, strerror(errno));
		return 0;
	}

	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] ",
			bo->handle, bo->fb_id, name);

	return name;
}

uint32_t omap_bo_handle(struct omap_bo *bo)
{
	return bo->handle;
}

uint32_t omap_bo_width(struct omap_bo *bo)
{
	return bo->width;
}

uint32_t omap_bo_height(struct omap_bo *bo)
{
	return bo->height;
}

uint32_t omap_bo_bpp(struct omap_bo *bo)
{
	return bo->bpp;
}

/* Bytes per pixel */
uint32_t omap_bo_Bpp(struct omap_bo *bo)
{
	return (bo->bpp + 7) / 8;
}

uint32_t omap_bo_pitch(struct omap_bo *bo)
{
	return bo->pitch;
}

uint32_t omap_bo_depth(struct omap_bo *bo)
{
	return bo->depth;
}

uint32_t omap_bo_fb(struct omap_bo *bo)
{
	return bo->fb_id;
}

void *omap_bo_map(struct omap_bo *bo)
{
	struct omap_device *dev = bo->dev;
	ScrnInfoPtr pScrn = dev->pScrn;
	void *map_addr;

	map_addr = dev->ops->bo_map(bo);
	if (!map_addr) {
		ERROR_MSG("[BO:%u] bo_MAP failed: %s",
				bo->handle, strerror(errno));
		return NULL;
	}

	return map_addr;
}

int omap_bo_cpu_prep(struct omap_bo *bo, enum omap_gem_op op)
{
	struct omap_device *dev = bo->dev;
	ScrnInfoPtr pScrn = dev->pScrn;
	int ret;

	if (bo->acquire_cnt) {
		if ((op & OMAP_GEM_WRITE) && !bo->acquired_exclusive) {
			ERROR_MSG("attempting to acquire read locked surface for write");
			return 1;
		}
		bo->acquire_cnt++;
		return 0;
	}

	ret = dev->ops->bo_cpu_prep(bo, op);
	if (!ret) {
		bo->acquired_exclusive = op & OMAP_GEM_WRITE;
		bo->acquire_cnt++;
		if (bo->acquired_exclusive) {
			bo->dirty = TRUE;
		}
	}

	return ret;
}

int omap_bo_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	struct omap_device *dev = bo->dev;

	assert(bo->acquire_cnt > 0);

	if (--bo->acquire_cnt != 0) {
		return 0;
	}

	return dev->ops->bo_cpu_fini(bo, op);
}

int omap_bo_get_dirty(struct omap_bo *bo)
{
	return bo->dirty;
}

void omap_bo_clear_dirty(struct omap_bo *bo)
{
	bo->dirty = FALSE;
}

