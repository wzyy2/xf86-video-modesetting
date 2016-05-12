/*
 * Copyright © 2007 Red Hat, Inc.
 * Copyright © 2008 Maarten Maathuis
 * Copyright © 2011 Texas Instruments, Inc
 *
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
 *    Dave Airlie <airlied@redhat.com>
 *    Ian Elliott <ianelliottus@yahoo.com>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* TODO cleanup #includes, remove unnecessary ones */

#include "xorg-server.h"
#include "xorgVersion.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>

#include <linux/fb.h>


/* All drivers should typically include these */
#include "xf86.h"
#include "xf86_OSproc.h"
#define PPC_MMIO_IS_BE
#include "compiler.h"
#include "mipointer.h"

/* All drivers implementing backing store need this */

#include "micmap.h"

#include "xf86DDC.h"

#include "xf86RandR12.h"
#include "dixstruct.h"
#include "scrnintstr.h"
#include "fb.h"
#include "xf86cmap.h"
#include "shadowfb.h"

#include "xf86Cursor.h"
#include "xf86DDC.h"

#include "region.h"

#include <X11/extensions/randr.h>

#include <X11/extensions/dpmsconst.h>

#include "omap_driver.h"

#include "xf86Crtc.h"

#include "xf86drmMode.h"
#include "drm_fourcc.h"
#include "X11/Xatom.h"

#include <sys/ioctl.h>
#include <libudev.h>

typedef struct {
	int fd;
	struct udev_monitor *uevent_monitor;
	InputHandlerProc uevent_handler;
} drmmode_rec, *drmmode_ptr;

typedef struct {
	drmmode_ptr drmmode;
	uint32_t id;
	struct omap_bo *cursor_bo;
} drmmode_crtc_private_rec, *drmmode_crtc_private_ptr;

typedef struct {
	drmModePropertyPtr mode_prop;
	int index; /* Index within the kernel-side property arrays for
	 * this connector. */
	int num_atoms; /* if range prop, num_atoms == 1; if enum prop,
	 * num_atoms == num_enums + 1 */
	Atom *atoms;
} drmmode_prop_rec, *drmmode_prop_ptr;

typedef struct {
	drmmode_ptr drmmode;
	int id;
	drmModeConnectorPtr mode_output;
	drmModePropertyBlobPtr edid_blob;
	uint32_t dpms_id;
	int num_props;
	drmmode_prop_ptr props;
} drmmode_output_private_rec, *drmmode_output_private_ptr;

static void drmmode_output_dpms(xf86OutputPtr output, int mode);

static uint32_t
drmmode_get_prop_id(int fd, uint32_t count_props, const uint32_t props[],
		const char *name, uint32_t flags)
{
	uint32_t i;
	uint32_t prop_id;

	for (prop_id = 0, i = 0; i < count_props && !prop_id; i++) {
		drmModePropertyPtr prop = drmModeGetProperty(fd, props[i]);
		if (!prop)
			continue;
		if (prop->flags == flags && !strcmp(prop->name, name))
			prop_id = props[i];
		drmModeFreeProperty(prop);
	}

	return prop_id;
}

static OMAPScanoutPtr
drmmode_scanout_from_size(OMAPScanoutPtr scanouts, int x, int y, int width,
		int height)
{
	int i;
	for (i = 0; i < MAX_SCANOUTS; i++) {
		if (scanouts[i].x == x && scanouts[i].y == y &&
		    scanouts[i].width == width && scanouts[i].height == height)
			return &scanouts[i];
	}
	return NULL;
}

static OMAPScanoutPtr
drmmode_scanout_from_crtc(OMAPScanoutPtr scanouts, xf86CrtcPtr crtc)
{
	return drmmode_scanout_from_size(scanouts, crtc->x, crtc->y,
			crtc->mode.HDisplay, crtc->mode.VDisplay);
}

OMAPScanoutPtr
drmmode_scanout_from_drawable(OMAPScanoutPtr scanouts, DrawablePtr pDraw)
{
	return drmmode_scanout_from_size(scanouts, pDraw->x, pDraw->y,
			pDraw->width, pDraw->height);
}

static OMAPScanoutPtr
drmmode_scanout_add(OMAPScanoutPtr scanouts, xf86CrtcPtr crtc,
		struct omap_bo *bo)
{
	int i;
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr s = &scanouts[i];

		if (s->bo)
			continue;

		omap_bo_reference(bo);
		s->x = crtc->x;
		s->y = crtc->y;
		s->width = crtc->mode.HDisplay;
		s->height = crtc->mode.VDisplay;
		s->bo = bo;
		return s;
	}

	return NULL;
}

void
drmmode_scanout_set(OMAPScanoutPtr scanouts, int x, int y, struct omap_bo *bo)
{
	OMAPScanoutPtr s;

	s = drmmode_scanout_from_size(scanouts, x, y, omap_bo_width(bo),
			omap_bo_height(bo));
	if (!s) {
		/* The scanout may not exist after flip, just ignore */
		return;
	}

	omap_bo_reference(bo);
	omap_bo_unreference(s->bo);
	s->bo = bo;
}

static uint32_t drmmode_crtc_id(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	return drmmode_crtc->id;
}

int drmmode_crtc_index_from_drawable(ScrnInfoPtr pScrn, DrawablePtr pDraw)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc;
	DisplayModePtr mode;
	int i;

	for (i = 0; i < xf86_config->num_crtc; i++) {
		crtc = xf86_config->crtc[i];
		if (!crtc->enabled)
			continue;
		mode = &crtc->mode;
		if (crtc->x == pDraw->x && crtc->y == pDraw->y &&
		    mode->HDisplay == pDraw->width &&
		    mode->VDisplay == pDraw->height)
			return i;
	}
	return -1;
}

int drmmode_crtc_id_from_drawable(ScrnInfoPtr pScrn, DrawablePtr pDraw)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc;
	int index;

	index = drmmode_crtc_index_from_drawable(pScrn, pDraw);
	if (index == -1)
		return 0;
	crtc = xf86_config->crtc[index];
	return drmmode_crtc_id(crtc);
}

static drmmode_ptr
drmmode_from_scrn(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	drmmode_crtc_private_ptr drmmode_crtc;

	drmmode_crtc = xf86_config->crtc[0]->driver_private;
	return drmmode_crtc->drmmode;
}

static void
drmmode_ConvertFromKMode(ScrnInfoPtr pScrn, drmModeModeInfo *kmode,
		DisplayModePtr	mode)
{
	memset(mode, 0, sizeof(DisplayModeRec));
	mode->status = MODE_OK;

	mode->Clock = kmode->clock;

	mode->HDisplay = kmode->hdisplay;
	mode->HSyncStart = kmode->hsync_start;
	mode->HSyncEnd = kmode->hsync_end;
	mode->HTotal = kmode->htotal;
	mode->HSkew = kmode->hskew;

	mode->VDisplay = kmode->vdisplay;
	mode->VSyncStart = kmode->vsync_start;
	mode->VSyncEnd = kmode->vsync_end;
	mode->VTotal = kmode->vtotal;
	mode->VScan = kmode->vscan;

	mode->Flags = kmode->flags; //& FLAG_BITS;
	mode->name = strdup(kmode->name);

	DEBUG_MSG("copy mode %s (%p %p)", kmode->name, mode->name, mode);

	if (kmode->type & DRM_MODE_TYPE_DRIVER)
		mode->type = M_T_DRIVER;
	if (kmode->type & DRM_MODE_TYPE_PREFERRED)
		mode->type |= M_T_PREFERRED;

	xf86SetModeCrtc (mode, pScrn->adjustFlags);
}

