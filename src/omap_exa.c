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
 *    Rob Clark <rob@ti.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "omap_exa.h"
#include "omap_driver.h"

/* keep this here, instead of static-inline so submodule doesn't
 * need to know layout of OMAPPtr..
 */
_X_EXPORT OMAPEXAPtr
OMAPEXAPTR(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	return pOMAP->pOMAPEXA;
}

/* Common OMAP EXA functions, mostly related to pixmap/buffer allocation.
 * Individual driver submodules can use these directly, or wrap them with
 * there own functions if anything additional is required.  Submodules
 * can use OMAPPrixmapPrivPtr#priv for their own private data.
 */

/* used by DRI2 code to play buffer switcharoo */
void
OMAPPixmapExchange(PixmapPtr a, PixmapPtr b)
{
	OMAPPixmapPrivPtr apriv = exaGetPixmapDriverPrivate(a);
	OMAPPixmapPrivPtr bpriv = exaGetPixmapDriverPrivate(b);
	exchange(apriv->bo, bpriv->bo);
}

_X_EXPORT void *
OMAPCreatePixmap (ScreenPtr pScreen, int width, int height,
		int depth, int usage_hint, int bitsPerPixel,
		int *new_fb_pitch)
{
	OMAPPixmapPrivPtr priv;

	priv = calloc(1, sizeof *priv);
	/* actual allocation of buffer is in OMAPModifyPixmapHeader */

	return priv;
}

_X_EXPORT void
OMAPDestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
	OMAPPixmapPrivPtr priv = driverPriv;

	omap_bo_unreference(priv->bo);

	free(priv);
}

_X_EXPORT Bool
OMAPModifyPixmapHeader(PixmapPtr pPixmap, int width, int height,
		int depth, int bitsPerPixel, int devKind,
		pointer pPixData)
{
	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
	ScrnInfoPtr pScrn = pix2scrn(pPixmap);
	OMAPPtr pOMAP = OMAPPTR(pScrn);

	if (pPixData)
		pPixmap->devPrivate.ptr = pPixData;

	if (devKind > 0)
		pPixmap->devKind = devKind;

	/*
	 * We can't accelerate this pixmap, and don't ever want to
	 * see it again..
	 */
	if (pPixData && pPixData != omap_bo_map(pOMAP->scanout)) {
		/* scratch-pixmap (see GetScratchPixmapHeader()) gets recycled,
		 * so could have a previous bo!
		 */
		omap_bo_unreference(priv->bo);
		priv->bo = NULL;

		/* Returning FALSE calls miModifyPixmapHeader */
		return FALSE;
	}

	if (pPixData == omap_bo_map(pOMAP->scanout)) {
		omap_bo_reference(pOMAP->scanout);
		omap_bo_unreference(priv->bo);
		priv->bo = pOMAP->scanout;
	}

	if (depth > 0)
		pPixmap->drawable.depth = depth;

	if (bitsPerPixel > 0)
		pPixmap->drawable.bitsPerPixel = bitsPerPixel;

	if (width > 0)
		pPixmap->drawable.width = width;

	if (height > 0)
		pPixmap->drawable.height = height;

	/*
	 * X will sometimes create an empty pixmap (width/height == 0) and then
	 * use ModifyPixmapHeader to point it at PixData. We'll hit this path
	 * during the CreatePixmap call. Just return true and skip the allocate
	 * in this case.
	 */
	if (!pPixmap->drawable.width || !pPixmap->drawable.height)
		return TRUE;

	if (!priv->bo ||
	    omap_bo_width(priv->bo) != pPixmap->drawable.width ||
	    omap_bo_height(priv->bo) != pPixmap->drawable.height ||
	    omap_bo_bpp(priv->bo) != pPixmap->drawable.bitsPerPixel) {
		/* re-allocate buffer! */
		omap_bo_unreference(priv->bo);
		priv->bo = omap_bo_new_with_depth(pOMAP->dev,
				pPixmap->drawable.width,
				pPixmap->drawable.height,
				pPixmap->drawable.depth,
				pPixmap->drawable.bitsPerPixel);

		if (!priv->bo) {
			ERROR_MSG("failed to allocate %ux%u bo",
					pPixmap->drawable.width,
					pPixmap->drawable.height);
			return FALSE;
		}
		pPixmap->devKind = omap_bo_pitch(priv->bo);
	}

	return TRUE;
}

/**
 * WaitMarker is a required EXA callback but synchronization is
 * performed during OMAPPrepareAccess so this function does not
 * have anything to do at present
 */
_X_EXPORT void
OMAPWaitMarker(ScreenPtr pScreen, int marker)
{
	/* no-op */
}

static inline enum omap_gem_op idx2op(int index)
{
	switch (index) {
	case EXA_PREPARE_SRC:
	case EXA_PREPARE_MASK:
	case EXA_PREPARE_AUX_SRC:
	case EXA_PREPARE_AUX_MASK:
		return OMAP_GEM_READ;
	case EXA_PREPARE_AUX_DEST:
	case EXA_PREPARE_DEST:
	default:
		return OMAP_GEM_READ | OMAP_GEM_WRITE;
	}
}

