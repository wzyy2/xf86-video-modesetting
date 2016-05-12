/*
 * Copyright 2016 Rockchip, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Author: Jacob Chen <jacob2.chen@rock-chips.com>
 */

#ifndef RK_EXA_H_
#define RK_EXA_H_

#include <xf86.h>
#include <xf86_OSproc.h>
#include <xf86drm.h>
#include <exa.h>

#include "compat-api.h"
#include "libdrm/rockchip_drm.h"
#include "libdrm/rockchip_drmif.h"
#include "libdrm/rockchip_rga.h"

typedef struct {
    /* buffer object */
    struct dumb_bo *bo;

} RKPixmapREC, *RKPixmapPrivPtr;

Bool rkExaInit(ScreenPtr pScreen);
Bool rkExaExit(ScreenPtr pScreen);

int rkExaRGAInit(ScrnInfoPtr pScrn, ExaDriverPtr exa);
int rkExaRGAExit(ScrnInfoPtr pScrn);

#endif /* RK_EXA_H_ */