static void
drmmode_ConvertToKMode(ScrnInfoPtr pScrn, drmModeModeInfo *kmode,
		DisplayModePtr mode)
{
	memset(kmode, 0, sizeof(*kmode));

	kmode->clock = mode->Clock;
	kmode->hdisplay = mode->HDisplay;
	kmode->hsync_start = mode->HSyncStart;
	kmode->hsync_end = mode->HSyncEnd;
	kmode->htotal = mode->HTotal;
	kmode->hskew = mode->HSkew;

	kmode->vdisplay = mode->VDisplay;
	kmode->vsync_start = mode->VSyncStart;
	kmode->vsync_end = mode->VSyncEnd;
	kmode->vtotal = mode->VTotal;
	kmode->vscan = mode->VScan;

	kmode->flags = mode->Flags; //& FLAG_BITS;
	if (mode->name)
		strncpy(kmode->name, mode->name, DRM_DISPLAY_MODE_LEN);
	kmode->name[DRM_DISPLAY_MODE_LEN-1] = 0;
}

static void
drmmode_crtc_dpms(xf86CrtcPtr drmmode_crtc, int mode)
{
	// FIXME - Implement this function
}

static Bool
drmmode_set_crtc_off(xf86CrtcPtr crtc)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	uint32_t crtc_id = drmmode_crtc_id(crtc);
	int rc;

	rc = drmModeSetCrtc(drmmode_crtc->drmmode->fd, crtc_id, 0, 0, 0, NULL,
			0, NULL);
	if (rc)
		ERROR_MSG("[CRTC:%u] disable failed: %s", crtc_id,
				strerror(errno));

	/* drmModeSetCrtc returns non-zero on error; convert to Bool */
	return (rc) ? FALSE : TRUE;
}

static Bool
drmmode_set_crtc(ScrnInfoPtr pScrn, xf86CrtcPtr crtc, struct omap_bo *bo, int x,
			int y)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output;
	drmmode_crtc_private_ptr drmmode_crtc;
	drmmode_output_private_ptr drmmode_output;
	int rc, output_count, i;
	uint32_t *output_ids = NULL;
	uint32_t fb_id;
	uint32_t crtc_id = drmmode_crtc_id(crtc);
	drmModeModeInfo kmode;
	Bool ret;

	output_ids = calloc(xf86_config->num_output, sizeof *output_ids);
	assert(output_ids);

	output_count = 0;
	for (i = 0; i < xf86_config->num_output; i++) {
		output = xf86_config->output[i];
		drmmode_output = output->driver_private;

		if (output->crtc != crtc)
			continue;

		output_ids[output_count] = drmmode_output->id;
		output_count++;
	}
	if (!output_count) {
		ERROR_MSG("[CRTC:%u] No outputs found", crtc_id);
		ret = FALSE;
		goto out;
	}

	drmmode_ConvertToKMode(pScrn, &kmode, &crtc->mode);

	drmmode_crtc = crtc->driver_private;
	fb_id = omap_bo_fb(bo);
	/* drmModeSetCrtc returns non-zero on error; convert to Bool */
	rc = drmModeSetCrtc(drmmode_crtc->drmmode->fd, crtc_id, fb_id, x, y,
			output_ids, output_count, &kmode);
	if (rc)
		ERROR_MSG("[CRTC:%u] failed to set mode with [FB:%u] @ (%d, %d): %s",
				crtc_id, fb_id, x, y, strerror(errno));
	ret = rc ? FALSE : TRUE;

out:
	free(output_ids);
	return ret;
}

/*
 * Copy region of @src starting at (@src_x, @src_y) to @dst at (@dst_x, dst_y).
 * This function does no conversions, so it assumes same src and dst bpp.
 *  @src         source buffer
 *  @src_x       x coordinate from which to start copying
 *  @src_y       y coordinate from which to start copying
 *  @src_width   max number of pixels per src row to copy
 *  @src_height  max number of src rows to copy
 *  @src_pitch   total length of each src row, in bytes
 *  @src_cpp     bytes (ie, chars) per pixel of source
 *  @dst         destination buffer
 *  @dst_x       x coordinate to which to start copying
 *  @dst_y       y coordinate to which to start copying
 *  @dst_width   max number of pixels per dst row to copy
 *  @dst_height  max number of dst rows to copy
 *  @dst_pitch   total length of each dst row, in bytes
 *  @dst_cpp     bytes (ie, chars) per pixel of dst, must be same as src_cpp
 */
static void
drmmode_copy_from_to(const uint8_t *src, int src_x, int src_y, int src_width,
		     int src_height, int src_pitch, int src_cpp,
		     uint8_t *dst, int dst_x, int dst_y, int dst_width,
		     int dst_height, int dst_pitch, int dst_cpp)
{
	int y;
	int src_x_start = max(dst_x - src_x, 0);
	int dst_x_start = max(src_x - dst_x, 0);
	int src_y_start = max(dst_y - src_y, 0);
	int dst_y_start = max(src_y - dst_y, 0);
	int width = min(src_width - src_x_start, dst_width - dst_x_start);
	int height = min(src_height - src_y_start, dst_height - dst_y_start);


	assert(src_cpp == dst_cpp);

	if (width <= 0 || height <= 0)
		return;

	src += src_y_start * src_pitch + src_x_start * src_cpp;
	dst += dst_y_start * dst_pitch + dst_x_start * src_cpp;

	for (y = 0; y < height; y++, src += src_pitch, dst += dst_pitch)
		memcpy(dst, src, width * dst_cpp);
}

/*
 * Copy region of src buffer located at (src_x, src_y) that overlaps the dst
 * buffer at dst_x, dst_y.
 * This function does no conversions, so it assumes same bpp and depth.
 * It also assumes the two regions are non-overlapping memory areas, even though
 * they may overlap in pixel space.
 */
static Bool
drmmode_copy_bo(ScrnInfoPtr pScrn, struct omap_bo *src_bo, int src_x, int src_y,
		struct omap_bo *dst_bo, int dst_x, int dst_y)
{
	void *dst;
	const void *src;

	if (!src_bo || !dst_bo) {
		ERROR_MSG("copy_bo received invalid arguments");
		return FALSE;
	}

	assert(omap_bo_bpp(src_bo) == omap_bo_bpp(dst_bo));

	src = omap_bo_map(src_bo);
	if (!src) {
		ERROR_MSG("Couldn't map src bo");
		return FALSE;
	}
	dst = omap_bo_map(dst_bo);
	if (!dst) {
		ERROR_MSG("Couldn't map dst bo");
		return FALSE;
	}

	// acquire for write first, so if (probably impossible) src==dst acquire
	// for read can succeed
	omap_bo_cpu_prep(dst_bo, OMAP_GEM_WRITE);
	omap_bo_cpu_prep(src_bo, OMAP_GEM_READ);

	drmmode_copy_from_to(src, src_x, src_y,
			     omap_bo_width(src_bo), omap_bo_height(src_bo),
			     omap_bo_pitch(src_bo), omap_bo_Bpp(src_bo),
			     dst, dst_x, dst_y,
			     omap_bo_width(dst_bo), omap_bo_height(dst_bo),
			     omap_bo_pitch(dst_bo), omap_bo_Bpp(dst_bo));

	omap_bo_cpu_fini(src_bo, 0);
	omap_bo_cpu_fini(dst_bo, 0);

	return TRUE;
}

static Bool drmmode_set_blit_crtc(ScrnInfoPtr pScrn, xf86CrtcPtr crtc)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	Bool ret;

	if (!crtc->enabled)
		return TRUE;

	ret = drmmode_set_crtc(pScrn, crtc, pOMAP->scanout, crtc->x, crtc->y);
	if (!ret) {
		ERROR_MSG("[CRTC:%u] set root scanout failed",
				drmmode_crtc_id(crtc));
		drmmode_set_crtc_off(crtc);
	}

	return ret;
}

