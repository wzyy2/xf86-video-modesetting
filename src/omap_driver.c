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
 *    Ian Elliott <ianelliottus@yahoo.com>
 *    Rob Clark <rob@ti.com>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "omap_driver.h"
#include "compat-api.h"

Bool omapDebug = 0;

/*
 * Forward declarations:
 */
static const OptionInfoRec *OMAPAvailableOptions(int chipid, int busid);
static void OMAPIdentify(int flags);
static Bool OMAPProbe(DriverPtr drv, int flags);
static Bool OMAPPreInit(ScrnInfoPtr pScrn, int flags);
static Bool OMAPScreenInit(SCREEN_INIT_ARGS_DECL);
static void OMAPLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices,
		LOCO * colors, VisualPtr pVisual);
static Bool OMAPCloseScreen(CLOSE_SCREEN_ARGS_DECL);
static Bool OMAPSwitchMode(SWITCH_MODE_ARGS_DECL);
static void OMAPAdjustFrame(ADJUST_FRAME_ARGS_DECL);
static Bool OMAPEnterVT(VT_FUNC_ARGS_DECL);
static void OMAPLeaveVT(VT_FUNC_ARGS_DECL);
static void OMAPFreeScreen(FREE_SCREEN_ARGS_DECL);



/**
 * A structure used by the XFree86 code when loading this driver, so that it
 * can access the Probe() function, and other functions/info that it uses
 * before it calls the Probe() function.  The name of this structure must be
 * the all-upper-case version of the driver name.
 */
_X_EXPORT DriverRec OMAP = {
		OMAP_VERSION,
		(char *)OMAP_DRIVER_NAME,
		OMAPIdentify,
		OMAPProbe,
		OMAPAvailableOptions,
		NULL,
		0,
		NULL,
#ifdef XSERVER_LIBPCIACCESS
		NULL,
		NULL
#endif
};

/** Supported "chipsets." */
static SymTabRec OMAPChipsets[] = {
	{ OMAP_CHIPSET_EXYNOS5, "exynos5" },
	{-1, NULL }
};

/** Supported options, as enum values. */
typedef enum {
	OPTION_DEBUG,
} OMAPOpts;

/** Supported options. */
static const OptionInfoRec OMAPOptions[] = {
	{ OPTION_DEBUG,		"Debug",	OPTV_BOOLEAN,	{0},	FALSE },
	{ -1,			NULL,		OPTV_NONE,	{0},	FALSE }
};

/**
 * Helper function for opening a connection to the DRM.
 */

static int
OMAPOpenDRM(int n)
{
	return open("/dev/dri/card0", O_RDWR, 0);
}

static Bool
OMAPOpenDRMMaster(ScrnInfoPtr pScrn, int n)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	drmSetVersion sv;
	int err;

	pOMAP->drmFD = OMAPOpenDRM(n);
	if (pOMAP->drmFD == -1) {
		ERROR_MSG("Cannot open a connection with the DRM.");
		return FALSE;
	}

	/* Check that what we opened was a master or a master-capable FD,
	 * by setting the version of the interface we'll use to talk to it.
	 * (see DRIOpenDRMMaster() in DRI1)
	 */
	sv.drm_di_major = 1;
	sv.drm_di_minor = 1;
	sv.drm_dd_major = -1;
	sv.drm_dd_minor = -1;
	err = drmSetInterfaceVersion(pOMAP->drmFD, &sv);
	if (err != 0) {
		ERROR_MSG("Cannot set the DRM interface version.");
		drmClose(pOMAP->drmFD);
		pOMAP->drmFD = -1;
		return FALSE;
	}

	pOMAP->deviceName = drmGetDeviceNameFromFd(pOMAP->drmFD);

	return TRUE;
}



/**
 * Helper function for closing a connection to the DRM.
 */
