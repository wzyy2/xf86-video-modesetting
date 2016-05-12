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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <fcntl.h>
#include "xf86Crtc.h"
#include <xorg-server.h>
#include <drm_fourcc.h>

#include "driver.h"
#include "rk_exa.h"

#define MIN_RGA_PIXMAP_WIDTH 40
#define MIN_RGA_PIXMAP_HEIGHT 40
#define MIN_RGA_OP_WIDTH 40
#define MIN_RGA_OP_HEIGHT 40

#define IS_ZERO(a)    (a == 0)

typedef struct {
    struct rga_image solid_img;

    int src_fd;
    GCPtr pGC;
} OpSoild;

typedef struct {
    struct rga_image src_img;
    struct rga_image dst_img;

    Pixel pm;
    int alu;
    int reverse;
    int upsidedown;
    PixmapPtr pSrcPixmap;
    PixmapPtr pDstPixmap;
} OpCopy;

typedef struct {
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

} OpComposite;

static OpSoild gOpSoild;
static OpCopy gOpCopy;
static OpComposite gOpComposite;

static Bool
RGACheckPicture(PicturePtr pPicture, char *rot90, double *scaleX,
                double *scaleY, char *repeat)
{
    struct pixman_transform *t;

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

    if (pPicture == NULL) {
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

    if (pPicture->transform == NULL) {
        *rot90 = 0;
        *scaleX = 1.0;
        *scaleY = 1.0;
        return TRUE;
    }

    t = pPicture->transform;

    if (!IS_ZERO(t->matrix[0][0]) && IS_ZERO(t->matrix[0][1])
            && IS_ZERO(t->matrix[1][0]) && !IS_ZERO(t->matrix[1][1])) {
        *rot90 = FALSE;
        *scaleX = pixman_fixed_to_double(t->matrix[0][0]);
        *scaleY = pixman_fixed_to_double(t->matrix[1][1]);
    } else if (IS_ZERO(t->matrix[0][0]) && !IS_ZERO(t->matrix[0][1])
               && !IS_ZERO(t->matrix[1][0]) && IS_ZERO(t->matrix[1][1])) {
        /* FIMG2D 90 => PIXMAN 270 */
        *rot90 = TRUE;
        *scaleX = pixman_fixed_to_double(t->matrix[0][1]);
        *scaleY = pixman_fixed_to_double(t->matrix[1][0] * -1);
    } else {
        return FALSE;
    }

    return TRUE;
}

static Bool RGAIsSupport(PixmapPtr pPix, int Mask)
{
    RKPixmapPrivPtr priv;

    if (!pPix)
        return TRUE;

    if (Mask != -1)
        return FALSE;

    if (pPix->drawable.width < MIN_RGA_PIXMAP_WIDTH || 
            pPix->drawable.height < MIN_RGA_PIXMAP_HEIGHT)
        return FALSE;

    if (pPix->drawable.depth != 32 && pPix->drawable.depth != 24)
        return FALSE;

    priv = exaGetPixmapDriverPrivate(pPix);
    if (!priv->bo)
        return FALSE;

    return TRUE;
}

static int RGAGetMode(PixmapPtr pPix)
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
RKExaRGAPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask,
                     Pixel fill_colour)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    ChangeGCVal tmpval[3];
    int src_fd;

    if (!RGAIsSupport(pPixmap, planemask)) {
        return FALSE;
    }

    drmPrimeHandleToFD(ms->dev->fd, priv->bo->handle, 0, &src_fd);

    /* for sw */
    gOpSoild.pGC =
            GetScratchGC(pPixmap->drawable.depth, pPixmap->drawable.pScreen);
    tmpval[0].val = alu;
    tmpval[1].val = planemask;
    tmpval[2].val = fill_colour;
    ChangeGC(NullClient, gOpSoild.pGC,
             GCFunction | GCPlaneMask | GCForeground, tmpval);
    ValidateGC(&pPixmap->drawable, gOpSoild.pGC);
    /* for sw access */
    dumb_bo_map(ms->fd, priv->bo);
    pPixmap->devPrivate.ptr = priv->bo->ptr;

    fill_colour =
            (fill_colour & 0xff00ff00) + ((fill_colour & 0xff) << 16) +
            ((fill_colour & 0xff0000) >> 16);
    gOpSoild.solid_img.bo[0] = src_fd;
    gOpSoild.solid_img.width = pPixmap->drawable.width;
    gOpSoild.solid_img.height = pPixmap->drawable.height;
    gOpSoild.solid_img.stride = pPixmap->devKind;
    gOpSoild.solid_img.buf_type = RGA_IMGBUF_GEM;
    gOpSoild.solid_img.color_mode = RGAGetMode(pPixmap);
    gOpSoild.solid_img.fill_color = fill_colour;

    return TRUE;
}