static Bool drmmode_set_flip_crtc(ScrnInfoPtr pScrn, xf86CrtcPtr crtc)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	OMAPScanoutPtr scanout;
	Bool ret;

	if (!crtc->enabled)
		return TRUE;

	scanout = drmmode_scanout_from_crtc(pOMAP->scanouts, crtc);
	if (!scanout)
		return TRUE;

	ret = drmmode_set_crtc(pScrn, crtc, scanout->bo, 0, 0);
	if (!ret) {
		ERROR_MSG("[CRTC:%u] set per-crtc scanout failed",
				drmmode_crtc_id(crtc));
		drmmode_set_crtc_off(crtc);
	}

	return ret;
}

Bool drmmode_update_scanout_from_crtcs(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	int i;
	Bool res;

	if (pOMAP->flip_mode == OMAP_FLIP_DISABLED)
		return TRUE;

	TRACE_ENTER();

	/* Only copy if source is valid. */
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr scanout = &pOMAP->scanouts[i];

		if (!scanout->bo)
			continue;
		if (!scanout->valid)
			continue;

		res = drmmode_copy_bo(pScrn, scanout->bo, scanout->x,
				scanout->y, pOMAP->scanout, 0, 0);
		if (!res) {
			ERROR_MSG("Copy crtc to scanout failed");
			goto out;
		}
	}
	res = TRUE;
out:
	TRACE_EXIT();
	return res;
}

/*
 * Enter blit mode.
 *
 * First, wait for all pending flips to complete.
 * Next, copy all valid per-crtc bo contents to the root bo, and mark their
 * scanouts as invalid to ensure they get updated when switching back to flip
 * mode.
 * Lastly, set all enabled crtcs to scan out from the root bo.
 */
Bool drmmode_set_blit_mode(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;
	Bool ret;

	if (pOMAP->flip_mode == OMAP_FLIP_DISABLED)
		return TRUE;

	// wait for all flips to finish so we will read from the current buffer
	while (pOMAP->pending_flips > 0)
		drmmode_wait_for_event(pScrn);

	/* Only copy if source is valid. */
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr scanout = &pOMAP->scanouts[i];

		if (!scanout->bo)
			continue;
		if (!scanout->valid)
			continue;

		ret = drmmode_copy_bo(pScrn, scanout->bo, scanout->x,
					  scanout->y, pOMAP->scanout, 0, 0);
		if (!ret) {
			ERROR_MSG("Copy crtc to scanout failed");
			return FALSE;
		}
		scanout->valid = FALSE;
	}
	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_blit_crtc(pScrn, crtc)) {
			ERROR_MSG("[CRTC:%u] could not set blit mode",
					drmmode_crtc_id(crtc));
			goto unwind;
		}
	}
	pOMAP->flip_mode = OMAP_FLIP_DISABLED;
	return TRUE;

unwind:
	/* try restoring already transitioned CRTCs back to flip mode */
	while (--i >= 0) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_flip_crtc(pScrn, crtc))
			ERROR_MSG("[CRTC:%u] could not restore flip mode",
					drmmode_crtc_id(crtc));
	}
	return FALSE;
}

/*
 * Enter flip mode.
 *
 * First, copy contents from the root bo to each invalid per-crtc bo, and mark
 * its scanout as valid.
 * Lastly, set all enabled crtcs to scan out from their per-crtc bos.
 */
Bool drmmode_set_flip_mode(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int i;
	Bool ret;

	if (pOMAP->flip_mode == OMAP_FLIP_ENABLED)
		return TRUE;

	/* Only copy if destination is invalid. */
	for (i = 0; i < MAX_SCANOUTS; i++) {
		OMAPScanoutPtr scanout = &pOMAP->scanouts[i];

		if (!scanout->bo)
			continue;
		if (scanout->valid)
			continue;

		ret = drmmode_copy_bo(pScrn, pOMAP->scanout, 0, 0,
					  scanout->bo, scanout->x,
					  scanout->y);
		if (!ret) {
			ERROR_MSG("Copy scanout to crtc failed");
			return FALSE;
		}
		scanout->valid = TRUE;
	}

	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_flip_crtc(pScrn, crtc)) {
			ERROR_MSG("[CRTC:%u] could not set flip mode",
					drmmode_crtc_id(crtc));
			goto unwind;
		}
	}
	pOMAP->flip_mode = OMAP_FLIP_ENABLED;
	return TRUE;

unwind:
	/* try restoring already transitioned CRTCs back to blit mode */
	while (--i >= 0) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		if (!drmmode_set_blit_crtc(pScrn, crtc))
			ERROR_MSG("[CRTC:%u] could not restore blit mode",
					drmmode_crtc_id(crtc));
	}
	return FALSE;
}

static Bool drmmode_update_scanouts(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	OMAPScanoutPtr scanout;
	int i;
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86CrtcPtr crtc;
	struct omap_bo *bo;
	Bool valid;

	OMAPScanout old_scanouts[MAX_SCANOUTS];
	memcpy(old_scanouts, pOMAP->scanouts, sizeof(old_scanouts));
	memset(pOMAP->scanouts, 0, sizeof(pOMAP->scanouts));

	for (i = 0; i < xf86_config->num_crtc; i++) {
		crtc = xf86_config->crtc[i];
		if (!crtc->enabled || !crtc->mode.HDisplay ||
				!crtc->mode.VDisplay)
			continue;

		scanout = drmmode_scanout_from_crtc(old_scanouts, crtc);
		if (scanout) {
			/* Use existing BO */
			bo = scanout->bo;
			valid = scanout->valid;
			memset(scanout, 0, sizeof(*scanout));
		} else {
			/* Allocate a new BO */
			bo = omap_bo_new_with_depth(pOMAP->dev,
					crtc->mode.HDisplay,
					crtc->mode.VDisplay, pScrn->depth,
					pScrn->bitsPerPixel);
			if (!bo) {
				ERROR_MSG("Scanout buffer allocation failed");
				return FALSE;
			}
			valid = FALSE;
		}
		scanout = drmmode_scanout_add(pOMAP->scanouts, crtc, bo);
		if (!scanout) {
			ERROR_MSG("Add scanout failed");
			omap_bo_unreference(bo);
			return FALSE;
		}
		scanout->valid = valid;

		/*
		 * drmmode_scanout_add() adds a reference, but we either:
		 * * already have a reference, from a recycled BO
		 * * was given a reference when a new BO was allocated
		 */
		omap_bo_unreference(bo);
	}

	/* Drop the remaining unused BOs. */
	for (i = 0; i < MAX_SCANOUTS; i++)
		if (old_scanouts[i].bo != NULL) {
			/*
			 * Set has_resized when discarding active scanouts. This
			 * ensures we trigger the blit/flip logic which will
			 * setcrtc to a valid fb if needed
			 */
			pOMAP->has_resized = TRUE;
			omap_bo_unreference(old_scanouts[i].bo);
		}

	return TRUE;
}

static Bool
drmmode_set_mode_major(xf86CrtcPtr crtc, DisplayModePtr mode,
		Rotation rotation, int x, int y)
{
	ScrnInfoPtr pScrn = crtc->scrn;
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr   xf86_config = XF86_CRTC_CONFIG_PTR(crtc->scrn);
	Bool ret;
	int i;

	TRACE_ENTER();

	ret = xf86CrtcRotate(crtc);
	if (!ret)
		goto done;

	// On a modeset, we should switch to blit mode to get a single scanout buffer
	// and we will switch back to flip mode on the next flip request
	if (pOMAP->flip_mode == OMAP_FLIP_DISABLED)
		ret = drmmode_set_crtc(pScrn, crtc, pOMAP->scanout, crtc->x, crtc->y);
	else
		ret = drmmode_set_blit_mode(pScrn);
	if (!ret)
		goto done;

	// Fixme - Intel puts this function here, and Nouveau puts it at the end
	// of this function -> determine what's best for TI'S OMAP4:
	if (crtc->funcs->gamma_set)
		crtc->funcs->gamma_set(crtc, crtc->gamma_red, crtc->gamma_green,
				       crtc->gamma_blue, crtc->gamma_size);

	ret = drmmode_update_scanouts(pScrn);
	if (!ret) {
		ERROR_MSG("Update scanouts failed");
		goto done;
	}

	// FIXME - DO WE NEED TO CALL TO THE PVR EXA/DRI2 CODE TO UPDATE THEM???

	/* Turn on any outputs on this crtc that may have been disabled: */
	for (i = 0; i < xf86_config->num_output; i++) {
		xf86OutputPtr output = xf86_config->output[i];

		if (output->crtc != crtc)
			continue;

		drmmode_output_dpms(output, DPMSModeOn);
	}

	/*
	 * The screen has reconfigured, so reload hw cursor images as needed,
	 * and adjust cursor positions.
	 */
	xf86_reload_cursors(pScrn->pScreen);

done:
	TRACE_EXIT();
	return ret;
}