static void
OMAPCloseDRMMaster(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);

	if (pOMAP && (pOMAP->drmFD > 0)) {
		drmFree(pOMAP->deviceName);
		drmClose(pOMAP->drmFD);
		pOMAP->drmFD = -1;
	}
}

static Bool
OMAPMapMem(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);

	DEBUG_MSG("allocating new scanout buffer: %dx%d",
			pScrn->virtualX, pScrn->virtualY);

	pOMAP->scanout = omap_bo_new_with_depth(pOMAP->dev, pScrn->virtualX,
			pScrn->virtualY, pScrn->depth, pScrn->bitsPerPixel);
	if (!pOMAP->scanout) {
		xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
			   "Error allocating scanout buffer\n");
		return FALSE;
	}

	pScrn->displayWidth = omap_bo_pitch(pOMAP->scanout) / (pScrn->bitsPerPixel / 8);

	return TRUE;
}


static Bool
OMAPUnmapMem(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	omap_bo_unreference(pOMAP->scanout);
	pOMAP->scanout = NULL;
	pScrn->displayWidth = 0;
	return TRUE;
}



/** Let the XFree86 code know the Setup() function. */
static MODULESETUPPROTO(OMAPSetup);

/** Provide basic version information to the XFree86 code. */
static XF86ModuleVersionInfo OMAPVersRec =
{
		OMAP_DRIVER_NAME,
		MODULEVENDORSTRING,
		MODINFOSTRING1,
		MODINFOSTRING2,
		XORG_VERSION_CURRENT,
		PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
		ABI_CLASS_VIDEODRV,
		ABI_VIDEODRV_VERSION,
		MOD_CLASS_VIDEODRV,
		{0, 0, 0, 0}
};

/** Let the XFree86 code know about the VersRec and Setup() function. */
_X_EXPORT XF86ModuleData armsocModuleData = { &OMAPVersRec, OMAPSetup, NULL };


/**
 * The first function that the XFree86 code calls, after loading this module.
 */
static pointer
OMAPSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
	static Bool setupDone = FALSE;

	/* This module should be loaded only once, but check to be sure: */
	if (!setupDone) {
		setupDone = TRUE;
		xf86AddDriver(&OMAP, module, 0);

		/* The return value must be non-NULL on success even though there is no
		 * TearDownProc.
		 */
		return (pointer) 1;
	} else {
		if (errmaj)
			*errmaj = LDR_ONCEONLY;
		return NULL;
	}
}


/**
 * Allocate the driver's Screen-specific, "private" data structure and hook it
 * into the ScrnInfoRec's driverPrivate field.
 */
static Bool
OMAPGetRec(ScrnInfoPtr pScrn)
{
	OMAPPtr pOMAP = pScrn->driverPrivate;

	if (pOMAP != NULL)
		return TRUE;

	pOMAP = calloc(1, sizeof *pOMAP);
	if (pOMAP == NULL)
		return FALSE;

	pScrn->driverPrivate = pOMAP;

	return TRUE;
}


/**
 * Free the driver's Screen-specific, "private" data structure and NULL-out the
 * ScrnInfoRec's driverPrivate field.
 */
static void
OMAPFreeRec(ScrnInfoPtr pScrn)
{
	if (pScrn->driverPrivate == NULL)
		return;
	free(pScrn->driverPrivate);
	pScrn->driverPrivate = NULL;
}


/**
 * The mandatory AvailableOptions() function.  It returns the available driver
 * options to the "-configure" option, so that an xorg.conf file can be built
 * and the user can see which options are available for them to use.
 */
static const OptionInfoRec *
OMAPAvailableOptions(int chipid, int busid)
{
	return OMAPOptions;
}



/**
 * The mandatory Identify() function.  It is run before Probe(), and prints out
 * an identifying message, which includes the chipset(s) the driver supports.
 */
static void
OMAPIdentify(int flags)
{
	xf86PrintChipsets(OMAP_NAME, "Driver for TI OMAP", OMAPChipsets);
}



