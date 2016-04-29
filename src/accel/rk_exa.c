#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <fcntl.h>
#include "xf86Crtc.h"
#include <xorg-server.h>
#include <drm_fourcc.h>

#include "compat-api.h"
#include "driver.h"
#include "rk_exa.h"


int rga_copy(struct rga_context *ctx, struct rga_image *src,
             struct rga_image *dst, unsigned int src_x, unsigned int src_y,
             unsigned int dst_x, unsigned dst_y, unsigned int w, unsigned int h)
{
    return rga_multiple_transform(ctx, src, dst, src_x, src_y, w,
                                  h, dst_x, dst_y, w, h, 0, 0, 0);
}

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

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s  %x\n", __func__, priv->bo->handle);

    if(dumb_bo_map(ms->fd, priv->bo))
        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s  %x error\n", __func__, priv->bo->handle);
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

    if (priv->bo != ms->drmmode.front_bo) {
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
    uint32_t size;
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "1CREATE_PIXMAP_FB(%p) : (x,y,w,h)=(%d,%d,%d,%d, %x)\n",
               pPixmap, pPixmap->drawable.width, pPixmap->drawable.height,
               pPixmap->drawable.bitsPerPixel,  pPixmap->drawable.depth, pPixData);
    if (pPixData)
        pPixmap->devPrivate.ptr = pPixData;

    if (devKind > 0)
        pPixmap->devKind = devKind;

    if (pPixData == drmmode_map_front_bo(&ms->drmmode)) {
        dumb_bo_unreference(ms->fd, priv->bo);
        priv->bo = ms->drmmode.front_bo;
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

    size =
            pPixmap->drawable.height * pPixmap->drawable.width *
            pPixmap->drawable.bitsPerPixel / 8;
    /*
     * X will sometimes create an empty pixmap (width/height == 0) and then
     * use ModifyPixmapHeader to point it at PixData. We'll hit this path
     * during the CreatePixmap call. Just return true and skip the allocate
     * in this case.
     */
    if (!pPixmap->drawable.width || !pPixmap->drawable.height)
        return TRUE;

    if ((!priv->bo) || (priv->bo->size != size)) {
        /* pixmap drops ref on its old bo */
        dumb_bo_unreference(ms->fd, priv->bo);
        /* pixmap creates new bo and takes ref on it */
        priv->bo = dumb_bo_create(ms->fd, pPixmap->drawable.width,
                                  pPixmap->drawable.height, pPixmap->drawable.bitsPerPixel );
        if (!priv->bo) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                       "failed to allocate %dx%d bo, size=%d, flags=%08x \n",
                       pPixmap->drawable.width,
                       pPixmap->drawable.height, size, 0);
            return FALSE;
        }

        pPixmap->devKind =
                pPixmap->drawable.width * pPixmap->drawable.bitsPerPixel /
                8;
    }

    return priv->bo != NULL;
}

static void RKFinishAccess(PixmapPtr pPixmap, int index)
{
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);

    pPixmap->devPrivate.ptr = NULL;
}

static Bool RKPixmapIsOffscreen(PixmapPtr pPixmap)
{
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);

    return priv && priv->bo;
}

void rkExaInit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    ExaDriverPtr exa;

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
                   "EXA-RGA initialization failed \n");
        goto free_exa;
    }

    if (!exaDriverInit(pScreen, exa)) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
                   "EXA initialization failed \n");
        goto free_exa;
    }
    ms->exa = exa;

    return;
free_exa:
    free(exa);
out:
    return;
}

void rkExaExit(ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);

    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "< %s()\n", __func__);

    if (ms->exa) {
        rkExaRGAExit(pScrn);
        exaDriverFini(pScreen);
        free(ms->exa);
    }
}