#define CURSORW  64
#define CURSORH  64

static void
drmmode_set_cursor_position(xf86CrtcPtr crtc, int x, int y)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;

	drmModeMoveCursor(drmmode->fd, drmmode_crtc_id(crtc), x, y);
}

static void
drmmode_hide_cursor(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;

	drmModeSetCursor(drmmode->fd, drmmode_crtc_id(crtc), 0, CURSORW, CURSORH);
}

static void
drmmode_show_cursor(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	struct omap_bo* cursor_bo = drmmode_crtc->cursor_bo;
	uint32_t handle = cursor_bo ? omap_bo_handle(cursor_bo) : 0;
	drmModeSetCursor(drmmode->fd, drmmode_crtc_id(crtc), handle, CURSORW, CURSORH);
}

static void
drmmode_load_cursor_argb(xf86CrtcPtr crtc, CARD32 *image)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	struct omap_bo* cursor_bo = drmmode_crtc->cursor_bo;
	int row;
	void* dst;
	const char* src_row;
	char* dst_row;

	if (!cursor_bo)
		return;

	dst = omap_bo_map(cursor_bo);
	omap_bo_cpu_prep(cursor_bo, OMAP_GEM_WRITE);
	for (row = 0; row < CURSORH; row += 1) {
		// we're operating with ARGB data (32bpp)
		src_row = (const char*)image + row * 4 * CURSORW;
		dst_row = (char*)dst + row * 4 * CURSORW;
		memcpy(dst_row, src_row, 4 * CURSORW);
	}
	omap_bo_cpu_fini(cursor_bo, 0);
}

#ifdef OMAP_SUPPORT_GAMMA
static void
drmmode_gamma_set(xf86CrtcPtr crtc, CARD16 *red, CARD16 *green, CARD16 *blue,
		int size)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
	drmmode_ptr drmmode = drmmode_crtc->drmmode;
	int ret;

	ret = drmModeCrtcSetGamma(drmmode->fd, drmmode_crtc_id(crtc),
			size, red, green, blue);
	if (ret != 0) {
		xf86DrvMsg(crtc->scrn->scrnIndex, X_ERROR,
				"failed to set gamma: %s\n", strerror(-ret));
	}
}
#endif

static void
drmmode_crtc_destroy(xf86CrtcPtr crtc)
{
	drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

	omap_bo_unreference(drmmode_crtc->cursor_bo);
	free(drmmode_crtc);
	crtc->driver_private = NULL;
}

static const xf86CrtcFuncsRec drmmode_crtc_funcs = {
		.destroy = drmmode_crtc_destroy,
		.dpms = drmmode_crtc_dpms,
		.set_mode_major = drmmode_set_mode_major,
		.set_cursor_position = drmmode_set_cursor_position,
		.show_cursor = drmmode_show_cursor,
		.hide_cursor = drmmode_hide_cursor,
		.load_cursor_argb = drmmode_load_cursor_argb,
#ifdef OMAP_SUPPORT_GAMMA
		.gamma_set = drmmode_gamma_set,
#endif
};


static Bool
drmmode_crtc_pre_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
		const drmModeResPtr mode_res,
		const drmModePlaneResPtr plane_res, int num)
{
	xf86CrtcPtr crtc;
	drmmode_crtc_private_ptr drmmode_crtc;
	Bool ret;
	uint32_t crtc_id = mode_res->crtcs[num];
	OMAPPtr pOMAP = OMAPPTR(pScrn);

	TRACE_ENTER();

	drmmode_crtc = calloc(1, sizeof *drmmode_crtc);
	if (!drmmode_crtc) {
		ERROR_MSG("CRTC[%u]: Failed to allocate drmmode_crtc",
				crtc_id);
		ret = FALSE;
		goto out;
	}
	drmmode_crtc->id = crtc_id;
	drmmode_crtc->drmmode = drmmode;
	drmmode_crtc->cursor_bo = omap_bo_new_with_format(pOMAP->dev, CURSORW, CURSORH,
			DRM_FORMAT_ARGB8888, 32);
	if (!drmmode_crtc->cursor_bo) {
		ERROR_MSG("error allocating hw cursor buffer");
		ret = FALSE;
		goto err_free_drmmode_crtc;
	}

	crtc = xf86CrtcCreate(pScrn, &drmmode_crtc_funcs);
	if (crtc == NULL) {
		ERROR_MSG("CRTC[%u]: Failed to create xf86Crtc", crtc_id);
		ret = FALSE;
		goto err_destroy_cursor;
	}

	INFO_MSG("[CRTC:%u] HW Cursor using [BO:%u]",
			drmmode_crtc->id,
			omap_bo_handle(drmmode_crtc->cursor_bo));

	crtc->driver_private = drmmode_crtc;

	ret = TRUE;
	goto out;

err_destroy_cursor:
	omap_bo_unreference(drmmode_crtc->cursor_bo);
err_free_drmmode_crtc:
	free(drmmode_crtc);
out:
	TRACE_EXIT();
	return ret;
}

static xf86OutputStatus
drmmode_output_detect(xf86OutputPtr output)
{
	/* go to the hw and retrieve a new output struct */
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	xf86OutputStatus status;
	drmModeFreeConnector(drmmode_output->mode_output);

	drmmode_output->mode_output =
			drmModeGetConnector(drmmode->fd, drmmode_output->id);

	switch (drmmode_output->mode_output->connection) {
	case DRM_MODE_CONNECTED:
		status = XF86OutputStatusConnected;
		break;
	case DRM_MODE_DISCONNECTED:
		status = XF86OutputStatusDisconnected;
		break;
	default:
	case DRM_MODE_UNKNOWNCONNECTION:
		status = XF86OutputStatusUnknown;
		break;
	}
	return status;
}

static Bool
drmmode_output_mode_valid(xf86OutputPtr output, DisplayModePtr mode)
{
	if (mode->type & M_T_DEFAULT)
		/* Default modes are harmful here. */
		return MODE_BAD;

	return MODE_OK;
}

static DisplayModePtr
drmmode_output_get_modes(xf86OutputPtr output)
{
	ScrnInfoPtr pScrn = output->scrn;
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr koutput = drmmode_output->mode_output;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	DisplayModePtr Modes = NULL, Mode;
	drmModePropertyPtr prop;
	xf86MonPtr ddc_mon = NULL;
	int i;

	/* look for an EDID property */
	for (i = 0; i < koutput->count_props; i++) {
		prop = drmModeGetProperty(drmmode->fd, koutput->props[i]);
		if (!prop)
			continue;

		if ((prop->flags & DRM_MODE_PROP_BLOB) &&
		    !strcmp(prop->name, "EDID")) {
			if (drmmode_output->edid_blob)
				drmModeFreePropertyBlob(drmmode_output->edid_blob);
			drmmode_output->edid_blob =
					drmModeGetPropertyBlob(drmmode->fd,
							koutput->prop_values[i]);
		}
		drmModeFreeProperty(prop);
	}

	if (drmmode_output->edid_blob)
		ddc_mon = xf86InterpretEDID(pScrn->scrnIndex,
				drmmode_output->edid_blob->data);

	if (ddc_mon) {
		xf86OutputSetEDID(output, ddc_mon);
		xf86SetDDCproperties(pScrn, ddc_mon);
	}

	DEBUG_MSG("count_modes: %d", koutput->count_modes);

	/* modes should already be available */
	for (i = 0; i < koutput->count_modes; i++) {
		Mode = xnfalloc(sizeof(DisplayModeRec));

		drmmode_ConvertFromKMode(pScrn, &koutput->modes[i],
				Mode);
		Modes = xf86ModesAdd(Modes, Mode);

	}
	return Modes;
}

