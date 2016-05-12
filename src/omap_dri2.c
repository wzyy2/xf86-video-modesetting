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

#include "omap_driver.h"
#include "omap_exa.h"

#include <time.h>

#include "xf86drmMode.h"
#include "dri2.h"

/* any point to support earlier? */
#if DRI2INFOREC_VERSION < 4
#	error "Requires newer DRI2"
#endif

typedef struct {
	DRI2BufferRec base;

	/**
	 * Pixmap that is backing the buffer
	 *
	 * NOTE: don't track the pixmap ptr for the front buffer if it is
	 * a window.. this could get reallocated from beneath us, so we should
	 * always use draw2pix to be sure to have the correct one
	 */
	PixmapPtr pPixmap;

	/**
	 * The value of canflip() for the previous frame. Used so that we can tell
	 * whether the buffer should be re-allocated, e.g into scanout-able
	 * memory if the buffer can now be flipped.
	 *
	 * We don't want to re-allocate every frame because it is unnecessary
	 * overhead most of the time apart from when we switch from flipping
	 * to blitting or vice versa.
	 *
	 * We should bump the serial number of the drawable if canflip() returns
	 * something different to what is stored here, so that the DRI2 buffers
	 * will get re-allocated.
	 */
	int previous_canflip;

} OMAPDRI2BufferRec, *OMAPDRI2BufferPtr;

#define OMAPBUF(p)	((OMAPDRI2BufferPtr)(p))
#define DRIBUF(p)	((DRI2BufferPtr)(&(p)->base))


static inline DrawablePtr
dri2draw(DrawablePtr pDraw, DRI2BufferPtr buf)
{
	if (buf->attachment == DRI2BufferFrontLeft) {
		return pDraw;
	} else {
		return &(OMAPBUF(buf)->pPixmap->drawable);
	}
}

/*
 * Returns true if drawable is potentially flippable.
 *  A drawable may be flippable if it:
 *    (a) is a WINDOW
 *    (b) has a buffer object, and the buffer object size exactly matches
 *        the drawable size.
 *    (c) has the same dimensions as one of the scanouts
 *
 * Note: Even if a drawable may be flippable, it will not actually be flipped
 * if it is clipped.
 */
static Bool
mayflip(DrawablePtr pDraw, struct omap_bo *back_bo)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	Bool ret;

	if (pDraw->type != DRAWABLE_WINDOW) {
		ret = FALSE;
		goto out;
	}

	if (back_bo && (omap_bo_width(back_bo) != pDraw->width ||
	    omap_bo_height(back_bo) != pDraw->height)) {
		ret = FALSE;
		goto out;
	}

	if (!drmmode_scanout_from_drawable(pOMAP->scanouts, pDraw)) {
		ret = FALSE;
		goto out;
	}

	ret = TRUE;

out:
	DEBUG_MSG("pDraw %ux%u WINDOW? %d, back_bo %ux%u canflip: %d",
			pDraw->width, pDraw->height,
			(pDraw->type == DRAWABLE_WINDOW),
			(back_bo) ? omap_bo_width(back_bo) : 0,
			(back_bo) ? omap_bo_height(back_bo) : 0,
			ret);
	return ret;
}

/*
 * Returns true if drawable can be flipped.
 *  A drawable can be flipped if it:
 *    (a) is a WINDOW
 *    (b) has a buffer object, and the buffer object size exactly matches
 *        the drawable size.
 *    (c) has the same dimensions as one of the scanouts
 *    (d) has exactly one clip region
 *    (e) has exactly one clip region, and the regions dimensions match its own
 */
static Bool
canflip(DrawablePtr pDraw, struct omap_bo *back_bo)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	Bool ret;
	WindowPtr pWindow = NULL;
	BoxPtr pBox = NULL;
	int num_rects = -1;
	int width = 0;
	int height = 0;

	if (!mayflip(pDraw, back_bo)) {
		ret = FALSE;
		goto out;
	}

	pWindow = (WindowPtr)pDraw;

	num_rects = RegionNumRects(&pWindow->clipList);
	if (num_rects != 1) {
		ret = FALSE;
		goto out;
	}

	pBox = RegionRects(&pWindow->clipList);
	width = pBox->x2 - pBox->x1;
	height = pBox->y2 - pBox->y1;
	if (width != pDraw->width || height != pDraw->height) {
		ret = FALSE;
		goto out;
	}

	ret = TRUE;