static void RKExaRGASolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    int width = x2 - x1, height = y2 - y1;

    if (width < MIN_RGA_OP_WIDTH || height < MIN_RGA_OP_HEIGHT) {
        fbFill(&pPixmap->drawable,
               gOpSoild.pGC, x1, y1, x2 - x1, y2 - y1);
    } else {
        rga_solid_fill(ms->ctx, &gOpSoild.solid_img, x1, y1, x2 - x1,
                       y2 - y1);
        /* if too many cmd was send and we don't exec immediately, out of memory error will happend */
        rga_exec(ms->ctx);
    }
}

static void RKExaRGADoneSolid(PixmapPtr pPixmap)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);

    rga_exec(ms->ctx);

    close(gOpSoild.solid_img.bo[0]);
    // Finish Access
    pPixmap->devPrivate.ptr = NULL;
}

static Bool
RKExaRGAPrepareCopy(PixmapPtr pSrc, PixmapPtr pDst, int xdir, int ydir,
                    int alu, Pixel planemask)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pSrc->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    RKPixmapPrivPtr priv_src = exaGetPixmapDriverPrivate(pSrc);
    RKPixmapPrivPtr priv_dst = exaGetPixmapDriverPrivate(pDst);
    int src_fd, dst_fd;

    /* Check capability */
    if (!RGAIsSupport(pSrc, planemask) ||
            !RGAIsSupport(pDst, planemask) || !xdir || !ydir || alu != 0x03) {
        return FALSE;
    }

    gOpCopy.alu = alu;
    gOpCopy.pm = planemask;
    gOpCopy.reverse = (xdir == 1) ? 0 : 1;
    gOpCopy.upsidedown = (ydir == 1) ? 0 : 1;
    gOpCopy.pSrcPixmap = pSrc;
    gOpCopy.pDstPixmap = pDst;

    /* for sw access */
    dumb_bo_map(ms->fd, priv_src->bo);
    pSrc->devPrivate.ptr = priv_src->bo->ptr;
    dumb_bo_map(ms->fd, priv_dst->bo);
    pDst->devPrivate.ptr = priv_dst->bo->ptr;

    drmPrimeHandleToFD(ms->fd, priv_src->bo->handle, 0, &src_fd);
    drmPrimeHandleToFD(ms->fd, priv_dst->bo->handle, 0, &dst_fd);

    gOpCopy.src_img.bo[0] = src_fd;
    gOpCopy.src_img.width = pSrc->drawable.width;
    gOpCopy.src_img.height = pSrc->drawable.height;
    gOpCopy.src_img.stride = pSrc->devKind;
    gOpCopy.src_img.buf_type = RGA_IMGBUF_GEM;
    gOpCopy.src_img.color_mode = RGAGetMode(pSrc);

    gOpCopy.dst_img.bo[0] = dst_fd;
    gOpCopy.dst_img.width = pDst->drawable.width;
    gOpCopy.dst_img.height = pDst->drawable.height;
    gOpCopy.dst_img.stride = pDst->devKind;
    gOpCopy.dst_img.buf_type = RGA_IMGBUF_GEM;
    gOpCopy.dst_img.color_mode = RGAGetMode(pDst);

    return TRUE;
}

static void RKExaRGACopy(PixmapPtr pDstPixmap, int srcX, int srcY,
                         int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);

    if (width < MIN_RGA_OP_WIDTH || height < MIN_RGA_OP_HEIGHT) {
        CARD8 alu = gOpCopy.alu;
        FbBits pm = gOpCopy.pm;
        FbBits *src;
        FbStride srcStride;
        int srcBpp;
        FbBits *dst;
        FbStride dstStride;
        int dstBpp;
        int srcXoff, srcYoff;
        int dstXoff, dstYoff;

        fbGetDrawable(&(gOpCopy.pSrcPixmap->drawable), src, srcStride,
                      srcBpp, srcXoff, srcYoff);
        fbGetDrawable(&(gOpCopy.pDstPixmap->drawable), dst, dstStride,
                      dstBpp, dstXoff, dstYoff);

        fbBlt(src + srcY * srcStride,
              srcStride,
              srcX * srcBpp,
              dst + dstY * dstStride,
              dstStride,
              dstX * dstBpp,
              width * dstBpp,
              height,
              alu, pm, dstBpp, gOpCopy.reverse, gOpCopy.upsidedown);
    } else {
        rga_copy(ms->ctx, &gOpCopy.src_img, &gOpCopy.dst_img, srcX,
                 srcY, dstX, dstY, width, height);
        rga_exec(ms->ctx);

    }
}