static void
drmmode_output_destroy(xf86OutputPtr output)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	int i;

	if (drmmode_output->edid_blob)
		drmModeFreePropertyBlob(drmmode_output->edid_blob);
	for (i = 0; i < drmmode_output->num_props; i++) {
		drmModeFreeProperty(drmmode_output->props[i].mode_prop);
		free(drmmode_output->props[i].atoms);
	}
	free(drmmode_output->props);
	drmModeFreeConnector(drmmode_output->mode_output);
	free(drmmode_output);
	output->driver_private = NULL;
}

static void
drmmode_output_dpms(xf86OutputPtr output, int mode)
{
	ScrnInfoPtr pScrn = output->scrn;
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	int ret;

	if (!drmmode_output->dpms_id)
		return;

	ret = drmModeConnectorSetProperty(drmmode->fd, drmmode_output->id,
			drmmode_output->dpms_id, mode);
	if (ret)
		ERROR_MSG("[CONNECTOR:%u] Failed to set [DPMS:%u]",
				drmmode_output->id, mode);
}

static Bool
drmmode_property_ignore(drmModePropertyPtr prop)
{
	if (!prop)
		return TRUE;
	/* ignore blob prop */
	if (prop->flags & DRM_MODE_PROP_BLOB)
		return TRUE;
	/* ignore standard property */
	if (!strcmp(prop->name, "EDID") ||
			!strcmp(prop->name, "DPMS"))
		return TRUE;

	return FALSE;
}

static void
drmmode_output_create_resources(xf86OutputPtr output)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmModeConnectorPtr mode_output = drmmode_output->mode_output;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	drmModePropertyPtr drmmode_prop;
	uint32_t value;
	int i, j, err;

	drmmode_output->props = calloc(mode_output->count_props,
			sizeof *drmmode_output->props);
	if (!drmmode_output->props)
		return;

	drmmode_output->num_props = 0;
	for (i = 0, j = 0; i < mode_output->count_props; i++) {
		drmmode_prop = drmModeGetProperty(drmmode->fd, mode_output->props[i]);
		if (drmmode_property_ignore(drmmode_prop)) {
			drmModeFreeProperty(drmmode_prop);
			continue;
		}
		drmmode_output->props[j].mode_prop = drmmode_prop;
		drmmode_output->props[j].index = i;
		drmmode_output->num_props++;
		j++;
	}

	for (i = 0; i < drmmode_output->num_props; i++) {
		drmmode_prop_ptr p = &drmmode_output->props[i];
		drmmode_prop = p->mode_prop;

		value = drmmode_output->mode_output->prop_values[p->index];

		if (drmmode_prop->flags & DRM_MODE_PROP_RANGE) {
			INT32 range[2];

			p->num_atoms = 1;
			p->atoms = calloc(p->num_atoms, sizeof *p->atoms);
			if (!p->atoms)
				continue;
			p->atoms[0] = MakeAtom(drmmode_prop->name, strlen(drmmode_prop->name), TRUE);
			range[0] = drmmode_prop->values[0];
			range[1] = drmmode_prop->values[1];
			err = RRConfigureOutputProperty(output->randr_output, p->atoms[0],
					FALSE, TRUE,
					drmmode_prop->flags & DRM_MODE_PROP_IMMUTABLE ? TRUE : FALSE,
							2, range);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRConfigureOutputProperty error, %d\n", err);
			}
			err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
					XA_INTEGER, 32, PropModeReplace, 1,
					&value, FALSE, FALSE);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRChangeOutputProperty error, %d\n", err);
			}
		} else if (drmmode_prop->flags & DRM_MODE_PROP_ENUM) {
			p->num_atoms = drmmode_prop->count_enums + 1;
			p->atoms = calloc(p->num_atoms, sizeof *p->atoms);
			if (!p->atoms)
				continue;
			p->atoms[0] = MakeAtom(drmmode_prop->name, strlen(drmmode_prop->name), TRUE);
			for (j = 1; j <= drmmode_prop->count_enums; j++) {
				struct drm_mode_property_enum *e = &drmmode_prop->enums[j-1];
				p->atoms[j] = MakeAtom(e->name, strlen(e->name), TRUE);
			}
			err = RRConfigureOutputProperty(output->randr_output, p->atoms[0],
					FALSE, FALSE,
					drmmode_prop->flags & DRM_MODE_PROP_IMMUTABLE ? TRUE : FALSE,
							p->num_atoms - 1, (INT32 *)&p->atoms[1]);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRConfigureOutputProperty error, %d\n", err);
			}
			for (j = 0; j < drmmode_prop->count_enums; j++)
				if (drmmode_prop->enums[j].value == value)
					break;
			/* there's always a matching value */
			err = RRChangeOutputProperty(output->randr_output, p->atoms[0],
					XA_ATOM, 32, PropModeReplace, 1, &p->atoms[j+1], FALSE, FALSE);
			if (err != 0) {
				xf86DrvMsg(output->scrn->scrnIndex, X_ERROR,
						"RRChangeOutputProperty error, %d\n", err);
			}
		}
	}
}

static Bool
drmmode_output_set_property(xf86OutputPtr output, Atom property,
		RRPropertyValuePtr value)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	int i, ret;

	for (i = 0; i < drmmode_output->num_props; i++) {
		drmmode_prop_ptr p = &drmmode_output->props[i];

		if (p->atoms[0] != property)
			continue;

		if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
			uint32_t val;

			if (value->type != XA_INTEGER || value->format != 32 ||
					value->size != 1)
				return FALSE;
			val = *(uint32_t *)value->data;

			ret = drmModeConnectorSetProperty(drmmode->fd,
					drmmode_output->id,
					p->mode_prop->prop_id, (uint64_t)val);

			if (ret)
				return FALSE;

			return TRUE;

		} else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
			Atom	atom;
			const char	*name;
			int		j;

			if (value->type != XA_ATOM || value->format != 32 || value->size != 1)
				return FALSE;
			memcpy(&atom, value->data, 4);
			name = NameForAtom(atom);
			if (name == NULL)
				return FALSE;

			/* search for matching name string, then set its value down */
			for (j = 0; j < p->mode_prop->count_enums; j++) {
				if (!strcmp(p->mode_prop->enums[j].name, name)) {
					ret = drmModeConnectorSetProperty(drmmode->fd,
							drmmode_output->id,
							p->mode_prop->prop_id,
							p->mode_prop->enums[j].value);

					if (ret)
						return FALSE;

					return TRUE;
				}
			}

			return FALSE;
		}
	}

	return TRUE;
}

/* These are borrowed from libdrm/xf86drmMode.c */
#define U642VOID(x) ((void *)(unsigned long)(x))
#define VOID2U64(x) ((uint64_t)(unsigned long)(x))

static Bool
drmmode_output_get_property_from_connector(xf86OutputPtr output, int prop_idx,
			uint64_t *val)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_ptr drmmode = drmmode_output->drmmode;
	struct drm_mode_get_connector conn, counts;
	struct drm_mode_modeinfo mode;
	uint64_t *prop_values;
	Bool r = TRUE;

