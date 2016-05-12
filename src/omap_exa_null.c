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

#include "exa.h"

/* for test */
 struct rockchip_device 	*rkdev;
 struct rga_context *rkctx;
 int rkfd;

#include "libdrm/rockchip_drm.h"
#include "libdrm/rockchip_drmif.h"
#include "libdrm/rockchip_rga.h"
#include <xf86drm.h>
#include <drm_fourcc.h>
#include "fbpict.h"

/* This file has a trivial EXA implementation which accelerates nothing.  It
 * is used as the fall-back in case the EXA implementation for the current
 * chipset is not available.  (For example, on chipsets which used the closed
 * source IMG PowerVR EXA implementation, if the closed-source submodule is
 * not installed.
 */

 typedef struct {
 	OMAPEXARec base;
 	ExaDriverPtr exa;
	/* add any other driver private data here.. */
 } OMAPNullEXARec, *OMAPNullEXAPtr;

 typedef struct
 {
 	struct rga_image solid_img;

 	int src_fd;
 	GCPtr pGC;
 } SoildBox;

 typedef struct
 {
 	struct rga_image src_img;
 	struct rga_image dst_img;

 	Pixel pm;
 	int alu;
 	int reverse;
 	int upsidedown;
 	PixmapPtr pSrcPixmap;
 	PixmapPtr pDstPixmap;
 } CopyBox;

 typedef struct
 {
 	struct rga_image src_img;
 	struct rga_image mask_img;    
 	struct rga_image dst_img;

	int op;

	PicturePtr pSrcPicture;
	PicturePtr pMaskPicture;
	PicturePtr pDstPicture;
	PixmapPtr pSrcPixmap;
	PixmapPtr pMaskPixmap;
	PixmapPtr pDstPixmap;

 	char srcRepeat;
 	char srcRotate;
 	double srcScaleX;
 	double srcScaleY;

 	char maskRepeat;
 	char maskRotate;
 	double maskScaleX;
 	double maskScaleY;

 } CompositeBox;

 static SoildBox gSoildBox;
 static CopyBox gCopyBox;
 static CompositeBox gCompositeBox;

 static Bool
 RGACheckPicture(PicturePtr pPicture, char *rot90, double *scaleX, double *scaleY, char* repeat)
 {
 	struct pixman_transform* t;

// #define EPSILON (pixman_fixed_t) (2)

// #define IS_SAME(a, b) (_g2d_check_within_epsilon (a, b, EPSILON))
// #define IS_ZERO(a)    (_g2d_check_within_epsilon (a, 0, EPSILON))
// #define IS_ONE(a)     (_g2d_check_within_epsilon (a, F (1), EPSILON))
// #define IS_UNIT(a)                          \
//                                     (_g2d_check_within_epsilon (a, F (1), EPSILON) ||  \
//                                     _g2d_check_within_epsilon (a, F (-1), EPSILON) || \
//                                     IS_ZERO (a))
// #define IS_INT(a)    (IS_ZERO (pixman_fixed_frac (a)))

/*RepeatNormal*/

 	if(pPicture == NULL)
 	{
 		return TRUE;
 	}

    // if(pPicture->repeat)
    // {
    //     switch(pPicture->repeatType)
    //     {
    //     case RepeatNormal:
    //         *repeat = G2D_REPEAT_MODE_REPEAT;
    //         break;
    //     case RepeatPad:
    //         *repeat = G2D_REPEAT_MODE_PAD;
    //         break;
    //     case RepeatReflect:
    //         *repeat = G2D_REPEAT_MODE_REFLECT;
    //         break;
    //     default:
    //         *repeat = G2D_REPEAT_MODE_NONE;
    //         break;
    //     }
    // }
    // else
    // {
    //     *repeat = G2D_REPEAT_MODE_NONE;
    // }

 	if(pPicture->transform == NULL)
 	{
 		*rot90 = 0;
 		*scaleX = 1.0;
 		*scaleY = 1.0;
 		return TRUE;
 	}

 	t= pPicture->transform;

 	if(!IS_ZERO(t->matrix[0][0]) && IS_ZERO(t->matrix[0][1]) && IS_ZERO(t->matrix[1][0]) && !IS_ZERO(t->matrix[1][1]))
 	{
 		*rot90 = FALSE;
 		*scaleX = pixman_fixed_to_double(t->matrix[0][0]);
 		*scaleY = pixman_fixed_to_double(t->matrix[1][1]);
 	}
 	else if(IS_ZERO(t->matrix[0][0]) && !IS_ZERO(t->matrix[0][1]) && !IS_ZERO(t->matrix[1][0]) && IS_ZERO(t->matrix[1][1]))
 	{
        /* FIMG2D 90 => PIXMAN 270 */
 		*rot90 = TRUE;
 		*scaleX = pixman_fixed_to_double(t->matrix[0][1]);
 		*scaleY = pixman_fixed_to_double(t->matrix[1][0]*-1);
 	}
 	else
 	{
 		return FALSE;
 	}

 	return TRUE;
 }

 static Bool
 RGAIsSupport(PixmapPtr pPix, int Mask)
 {
 	OMAPPixmapPrivPtr priv;

 	if(!pPix) return TRUE;

 	if(Mask != -1)
 		return FALSE;

 	if (pPix->drawable.width < 34 || pPix->drawable.height < 34)
 		return FALSE;

 	if (pPix->drawable.depth != 32 && pPix->drawable.depth != 24)
 		return FALSE;

 	priv = exaGetPixmapDriverPrivate(pPix);
 	if(!priv->bo)
 		return FALSE;

 	return TRUE;
 }

 static int
 RGAGetMode(PixmapPtr pPix)
 {
 	int mode = DRM_FORMAT_XRGB8888;

 	switch (pPix->drawable.depth) {
 		case 32:
 		mode = DRM_FORMAT_ARGB8888;
 		break;
 		case 24:
 		if (pPix->drawable.bitsPerPixel == 32)
 			mode = DRM_FORMAT_XRGB8888;
 		else
 			mode = DRM_FORMAT_RGB888;
 		break;
 	}

 	return mode;
 }

 static Bool
 RKExaRGAPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fill_colour)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
 	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
 	ChangeGCVal tmpval[3];
 	int mode, src_fd;

    /* Check capability */
 	if (!RGAIsSupport(pPixmap, planemask))
 	{
 		return FALSE;
 	}


 	drmPrimeHandleToFD(rkfd, priv->bo->handle, 0, &src_fd);

 	gSoildBox.pGC = GetScratchGC (pPixmap->drawable.depth, pPixmap->drawable.pScreen);
 	tmpval[0].val = alu;
 	tmpval[1].val = planemask;
 	tmpval[2].val = fill_colour;
 	ChangeGC (NullClient, gSoildBox.pGC, GCFunction|GCPlaneMask|GCForeground, tmpval);
 	ValidateGC (&pPixmap->drawable, gSoildBox.pGC);

 	fill_colour =
 	(fill_colour & 0xff00ff00) + ((fill_colour & 0xff) << 16) +
 	((fill_colour & 0xff0000) >> 16);

 	gSoildBox.solid_img.bo[0] = src_fd;
 	gSoildBox.solid_img.width = pPixmap->drawable.width;
 	gSoildBox.solid_img.height = pPixmap->drawable.height;
 	gSoildBox.solid_img.stride = pPixmap->devKind;
 	gSoildBox.solid_img.buf_type = RGA_IMGBUF_GEM;
 	gSoildBox.solid_img.color_mode = RGAGetMode(pPixmap);
 	gSoildBox.solid_img.fill_color = fill_colour;
 	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s ptr %x  alu %d planemask %x fill_color %x depth %d width %d height %d stride %d\n",
 		__func__,  omap_bo_map(priv->bo), alu, planemask, fill_colour, pPixmap->drawable.depth, gSoildBox.solid_img.width, gSoildBox.solid_img.height, gSoildBox.solid_img.stride);

 	return TRUE;
 }

 static void RKExaRGASolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
 	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
 	int width = x2 - x1,  height = y2 - y1;

 	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s  ptr %x x1 %d y1 %d x2 %d y2 %d devkind %d\n",
 		__func__, pPixmap->devPrivate.ptr, x1, y1, x2, y2, pPixmap->devKind);

 	if ( width < 34 || height < 34 ) {
 		// Access
 		pPixmap->devPrivate.ptr  = omap_bo_map(priv->bo);

 		fbFill (&pPixmap->drawable,
 			gSoildBox.pGC,
 			x1,
 			y1,
 			x2 - x1, y2 - y1);
 		// Finish Access
 		pPixmap->devPrivate.ptr  = NULL;
 	} else    {
 		rga_solid_fill(rkctx, &gSoildBox.solid_img, x1, y1, x2 - x1, y2 - y1);

                /* if too many cmd was send and we don't exec immediately, out of memory error will happend */
 		rga_exec(rkctx);

 	}

 }

 static void RKExaRGADoneSolid(PixmapPtr pPixmap)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);

    //    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n",
    //               __func__);
 	rga_exec(rkctx);
 	close(gSoildBox.solid_img.bo[0]);
 }



 static Bool
 RKExaRGAPrepareCopy(PixmapPtr pSrc, PixmapPtr pDst, int xdir, int ydir,
 	int alu, Pixel planemask)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pSrc->drawable.pScreen);

 	OMAPPixmapPrivPtr priv_src = exaGetPixmapDriverPrivate(pSrc);
 	OMAPPixmapPrivPtr priv_dst = exaGetPixmapDriverPrivate(pDst);
 	int mode, src_fd, dst_fd;

    /* Check capability */
 	if (!RGAIsSupport(pSrc, planemask) ||
 		!RGAIsSupport(pDst, planemask) ||
 		!xdir || !ydir || alu != 0x03)
 	{
 		return FALSE;
 	}

 	gCopyBox.alu = alu;
 	gCopyBox.pm = planemask;
 	gCopyBox.reverse = (xdir == 1)?0:1;
 	gCopyBox.upsidedown = (ydir == 1)?0:1;
 	gCopyBox.pSrcPixmap = pSrc;
 	gCopyBox.pDstPixmap = pDst;

 	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
 		"> %s xdir %d ydir %d  srcdepth %d dstdepth %d srcbo %x dstbo %x\n",
 		__func__, xdir, ydir, pSrc->drawable.depth,
 		pDst->drawable.depth, priv_src->bo->handle,
 		priv_dst->bo->handle);

 	drmPrimeHandleToFD(rkfd, priv_src->bo->handle, 0, &src_fd);
 	drmPrimeHandleToFD(rkfd, priv_dst->bo->handle, 0, &dst_fd);

 	gCopyBox.src_img.bo[0] = src_fd;
 	gCopyBox.src_img.width = pSrc->drawable.width;
 	gCopyBox.src_img.height = pSrc->drawable.height;
 	gCopyBox.src_img.stride = pSrc->devKind;
 	gCopyBox.src_img.buf_type = RGA_IMGBUF_GEM;
 	gCopyBox.src_img.color_mode = RGAGetMode(pSrc);

 	gCopyBox.dst_img.bo[0] = dst_fd;
 	gCopyBox.dst_img.width = pDst->drawable.width;
 	gCopyBox.dst_img.height = pDst->drawable.height;
 	gCopyBox.dst_img.stride = pDst->devKind;
 	gCopyBox.dst_img.buf_type = RGA_IMGBUF_GEM;
 	gCopyBox.dst_img.color_mode = RGAGetMode(pDst);

 	return TRUE;
 }

 static void RKExaRGACopySW(PixmapPtr pDstPixmap, int srcX, int srcY,
 	int dstX, int dstY, int width, int height)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
 	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
 	OMAPPixmapPrivPtr priv2 = exaGetPixmapDriverPrivate(gCopyBox.pSrcPixmap);
 	OMAPPixmapPrivPtr priv3 = exaGetPixmapDriverPrivate(gCopyBox.pDstPixmap);


 	dumb_bo_map(rkfd, priv3->bo);
 	dumb_bo_map(rkfd, priv2->bo);
 	unsigned int *begin = omap_bo_map(priv3->bo);
 	unsigned int *beginsrc = omap_bo_map(priv2->bo);
 	for (int i = 0; i < height; i += 1) {
 		for (int a = 0; a < width; a += 1) {
 			begin[dstX + a +
 				(dstY + i) * gCopyBox.pDstPixmap->drawable.width] =
 			beginsrc[srcX + a +
 				(srcY +
 					i) * gCopyBox.pSrcPixmap->drawable.width];
 		}
 	}    
 }

 static void RKExaRGACopy(PixmapPtr pDstPixmap, int srcX, int srcY,
 	int dstX, int dstY, int width, int height)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
 	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
 	OMAPPixmapPrivPtr priv2 = exaGetPixmapDriverPrivate(gCopyBox.pSrcPixmap);
 	OMAPPixmapPrivPtr priv3 = exaGetPixmapDriverPrivate(gCopyBox.pDstPixmap);



 	if (width < 34 || height < 34) {
        //RKExaRGACopySW(pDstPixmap, srcX, srcY, dstX, dstY, width, height);
 		CARD8 alu = gCopyBox.alu;
 		FbBits pm = gCopyBox.pm;
 		FbBits *src;
 		FbStride srcStride;
 		int	srcBpp;
 		FbBits *dst;
 		FbStride dstStride;
 		int	dstBpp;
 		int	srcXoff, srcYoff;
 		int	dstXoff, dstYoff;

 		gCopyBox.pSrcPixmap->devPrivate.ptr  = omap_bo_map(priv2->bo);
 		gCopyBox.pDstPixmap->devPrivate.ptr  = omap_bo_map(priv3->bo);

 		fbGetDrawable (&(gCopyBox.pSrcPixmap->drawable), src, srcStride, srcBpp, srcXoff, srcYoff);
 		fbGetDrawable (&(gCopyBox.pDstPixmap->drawable), dst, dstStride, dstBpp, dstXoff, dstYoff);
 		gCopyBox.pSrcPixmap->devPrivate.ptr  = NULL;
 		gCopyBox.pDstPixmap->devPrivate.ptr  = NULL;
 		fbBlt (src + srcY * srcStride,
 			srcStride,
 			srcX * srcBpp,

 			dst + dstY * dstStride,
 			dstStride,
 			dstX * dstBpp,

 			width * dstBpp,
 			height,

 			alu,
 			pm,
 			dstBpp,

 			gCopyBox.reverse,
 			gCopyBox.upsidedown);


 	} else  {
 		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
 			"> %s srcX %d srcY %d dstX %d dstY %d width %d height %d\n",
 			__func__, srcX, srcY, dstX, dstY, width, height);

 		rga_copy(rkctx, &gCopyBox.src_img, &gCopyBox.dst_img, srcX, srcY,
 			dstX, dstY, width, height);

        /* if too many cmd was send and we don't exec immediately, out of memory error will happend */
 		rga_exec(rkctx);
 	}
 }

 static void RKExaRGADoneCopy(PixmapPtr pDstPixmap)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    //        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n",
    //                   __func__);
 	rga_exec(rkctx);

 	close(gCopyBox.src_img.bo[0]);
 	close(gCopyBox.dst_img.bo[0]);
 }

 static struct rga_image compostie_scr_img;
 static struct rga_image compostie_dst_img;
 static Bool
 RKExaRGACheckComposite(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
 	PicturePtr pDstPicture)
 {
 	return TRUE;
 }

 static Bool
 RKExaRGAPrepareComposite(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
 	PicturePtr pDstPicture, PixmapPtr pSrc,
 	PixmapPtr pMask, PixmapPtr pDst)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pSrc->drawable.pScreen);
 	OMAPPixmapPrivPtr priv_src = exaGetPixmapDriverPrivate(pSrc);
 	OMAPPixmapPrivPtr priv_mask = exaGetPixmapDriverPrivate(pMask);    
 	OMAPPixmapPrivPtr priv_dst = exaGetPixmapDriverPrivate(pDst);
 	int mode, src_fd, dst_fd;

 	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
 		"> %s op %d\n",
 		__func__, op);

    /* Check capability */
 	if (!RGAIsSupport(pSrc, -1)||
 		pMask != NULL ||
 		!RGAIsSupport(pDst, -1))
 	{
 		return FALSE;
 	}


 	if (!RGACheckPicture(pSrcPicture,
 		&gCompositeBox.srcRotate,
 		&gCompositeBox.srcScaleX,
 		&gCompositeBox.srcScaleY,
 		&gCompositeBox.srcRepeat))
 	{
 		return FALSE;
 	}

 	xf86DrvMsg(pScrn->scrnIndex, X_INFO,
 		"> %s srcRotate %d srcScaleX %d srcScaleY %d \n",
 		__func__, gCompositeBox.srcRotate, gCompositeBox.srcScaleX, gCompositeBox.srcScaleY);

 	return FALSE;

 	drmPrimeHandleToFD(rkfd, priv_src->bo->handle, 0, &src_fd);
    // drmPrimeHandleToFD(ms->fd, priv_mask->bo->handle, 0, &mask_fd);    
 	drmPrimeHandleToFD(rkfd, priv_dst->bo->handle, 0, &dst_fd);

 	gCompositeBox.op = op;

 	gCompositeBox.pSrcPicture = pSrcPicture;
  	gCompositeBox.pMaskPicture = pMaskPicture;
 	gCompositeBox.pDstPicture = pDstPicture;  	
 	gCompositeBox.pSrcPixmap = pSrc;  	
 	gCompositeBox.pMaskPixmap = pMask;  
 	gCompositeBox.pDstPixmap = pDst;  


 	gCompositeBox.src_img.bo[0] = src_fd;
 	gCompositeBox.src_img.width = pSrc->drawable.width;
 	gCompositeBox.src_img.height = pSrc->drawable.height;
 	gCompositeBox.src_img.stride = pSrc->drawable.width * 4;
 	gCompositeBox.src_img.buf_type = RGA_IMGBUF_GEM;
 	gCompositeBox.src_img.color_mode = RGAGetMode(pSrc);


    // gCompositeBox.mask_img.bo[0] = mask_fd;
    // gCompositeBox.mask_img.width = pMask->drawable.width;
    // gCompositeBox.mask_img.height = pMask->drawable.height;
    // gCompositeBox.mask_img.stride = pMask->drawable.width * 4;
    // gCompositeBox.mask_img.buf_type = RGA_IMGBUF_GEM;
    // gCompositeBox.mask_img.color_mode = RGAGetMode(pMask);

 	gCompositeBox.dst_img.bo[0] = dst_fd;
 	gCompositeBox.dst_img.width = pDst->drawable.width;
 	gCompositeBox.dst_img.height = pDst->drawable.height;
 	gCompositeBox.dst_img.stride = pDst->drawable.width * 4;
 	gCompositeBox.dst_img.buf_type = RGA_IMGBUF_GEM;
 	gCompositeBox.dst_img.color_mode = RGAGetMode(pDst);

 	return FALSE;
 }

 static void
 RKExaRGAComposite(PixmapPtr pDstPixmap, int srcX, int srcY,
 	int maskX, int maskY, int dstX, int dstY, int width, int height)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
 	OMAPPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);

 	if (1) {
		/* just sw now */
 		PicturePtr	pDstPicture;
 		pixman_image_t *src, *mask, *dest;
 		int src_xoff, src_yoff, msk_xoff, msk_yoff;
 		FbBits *bits;
 		FbStride stride;
 		int bpp;

 		pDstPicture = gCompositeBox.pDstPicture;

 		src = image_from_pict (gCompositeBox.pSrcPicture, FALSE, &src_xoff, &src_yoff);
 		mask = image_from_pict (gCompositeBox.pMaskPicture, FALSE, &msk_xoff, &msk_yoff);

 		fbGetPixmapBitsData (gCompositeBox.pDstPixmap, bits, stride, bpp);
 		dest = pixman_image_create_bits (pDstPicture->format,
 			gCompositeBox.pDstPixmap->drawable.width,
 			gCompositeBox.pDstPixmap->drawable.height,
 			(uint32_t *)bits, stride * sizeof (FbStride));

 		pixman_image_composite (gCompositeBox.op,
 			src, mask, dest,
 			srcX,
 			srcY,
 			maskX,
 			maskY,
 			dstX,
 			dstY,
 			width,
 			height);

 		free_pixman_pict (gCompositeBox.pSrcPicture, src);
 		free_pixman_pict (gCompositeBox.pMaskPicture, mask);
 		pixman_image_unref (dest);	
 	} else {

 	}
   // rga_multiple_transform(ms->ctx, &gCompositeBox.src_img, &gCompositeBox.dst_img, srcX, srcY, gCompositeBox.src_img.width, gCompositeBox.src_img.height,
   //                        dstX, dstY, width, height, gCompositeBox.srcRotate, 0, 0);
 }