/**
 * The driver's Probe() function.  This function finds all instances of the
 * TI OMAP hardware that the driver supports (from within the "xorg.conf"
 * device sections), and for instances not already claimed by another driver,
 * claim the instances, and allocate a ScrnInfoRec.  Only minimal hardware
 * probing is allowed here.
 */
static Bool
OMAPProbe(DriverPtr drv, int flags)
{
	int i;
	ScrnInfoPtr pScrn;
	GDevPtr *devSections = NULL;
	int numDevSections;
	Bool foundScreen = FALSE;

	/* Get the "xorg.conf" file device sections that match this driver, and
	 * return (error out) if there are none:
	 */
	numDevSections = xf86MatchDevice(OMAP_DRIVER_NAME, &devSections);
	if (numDevSections <= 0) {
		EARLY_ERROR_MSG("Did not find any matching device section in "
				"configuration file");
		if (flags & PROBE_DETECT) {
			/* if we are probing, assume one and lets see if we can
			 * open the device to confirm it is there:
			 */
			numDevSections = 1;
		} else {
			goto out;
		}
	}

	for (i = 0; i < numDevSections; i++) {
		int fd = OMAPOpenDRM(i);
		if (fd != -1) {

			if (flags & PROBE_DETECT) {
				/* just add the device.. we aren't a PCI device, so
				 * call xf86AddBusDeviceToConfigure() directly
				 */
				xf86AddBusDeviceToConfigure(OMAP_DRIVER_NAME,
						BUS_NONE, NULL, i);
				foundScreen = TRUE;
				drmClose(fd);
				continue;
			}

			pScrn = xf86AllocateScreen(drv, 0);

			if (!pScrn) {
				EARLY_ERROR_MSG("Cannot allocate a ScrnInfoPtr");
				drmClose(fd);
				goto free_sections;
			}

			if (devSections) {
				int entity = xf86ClaimNoSlot(drv, 0, devSections[i], TRUE);
				xf86AddEntityToScreen(pScrn, entity);
			}

			foundScreen = TRUE;

			pScrn->driverVersion = OMAP_VERSION;
			pScrn->driverName    = (char *)OMAP_DRIVER_NAME;
			pScrn->name          = (char *)OMAP_NAME;
			pScrn->Probe         = OMAPProbe;
			pScrn->PreInit       = OMAPPreInit;
			pScrn->ScreenInit    = OMAPScreenInit;
			pScrn->SwitchMode    = OMAPSwitchMode;
			pScrn->AdjustFrame   = OMAPAdjustFrame;
			pScrn->EnterVT       = OMAPEnterVT;
			pScrn->LeaveVT       = OMAPLeaveVT;
			pScrn->FreeScreen    = OMAPFreeScreen;

			/* would be nice to be able to keep the connection open.. but
			 * currently we don't allocate the private until PreInit
			 */
			drmClose(fd);
		}
	}

free_sections:
	free(devSections);

out:
	return foundScreen;
}



/**
 * The driver's PreInit() function.  Additional hardware probing is allowed
 * now, including display configuration.
 */
