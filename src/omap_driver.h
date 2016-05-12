/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright © 2011 Texas Instruments, Inc
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
 * Authors:
 *    Ian Elliott <ianelliottus@yahoo.com>
 *    Rob Clark <rob@ti.com>
 */

#ifndef __OMAP_DRV_H__
#define __OMAP_DRV_H__

/* All drivers need the following headers: */
#include "xorg-server.h"
#include "xf86.h"
#include "xf86_OSproc.h"

/* XXX - Perhaps, the following header files will only be used temporarily
 * (i.e. so we can use fbdevHW, SW cursor, etc):
 * XXX - figure out what can be removed..
 */
#include "mipointer.h"
#include "micmap.h"
#include "colormapst.h"
#include "xf86cmap.h"
#include "shadow.h"
/* for visuals */
#include "fb.h"
#if GET_ABI_MAJOR(ABI_VIDEODRV_VERSION) < 6
#include "xf86Resources.h"
#include "xf86RAC.h"
#endif
#include "xf86Crtc.h"
#include "xf86RandR12.h"
#include "xf86drm.h"
#include "dri2.h"

#include "omap_dumb.h"
#include "omap_msg.h"

#include <errno.h>

#include "omap_exa.h"

/* Supported chipsets */
enum OMAP_CHIPSET {
	OMAP_CHIPSET_EXYNOS5,	/* Quelle bizarre... :) */
};

#define OMAP_VERSION		1000	/* Apparently not used by X server */
#define OMAP_NAME			"ARMSOC"	/* Name used to prefix messages */
#define OMAP_DRIVER_NAME	"armsoc"	/* Driver name as used in config file */

#define OMAP_USE_PAGE_FLIP_EVENTS	1
/*#define OMAP_SUPPORT_GAMMA		1 -- Not supported on exynos*/

#define MAX_SCANOUTS		3
#define DRI2_ARMSOC_PRIVATE_CRC_DIRTY 1 /* DRI2 private buffer flag */

typedef struct _OMAPScanout
{
	struct omap_bo *bo;
	int width;
	int height;
	int x;
	int y;
	Bool valid;
} OMAPScanout, *OMAPScanoutPtr;

enum OMAPFlipMode
{
	/*
	 * Set to invalid to guarantee the next transition to flip or blit mode
	 * is run. This is useful on initialization where we don't know which
	 * mode we're going to start with, but we definitely want to transition
	 * to it.
	 */
	OMAP_FLIP_INVALID = 0,
	OMAP_FLIP_ENABLED,
	OMAP_FLIP_DISABLED,
};

/** The driver's Screen-specific, "private" data structure. */
typedef struct _OMAPRec
{
	/**
	 * Pointer to a structure used to communicate and coordinate with an
	 * external EXA library (if loaded).
	 */
	OMAPEXAPtr			pOMAPEXA;

	/** File descriptor of the connection with the DRM. */
	int					drmFD;

	char 				*deviceName;

	/** DRM device instance */
	struct omap_device	*dev;

	/** Scan-out buffer. */
	enum OMAPFlipMode	flip_mode;
	struct omap_bo		*scanout;
	OMAPScanout scanouts[MAX_SCANOUTS];

	/** Pointer to the options for this screen. */
	OptionInfoPtr		pOptionInfo;

	/** Save (wrap) the original pScreen functions. */
	CloseScreenProcPtr				SavedCloseScreen;
	CreateScreenResourcesProcPtr	SavedCreateScreenResources;
	ScreenBlockHandlerProcPtr		SavedBlockHandler;

	/** Pointer to the entity structure for this screen. */
	EntityInfoPtr		pEntityInfo;

	/** Flips we are waiting for: */
	int					pending_flips;
	/* For invalidating backbuffers on Hotplug */
	Bool			has_resized;
} OMAPRec, *OMAPPtr;

/*
 * Misc utility macros:
 */

/** Return a pointer to the driver's private structure. */
#define OMAPPTR(p) ((OMAPPtr)((p)->driverPrivate))
#define OMAPPTR_FROM_SCREEN(pScreen) \
	((OMAPPtr)(xf86ScreenToScrn(pScreen))->driverPrivate);

#define wrap(priv, real, mem, func) {\
    priv->Saved##mem = real->mem; \
    real->mem = func; \
}

#define unwrap(priv, real, mem) {\
    real->mem = priv->Saved##mem; \
}

#define swap(priv, real, mem) {\
    void *tmp = priv->Saved##mem; \
    priv->Saved##mem = real->mem; \
    real->mem = tmp; \
}

#define exchange(a, b) {\
	typeof(a) tmp = a; \
	a = b; \
	b = tmp; \
}

#ifndef ARRAY_SIZE
#  define ARRAY_SIZE(a)  (sizeof(a) / sizeof(a[0]))
#endif
#define ALIGN(val, align)	(((val) + (align) - 1) & ~((align) - 1))


/**
 * drmmode functions..
 */
Bool drmmode_pre_init(ScrnInfoPtr pScrn, int fd);
Bool drmmode_screen_init(ScrnInfoPtr pScrn);
void drmmode_close_screen(ScrnInfoPtr pScrn);
void drmmode_adjust_frame(ScrnInfoPtr pScrn, int x, int y);
int drmmode_page_flip(DrawablePtr draw, uint32_t fb_id, void *priv,
		int* num_flipped);
void drmmode_wait_for_event(ScrnInfoPtr pScrn);
void drmmode_copy_fb(ScrnInfoPtr pScrn);
OMAPScanoutPtr drmmode_scanout_from_drawable(OMAPScanoutPtr scanouts,
		DrawablePtr pDraw);
void drmmode_scanout_set(OMAPScanoutPtr scanouts, int x, int y,
		struct omap_bo *bo);
int drmmode_crtc_id_from_drawable(ScrnInfoPtr pScrn, DrawablePtr pDraw);
int drmmode_crtc_index_from_drawable(ScrnInfoPtr pScrn, DrawablePtr pDraw);
Bool drmmode_set_blit_mode(ScrnInfoPtr pScrn);
Bool drmmode_set_flip_mode(ScrnInfoPtr pScrn);
Bool drmmode_update_scanout_from_crtcs(ScrnInfoPtr pScrn);


/**
 * DRI2 functions..
 */
typedef struct _OMAPDRISwapCmd OMAPDRISwapCmd;
Bool OMAPDRI2ScreenInit(ScreenPtr pScreen);
void OMAPDRI2CloseScreen(ScreenPtr pScreen);
void OMAPDRI2SwapComplete(OMAPDRISwapCmd *cmd);

#endif /* __OMAP_DRV_H__ */