out:
	DEBUG_MSG("pDraw %ux%u clipList numRects: %d rect[0]: %dx%d noclip: %d",
			pDraw->width, pDraw->height, num_rects, width, height, ret);
	return ret;
}

/**
 * Create Buffer.
 *
 * Note that 'format' is used from the client side to specify the DRI buffer
 * format, which could differ from the drawable format.  For example, the
 * drawable could be 32b RGB, but the DRI buffer some YUV format (video) or
 * perhaps lower bit depth RGB (GL).  The color conversion is handled when
 * blitting to front buffer, and page-flipping (overlay or flipchain) can
 * only be used if the display supports.
 */
static DRI2BufferPtr
OMAPDRI2CreateBuffer(DrawablePtr pDraw, unsigned int attachment,
		unsigned int format)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPDRI2BufferPtr buf;
	PixmapPtr pPixmap;
	struct omap_bo *bo;

	DEBUG_MSG("pDraw=%p, attachment=%d, format=%08x",
			pDraw, attachment, format);

	buf = calloc(1, sizeof *buf);
	if (!buf)
		return NULL;

	if (attachment == DRI2BufferFrontLeft) {
		pPixmap = draw2pix(pDraw);

		/* to do flipping, if we don't have DMM, then we need a scanout
		 * capable (physically contiguous) buffer.. this bit of gymnastics
		 * ensures that.
		 *
		 * TODO we may want to re-allocate and switch back to non-scanout
		 * buffer when client disconnects from drawable..
		 */

		pPixmap->refcnt++;
	} else {
		pPixmap = pScreen->CreatePixmap(pScreen, pDraw->width,
				pDraw->height, pDraw->depth, 0);
	}

	bo = OMAPPixmapBo(pPixmap);
	if (!bo)
	{
		ERROR_MSG("Attempting to DRI2 wrap a pixmap with no DRM buffer object backing");
		/* TODO: Returning NULL here ends up in a segfault all the way in pixman which has no backtrace. We get
		 * a more friendly segfault if we just let it be dereferenced in a few lines */
	}

	DRIBUF(buf)->attachment = attachment;
	DRIBUF(buf)->pitch = exaGetPixmapPitch(pPixmap);
	DRIBUF(buf)->cpp = pPixmap->drawable.bitsPerPixel / 8;
	DRIBUF(buf)->format = format;
	DRIBUF(buf)->flags = 0;//omap_bo_get_dirty(bo) ? DRI2_ARMSOC_PRIVATE_CRC_DIRTY : 0;
	buf->pPixmap = pPixmap;
	buf->previous_canflip = -1;

	DRIBUF(buf)->name = omap_bo_get_name(bo);
	if (!DRIBUF(buf)->name) {
		ERROR_MSG("could not get global buffer name");
		/* TODO cleanup */
		return NULL;
	}

	return DRIBUF(buf);
}

/**
 * Destroy Buffer
 *
 * TODO: depending on how flipping ends up working, we may need a refcnt or
 * something like this to defer destroying a buffer that is currently being
 * scanned out..
 */
static void
OMAPDRI2DestroyBuffer(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
	OMAPDRI2BufferPtr buf = OMAPBUF(buffer);
	/* Note: pDraw may already be deleted, so use the pPixmap here
	 * instead (since it is at least refcntd)
	 */
	ScreenPtr pScreen = buf->pPixmap->drawable.pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

	DEBUG_MSG("pDraw=%p, buffer=%p", pDraw, buffer);

	pScreen->DestroyPixmap(buf->pPixmap);

	free(buf);
}

/**
 *
 */