static Bool
OMAPPreInit(ScrnInfoPtr pScrn, int flags)
{
	OMAPPtr pOMAP;
	int default_depth, fbbpp;
	rgb defaultWeight = { 0, 0, 0 };
	rgb defaultMask = { 0, 0, 0 };
	Gamma defaultGamma = { 0.0, 0.0, 0.0 };

	TRACE_ENTER();

	if (flags & PROBE_DETECT) {
		ERROR_MSG("The %s driver does not support the \"-configure\" or "
				"\"-probe\" command line arguments.", OMAP_NAME);
		return FALSE;
	}

	/* Check the number of entities, and fail if it isn't one. */
	if (pScrn->numEntities != 1) {
		ERROR_MSG("Driver expected 1 entity, but found %d for screen %d",
				pScrn->numEntities, pScrn->scrnIndex);
		return FALSE;
	}

	/* Allocate the driver's Screen-specific, "private" data structure: */
	OMAPGetRec(pScrn);
	pOMAP = OMAPPTR(pScrn);

	pOMAP->pEntityInfo = xf86GetEntityInfo(pScrn->entityList[0]);

	pScrn->monitor = pScrn->confScreen->monitor;

	/* Get the current depth, and set it for XFree86: */
	default_depth = 24;  /* TODO: get from kernel */
	fbbpp = 32;  /* TODO: get from kernel */

	if (!xf86SetDepthBpp(pScrn, default_depth, 0, fbbpp, Support32bppFb)) {
		/* The above function prints an error message. */
		goto fail;
	}
	xf86PrintDepthBpp(pScrn);

	/* Set the color weight: */
	if (!xf86SetWeight(pScrn, defaultWeight, defaultMask)) {
		/* The above function prints an error message. */
		goto fail;
	}

	/* Set the gamma: */
	if (!xf86SetGamma(pScrn, defaultGamma)) {
		/* The above function prints an error message. */
		goto fail;
	}

	/* Visual init: */
	if (!xf86SetDefaultVisual(pScrn, -1)) {
		/* The above function prints an error message. */
		goto fail;
	}

	/* We don't support 8-bit depths: */
	if (pScrn->depth < 16) {
		ERROR_MSG("The requested default visual (%s) has an unsupported "
				"depth (%d).",
				xf86GetVisualName(pScrn->defaultVisual), pScrn->depth);
		goto fail;
	}

	/* Using a programmable clock: */
	pScrn->progClock = TRUE;

	/* Open a connection to the DRM, so we can communicate with the KMS code: */
	if (!OMAPOpenDRMMaster(pScrn, 0)) {
		goto fail;
	}
	DEBUG_MSG("Became DRM master.");

	/* create DRM device instance: */
	pOMAP->dev = omap_device_new(pOMAP->drmFD, pScrn);

	pScrn->chipset = (char *)xf86TokenToString(OMAPChipsets,
			OMAP_CHIPSET_EXYNOS5);

	INFO_MSG("Chipset: %s", pScrn->chipset);

	/*
	 * Process the "xorg.conf" file options:
	 */
	xf86CollectOptions(pScrn, NULL);
	if (!(pOMAP->pOptionInfo = malloc(sizeof(OMAPOptions))))
		return FALSE;
	memcpy(pOMAP->pOptionInfo, OMAPOptions, sizeof(OMAPOptions));
	xf86ProcessOptions(pScrn->scrnIndex, pOMAP->pEntityInfo->device->options,
			pOMAP->pOptionInfo);

	/* Determine if the user wants debug messages turned on: */
	omapDebug = xf86ReturnOptValBool(pOMAP->pOptionInfo, OPTION_DEBUG, FALSE);

	/*
	 * Select the video modes:
	 */
	INFO_MSG("Setting the video modes ...");

	/* Don't call drmCheckModesettingSupported() as its written only for
	 * PCI devices.
	 */

	/* Do initial KMS setup: */
	if (!drmmode_pre_init(pScrn, pOMAP->drmFD)) {
		ERROR_MSG("Cannot get KMS resources");
	} else {
		INFO_MSG("Got KMS resources");
	}

	xf86RandR12PreInit(pScrn);

	/* Let XFree86 calculate or get (from command line) the display DPI: */
	xf86SetDpi(pScrn, 0, 0);

	/* Ensure we have a supported depth: */
	switch (pScrn->bitsPerPixel) {
	case 16:
	case 24:
	case 32:
		break;
	default:
		ERROR_MSG("The requested number of bits per pixel (%d) is unsupported.",
				pScrn->bitsPerPixel);
		goto fail;
	}


	/* Load external sub-modules now: */

	if (!(xf86LoadSubModule(pScrn, "dri2") &&
			xf86LoadSubModule(pScrn, "exa") &&
			xf86LoadSubModule(pScrn, "fb"))) {
		goto fail;
	}


	TRACE_EXIT();
	return TRUE;

fail:
	TRACE_EXIT();
	return FALSE;
}


