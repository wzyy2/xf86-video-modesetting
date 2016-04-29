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

typedef struct
{
    struct rga_image solid_img;

    int src_fd;

} SoildBox;

typedef struct
{
    static struct rga_image scr_img;
    static struct rga_image dst_img;

    int src_fd;
    int dst_fd;

    PixmapPtr pSrc;
    PixmapPtr pDst;

} CopyBox;

static SoildBox gSoildBox;
static CopyBox gCopyBox;

static Bool
RKExaRGAPrepareSolid(PixmapPtr pPixmap, int alu, Pixel planemask, Pixel fill_colour)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    int mode;

    if (! !priv->bo)
        return FALSE;

    drmPrimeHandleToFD(ms->dev->fd, priv->bo->handle, 0, &gSoildBox.src_fd);

    switch (pPixmap->drawable.depth) {
    case 32:
        mode = DRM_FORMAT_ARGB8888;
        break;
    case 24:
        if (pPixmap->drawable.bitsPerPixel == 32)
            mode = DRM_FORMAT_XRGB8888;
        else
            mode = DRM_FORMAT_RGB888;

        break;
    }

    fill_colour =
            (fill_colour & 0xff00ff00) + ((fill_colour & 0xff) << 16) +
            ((fill_colour & 0xff0000) >> 16);

    gSoildBox.solid_img.bo[0] = src_fd;
    gSoildBox.solid_img.width = pPixmap->drawable.width;
    gSoildBox.solid_img.height = pPixmap->drawable.height;
    gSoildBox.solid_img.stride = pPixmap->drawable.width * 4;
    gSoildBox.solid_img.buf_type = RGA_IMGBUF_GEM;
    gSoildBox.solid_img.color_mode = mode;
    gSoildBox.solid_img.fill_color = fill_colour;
    //    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s ptr %x  alu %d planemask %x fill_color %x depth %d width %d height %d stride %d\n",
    //               __func__,  rockchip_bo_map(priv->bo), alu, planemask, fill_colour, pPixmap->drawable.depth, solid_img.width, solid_img.height, solid_img.stride);

    if (pPixmap->drawable.depth != 32 && pPixmap->drawable.depth != 24)
        return FALSE;

    return TRUE;
}

static void RKExaRGASolid(PixmapPtr pPixmap, int x1, int y1, int x2, int y2)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);

    //    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s  ptr %x x1 %d y1 %d x2 %d y2 %d\n",
    //               __func__, pPixmap->devPrivate.ptr, x1, y1, x2, y2);

    /*    if (1) {

                unsigned char *begin = rockchip_bo_map(priv->bo);
                for(int i = y1;
                        i < y2 ; i += 1 ) {
                        for(int a = x1;
                                a< x2 ; a += 1 ) {

                                begin[(i * pPixmap->drawable.width + a) * 4] = 0x00;
                                begin[(i * pPixmap->drawable.width + a) * 4 + 1] = 0x00;
                                begin[(i * pPixmap->drawable.width + a) * 4 + 2] = 0xff;
                                begin[(i * pPixmap->drawable.width + a) * 4 + 3] = 0x00;;
                            }
                }

    } else */  {
        rga_solid_fill(ms->ctx, &gSoildBox.solid_img, x1, y1, x2 - x1, y2 - y1);

    }

}

static void RKExaRGADoneSolid(PixmapPtr pPixmap)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pPixmap->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    //    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n",
    //               __func__);
    rga_exec(ms->ctx);
    close(src_fd);
}



static Bool
RKExaRGAPrepareCopy(PixmapPtr pSrc, PixmapPtr pDst, int xdir, int ydir,
                    int alu, Pixel planemask)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pSrc->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    RKPixmapPrivPtr priv_src = exaGetPixmapDriverPrivate(pSrc);
    RKPixmapPrivPtr priv_dst = exaGetPixmapDriverPrivate(pDst);
    int mode;

    gCopyBox.pSrc = pSrc;
    gCopyBox.pDst = pDst;

    if (pSrc->drawable.depth != 24 && pSrc->drawable.depth != 24)
        return FALSE;
    if (pDst->drawable.depth != 24 && pDst->drawable.depth != 24)
        return FALSE;
    if (pSrc->drawable.width < 40 || pSrc->drawable.height < 40)
        return FALSE;
    if (pDst->drawable.width < 40 || pDst->drawable.height < 40)
        return FALSE;

    if (priv_dst->bo->handle == 1)
        return FALSE;

    //    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
    //           "> %s xdir %d ydir %d  srcdepth %d dstdepth %d srcbo %x dstbo %x\n",
    //           __func__, xdir, ydir, pSrc->drawable.depth,
    //           pDst->drawable.depth, priv_src->bo->handle,
    //           priv_dst->bo->handle);

    drmPrimeHandleToFD(ms->fd, priv_src->bo->handle, 0, &gCopyBox.src_fd);
    drmPrimeHandleToFD(ms->fd, priv_dst->bo->handle, 0, &gCopyBox.dst_fd);

    switch (pSrc->drawable.depth) {
    case 32:
        mode = DRM_FORMAT_ARGB8888;
        break;
    case 24:
        if (pSrc->drawable.bitsPerPixel == 32)
            mode = DRM_FORMAT_XRGB8888;
        else
            mode = DRM_FORMAT_RGB888;

        break;
    }

    gCopyBox.src_img.bo[0] = copy_src_fd;
    gCopyBox.src_img.width = pSrc->drawable.width;
    gCopyBox.src_img.height = pSrc->drawable.height;
    gCopyBox.src_img.stride = pSrc->drawable.width * 4;
    gCopyBox.src_img.buf_type = RGA_IMGBUF_GEM;
    gCopyBox.src_img.color_mode = mode;

    switch (pDst->drawable.depth) {
    case 32:
        mode = DRM_FORMAT_ARGB8888;
        break;
    case 24:
        if (pDst->drawable.bitsPerPixel == 32)
            mode = DRM_FORMAT_XRGB8888;
        else
            mode = DRM_FORMAT_RGB888;

        break;
    }

    gCopyBox.dst_img.bo[0] = copy_dst_fd;
    gCopyBox.dst_img.width = pDst->drawable.width;
    gCopyBox.dst_img.height = pDst->drawable.height;
    gCopyBox.dst_img.stride = pDst->drawable.width * 4;
    gCopyBox.dst_img.buf_type = RGA_IMGBUF_GEM;
    gCopyBox.dst_img.color_mode = mode;

    return TRUE;
}

