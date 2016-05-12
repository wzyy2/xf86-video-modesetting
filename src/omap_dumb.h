/*
 * Copyright (C) 2012 ARM Limited
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
 */

#ifndef OMAP_DUMB_H_
#define OMAP_DUMB_H_

#include <stdint.h>
#include "bo.h"

struct omap_bo;
struct omap_device;

enum omap_gem_op {
	OMAP_GEM_READ = 0x01,
	OMAP_GEM_WRITE = 0x02,
};

struct omap_device {
	int fd;
	void *bo_dev;
	const struct bo_ops *ops;
	ScrnInfoPtr pScrn;
};

struct omap_bo {
	struct omap_device *dev;
	void *priv_bo;
	uint32_t handle;
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint8_t depth;
	uint8_t bpp;
	uint32_t pixel_format;
	int refcnt;
	int acquired_exclusive;
	int acquire_cnt;
	int dirty;
};

struct omap_device *omap_device_new(int fd, ScrnInfoPtr pScrn);
void omap_device_del(struct omap_device *dev);

/* Getters with side-effects all return 0 (or NULL) on failure */
uint32_t omap_bo_get_name(struct omap_bo *bo);
uint32_t omap_bo_handle(struct omap_bo *bo);
void *omap_bo_map(struct omap_bo *bo);

int omap_bo_cpu_prep(struct omap_bo *bo, enum omap_gem_op op);
int omap_bo_cpu_fini(struct omap_bo *bo, enum omap_gem_op op);
int omap_bo_get_dirty(struct omap_bo *bo);
void omap_bo_clear_dirty(struct omap_bo *bo);

struct omap_bo *omap_bo_new_with_depth(struct omap_device *dev, uint32_t width,
		uint32_t height, uint8_t depth, uint8_t bpp);
struct omap_bo *omap_bo_new_with_format(struct omap_device *dev, uint32_t width,
		uint32_t height, uint32_t pixel_format, uint8_t bpp);

/* Getters without side-effects */
uint32_t omap_bo_width(struct omap_bo *bo);
uint32_t omap_bo_height(struct omap_bo *bo);
uint32_t omap_bo_bpp(struct omap_bo *bo);
uint32_t omap_bo_Bpp(struct omap_bo *bo);
uint32_t omap_bo_pitch(struct omap_bo *bo);
uint32_t omap_bo_depth(struct omap_bo *bo);
uint32_t omap_bo_fb(struct omap_bo *bo);

void omap_bo_reference(struct omap_bo *bo);
void omap_bo_unreference(struct omap_bo *bo);

#endif /* OMAP_DUMB_H_ */