static void
OMAPDRI2CopyRegion(DrawablePtr pDraw, RegionPtr pRegion,
		DRI2BufferPtr pDstBuffer, DRI2BufferPtr pSrcBuffer)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	DrawablePtr pSrcDraw = dri2draw(pDraw, pSrcBuffer);
	DrawablePtr pDstDraw = dri2draw(pDraw, pDstBuffer);
	RegionPtr pCopyClip;
	GCPtr pGC;

	DEBUG_MSG("pDraw=%p, pDstBuffer=%p (%p), pSrcBuffer=%p (%p)",
			pDraw, pDstBuffer, pSrcDraw, pSrcBuffer, pDstDraw);

	pGC = GetScratchGC(pDstDraw->depth, pScreen);
	if (!pGC) {
		return;
	}

	pCopyClip = REGION_CREATE(pScreen, NULL, 0);
	RegionCopy(pCopyClip, pRegion);
	(*pGC->funcs->ChangeClip) (pGC, CT_REGION, pCopyClip, 0);
	ValidateGC(pDstDraw, pGC);

	/* If the dst is the framebuffer, and we had a way to
	 * schedule a deferred blit synchronized w/ vsync, that
	 * would be a nice thing to do utilize here to avoid
	 * tearing..  when we have sync object support for GEM
	 * buffers, I think we could do something more clever
	 * here.
	 */

	pGC->ops->CopyArea(pSrcDraw, pDstDraw, pGC,
			0, 0, pDraw->width, pDraw->height, 0, 0);

	FreeScratchGC(pGC);
}

static uint64_t gettime_us(void)
{
	struct timespec tv;

	if (clock_gettime(CLOCK_MONOTONIC, &tv))
		return 0;

	return (uint64_t)tv.tv_sec * 1000000 + tv.tv_nsec / 1000;
}

/**
 * Get current frame count and frame count timestamp, based on drawable's
 * crtc.
 */
static int
OMAPDRI2GetMSC(DrawablePtr pDraw, CARD64 *ust, CARD64 *msc)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	int crtc_index = drmmode_crtc_index_from_drawable(pScrn, pDraw);
	drmVBlank vbl = { .request = {
		.type = DRM_VBLANK_RELATIVE |
			(crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT),
		.sequence = 0,
	} };
	int ret;

	/*
	 * Drawable not full screen, use *monotonic* ust value.
	 * Note, this is slightly different than other drivers which try harder
	 * to match a Drawable to a crtc.
	 */
	if (crtc_index == -1) {
		if (ust)
			*ust = gettime_us();
		if (msc)
			*msc = 0;
		return TRUE;
	}

	ret = drmWaitVBlank(pOMAP->drmFD, &vbl);
	if (ret) {
		static int limit = 5;
		if (limit) {
			ERROR_MSG("get vblank counter failed: %s",
				  strerror(errno));
			limit--;
		}
		return FALSE;
	}

	if (ust) {
		*ust = ((CARD64)vbl.reply.tval_sec * 1000000) + vbl.reply.tval_usec;
	}
	if (msc) {
		*msc = vbl.reply.sequence;
	}

	return TRUE;
}

#define OMAP_SWAP_FAKE_FLIP (1 << 0)
#define OMAP_SWAP_FAIL      (1 << 1)

struct _OMAPDRISwapCmd {
	int type;
	ClientPtr client;
	ScreenPtr pScreen;
	/* Note: store drawable ID, rather than drawable.  It's possible that
	 * the drawable can be destroyed while we wait for page flip event:
	 */
	XID draw_id;
	PixmapPtr pDstPixmap;
	PixmapPtr pSrcPixmap;
	DRI2SwapEventPtr func;
	int swapCount;
	int flags;
	int x;
	int y;
	void *data;
};