static void RKExaRGADoneCopy(PixmapPtr pDstPixmap)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    //        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n",
    //                   __func__);
    rga_exec(ms->ctx);

    close(gOpCopy.src_img.bo[0]);
    close(gOpCopy.dst_img.bo[0]);

    gOpCopy.pSrcPixmap->devPrivate.ptr = NULL;
    gOpCopy.pDstPixmap->devPrivate.ptr = NULL;
}

static Bool
RKExaRGACheckComposite(int op, PicturePtr pSrcPicture, PicturePtr pMaskPicture,
                       PicturePtr pDstPicture)
{
    if (!pSrcPicture || !pMaskPicture || !pDstPicture
            || !pSrcPicture->pDrawable || !pMaskPicture->pDrawable
            || !pDstPicture->pDrawable)
        return FALSE;

    return TRUE;
}

static Bool
RKExaRGAPrepareComposite(int op, PicturePtr pSrcPicture,
                         PicturePtr pMaskPicture, PicturePtr pDstPicture,
                         PixmapPtr pSrcPixmap, PixmapPtr pMaskPixmap,
                         PixmapPtr pDstPixmap)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv_src = exaGetPixmapDriverPrivate(pSrcPixmap);
    RKPixmapPrivPtr priv_mask = exaGetPixmapDriverPrivate(pMaskPixmap);
    RKPixmapPrivPtr priv_dst = exaGetPixmapDriverPrivate(pDstPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    int mode, src_fd, dst_fd, mask_fd;

    /* Check capability */
    // if (!RGAIsSupport(pSrc, -1)||
    //  pMask != NULL ||
    //  !RGAIsSupport(pDst, -1))
    // {
    //  return FALSE;
    // }

    // if (!RGACheckPicture(pSrcPicture,
    //  &gOpComposite.srcRotate,
    //  &gOpComposite.srcScaleX,
    //  &gOpComposite.srcScaleY,
    //  &gOpComposite.srcRepeat))
    // {
    //  return FALSE;
    // }

    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "> %s srcRotate %d srcScaleX %d srcScaleY %d \n",
               __func__, gOpComposite.srcRotate, gOpComposite.srcScaleX,
               gOpComposite.srcScaleY);

    drmPrimeHandleToFD(ms->fd, priv_src->bo->handle, 0, &src_fd);
    drmPrimeHandleToFD(ms->fd, priv_mask->bo->handle, 0, &mask_fd);
    drmPrimeHandleToFD(ms->fd, priv_dst->bo->handle, 0, &dst_fd);

    gOpComposite.op = op;

    gOpComposite.pSrcPicture = pSrcPicture;
    gOpComposite.pMaskPicture = pMaskPicture;
    gOpComposite.pDstPicture = pDstPicture;
    gOpComposite.pSrcPixmap = pSrcPixmap;
    gOpComposite.pMaskPixmap = pMaskPixmap;
    gOpComposite.pDstPixmap = pDstPixmap;

    /* for sw access */
    dumb_bo_map(ms->fd, priv_src->bo);
    pSrcPixmap->devPrivate.ptr = priv_src->bo->ptr;
    dumb_bo_map(ms->fd, priv_mask->bo);
    pMaskPixmap->devPrivate.ptr = priv_mask->bo->ptr;
    dumb_bo_map(ms->fd, priv_dst->bo);
    pDstPixmap->devPrivate.ptr = priv_dst->bo->ptr;

    gOpComposite.src_img.bo[0] = src_fd;
    gOpComposite.src_img.width = pSrcPixmap->drawable.width;
    gOpComposite.src_img.height = pSrcPixmap->drawable.height;
    gOpComposite.src_img.stride = pSrcPixmap->drawable.width * 4;
    gOpComposite.src_img.buf_type = RGA_IMGBUF_GEM;
    gOpComposite.src_img.color_mode = RGAGetMode(pSrcPixmap);

    gOpComposite.mask_img.bo[0] = mask_fd;
    gOpComposite.mask_img.width = pMaskPixmap->drawable.width;
    gOpComposite.mask_img.height = pMaskPixmap->drawable.height;
    gOpComposite.mask_img.stride = pMaskPixmap->drawable.width * 4;
    gOpComposite.mask_img.buf_type = RGA_IMGBUF_GEM;
    gOpComposite.mask_img.color_mode = RGAGetMode(pMaskPixmap);

    gOpComposite.dst_img.bo[0] = dst_fd;
    gOpComposite.dst_img.width = pDstPixmap->drawable.width;
    gOpComposite.dst_img.height = pDstPixmap->drawable.height;
    gOpComposite.dst_img.stride = pDstPixmap->drawable.width * 4;
    gOpComposite.dst_img.buf_type = RGA_IMGBUF_GEM;
    gOpComposite.dst_img.color_mode = RGAGetMode(pDstPixmap);

    return TRUE;
}

