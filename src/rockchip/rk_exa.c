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

static void RKWaitMarker(ScreenPtr pScreen, int marker)
{
    /* no-op */
}

static Bool RKPrepareAccess(PixmapPtr pPixmap, int index)
{
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);

    if (!priv->bo)
        return FALSE;

    if (dumb_bo_map(ms->fd, priv->bo))
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s  %x error\n",
                   __func__, priv->bo->handle);
    pPixmap->devPrivate.ptr = priv->bo->ptr;

    if (!pPixmap->devPrivate.ptr)
        return FALSE;

    return TRUE;
}

static void *RKCreatePixmap2(ScreenPtr pScreen, int width, int height,
                             int depth, int usage_hint, int bitsPerPixel,
                             int *new_fb_pitch)
{
    RKPixmapPrivPtr priv = calloc(1, sizeof(RKPixmapREC));
    /* actual allocation of buffer is in ModifyPixmapHeader */

    return priv;
}

static void RKDestroyPixmap(ScreenPtr pScreen, void *driverPriv)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    RKPixmapPrivPtr priv = driverPriv;

    if (priv->bo != ms->drmmode.front_bo.dumb) {
        dumb_bo_unreference(ms->fd, priv->bo);
    }

    free(priv);
}

static Bool
RKModifyPixmapHeader(PixmapPtr pPixmap, int width, int height,
                     int depth, int bitsPerPixel, int devKind, pointer pPixData)
{
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);

    if (pPixData)
        pPixmap->devPrivate.ptr = pPixData;

    if (devKind > 0)
        pPixmap->devKind = devKind;

    if (pPixData == drmmode_map_front_bo(&ms->drmmode)) {
        dumb_bo_unreference(ms->fd, priv->bo);
        priv->bo = ms->drmmode.front_bo.dumb;
    } else if (pPixData) {
        /*
         * We can't accelerate this pixmap, and don't ever want to
         * see it again..
         */
        pPixmap->devPrivate.ptr = pPixData;
        pPixmap->devKind = devKind;
        dumb_bo_unreference(ms->fd, priv->bo);
        priv->bo = NULL;

        /* Returning FALSE calls miModifyPixmapHeader */
        return FALSE;
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

    if ((!priv->bo) || priv->bo->width != pPixmap->drawable.width
            || priv->bo->height != pPixmap->drawable.height
            || priv->bo->bpp != pPixmap->drawable.bitsPerPixel) {
        /* pixmap drops ref on its old bo */
        dumb_bo_unreference(ms->fd, priv->bo);
        /* pixmap creates new bo and takes ref on it */
        priv->bo = dumb_bo_create(ms->fd, pPixmap->drawable.width,
                                  pPixmap->drawable.height,
                                  pPixmap->drawable.bitsPerPixel);
        if (!priv->bo) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "failed to allocate %dx%d bo, flags=%08x \n",
                       pPixmap->drawable.width,
                       pPixmap->drawable.height, 0);
            return FALSE;
        }

        pPixmap->devKind = priv->bo->pitch;
    }

    return priv->bo != NULL;
}

static void RKFinishAccess(PixmapPtr pPixmap, int index)
{
    pPixmap->devPrivate.ptr = NULL;
}

static Bool RKPixmapIsOffscreen(PixmapPtr pPixmap)
{
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);

    return priv && priv->bo;
}

Bool rkExaInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    ExaDriverPtr exa;

    /* create DRM device instance: */
    ms->dev = rockchip_device_create(ms->fd);
    if (!ms->dev)
        goto out;

    exa = exaDriverAlloc();
    if (!exa)
        goto out;

    exa->exa_major = EXA_VERSION_MAJOR;
    exa->exa_minor = EXA_VERSION_MINOR;

    exa->pixmapOffsetAlign = 0;
    exa->pixmapPitchAlign = 32;
    exa->flags = EXA_OFFSCREEN_PIXMAPS |
            EXA_HANDLES_PIXMAPS | EXA_SUPPORTS_PREPARE_AUX;
    exa->maxX = 4096;
    exa->maxY = 4096;

    /* Required EXA functions: */
    exa->WaitMarker = RKWaitMarker;
    exa->CreatePixmap2 = RKCreatePixmap2;
    exa->DestroyPixmap = RKDestroyPixmap;
    exa->ModifyPixmapHeader = RKModifyPixmapHeader;

    exa->PrepareAccess = RKPrepareAccess;
    exa->FinishAccess = RKFinishAccess;
    exa->PixmapIsOffscreen = RKPixmapIsOffscreen;

    if (!rkExaRGAInit(pScrn, exa)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "RGA initialization failed \n");
        goto free_exa;
    }

    if (!exaDriverInit(pScreen, exa)) {
        goto free_exa;
    }

    ms->exa = exa;

    return TRUE;
free_exa:
    free(ms->dev);
    free(exa);
out:
    return FALSE;
}

Bool rkExaExit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "EXA exit\n");

    if (ms->exa) {
        rkExaRGAExit(pScrn);
        exaDriverFini(pScreen);
        free(ms->exa);
    }

    rockchip_device_destroy(ms->dev);

    return TRUE;
}