/**
 * The driver's ScreenInit() function.  Fill in pScreen, map the frame buffer,
 * save state, initialize the mode, etc.
 */
static Bool
OMAPScreenInit(SCREEN_INIT_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	VisualPtr visual;
	xf86CrtcConfigPtr xf86_config;
	int i;

	TRACE_ENTER();

	/* Allocate a bo for the root window pixmap */
	if (!OMAPMapMem(pScrn))
		return FALSE;

	/* For a smooth transition from console to X, copy the current fbcon
	 * contents to the root window.
	 */
	drmmode_copy_fb(pScrn);

	/* The root window pixmap bo (pOMAP->scanout) has valid contents now,
	 * so we start out claiming we're in blit mode.
	 * The root window is displayed when we do:
	 *   OMAPEnterVT() ->
	 *    xf86SetDesiredModes() ->
	 *     drmmode_set_mode_major() ->
	 *      drmmode_set_crtc() ->
	 *       drmModeSetCrtc()
	 */
	pOMAP->flip_mode = OMAP_FLIP_DISABLED;

	xf86_config = XF86_CRTC_CONFIG_PTR(pScrn);

	/* need to point to new screen on server regeneration */
	for (i = 0; i < xf86_config->num_crtc; i++)
		xf86_config->crtc[i]->scrn = pScrn;
	for (i = 0; i < xf86_config->num_output; i++)
		xf86_config->output[i]->scrn = pScrn;

	/*
	 * The next step is to setup the screen's visuals, and initialize the
	 * framebuffer code.  In cases where the framebuffer's default
	 * choices for things like visual layouts and bits per RGB are OK,
	 * this may be as simple as calling the framebuffer's ScreenInit()
	 * function.  If not, the visuals will need to be setup before calling
	 * a fb ScreenInit() function and fixed up after.
	 *
	 * For most PC hardware at depths >= 8, the defaults that fb uses
	 * are not appropriate.  In this driver, we fixup the visuals after.
	 */

	/*
	 * Reset the visual list.
	 */

	miClearVisualTypes();
	if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth),
			pScrn->rgbBits, pScrn->defaultVisual)) {
		ERROR_MSG("Cannot initialize the visual type for %d bits per pixel!",
				pScrn->bitsPerPixel);
		goto fail;
	}

	/* Also add a 32-bit depth XRGB8888 visual */
	if (!miSetVisualTypes(32, miGetDefaultVisualMask(pScrn->depth),
				pScrn->rgbBits, pScrn->defaultVisual)) {
		WARNING_MSG("Cannot initialize a depth-32 visual");
	} else {
		INFO_MSG("Initialized a depth-32 visual for XRGB8888");
	}

	if (!miSetPixmapDepths()) {
		ERROR_MSG("Cannot initialize the pixmap depth!");
		goto fail;
	}

	/* Initialize some generic 2D drawing functions: */
	if (!fbScreenInit(pScreen, omap_bo_map(pOMAP->scanout),
			pScrn->virtualX, pScrn->virtualY,
			pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth,
			pScrn->bitsPerPixel)) {
		ERROR_MSG("fbScreenInit() failed!");
		goto fail;
	}

	/* Fixup RGB ordering: */
	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
		if ((visual->class | DynamicClass) == DirectColor) {
			unsigned widest_component;
			unsigned red_width, green_width, blue_width;

			visual->offsetRed = pScrn->offset.red;
			visual->offsetGreen = pScrn->offset.green;
			visual->offsetBlue = pScrn->offset.blue;
			visual->redMask = pScrn->mask.red;
			visual->greenMask = pScrn->mask.green;
			visual->blueMask = pScrn->mask.blue;
			/*
			 * crbug.com/340790: Override default nplanes and
			 * ColormapEntries computed by mi for our depth 32
			 * xRGB888 visual.
			 */
			visual->nplanes = Ones(visual->redMask |
					visual->greenMask | visual->blueMask);

			/* find widest component */
			red_width = Ones(visual->redMask);
			green_width = Ones(visual->greenMask);
			blue_width = Ones(visual->blueMask);
			widest_component = max(red_width,
					max(green_width, blue_width));
			visual->ColormapEntries = 1 << widest_component;
		}
	}

	/* Continue initializing the generic 2D drawing functions after fixing the
	 * RGB ordering:
	 */
	if (!fbPictureInit(pScreen, NULL, 0)) {
		ERROR_MSG("fbPictureInit() failed!");
		goto fail;
	}

	/* Set the initial black & white colormap indices: */
	xf86SetBlackWhitePixels(pScreen);

	/* Initialize external sub-modules for EXA now, this has to be before
	 * miDCInitialize() otherwise stacking order for wrapped ScreenPtr fxns
	 * ends up in the wrong order.
	 */
	pOMAP->pOMAPEXA = InitNullEXA(pScreen, pScrn, pOMAP->drmFD);
	if (!pOMAP->pOMAPEXA) {
		ERROR_MSG("InitNullEXA() failed!");
		goto fail;
	}

	if (!OMAPDRI2ScreenInit(pScreen)) {
		ERROR_MSG("OMAPDRI2ScreenInit() failed!");
		goto fail;
	}

	/* Initialize backing store: */