void
OMAPDRI2SwapComplete(OMAPDRISwapCmd *cmd)
{
	ScreenPtr pScreen = cmd->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	DrawablePtr pDraw = NULL;
	int status, i;
	OMAPPixmapPrivPtr dst_priv;

	if (--cmd->swapCount > 0)
		return;

	if ((cmd->flags & OMAP_SWAP_FAIL) == 0) {
		status = dixLookupDrawable(&pDraw, cmd->draw_id, serverClient,
				M_ANY, DixWriteAccess);

		if (status == Success) {
			if (cmd->type != DRI2_BLIT_COMPLETE && (cmd->flags & OMAP_SWAP_FAKE_FLIP) == 0) {
				assert(cmd->type == DRI2_FLIP_COMPLETE);
				OMAPPixmapExchange(cmd->pSrcPixmap, cmd->pDstPixmap);
			}

			DRI2SwapComplete(cmd->client, pDraw, 0, 0, 0, cmd->type,
					cmd->func, cmd->data);

			if (cmd->type == DRI2_BLIT_COMPLETE) {
				/* For blits, invalidate the per-crtc scanouts.
				 */
				for (i = 0; i < MAX_SCANOUTS; i++) {
					pOMAP->scanouts[i].valid = FALSE;
				}
			} else {
				assert(cmd->type == DRI2_FLIP_COMPLETE);
				dst_priv = exaGetPixmapDriverPrivate(cmd->pDstPixmap);
				/* For flips, validate the per-crtc scanout.
				 */
				for (i = 0; i < MAX_SCANOUTS; i++) {
					if (pOMAP->scanouts[i].bo == dst_priv->bo) {
						pOMAP->scanouts[i].valid = TRUE;
						break;
					}
				}
				if ((cmd->flags & OMAP_SWAP_FAKE_FLIP) == 0) {
					drmmode_scanout_set(pOMAP->scanouts, cmd->x, cmd->y, dst_priv->bo);
				}
			}
		}
	}

	/* drop extra refcnt we obtained prior to swap:
	 */
	pScreen->DestroyPixmap(cmd->pSrcPixmap);
	pScreen->DestroyPixmap(cmd->pDstPixmap);
	if (cmd->type != DRI2_BLIT_COMPLETE) {
		pOMAP->pending_flips--;
	}

	free(cmd);
}

/**
 * ScheduleSwap is responsible for requesting a DRM vblank event for the
 * appropriate frame.
 *
 * In the case of a blit (e.g. for a windowed swap) or buffer exchange,
 * the vblank requested can simply be the last queued swap frame + the swap
 * interval for the drawable.
 *
 * In the case of a page flip, we request an event for the last queued swap
 * frame + swap interval - 1, since we'll need to queue the flip for the frame
 * immediately following the received event.
 */