static void
RKExaRGAComposite(PixmapPtr pDstPixmap, int srcX, int srcY,
                  int maskX, int maskY, int dstX, int dstY, int width,
                  int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
    RKPixmapPrivPtr priv_src =
            exaGetPixmapDriverPrivate(gOpComposite.pSrcPixmap);
    RKPixmapPrivPtr priv_mask =
            exaGetPixmapDriverPrivate(gOpComposite.pMaskPixmap);
    RKPixmapPrivPtr priv_dst =
            exaGetPixmapDriverPrivate(gOpComposite.pDstPixmap);

    if (1) {
        /* just sw now */
        PicturePtr pDstPicture;
        pixman_image_t *src, *mask, *dest;
        int src_xoff, src_yoff, msk_xoff, msk_yoff;
        FbBits *bits;
        FbStride stride;
        int bpp;

        pDstPicture = gOpComposite.pDstPicture;

        src =
                image_from_pict(gOpComposite.pSrcPicture, FALSE, &src_xoff,
                                &src_yoff);
        mask =
                image_from_pict(gOpComposite.pMaskPicture, FALSE,
                                &msk_xoff, &msk_yoff);

        fbGetPixmapBitsData(gOpComposite.pDstPixmap, bits, stride,
                            bpp);
        dest =
                pixman_image_create_bits(pDstPicture->format,
                                         gOpComposite.pDstPixmap->drawable.
                                         width,
                                         gOpComposite.pDstPixmap->drawable.
                                         height, (uint32_t *) bits,
                                         stride * sizeof(FbStride));

        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                   "> %s %p %p %p %d %d \n",
                   __func__, src, mask, dest, width, height);

        pixman_image_composite(gOpComposite.op,
                               src, mask, dest,
                               srcX,
                               srcY,
                               maskX, maskY, dstX, dstY, width, height);

        free_pixman_pict(gOpComposite.pSrcPicture, src);
        free_pixman_pict(gOpComposite.pMaskPicture, mask);
        pixman_image_unref(dest);
    } else {

    }
}

/* done composite : sw done composite, not using pvr2d */
static void RKExaRGADoneComposite(PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n", __func__);
    rga_exec(ms->ctx);

    close(gOpComposite.src_img.bo[0]);
    close(gOpComposite.mask_img.bo[0]);
    close(gOpComposite.dst_img.bo[0]);

    gOpComposite.pSrcPixmap->devPrivate.ptr = NULL;
    gOpComposite.pMaskPixmap->devPrivate.ptr = NULL;
    gOpComposite.pDstPixmap->devPrivate.ptr = NULL;
}

int rkExaRGAInit(ScrnInfoPtr pScrn, ExaDriverPtr exa)
{
    modesettingPtr ms = modesettingPTR(pScrn);

    /* Always fallback for software operations */
    exa->PrepareSolid = RKExaRGAPrepareSolid;
    exa->Solid = RKExaRGASolid;
    exa->DoneSolid = RKExaRGADoneSolid;

    exa->PrepareCopy = RKExaRGAPrepareCopy;
    exa->Copy = RKExaRGACopy;
    exa->DoneCopy = RKExaRGADoneCopy;

    exa->CheckComposite = RKExaRGACheckComposite;
    exa->PrepareComposite = RKExaRGAPrepareComposite;
    exa->Composite = RKExaRGAComposite;
    exa->DoneComposite = RKExaRGADoneComposite;

    ms->ctx = rga_init(ms->dev->fd);

    if (!ms->ctx)
        return FALSE;

    return TRUE;
}

int rkExaRGAExit(ScrnInfoPtr pScrn)
{
    modesettingPtr ms = modesettingPTR(pScrn);

    if (ms->ctx)
        rga_fini(ms->ctx);

    return TRUE;
}
