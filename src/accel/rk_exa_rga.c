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
    struct rga_image src_img;
    struct rga_image dst_img;

    PixmapPtr pSrc;
    PixmapPtr pDst;
} CopyBox;

typedef struct
{
    struct rga_image src_img;
    struct rga_image mask_img;    
    struct rga_image dst_img;

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
    RKPixmapPrivPtr *priv;

    if(!pPix) return TRUE;

    if(Mask != -1 && pPix->drawable.depth < 8)
        return FALSE;

    priv = exaGetPixmapDriverPrivate(pPixmap);
    if(!privPixmap->bo)
        return FALSE;

    if (pPix->drawable.width < 34 || pPix->drawable.height < 34)
        return FALSE;

    if (pPix->drawable.depth != 32 && pPix->drawable.depth != 24)
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
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    int mode, src_fd;

    /* Check capability */
    if (!RGAIsSupport(pPixmap, planemask))
    {
        return FALSE;
    }

    drmPrimeHandleToFD(ms->dev->fd, priv->bo->handle, 0, &src_fd);


    fill_colour =
            (fill_colour & 0xff00ff00) + ((fill_colour & 0xff) << 16) +
            ((fill_colour & 0xff0000) >> 16);

    gSoildBox.solid_img.bo[0] = src_fd;
    gSoildBox.solid_img.width = pPixmap->drawable.width;
    gSoildBox.solid_img.height = pPixmap->drawable.height;
    gSoildBox.solid_img.stride = pPixmap->drawable.width * 4;
    gSoildBox.solid_img.buf_type = RGA_IMGBUF_GEM;
    gSoildBox.solid_img.color_mode = RGAGetMode(pPixmap);
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
    close(gSoildBox.solid_img.bo[0]);
}



static Bool
RKExaRGAPrepareCopy(PixmapPtr pSrc, PixmapPtr pDst, int xdir, int ydir,
                    int alu, Pixel planemask)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pSrc->drawable.pScreen);
    modesettingPtr ms = modesettingPTR(pScrn);
    RKPixmapPrivPtr priv_src = exaGetPixmapDriverPrivate(pSrc);
    RKPixmapPrivPtr priv_dst = exaGetPixmapDriverPrivate(pDst);
    int mode, src_fd, dst_fd;

    /* Check capability */
    if (!RGAIsSupport(pSrc, planemask) ||
        !RGAIsSupport(pDst, planemask) ||
        !xdir || !ydir || alu != 0x03)
    {
        return FALSE;
    }

    gCopyBox.pSrc = pSrc;
    gCopyBox.pDst = pDst;

        xf86DrvMsg(pScrn->scrnIndex, X_INFO,
               "> %s xdir %d ydir %d  srcdepth %d dstdepth %d srcbo %x dstbo %x\n",
               __func__, xdir, ydir, pSrc->drawable.depth,
               pDst->drawable.depth, priv_src->bo->handle,
               priv_dst->bo->handle);

    drmPrimeHandleToFD(ms->fd, priv_src->bo->handle, 0, &src_fd);
    drmPrimeHandleToFD(ms->fd, priv_dst->bo->handle, 0, &dst_fd);

    gCopyBox.src_img.bo[0] = src_fd;
    gCopyBox.src_img.width = pSrc->drawable.width;
    gCopyBox.src_img.height = pSrc->drawable.height;
    gCopyBox.src_img.stride = pSrc->drawable.width * 4;
    gCopyBox.src_img.buf_type = RGA_IMGBUF_GEM;
    gCopyBox.src_img.color_mode = RGAGetMode(pSrc);

    gCopyBox.dst_img.bo[0] = dst_fd;
    gCopyBox.dst_img.width = pDst->drawable.width;
    gCopyBox.dst_img.height = pDst->drawable.height;
    gCopyBox.dst_img.stride = pDst->drawable.width * 4;
    gCopyBox.dst_img.buf_type = RGA_IMGBUF_GEM;
    gCopyBox.dst_img.color_mode = RGAGetMode(pDst);

    return TRUE;
}

static void RKExaRGACopySW(PixmapPtr pDstPixmap, int srcX, int srcY,
                         int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    RKPixmapPrivPtr priv2 = exaGetPixmapDriverPrivate(gCopyBox.pSrc);
    RKPixmapPrivPtr priv3 = exaGetPixmapDriverPrivate(gCopyBox.pDst);


        dumb_bo_map(ms->fd, priv3->bo);
        dumb_bo_map(ms->fd, priv2->bo);
        unsigned int *begin = priv3->bo->ptr;
        unsigned int *beginsrc = priv2->bo->ptr;
        for (int i = 0; i < height; i += 1) {
            for (int a = 0; a < width; a += 1) {
                begin[dstX + a +
                        (dstY + i) * gCopyBox.pDst->drawable.width] =
                        beginsrc[srcX + a +
                        (srcY +
                         i) * gCopyBox.pSrc->drawable.width];
            }
        }    
}