static int
OMAPDRI2ScheduleSwap(ClientPtr client, DrawablePtr pDraw,
		DRI2BufferPtr pDstBuffer, DRI2BufferPtr pSrcBuffer,
		CARD64 *target_msc, CARD64 divisor, CARD64 remainder,
		DRI2SwapEventPtr func, void *data)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	OMAPDRI2BufferPtr src = OMAPBUF(pSrcBuffer);
	OMAPDRI2BufferPtr dst = OMAPBUF(pDstBuffer);
	OMAPDRISwapCmd *cmd;
	OMAPPixmapPrivPtr src_priv, dst_priv;
	int new_canflip, ret, num_flipped;
	RegionRec region;

	cmd = calloc(1, sizeof *cmd);
	if (!cmd)
		return FALSE;

	cmd->client = client;
	cmd->pScreen = pScreen;
	cmd->draw_id = pDraw->id;
	cmd->pSrcPixmap = draw2pix(dri2draw(pDraw, pSrcBuffer));
	cmd->pDstPixmap = draw2pix(dri2draw(pDraw, pDstBuffer));
	cmd->swapCount = 0;
	cmd->flags = 0;
	cmd->func = func;
	cmd->data = data;
	cmd->x = pDraw->x;
	cmd->y = pDraw->y;

	region.extents.x1 = region.extents.y1 = 0;
	region.extents.x2 = cmd->pDstPixmap->drawable.width;
	region.extents.y2 = cmd->pDstPixmap->drawable.height;
	region.data = NULL;
	DamageRegionAppend(&cmd->pDstPixmap->drawable, &region);

	DEBUG_MSG("%d -> %d", pSrcBuffer->attachment, pDstBuffer->attachment);

	src_priv = exaGetPixmapDriverPrivate(src->pPixmap);
	dst_priv = exaGetPixmapDriverPrivate(dst->pPixmap);

	/* src bo was just rendered to by GPU so it is not dirty */
	omap_bo_clear_dirty(src_priv->bo);
	new_canflip = canflip(pDraw, src_priv->bo);

	/* If we can flip using a crtc scanout, switch the front buffer bo */
	if (new_canflip && !pOMAP->has_resized) {
		struct omap_bo *old_bo;

		old_bo = dst_priv->bo;
		dst_priv->bo = drmmode_scanout_from_drawable(pOMAP->scanouts,
				pDraw)->bo;
		omap_bo_reference(dst_priv->bo);
		if (!drmmode_set_flip_mode(pScrn)) {
			ERROR_MSG("Could not set flip mode");
			new_canflip = FALSE;
			omap_bo_unreference(dst_priv->bo);
			dst_priv->bo = old_bo;
		} else {
			omap_bo_unreference(old_bo);
		}
	} else {
		struct omap_bo *old_bo;

		old_bo = dst_priv->bo;
		omap_bo_reference(pOMAP->scanout);
		dst_priv->bo = pOMAP->scanout;
		if (!drmmode_set_blit_mode(pScrn)) {
			ERROR_MSG("Could not set blit mode");
			omap_bo_unreference(pOMAP->scanout);
			DamageRegionProcessPending(&cmd->pDstPixmap->drawable);
			return FALSE;
		}
		omap_bo_unreference(old_bo);
	}
	DamageRegionProcessPending(&cmd->pDstPixmap->drawable);

	/* obtain extra ref on pixmaps to avoid them going away while we await
	 * the page flip event:
	 */
	cmd->pSrcPixmap->refcnt++;
	cmd->pDstPixmap->refcnt++;

	if ((src->previous_canflip != -1 && src->previous_canflip != new_canflip) ||
	    (dst->previous_canflip != -1 && dst->previous_canflip != new_canflip) ||
	    (pOMAP->has_resized))
	{
		/* The drawable has transitioned between being flippable and non-flippable
		 * or vice versa. Bump the serial number to force the DRI2 buffers to be
		 * re-allocated during the next frame so that:
		 * - It is able to be scanned out (if drawable is now flippable), or
		 * - It is not taking up possibly scarce scanout-able memory (if drawable
		 * is now not flippable)
		 *
		 * has_resized: On hotplugging back buffer needs to be invalidates as well
		 * as Xsever invalidates only the front buffer.
		 */

		PixmapPtr pPix = pScreen->GetWindowPixmap((WindowPtr)pDraw);
		pPix->drawable.serialNumber = NEXT_SERIAL_NUMBER;
	}

	src->previous_canflip = new_canflip;
	dst->previous_canflip = new_canflip;

	if (new_canflip && !(pOMAP->has_resized)) {
		uint32_t src_fb_id;

		/* has_resized: On hotplug the fb size and crtc sizes arent updated
		* hence on this event we do a copyb but flip from the next frame
		* when the sizes are updated.
		*/
		src_fb_id = omap_bo_fb(src_priv->bo);
		DEBUG_MSG("can flip:  %d", src_fb_id);
		cmd->type = DRI2_FLIP_COMPLETE;
		/* TODO: handle rollback if only multiple CRTC flip is only partially successful
		 */
		pOMAP->pending_flips++;
		ret = drmmode_page_flip(pDraw, src_fb_id, cmd, &num_flipped);

		/* If using page flip events, we'll trigger an immediate completion in
		 * the case that no CRTCs were enabled to be flipped.  If not using page
		 * flip events, trigger immediate completion unconditionally.
		 */
		if (ret) {
			/*
			 * Error while flipping; bail.
			 */
			cmd->flags |= OMAP_SWAP_FAIL;
#if !OMAP_USE_PAGE_FLIP_EVENTS
			cmd->swapCount = 0;
#else
			cmd->swapCount = num_flipped;
			if (cmd->swapCount == 0)
#endif
			{
				OMAPDRI2SwapComplete(cmd);
			}
			return FALSE;
		} else {
			if (num_flipped == 0)
				cmd->flags |= OMAP_SWAP_FAKE_FLIP;
#if !OMAP_USE_PAGE_FLIP_EVENTS
			cmd->swapCount = 0;
#else
			cmd->swapCount = num_flipped;
			if (cmd->swapCount == 0)
#endif
			{
				OMAPDRI2SwapComplete(cmd);
			}
		}
	} else {
		/* fallback to blit: */
		BoxRec box = {
				.x1 = 0,
				.y1 = 0,
				.x2 = pDraw->width,
				.y2 = pDraw->height,
		};
		RegionRec region;
		RegionInit(&region, &box, 0);
		OMAPDRI2CopyRegion(pDraw, &region, pDstBuffer, pSrcBuffer);
		cmd->type = DRI2_BLIT_COMPLETE;
		OMAPDRI2SwapComplete(cmd);
		pOMAP->has_resized = FALSE;
	}

	return TRUE;
}