retry:
	memset(&conn, 0, sizeof(struct drm_mode_get_connector));
	conn.connector_id = drmmode_output->id;

	/*
	 * This is a hack to prevent the kernel from calling get_modes()
	 * on this connector when all we really want is a property
	 * value. In certain cases, fetching the display modes can take
	 * on the order of seconds, and we don't want to spend that
	 * long here.
	 *
	 * By requesting one mode, the kernel will just fill up modes_ptr with
	 * the first mode (if it exists), and it'll be a noop if there are
	 * no modes. It's important that we don't use this mode for anything
	 * since it could be out-of-date.
	 */
	conn.count_modes = 1;
	conn.modes_ptr = VOID2U64(&mode);
	if (drmIoctl(drmmode->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn))
		return FALSE;

	counts = conn;

	if (!conn.count_props)
		return FALSE;

	conn.props_ptr = VOID2U64(
			drmMalloc(conn.count_props * sizeof(uint32_t)));
	if (!conn.props_ptr) {
		r = FALSE;
		goto err_allocs;
	}

	conn.prop_values_ptr = VOID2U64(
			drmMalloc(conn.count_props * sizeof(uint64_t)));
	if (!conn.prop_values_ptr) {
		r = FALSE;
		goto err_allocs;
	}

	/* Reset the encoder/mode counts so we don't fetch them */
	conn.count_encoders = 0;
	conn.count_modes = 1;
	if (drmIoctl(drmmode->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn)) {
		r = FALSE;
		goto err_allocs;
	}

	/*
	 * The number of props may have changed with a hotplug event in between
	 * the ioctls, in which case the field is silently ignored by the
	 * kernel.
	 */
	if (counts.count_props < conn.count_props) {
		drmFree(U642VOID(conn.props_ptr));
		drmFree(U642VOID(conn.prop_values_ptr));
		goto retry;
	}

	if (conn.count_props <= prop_idx) {
		r = FALSE;
		goto err_allocs;
	}

	prop_values = U642VOID(conn.prop_values_ptr);
	*val = prop_values[prop_idx];

err_allocs:
	drmFree(U642VOID(conn.prop_values_ptr));
	drmFree(U642VOID(conn.props_ptr));

	return r;
}

static Bool
drmmode_output_get_property(xf86OutputPtr output, Atom property)
{
	drmmode_output_private_ptr drmmode_output = output->driver_private;
	drmmode_prop_ptr p = NULL;
	uint64_t value;
	int err, i;

	for (i = 0; i < drmmode_output->num_props; i++) {
		p = &drmmode_output->props[i];
		if (p->atoms[0] == property)
			break;
	}
	if (i == drmmode_output->num_props)
		return FALSE;

	if (output->scrn->vtSema) {
		if (!drmmode_output_get_property_from_connector(output,
				p->index, &value))
			return FALSE;
	} else {
		value = drmmode_output->mode_output->prop_values[p->index];
	}

	if (p->mode_prop->flags & DRM_MODE_PROP_RANGE) {
		err = RRChangeOutputProperty(output->randr_output,
				property, XA_INTEGER, 32,
				PropModeReplace, 1, &value,
				FALSE, FALSE);

		return !err;
	} else if (p->mode_prop->flags & DRM_MODE_PROP_ENUM) {
		int		j;

		/* search for matching name string, then set its value down */
		for (j = 0; j < p->mode_prop->count_enums; j++) {
			if (p->mode_prop->enums[j].value == value)
				break;
		}

		err = RRChangeOutputProperty(output->randr_output, property,
				XA_ATOM, 32, PropModeReplace, 1,
				&p->atoms[j+1], FALSE, FALSE);

		return !err;
	}

	return FALSE;
}

static const xf86OutputFuncsRec drmmode_output_funcs = {
		.create_resources = drmmode_output_create_resources,
		.dpms = drmmode_output_dpms,
		.detect = drmmode_output_detect,
		.mode_valid = drmmode_output_mode_valid,
		.get_modes = drmmode_output_get_modes,
		.set_property = drmmode_output_set_property,
		.get_property = drmmode_output_get_property,
		.destroy = drmmode_output_destroy
};

// FIXME - Eliminate the following values that aren't accurate for OMAP4:
const char *output_names[] = { "None",
		"VGA",
		"DVI-I",
		"DVI-D",
		"DVI-A",
		"Composite",
		"SVIDEO",
		"LVDS",
		"CTV",
		"DIN",
		"DP",
		"HDMI",
		"HDMI",
		"TV",
		"eDP",
};
#define NUM_OUTPUT_NAMES (sizeof(output_names) / sizeof(output_names[0]))

static Bool
drmmode_output_pre_init(ScrnInfoPtr pScrn, drmmode_ptr drmmode,
		const drmModeResPtr mode_res, int num)
{
	xf86OutputPtr output;
	drmModeConnectorPtr koutput;
	drmModeEncoderPtr kencoder;
	drmmode_output_private_ptr drmmode_output;
	char name[32];
	CARD32 possible_crtcs, possible_clones;
	uint32_t connector_id = mode_res->connectors[num];

	Bool ret;

	TRACE_ENTER();

	koutput = drmModeGetConnector(drmmode->fd, connector_id);
	if (!koutput) {
		ERROR_MSG("[CONNECTOR:%u] Failed drmModeGetConnector",
				connector_id);
		ret = FALSE;
		goto out;
	}

	/*
	 * Fetch possible clones and crtcs from this connector's encoder.
	 * We assume here that there is only one possible encoder for this
	 * connector.
	 */
	kencoder = drmModeGetEncoder(drmmode->fd, koutput->encoders[0]);
	if (!kencoder) {
		ERROR_MSG("[CONNECTOR:%u] Failed drmModeGetEncoder",
				connector_id);
		ret = FALSE;
		goto err_free_drm_mode_connector;
	}

	possible_crtcs = kencoder->possible_crtcs;
	possible_clones = kencoder->possible_clones;
	drmModeFreeEncoder(kencoder);

	if (koutput->connector_type >= NUM_OUTPUT_NAMES)
		snprintf(name, 32, "Unknown%d-%d", koutput->connector_type,
				koutput->connector_type_id);
	else
		snprintf(name, 32, "%s-%d",
				output_names[koutput->connector_type],
				koutput->connector_type_id);

	drmmode_output = calloc(1, sizeof *drmmode_output);
	if (!drmmode_output) {
		ERROR_MSG("[CONNECTOR:%u] Failed to allocate drmmode_output",
				connector_id);
		ret = FALSE;
		goto err_free_drm_mode_connector;
	}

	drmmode_output->id = connector_id;
	drmmode_output->mode_output = koutput;
	drmmode_output->drmmode = drmmode;
	drmmode_output->dpms_id = drmmode_get_prop_id(drmmode->fd,
			koutput->count_props, koutput->props,
			"DPMS", DRM_MODE_PROP_ENUM);

	output = xf86OutputCreate(pScrn, &drmmode_output_funcs, name);
	if (!output) {
		ERROR_MSG("[CONNECTOR:%u] Failed xf86OutputCreate",
				connector_id);
		ret = FALSE;
		goto err_free_drmmode_output;
	}

	output->mm_width = koutput->mmWidth;
	output->mm_height = koutput->mmHeight;
	output->driver_private = drmmode_output;

	output->possible_crtcs = possible_crtcs;
	output->possible_clones = possible_clones;
	output->interlaceAllowed = TRUE;

	ret = TRUE;
	goto out;

err_free_drmmode_output:
	free(drmmode_output);
err_free_drm_mode_connector:
	drmModeFreeConnector(koutput);
out:
	TRACE_EXIT();
	return ret;
}

