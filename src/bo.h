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

#ifndef _BO_H_
#define _BO_H_

struct omap_bo;
struct omap_device;
enum omap_gem_op;
struct bo_ops {
	void *(*bo_create)(struct omap_device *dev,
			   size_t width, size_t height, uint32_t flags,
			   uint32_t *handle, uint32_t *pitch);
	void (*bo_destroy)(struct omap_bo *bo);
	int (*bo_get_name)(struct omap_bo *bo, uint32_t *name);
	void *(*bo_map)(struct omap_bo *bo);
	int (*bo_cpu_prep)(struct omap_bo *bo, enum omap_gem_op op);
	int (*bo_cpu_fini)(struct omap_bo *bo, enum omap_gem_op op);
};

int bo_device_init(struct omap_device *dev);
void bo_device_deinit(struct omap_device *dev);
#endif