/**
 * Request a DRM event when the requested conditions will be satisfied.
 *
 * We need to handle the event and ask the server to wake up the client when
 * we receive it.
 */
static int
OMAPDRI2ScheduleWaitMSC(ClientPtr client, DrawablePtr pDraw, CARD64 target_msc,
		CARD64 divisor, CARD64 remainder)
{
	ScreenPtr pScreen = pDraw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);

	ERROR_MSG("not implemented");
	return FALSE;
}

/**
 * Sync up X's view of a DRI2BufferPtr with our internal reckoning of it.
 *
 * We do some BO renaming and other tricksy businesses that X needs to know
 * about.  Do the sync-up here.
 */
static void
OMAPDRI2ReuseBufferNotify(DrawablePtr pDraw, DRI2BufferPtr buffer)
{
	OMAPDRI2BufferPtr omap_buffer = OMAPBUF(buffer);
	PixmapPtr pPixmap = omap_buffer->pPixmap;
	OMAPPixmapPrivPtr omap_priv = exaGetPixmapDriverPrivate(pPixmap);

	buffer->name = omap_bo_get_name(omap_priv->bo);
	buffer->flags = 0;// omap_bo_get_dirty(omap_priv->bo) ? DRI2_ARMSOC_PRIVATE_CRC_DIRTY : 0;
}

/**
 * The DRI2 ScreenInit() function.. register our handler fxns w/ DRI2 core
 */
Bool
OMAPDRI2ScreenInit(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	DRI2InfoRec info = {
			.version           = 6,
			.fd                = pOMAP->drmFD,
			.driverName        = "armsoc",
			.deviceName        = pOMAP->deviceName,
			.CreateBuffer      = &OMAPDRI2CreateBuffer,
			.DestroyBuffer     = &OMAPDRI2DestroyBuffer,
			.CopyRegion        = &OMAPDRI2CopyRegion,
			.Wait              = NULL,
			.ScheduleSwap      = &OMAPDRI2ScheduleSwap,
			.GetMSC            = &OMAPDRI2GetMSC,
			.ScheduleWaitMSC   = &OMAPDRI2ScheduleWaitMSC,
			.numDrivers        = 0,
			.driverNames       = NULL,
			.AuthMagic         = &drmAuthMagic,
			.ReuseBufferNotify = &OMAPDRI2ReuseBufferNotify,
			.SwapLimitValidate = NULL,
	};
	int minor = 1, major = 0;

	if (xf86LoaderCheckSymbol("DRI2Version")) {
		DRI2Version(&major, &minor);
	}

	if (minor < 1) {
		WARNING_MSG("DRI2 requires DRI2 module version 1.1.0 or later");
		return FALSE;
	}

	return DRI2ScreenInit(pScreen, &info);
}

/**
 * The DRI2 CloseScreen() function.. unregister ourself w/ DRI2 core.
 */
void
OMAPDRI2CloseScreen(ScreenPtr pScreen)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	while (pOMAP->pending_flips > 0) {
		DEBUG_MSG("waiting..");
		drmmode_wait_for_event(pScrn);
	}
	DRI2CloseScreen(pScreen);
}