static void RKExaRGACopy(PixmapPtr pDstPixmap, int srcX, int srcY,
                         int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    RKPixmapPrivPtr priv2 = exaGetPixmapDriverPrivate(pSrcbox);
    RKPixmapPrivPtr priv3 = exaGetPixmapDriverPrivate(pDstbox);

    if (width < 40 || height < 40) {
        dumb_bo_map(ms->fd, priv3->bo);
        dumb_bo_map(ms->fd, priv2->bo);
        unsigned int *begin = priv3->bo->ptr;
        unsigned int *beginsrc = priv2->bo->ptr;
        for (int i = 0; i < height; i += 1) {
            for (int a = 0; a < width; a += 1) {
                begin[dstX + a +
                        (dstY + i) * pDstbox->drawable.width] =
                        beginsrc[srcX + a +
                        (srcY +
                         i) * pSrcbox->drawable.width];
            }
        }

    } else {
        //		xf86DrvMsg(pScrn->scrnIndex, X_INFO,
        //			   "> %s srcX %d srcY %d dstX %d dstY %d width %d height %d\n",
        //			   __func__, srcX, srcY, dstX, dstY, width, height);

        rga_copy(ms->ctx, &copy_scr_img, &copy_dst_img, srcX, srcY,
                 dstX, dstY, width, height);

        //            copy_dst_img.fill_color = 0xff00;
        //             rga_solid_fill(ms->ctx, &copy_dst_img, dstX, dstY, width, height);

    }
}

static void RKExaRGADoneCopy(PixmapPtr pDstPixmap)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    //        xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n",
    //                   __func__);
    rga_exec(ms->ctx);

    close(gCopyBox.src_fd);
    close((gCopyBox.dst_fd);
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
    //    switch(pSrc->drawable.depth)
    //        {
    //        case 32:
    //            mode = DRM_FORMAT_ARGB8888;
    //            break;
    //        case 24:
    //            if(pSrc->drawable.bitsPerPixel == 32)
    //                mode = DRM_FORMAT_XRGB8888;
    //            else
    //                mode = DRM_FORMAT_RGB888;

    //            break;
    //        }

    //    copy_scr_img.bo[0] = src_fd;
    //    copy_scr_img.width = pSrc->drawable.width;
    //    copy_scr_img.height = pSrc->drawable.height;
    //    copy_scr_img.stride =  pSrc->drawable.width * 4;
    //    copy_scr_img.buf_type = RGA_IMGBUF_GEM;
    //    copy_scr_img.color_mode = mode;

    //    switch(pDst->drawable.depth)
    //        {
    //        case 32:
    //            mode = DRM_FORMAT_ARGB8888;
    //            break;
    //        case 24:
    //            if(pDst->drawable.bitsPerPixel == 32)
    //                mode = DRM_FORMAT_XRGB8888;
    //            else
    //                mode = DRM_FORMAT_RGB888;

    //            break;
    //        }

    //    copy_dst_img.bo[0] = dst_fd;
    //    copy_dst_img.width = pDst->drawable.width;
    //    copy_dst_img.height = pDst->drawable.height;
    //    copy_dst_img.stride =  pDst->drawable.width * 4;
    //    copy_dst_img.buf_type = RGA_IMGBUF_GEM;
    //    copy_dst_img.color_mode = mode;

    return FALSE;
}

static void
RKExaRGAComposite(PixmapPtr pDstPixmap, int srcX, int srcY,
                  int maskX, int maskY, int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);

    //    rga_multiple_transform(ms->ctx, &compostie_src_img, &compostie_dst_img, srcX, srcY, test1.width, test1.height,
    //                           dstX, dstY, width, height, 0, 0, 0);
}

/* done composite : sw done composite, not using pvr2d */
static void RKExaRGADoneComposite(PixmapPtr pDst)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDst->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "> %s \n", __func__);
    rga_exec(ms->ctx);
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

    //    exa->CheckComposite = RKExaRGACheckComposite;
    //    exa->PrepareComposite = RKExaRGAPrepareComposite;
    //    exa->Composite = RKExaRGAComposite;
    //    exa->DoneComposite = RKExaRGADoneComposite;

    //  exa->UploadToScreen = ExaG2dUploadToScreen;
    //  exa->DownloadFromScreen = ExaG2dDownloadFromScreen;

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