/* TODO: Move to EXA core */
static const char *
exa_index_to_string(int index)
{
	switch (index) {
	case EXA_PREPARE_DEST:
		return "DEST";
	case EXA_PREPARE_SRC:
		return "SRC";
	case EXA_PREPARE_MASK:
		return "MASK";
	case EXA_PREPARE_AUX_DEST:
		return "AUX_DEST";
	case EXA_PREPARE_AUX_SRC:
		return "AUX_SRC";
	case EXA_PREPARE_AUX_MASK:
		return "AUX_MASK";
	default:
		return "unknown";
	}
}

/**
 * Returns TRUE if the bo backing a pixmap has the same dimensions as the
 * pixmap's drawable, and the same pitch as the pixmap.
 *
 * The backing bo for the root window pixmap is the root scanout
 * (pOMAP->scanout).  The dimensions of the root window and its scanout are the
 * same.
 *
 * The pixmap for un-redirected windows is also the root window pixmap.
 *
 * When swapping DRI2 buffers for a full-crtc un-clipped window, we enable
 * "flip" mode, which switches the window's pixmap's backing bo from the root
 * scanout to a per-crtc scanout.  When the root window only spans a single
 * CRTC, then the per-crtc scanout dimensions match the root scanout, and this
 * function returns TRUE.
 *
 * However, if the root window spans multiple CRTCs, then its dimensions will
 * differ from the per-crtc scanout and this function returns FALSE.
 */
static Bool has_fullsize_bo(PixmapPtr pPixmap, struct omap_bo *bo)
{
	DrawablePtr pDraw = &pPixmap->drawable;

	if (!bo)
		return FALSE;

	return (omap_bo_width(bo) == pDraw->width &&
		omap_bo_height(bo) == pDraw->height &&
		omap_bo_bpp(bo) == pDraw->bitsPerPixel &&
		omap_bo_pitch(bo) == pPixmap->devKind);
}

/**
 * PrepareAccess() is called before CPU access to an offscreen pixmap.
 *
 * @param pPix the pixmap being accessed
 * @param index the index of the pixmap being accessed.
 *
 * PrepareAccess() will be called before CPU access to an offscreen pixmap.
 * This can be used to set up hardware surfaces for byteswapping or
 * untiling, or to adjust the pixmap's devPrivate.ptr for the purpose of
 * making CPU access use a different aperture.
 *
 * The index is one of #EXA_PREPARE_DEST, #EXA_PREPARE_SRC,
 * #EXA_PREPARE_MASK, #EXA_PREPARE_AUX_DEST, #EXA_PREPARE_AUX_SRC, or
 * #EXA_PREPARE_AUX_MASK. Since only up to #EXA_NUM_PREPARE_INDICES pixmaps
 * will have PrepareAccess() called on them per operation, drivers can have
 * a small, statically-allocated space to maintain state for PrepareAccess()
 * and FinishAccess() in.  Note that PrepareAccess() is only called once per
 * pixmap and operation, regardless of whether the pixmap is used as a
 * destination and/or source, and the index may not reflect the usage.
 *
 * PrepareAccess() may fail.  An example might be the case of hardware that
 * can set up 1 or 2 surfaces for CPU access, but not 3.  If PrepareAccess()
 * fails, EXA will migrate the pixmap to system memory.
 * DownloadFromScreen() must be implemented and must not fail if a driver
 * wishes to fail in PrepareAccess().  PrepareAccess() must not fail when
 * pPix is the visible screen, because the visible screen can not be
 * migrated.
 *
 * @return TRUE if PrepareAccess() successfully prepared the pixmap for CPU
 * drawing.
 * @return FALSE if PrepareAccess() is unsuccessful and EXA should use
 * DownloadFromScreen() to migate the pixmap out.
 */