//	miInitializeBackingStore(pScreen);
	xf86SetBackingStore(pScreen);

	/* Cause the cursor position to be updated by the mouse signal handler: */
	xf86SetSilkenMouse(pScreen);

	/* Initialize the cursor: */
	miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

	/* XXX -- Is this the right place for this?  The Intel i830 driver says:
	 * "Must force it before EnterVT, so we are in control of VT..."
	 */
	pScrn->vtSema = TRUE;

	/* Take over the virtual terminal from the console, set the desired mode,
	 * etc.:
	 */
	if (!OMAPEnterVT(VT_FUNC_ARGS(0))) {
		ERROR_MSG("OMAPEnterVT() failed!");
		goto fail;
	}

	/* Do some XRandR initialization: */
	if (!xf86CrtcScreenInit(pScreen)) {
		ERROR_MSG("xf86CrtcScreenInit() failed!");
		goto fail;
	}

	if (!miCreateDefColormap(pScreen)) {
		ERROR_MSG("Cannot create colormap!");
		goto fail;
	}

	/* The default xRGB888 visual uses 8 bits per component */
	if (!xf86HandleColormaps(pScreen, 256, 8, OMAPLoadPalette, NULL,
			CMAP_PALETTED_TRUECOLOR)) {
		ERROR_MSG("xf86HandleColormaps() failed!");
		goto fail;
	}

	/* Setup power management: */
	xf86DPMSInit(pScreen, xf86DPMSSet, 0);

	pScreen->SaveScreen = xf86SaveScreen;

	/* Wrap some screen functions: */
	wrap(pOMAP, pScreen, CloseScreen, OMAPCloseScreen);

	if (!drmmode_screen_init(pScrn)) {
		ERROR_MSG("drmmode_screen_init() failed!");
		goto fail;
	}

	TRACE_EXIT();
	return TRUE;

fail:
	TRACE_EXIT();
	return FALSE;
}


static void
OMAPLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices,
		LOCO * colors, VisualPtr pVisual)
{
	TRACE_ENTER();
	TRACE_EXIT();
}


/**
 * The driver's CloseScreen() function.  This is called at the end of each
 * server generation.  Restore state, unmap the frame buffer (and any other
 * mapped memory regions), and free per-Screen data structures (except those
 * held by pScrn).
 */
