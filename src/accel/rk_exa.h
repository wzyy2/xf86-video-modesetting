#ifndef RK_EXA_H_
#define RK_EXA_H_

#include <xf86.h>
#include <xf86_OSproc.h>
#include <xf86drm.h>
#include <exa.h>

#include "libdrm/rockchip_drm.h"
#include "libdrm/rockchip_drmif.h"
#include "libdrm/rockchip_rga.h"

#include "compat-api.h"

typedef struct {

	/* buffer object */
    struct dumb_bo *bo;

} RKPixmapREC, *RKPixmapPrivPtr;

void rkExaInit(ScreenPtr pScreen);
void rkExaExit(ScreenPtr pScreen);

int rkExaRGAInit(ScrnInfoPtr pScrn, ExaDriverPtr exa);
int rkExaRGAExit(ScrnInfoPtr pScrn);

#endif /* RK_EXA_H_ */