static Bool
drmmode_xf86crtc_resize(ScrnInfoPtr pScrn, int width, int height)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	ScreenPtr pScreen = pScrn->pScreen;
	struct omap_bo *new_scanout;
	uint32_t pitch;

	TRACE_ENTER();

	/* if fb required size has changed, realloc! */

	DEBUG_MSG("Resize!  %dx%d", width, height);

	if (  (width != omap_bo_width(pOMAP->scanout))
	      || (height != omap_bo_height(pOMAP->scanout))
	      || (pScrn->bitsPerPixel != omap_bo_bpp(pOMAP->scanout)) ) {

		DEBUG_MSG("allocating new scanout buffer: %dx%d",
				width, height);

		/* allocate new scanout buffer */
		new_scanout = omap_bo_new_with_depth(pOMAP->dev, width,
					     height, pScrn->depth,
					     pScrn->bitsPerPixel);
		if (!new_scanout) {
			ERROR_MSG("Error reallocating scanout buffer");
			return FALSE;
		}

		pOMAP->has_resized = TRUE;
		omap_bo_unreference(pOMAP->scanout);
		pOMAP->scanout = new_scanout;
	}

	pScrn->virtualX = width;
	pScrn->virtualY = height;
	pitch = omap_bo_pitch(pOMAP->scanout);
	pScrn->displayWidth = pitch / ((pScrn->bitsPerPixel + 7) / 8);

	if (pScreen && pScreen->ModifyPixmapHeader) {
		PixmapPtr rootPixmap = pScreen->GetScreenPixmap(pScreen);
		pScreen->ModifyPixmapHeader(rootPixmap,
				pScrn->virtualX, pScrn->virtualY,
				pScrn->depth, pScrn->bitsPerPixel, pitch,
				omap_bo_map(pOMAP->scanout));
	}

	TRACE_EXIT();
	return TRUE;
}

static const xf86CrtcConfigFuncsRec drmmode_xf86crtc_config_funcs = {
		.resize = drmmode_xf86crtc_resize
};

static void drmmode_crtcs_destroy(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

	/*
	 * xf86CrtcDestroy() removes crtc from list and memmoves the other
	 * entries, and decrements num_crtc.
	 * Hence, as an optimization, we work backwards num_crtc.
	 */
	while (xf86_config->num_crtc) {
		int c = xf86_config->num_crtc - 1;
		xf86CrtcPtr crtc = xf86_config->crtc[c];
		xf86CrtcDestroy(crtc);
	}
}

static void drmmode_outputs_destroy(ScrnInfoPtr pScrn)
{
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

	/*
	 * xf86OutputDestroy() removes output from list and memmoves the other
	 * entries, and decrements num_output.
	 * Hence, as an optimization, we work backwards num_output.
	 */
	while (xf86_config->num_output) {
		int o = xf86_config->num_output - 1;
		xf86OutputPtr output = xf86_config->output[o];
		xf86OutputDestroy(output);
	}
}

Bool drmmode_pre_init(ScrnInfoPtr pScrn, int fd)
{
	drmmode_ptr drmmode;
	drmModeResPtr mode_res;
	drmModePlaneResPtr plane_res;
	int i;
	Bool ret;

	TRACE_ENTER();

//	pScrn->canDoBGNoneRoot = TRUE;

	xf86CrtcConfigInit(pScrn, &drmmode_xf86crtc_config_funcs);

	mode_res = drmModeGetResources(fd);
	if (!mode_res) {
		ret = FALSE;
		goto out;
	}
	DEBUG_MSG("Got KMS resources");
	DEBUG_MSG("  %d connectors, %d encoders",
			mode_res->count_connectors,
			mode_res->count_encoders);
	DEBUG_MSG("  %d crtcs, %d fbs",
			mode_res->count_crtcs, mode_res->count_fbs);
	DEBUG_MSG("  %dx%d minimum resolution",
			mode_res->min_width, mode_res->min_height);
	DEBUG_MSG("  %dx%d maximum resolution",
			mode_res->max_width, mode_res->max_height);

	xf86CrtcSetSizeRange(pScrn, 320, 200, mode_res->max_width,
			mode_res->max_height);

	plane_res = drmModeGetPlaneResources(fd);
	if (!plane_res) {
		ERROR_MSG("drmModeGetPlaneResources failed: %s",
				strerror(errno));
		ret = FALSE;
		goto err_free_drm_resources;
	}
	DEBUG_MSG("  %d planes", plane_res->count_planes);

	drmmode = calloc(1, sizeof *drmmode);
	if (!drmmode) {
		ERROR_MSG("Failed to allocate drmmode");
		ret = FALSE;
		goto err_free_drm_plane_resources;
	}
	drmmode->fd = fd;

	ret = TRUE;
	for (i = 0; i < mode_res->count_crtcs && ret; i++)
		ret = drmmode_crtc_pre_init(pScrn, drmmode, mode_res,
				plane_res, i);
	if (!ret)
		goto err_crtcs_destroy;

	for (i = 0; i < mode_res->count_connectors && ret; i++)
		ret = drmmode_output_pre_init(pScrn, drmmode, mode_res, i);
	if (!ret)
		goto err_outputs_destroy;

	ret = xf86InitialConfiguration(pScrn, TRUE);
	if (!ret) {
		ERROR_MSG("xf86 Initial Configuration failed");
		goto err_outputs_destroy;
	}

	drmModeFreePlaneResources(plane_res);
	drmModeFreeResources(mode_res);
	goto out;

err_outputs_destroy:
	drmmode_outputs_destroy(pScrn);
err_crtcs_destroy:
	drmmode_crtcs_destroy(pScrn);
	free(drmmode);
err_free_drm_plane_resources:
	drmModeFreePlaneResources(plane_res);
err_free_drm_resources:
	drmModeFreeResources(mode_res);
out:
	TRACE_EXIT();
	return ret;
}

void
drmmode_adjust_frame(ScrnInfoPtr pScrn, int x, int y)
{
	xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(pScrn);
	xf86OutputPtr output = config->output[config->compat_output];
	xf86CrtcPtr crtc = output->crtc;
	int saved_x, saved_y;
	Bool ret;

	if (!crtc || !crtc->enabled)
		return;

	saved_x = crtc->x;
	saved_y = crtc->y;
	ret = drmmode_set_mode_major(crtc, &crtc->mode, crtc->rotation, x, y);
	if (!ret) {
		crtc->x = saved_x;
		crtc->y = saved_y;
	}
}

/*
 * Page Flipping
 */

static void
page_flip_handler(int fd, unsigned int sequence, unsigned int tv_sec,
		unsigned int tv_usec, void *user_data)
{
	OMAPDRI2SwapComplete(user_data);
}

static drmEventContext event_context = {
		.version = DRM_EVENT_CONTEXT_VERSION,
		.page_flip_handler = page_flip_handler,
};

int
drmmode_page_flip(DrawablePtr draw, uint32_t fb_id, void *priv,
		int* num_flipped)
{
	ScreenPtr pScreen = draw->pScreen;
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	xf86CrtcConfigPtr xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);
	int ret, i;
	unsigned int flags = 0;

#if OMAP_USE_PAGE_FLIP_EVENTS
	flags |= DRM_MODE_PAGE_FLIP_EVENT;
#endif

	/* Flip all crtc's that match this drawable's position and size */
	*num_flipped = 0;
	for (i = 0; i < xf86_config->num_crtc; i++) {
		xf86CrtcPtr crtc = xf86_config->crtc[i];
		uint32_t crtc_id = drmmode_crtc_id(crtc);
		Bool connected = FALSE;
		int j;

		if (!crtc->enabled)
			continue;
		/* crtc can be enabled but all the outputs disabled, which
		   will cause flip to fail with EBUSY, so don't even try.
		   eventually the mode on this CRTC will be disabled */
		for (j = 0; j < xf86_config->num_output; j++) {
			xf86OutputPtr output = xf86_config->output[j];
			connected = connected || (output->crtc == crtc
					&& output->status
					== XF86OutputStatusConnected);
		}
		if (!connected)
			continue;

		if (crtc->x != draw->x || crtc->y != draw->y ||
		    crtc->mode.HDisplay != draw->width ||
		    crtc->mode.VDisplay != draw->height)
			continue;

		DEBUG_MSG("[CRTC:%u] [FB:%u]", crtc_id, fb_id);
		ret = drmModePageFlip(pOMAP->drmFD, crtc_id, fb_id, flags,
				priv);
		if (ret) {
			ERROR_MSG("[CRTC:%u] [FB:%u] page flip failed: %s",
					crtc_id, fb_id, strerror(errno));
			return ret;
		}
		(*num_flipped)++;
	}
	return 0;
}