static void RKExaRGACopy(PixmapPtr pDstPixmap, int srcX, int srcY,
                         int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);
    RKPixmapPrivPtr priv2 = exaGetPixmapDriverPrivate(gCopyBox.pSrc);
    RKPixmapPrivPtr priv3 = exaGetPixmapDriverPrivate(gCopyBox.pDst);

    if (width < 34 || height < 34) {
        RKExaRGACopySW(pDstPixmap, srcX, srcY, dstX, dstY, width, height);
    } else  {
                xf86DrvMsg(pScrn->scrnIndex, X_INFO,
                       "> %s srcX %d srcY %d dstX %d dstY %d width %d height %d\n",
                       __func__, srcX, srcY, dstX, dstY, width, height);

        rga_copy(ms->ctx, &gCopyBox.src_img, &gCopyBox.dst_img, srcX, srcY,
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
    // ScrnInfoPtr pScrn = xf86ScreenToScrn(pSrc->drawable.pScreen);
    // modesettingPtr ms = modesettingPTR(pScrn);
    // RKPixmapPrivPtr priv_src = exaGetPixmapDriverPrivate(pSrc);
    // RKPixmapPrivPtr priv_mask = exaGetPixmapDriverPrivate(pMask);    
    // RKPixmapPrivPtr priv_dst = exaGetPixmapDriverPrivate(pDst);
    // int mode, src_fd, dst_fd;

    //     xf86DrvMsg(pScrn->scrnIndex, X_INFO,
    //            "> %s op %d\n",
    //            __func__, op);

    // /* Check capability */
    // if (!RGAIsSupport(pSrc, planemask)||
    //         pMask != NULL ||
    //     !RGAIsSupport(pDst, planemask))
    // {
    //     return FALSE;
    // }


    // if (!RGACheckPicture(pSrcPicture,
    //                                 &gCompositeBox.srcRotate,
    //                                 &gCompositeBox.srcScaleX,
    //                                 &gCompositeBox.srcScaleY,
    //                                 &gCompositeBox.srcRepeat))
    // {
    //     return FALSE;
    // }


    // drmPrimeHandleToFD(ms->fd, priv_src->bo->handle, 0, &src_fd);
    // // drmPrimeHandleToFD(ms->fd, priv_mask->bo->handle, 0, &mask_fd);    
    // drmPrimeHandleToFD(ms->fd, priv_dst->bo->handle, 0, &dst_fd);


    // gCompositeBox.src_img.bo[0] = src_fd;
    // gCompositeBox.src_img.width = pSrc->drawable.width;
    // gCompositeBox.src_img.height = pSrc->drawable.height;
    // gCompositeBox.src_img.stride = pSrc->drawable.width * 4;
    // gCompositeBox.src_img.buf_type = RGA_IMGBUF_GEM;
    // gCompositeBox.src_img.color_mode = RGAGetMode(pSrc);


    // // gCompositeBox.mask_img.bo[0] = mask_fd;
    // // gCompositeBox.mask_img.width = pMask->drawable.width;
    // // gCompositeBox.mask_img.height = pMask->drawable.height;
    // // gCompositeBox.mask_img.stride = pMask->drawable.width * 4;
    // // gCompositeBox.mask_img.buf_type = RGA_IMGBUF_GEM;
    // // gCompositeBox.mask_img.color_mode = RGAGetMode(pMask);

    // gCompositeBox.dst_img.bo[0] = dst_fd;
    // gCompositeBox.dst_img.width = pDst->drawable.width;
    // gCompositeBox.dst_img.height = pDst->drawable.height;
    // gCompositeBox.dst_img.stride = pDst->drawable.width * 4;
    // gCompositeBox.dst_img.buf_type = RGA_IMGBUF_GEM;
    // gCompositeBox.dst_img.color_mode = RGAGetMode(pDst);

    return FALSE;
}

static void
RKExaRGAComposite(PixmapPtr pDstPixmap, int srcX, int srcY,
                  int maskX, int maskY, int dstX, int dstY, int width, int height)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn(pDstPixmap->drawable.pScreen);
    RKPixmapPrivPtr priv = exaGetPixmapDriverPrivate(pDstPixmap);
    modesettingPtr ms = modesettingPTR(pScrn);

   // rga_multiple_transform(ms->ctx, &gCompositeBox.src_img, &gCompositeBox.dst_img, srcX, srcY, gCompositeBox.src_img.width, gCompositeBox.src_img.height,
   //                        dstX, dstY, width, height, gCompositeBox.srcRotate, 0, 0);
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