static Bool
OMAPCloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(pScreen);
	OMAPPtr pOMAP = OMAPPTR(pScrn);
	Bool ret;

	TRACE_ENTER();

	drmmode_close_screen(pScrn);

	if (pScrn->vtSema == TRUE)
		OMAPLeaveVT(VT_FUNC_ARGS(0));

	unwrap(pOMAP, pScreen, CloseScreen);

	ret = (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);

	if (pOMAP->pOMAPEXA->CloseScreen)
		pOMAP->pOMAPEXA->CloseScreen(CLOSE_SCREEN_ARGS);

	OMAPDRI2CloseScreen(pScreen);

	OMAPUnmapMem(pScrn);

	pScrn->vtSema = FALSE;

	TRACE_EXIT();

	return ret;
}


/**
 * The driver's SwitchMode() function.  Initialize the new mode for the
 * Screen.
 */
static Bool
OMAPSwitchMode(SWITCH_MODE_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	return xf86SetSingleMode(pScrn, mode, RR_Rotate_0);
}



/**
 * The driver's AdjustFrame() function.  For cases where the frame buffer is
 * larger than the monitor resolution, this function can pan around the frame
 * buffer within the "viewport" of the monitor.
 */
static void
OMAPAdjustFrame(ADJUST_FRAME_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	drmmode_adjust_frame(pScrn, x, y);
}



/**
 * The driver's EnterVT() function.  This is called at server startup time, and
 * when the X server takes over the virtual terminal from the console.  As
 * such, it may need to save the current (i.e. console) HW state, and set the
 * HW state as needed by the X server.
 */
static Bool
OMAPEnterVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	OMAPPtr pOMAP = OMAPPTR(pScrn);

	TRACE_ENTER();

	if (geteuid() == 0) {
		if (drmSetMaster(pOMAP->drmFD)) {
			ERROR_MSG("Cannot get DRM master: %s", strerror(errno));
			return FALSE;
		}
	}

	if (!xf86SetDesiredModes(pScrn)) {
		ERROR_MSG("xf86SetDesiredModes() failed!");
		return FALSE;
	}

	TRACE_EXIT();
	return TRUE;
}



/**
 * The driver's LeaveVT() function.  This is called when the X server
 * temporarily gives up the virtual terminal to the console.  As such, it may
 * need to restore the console's HW state.
 */
static void
OMAPLeaveVT(VT_FUNC_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	OMAPPtr pOMAP = OMAPPTR(pScrn);

	TRACE_ENTER();

	if (geteuid() == 0) {
		if (drmDropMaster(pOMAP->drmFD)) {
			WARNING_MSG("drmDropMaster failed: %s", strerror(errno));
		}
	}

	TRACE_EXIT();
}



/**
 * The driver's FreeScreen() function.  It is only called if PreInit() returns
 * FALSE.  It is not called during normal (error free) operation.
 * Its primary function is to free any data allocated by PreInit that persists
 * across server generations, such as the ScrnInfoRec driverPrivate field, and
 * any privates entries that modules may have allocated.
 * Per-generation data should be freed by the CloseScreen() function.
 */
static void
OMAPFreeScreen(FREE_SCREEN_ARGS_DECL)
{
	SCRN_INFO_PTR(arg);
	OMAPPtr pOMAP = OMAPPTR(pScrn);

	TRACE_ENTER();

	if (!pOMAP) {
		/* This can happen if a Screen is deleted after Probe(): */
		return;
	}

	if (pOMAP->pOMAPEXA) {
		if (pOMAP->pOMAPEXA->FreeScreen) {
			pOMAP->pOMAPEXA->FreeScreen(FREE_SCREEN_ARGS(pScrn));
		}
		free(pOMAP->pOMAPEXA);
	}

	omap_device_del(pOMAP->dev);

	OMAPCloseDRMMaster(pScrn);

	OMAPFreeRec(pScrn);

	TRACE_EXIT();
}