_X_EXPORT Bool
OMAPPrepareAccess(PixmapPtr pPixmap, int index)
{
	ScreenPtr pScreen = pPixmap->drawable.pScreen;
	ScrnInfoPtr pScrn = pix2scrn(pPixmap);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	PixmapPtr rootPixmap;
	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
	const enum omap_gem_op op = idx2op(index);
	Bool res = FALSE;

	TRACE_ENTER();

	if (!priv->bo)
		goto out;

	/* The root pixmap requires special handling.
	 *
	 * For writes, we never give access to the per-crtc bos, all writes
	 * must be handled in blit mode.
	 *
	 * For reads, since 2-D may access any pixels of the root pixmap we
	 * must ensure the bo we give back has the the same dimensions
	 * (and pitch).  In flip mode, the root pixmap's current backing bo
	 * will be a per-crtc bo.
	 *
	 * When using just a single CRTC, this bo will
	 * have the same dimensions as the root pixmap and we can provide
	 * direct read access to it.
	 *
	 * When using multiple CRTCs, the current per-crtc bo will have
	 * different dimensions than the root pixmap, so we have to give back
	 * the root pixmap after first updating its contents from all per-crtc
	 * bos.
	 *
	 * In all cases, we grab a lock on the per-crtc bo to hold off any
	 * updates until this read access has completed.
	 */
	rootPixmap = pScreen->GetScreenPixmap(pScreen);
	if (pPixmap != rootPixmap || priv->bo == pOMAP->scanout) {
		/* If not the root pixmap, or if the root pixmap is already
		 * backed by the root bo, just give access to the pixmap's
		 * current bo.
		 */
		pPixmap->devPrivate.ptr = omap_bo_map(priv->bo);
	} else if (op & OMAP_GEM_WRITE) {
		/* For root pixmap write access:
		 * First, switch to blit mode, which copies all valid per-crtc
		 * bo contents to the root bo.
		 * Then, switch the root pixmap's backing bo to the root bo.
		 * Finally, give back the root bo.
		 */
		if (!drmmode_set_blit_mode(pScrn))
			goto out;
		omap_bo_reference(pOMAP->scanout);
		omap_bo_unreference(priv->bo);
		priv->bo = pOMAP->scanout;
		pPixmap->devPrivate.ptr = omap_bo_map(pOMAP->scanout);
	} else if (has_fullsize_bo(pPixmap, priv->bo)) {
		/* For root pixmap read access:
		 * If current per-crtc bo has the same dimensions as the root
		 * pixmap, it is safe to allow direct read access to it.
		 * This is an optimization to allow staying in flip mode when
		 * providing 2-D read access in the common single-crtc case.
		 */
		pPixmap->devPrivate.ptr = omap_bo_map(priv->bo);
	} else {
		/* For root pixmap read access:
		 * If current per-crtc bo has different dimensions than the
		 * pixmap we cannot give back the per-crtc bo because its pitch
		 * does not match the pixmap's devKind.
		 *
		 * Our only recourse is to update the root bo from the per-crtc
		 * bos, and give back the root bo.
		 *
		 * Grabbing a read lock on the per-crtc bo ensures its contents
		 * are not updated (e.g. by the GPU) during this 2-D read.
		 */
		if (!drmmode_update_scanout_from_crtcs(pScrn)) {
			res = FALSE;
			goto out;
		}
		pPixmap->devPrivate.ptr = omap_bo_map(pOMAP->scanout);
	}

	if (!pPixmap->devPrivate.ptr)
		goto out;

	/* If this bo is exported as a dma_buf (ie it is a back buffer shared
	 * via DRI2 with the mali driver), grab a read lock on it.  This will
	 * keep mali from writing to it, even after the kernel has released its
	 * own read lock following the page flip away from this scanout to a
	 * new scanout buffer.
	 */
	if (omap_bo_cpu_prep(priv->bo, op))
		goto out;

	res = TRUE;
out:
	if (!res)
		ERROR_MSG("Unable to prepare access for EXA PIXMAP %s (%d)",
				exa_index_to_string(index), index);
	TRACE_EXIT();
	return res;
}

/**
 * FinishAccess() is called after CPU access to an offscreen pixmap.
 *
 * @param pPix the pixmap being accessed
 * @param index the index of the pixmap being accessed.
 *
 * FinishAccess() will be called after finishing CPU access of an offscreen
 * pixmap set up by PrepareAccess().  Note that the FinishAccess() will not be
 * called if PrepareAccess() failed and the pixmap was migrated out.
 */
_X_EXPORT void
OMAPFinishAccess(PixmapPtr pPixmap, int index)
{
	ScrnInfoPtr pScrn = pix2scrn(pPixmap);
	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);

	TRACE_ENTER();
	pPixmap->devPrivate.ptr = NULL;

	/* NOTE: can we use EXA migration module to track which parts of the
	 * buffer was accessed by sw, and pass that info down to kernel to
	 * do a more precise cache flush..
	 */
	omap_bo_cpu_fini(priv->bo, idx2op(index));
	TRACE_EXIT();
}

/**
 * PixmapIsOffscreen() is an optional driver replacement to
 * exaPixmapHasGpuCopy(). Set to NULL if you want the standard behaviour
 * of exaPixmapHasGpuCopy().
 *
 * @param pPix the pixmap
 * @return TRUE if the given drawable is in framebuffer memory.
 *
 * exaPixmapHasGpuCopy() is used to determine if a pixmap is in offscreen
 * memory, meaning that acceleration could probably be done to it, and that it
 * will need to be wrapped by PrepareAccess()/FinishAccess() when accessing it
 * with the CPU.
 */
_X_EXPORT Bool
OMAPPixmapIsOffscreen(PixmapPtr pPixmap)
{
	/* offscreen means in 'gpu accessible memory', not that it's off the
	 * visible screen.  We currently have no special constraints, since
	 * OMAP has a flat memory model (no separate GPU memory).  If
	 * individual EXA implementation has additional constraints, like
	 * buffer size or mapping in GPU MMU, it should wrap this function.
	 */
	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
	return priv && priv->bo;
}