/*
 * Hot Plug Event handling:
 */

static void
drmmode_handle_uevents(int fd, void *closure)
{
	ScrnInfoPtr pScrn = closure;
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	struct udev_device *dev;
	const char *hotplug;
	struct stat s;
	dev_t udev_devnum;

	dev = udev_monitor_receive_device(drmmode->uevent_monitor);
	if (!dev)
		return;

	// FIXME - Do we need to keep this code, which Rob originally wrote
	// (i.e. up thru the "if" statement)?:

	/*
	 * Check to make sure this event is directed at our
	 * device (by comparing dev_t values), then make
	 * sure it's a hotplug event (HOTPLUG=1)
	 */
	udev_devnum = udev_device_get_devnum(dev);
	if (fstat(pOMAP->drmFD, &s)) {
		ERROR_MSG("fstat failed: %s", strerror(errno));
		udev_device_unref(dev);
		return;
	}

	hotplug = udev_device_get_property_value(dev, "HOTPLUG");

	xf86DrvMsg(pScrn->scrnIndex, X_INFO, "hotplug=%s, match=%d\n", hotplug,
			!memcmp(&s.st_rdev, &udev_devnum, sizeof (dev_t)));

	if (memcmp(&s.st_rdev, &udev_devnum, sizeof (dev_t)) == 0 &&
			hotplug && atoi(hotplug) == 1) {
		RRGetInfo(xf86ScrnToScreen(pScrn), TRUE);
	}
	udev_device_unref(dev);
}

static Bool
drmmode_uevent_init(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	struct udev *u;
	struct udev_monitor *mon;
	Bool ret;

	TRACE_ENTER();

	u = udev_new();
	if (!u) {
		ret = FALSE;
		goto out;
	}
	mon = udev_monitor_new_from_netlink(u, "udev");
	if (!mon) {
		ret = FALSE;
		goto err_udev_unref;
	}

	if (udev_monitor_filter_add_match_subsystem_devtype(mon,
			"drm",
			"drm_minor") < 0 ||
			udev_monitor_enable_receiving(mon) < 0) {
		ret = FALSE;
		goto err_udev_monitor_unref;
	}

	drmmode->uevent_handler =
			xf86AddGeneralHandler(udev_monitor_get_fd(mon),
					drmmode_handle_uevents, pScrn);
	if (!drmmode->uevent_handler) {
		ret = FALSE;
		goto err_udev_monitor_unref;
	}

	drmmode->uevent_monitor = mon;

	ret = TRUE;
	goto out;

err_udev_monitor_unref:
	udev_monitor_unref(mon);
err_udev_unref:
	udev_unref(u);
out:
	TRACE_EXIT();
	return ret;
}

static void
drmmode_uevent_fini(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode;
	struct udev *u;

	TRACE_ENTER();

	drmmode = drmmode_from_scrn(pScrn);
	u = udev_monitor_get_udev(drmmode->uevent_monitor);
	xf86RemoveGeneralHandler(drmmode->uevent_handler);
	drmmode->uevent_handler = NULL;
	udev_monitor_unref(drmmode->uevent_monitor);
	drmmode->uevent_monitor = NULL;
	udev_unref(u);

	TRACE_EXIT();
}

static void
drmmode_wakeup_handler(pointer data, int err, pointer p)
{
	ScrnInfoPtr pScrn = data;
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	fd_set *read_mask = p;

	if (pScrn == NULL || err < 0)
		return;

	if (FD_ISSET(drmmode->fd, read_mask))
		drmHandleEvent(drmmode->fd, &event_context);
}

void
drmmode_wait_for_event(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	drmHandleEvent(drmmode->fd, &event_context);
}

Bool
drmmode_screen_init(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	ScreenPtr pScreen = xf86ScrnToScreen(pScrn);
	Bool ret;

	/* Per ScreenInit cursor initialization */
	ret = xf86_cursors_init(pScreen, CURSORW, CURSORH, HARDWARE_CURSOR_ARGB);
	if (!ret) {
		ERROR_MSG("xf86_cursors_init() failed");
		goto out;
	}

	ret = drmmode_uevent_init(pScrn);
	if (!ret)
		goto err_fini_cursors;

	AddGeneralSocket(drmmode->fd);

	/* Register a wakeup handler to get informed on DRM events */
	ret = RegisterBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
			drmmode_wakeup_handler, pScrn);
	if (!ret) {
		ERROR_MSG("RegisterBlockAndWakeupHandlers() failed");
		goto err_drmmode_uevent_fini;
	}

	ret = TRUE;
	goto out;

err_drmmode_uevent_fini:
	RemoveGeneralSocket(drmmode->fd);
	drmmode_uevent_fini(pScrn);
err_fini_cursors:
	xf86_cursors_fini(pScreen);
out:
	return ret;
}

void
drmmode_close_screen(ScrnInfoPtr pScrn)
{
	drmmode_ptr drmmode = drmmode_from_scrn(pScrn);
	ScreenPtr pScreen = xf86ScrnToScreen(pScrn);

	RemoveBlockAndWakeupHandlers((BlockHandlerProcPtr)NoopDDA,
			drmmode_wakeup_handler, pScrn);
	RemoveGeneralSocket(drmmode->fd);
	drmmode_uevent_fini(pScrn);
	xf86_cursors_fini(pScreen);
}

void drmmode_copy_fb(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	int dst_cpp = (pScrn->bitsPerPixel + 7) / 8;
	uint32_t dst_pitch = pScrn->displayWidth * dst_cpp;
	int src_cpp;
	uint32_t src_pitch;
	unsigned int src_size;
	unsigned char *dst, *src;
	struct fb_var_screeninfo vinfo;
	int fd;
	int ret;

	if (!(dst = omap_bo_map(pOMAP->scanout))) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't map scanout bo\n");
		return;
	}

	fd = open("/dev/fb0", O_RDONLY | O_SYNC);
	if (fd == -1) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't open /dev/fb0: %s\n",
				strerror(errno));
		return;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Vscreeninfo ioctl failed: %s\n",
				strerror(errno));
		goto close_fd;
	}

	if (vinfo.bits_per_pixel != 32)
	{
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"FB found but not 32 bpp\n");
		goto close_fd;
	}

	src_cpp = (vinfo.bits_per_pixel + 7) / 8;
	src_pitch = vinfo.xres_virtual * src_cpp;
	src_size = vinfo.yres_virtual * src_pitch;

	src = mmap(NULL, src_size, PROT_READ, MAP_SHARED, fd, 0);
	if (src == MAP_FAILED) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't mmap /dev/fb0: %s\n",
				strerror(errno));
		goto close_fd;
	}

	ret = omap_bo_cpu_prep(pOMAP->scanout, OMAP_GEM_WRITE);
	if (ret)
		goto munmap_src;

	drmmode_copy_from_to(src, 0, 0, vinfo.xres_virtual, vinfo.yres_virtual,
			src_pitch, src_cpp,
			dst, 0, 0, pScrn->virtualX, pScrn->virtualY,
			dst_pitch, dst_cpp);

	omap_bo_cpu_fini(pOMAP->scanout, 0);

munmap_src:
	ret = munmap(src, src_size);
	if (ret == -1)
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
				"Couldn't munmap /dev/fb0: %s\n",
				strerror(errno));

close_fd:
	close(fd);
}