/* done composite : sw done composite, not using pvr2d */
 static void RKExaRGADoneComposite(PixmapPtr pDst)
 {
 	ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
 	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n", __func__);
 	rga_exec(rkctx);
 }

 static Bool
 CloseScreen(CLOSE_SCREEN_ARGS_DECL)
 {
#if 0 // TODO need to change CloseScreen/FreeScreen ..
	exaDriverFini(pScreen);
	free(pNv->EXADriverPtr);
#endif
	return TRUE;
}

static void
FreeScreen(FREE_SCREEN_ARGS_DECL)
{
}


OMAPEXAPtr
InitNullEXA(ScreenPtr pScreen, ScrnInfoPtr pScrn, int fd)
{
	OMAPNullEXAPtr null_exa;
	OMAPEXAPtr omap_exa;
	ExaDriverPtr exa;

	INFO_MSG("Soft EXA mode");

	null_exa = calloc(1, sizeof *null_exa);
	omap_exa = (OMAPEXAPtr)null_exa;
	if (!null_exa)
		goto out;

	exa = exaDriverAlloc();
	if (!exa)
		goto free_null_exa;

	null_exa->exa = exa;

	exa->exa_major = EXA_VERSION_MAJOR;
	exa->exa_minor = EXA_VERSION_MINOR;

	exa->pixmapOffsetAlign = 0;
	exa->pixmapPitchAlign = 32;
	exa->flags = EXA_OFFSCREEN_PIXMAPS |
	EXA_HANDLES_PIXMAPS | EXA_SUPPORTS_PREPARE_AUX;
	exa->maxX = 4096;
	exa->maxY = 4096;

	/* Required EXA functions: */
	exa->WaitMarker = OMAPWaitMarker;
	exa->CreatePixmap2 = OMAPCreatePixmap;
	exa->DestroyPixmap = OMAPDestroyPixmap;
	exa->ModifyPixmapHeader = OMAPModifyPixmapHeader;

	exa->PrepareAccess = OMAPPrepareAccess;
	exa->FinishAccess = OMAPFinishAccess;
	exa->PixmapIsOffscreen = OMAPPixmapIsOffscreen;

	exa->PrepareSolid = RKExaRGAPrepareSolid;
	exa->Solid = RKExaRGASolid;
	exa->DoneSolid = RKExaRGADoneSolid;

	exa->PrepareCopy = RKExaRGAPrepareCopy;
	exa->Copy = RKExaRGACopy;
	exa->DoneCopy = RKExaRGADoneCopy;

	//    exa->CheckComposite = RKExaRGACheckComposite;
	//    exa->PrepareComposite = RKExaRGAPrepareComposite;
	//    exa->Composite = RKExaRGAComposite;
	//    exa->DoneComposite = RKExaRGADoneComposite;

	if (!exaDriverInit(pScreen, exa)) {
		ERROR_MSG("exaDriverInit failed");
		goto free_exa;
	}

	omap_exa->CloseScreen = CloseScreen;
	omap_exa->FreeScreen = FreeScreen;

	/* for test: */
	rkdev = rockchip_device_create(fd);
	rkctx = rga_init(rkdev->fd);
	rkfd = fd;

	return omap_exa;

	free_exa:
	free(exa);
	free_null_exa:
	free(null_exa);
	out:
	return NULL;
}

