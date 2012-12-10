/*
 * Copyright © 2006-2010 Intel Corporation
 * Copyright (c) 2006 Dave Airlie <airlied@linux.ie>
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
 * Authors:
 *	Eric Anholt <eric@anholt.net>
 *      Dave Airlie <airlied@linux.ie>
 *      Jesse Barnes <jesse.barnes@intel.com>
 *      Chris Wilson <chris@chris-wilson.co.uk>
 */

#include "drmP.h"
#include "drm.h"
#include "i915_drv.h"
#include "i915_drm.h"
#include "intel_drv.h"

#define PCI_LBPC 0xf4 /* legacy/combination backlight modes */

int	 is_backlight_combination_mode(struct drm_device *);
u32	 i915_read_blc_pwm_ctl(struct inteldrm_softc *);
void	 intel_pch_panel_set_backlight(struct drm_device *, u32);
void	 intel_panel_actually_set_backlight(struct drm_device *, u32);
void	 intel_panel_init_backlight(struct drm_device *);

int i915_panel_ignore_lid = 0;

void
intel_fixed_panel_mode(struct drm_display_mode *fixed_mode,
    struct drm_display_mode *adjusted_mode)
{
	adjusted_mode->hdisplay = fixed_mode->hdisplay;
	adjusted_mode->hsync_start = fixed_mode->hsync_start;
	adjusted_mode->hsync_end = fixed_mode->hsync_end;
	adjusted_mode->htotal = fixed_mode->htotal;

	adjusted_mode->vdisplay = fixed_mode->vdisplay;
	adjusted_mode->vsync_start = fixed_mode->vsync_start;
	adjusted_mode->vsync_end = fixed_mode->vsync_end;
	adjusted_mode->vtotal = fixed_mode->vtotal;

	adjusted_mode->clock = fixed_mode->clock;
}

/* adjusted_mode has been preset to be the panel's fixed mode */
void
intel_pch_panel_fitting(struct drm_device *dev, int fitting_mode,
    const struct drm_display_mode *mode, struct drm_display_mode *adjusted_mode)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;
	int x, y, width, height;

	x = y = width = height = 0;

	/* Native modes don't need fitting */
	if (adjusted_mode->hdisplay == mode->hdisplay &&
	    adjusted_mode->vdisplay == mode->vdisplay)
		goto done;

	switch (fitting_mode) {
	case DRM_MODE_SCALE_CENTER:
		width = mode->hdisplay;
		height = mode->vdisplay;
		x = (adjusted_mode->hdisplay - width + 1)/2;
		y = (adjusted_mode->vdisplay - height + 1)/2;
		break;

	case DRM_MODE_SCALE_ASPECT:
		/* Scale but preserve the aspect ratio */
		{
			u32 scaled_width = adjusted_mode->hdisplay * mode->vdisplay;
			u32 scaled_height = mode->hdisplay * adjusted_mode->vdisplay;
			if (scaled_width > scaled_height) { /* pillar */
				width = scaled_height / mode->vdisplay;
				if (width & 1)
					width++;
				x = (adjusted_mode->hdisplay - width + 1) / 2;
				y = 0;
				height = adjusted_mode->vdisplay;
			} else if (scaled_width < scaled_height) { /* letter */
				height = scaled_width / mode->hdisplay;
				if (height & 1)
					height++;
				y = (adjusted_mode->vdisplay - height + 1) / 2;
				x = 0;
				width = adjusted_mode->hdisplay;
			} else {
				x = y = 0;
				width = adjusted_mode->hdisplay;
				height = adjusted_mode->vdisplay;
			}
		}
		break;

	default:
	case DRM_MODE_SCALE_FULLSCREEN:
		x = y = 0;
		width = adjusted_mode->hdisplay;
		height = adjusted_mode->vdisplay;
		break;
	}

done:
	dev_priv->pch_pf_pos = (x << 16) | y;
	dev_priv->pch_pf_size = (width << 16) | height;
}

int
is_backlight_combination_mode(struct drm_device *dev)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;

	if (INTEL_INFO(dev_priv)->gen >= 4)
		return I915_READ(BLC_PWM_CTL2) & BLM_COMBINATION_MODE;

	if (IS_GEN2(dev_priv))
		return I915_READ(BLC_PWM_CTL) & BLM_LEGACY_MODE;

	return 0;
}

u32
i915_read_blc_pwm_ctl(struct inteldrm_softc *dev_priv)
{
	u32 val;

	/* Restore the CTL value if it lost, e.g. GPU reset */

	if (HAS_PCH_SPLIT(dev_priv)) {
		val = I915_READ(BLC_PWM_PCH_CTL2);
		if (dev_priv->saveBLC_PWM_CTL2 == 0) {
			dev_priv->saveBLC_PWM_CTL2 = val;
		} else if (val == 0) {
			I915_WRITE(BLC_PWM_PCH_CTL2,
				   dev_priv->saveBLC_PWM_CTL2);
			val = dev_priv->saveBLC_PWM_CTL2;
		}
	} else {
		val = I915_READ(BLC_PWM_CTL);
		if (dev_priv->saveBLC_PWM_CTL == 0) {
			dev_priv->saveBLC_PWM_CTL = val;
			dev_priv->saveBLC_PWM_CTL2 = I915_READ(BLC_PWM_CTL2);
		} else if (val == 0) {
			I915_WRITE(BLC_PWM_CTL,
				   dev_priv->saveBLC_PWM_CTL);
			I915_WRITE(BLC_PWM_CTL2,
				   dev_priv->saveBLC_PWM_CTL2);
			val = dev_priv->saveBLC_PWM_CTL;
		}
	}

	return val;
}

u32
intel_panel_get_max_backlight(struct drm_device *dev)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;
	u32 max;

	max = i915_read_blc_pwm_ctl(dev_priv);
	if (max == 0) {
		/* XXX add code here to query mode clock or hardware clock
		 * and program max PWM appropriately.
		 */
#if 0
		printf("fixme: max PWM is zero.\n");
#endif
		return 1;
	}

	if (HAS_PCH_SPLIT(dev_priv)) {
		max >>= 16;
	} else {
		if (INTEL_INFO(dev_priv)->gen < 4)
			max >>= 17;
		else
			max >>= 16;

		if (is_backlight_combination_mode(dev))
			max *= 0xff;
	}

	DRM_DEBUG("max backlight PWM = %d\n", max);
	return max;
}

u32
intel_panel_get_backlight(struct drm_device *dev)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;
	u32 val;

	if (HAS_PCH_SPLIT(dev_priv)) {
		val = I915_READ(BLC_PWM_CPU_CTL) & BACKLIGHT_DUTY_CYCLE_MASK;
	} else {
		val = I915_READ(BLC_PWM_CTL) & BACKLIGHT_DUTY_CYCLE_MASK;
		if (INTEL_INFO(dev_priv)->gen < 4)
			val >>= 1;

		if (is_backlight_combination_mode(dev)) {
			u8 lbpc;

			lbpc = pci_conf_read(dev_priv->pc, dev_priv->tag,
			    PCI_LBPC) & 0xff;
			val *= lbpc;
		}
	}

	DRM_DEBUG("get backlight PWM = %d\n", val);
	return val;
}

void
intel_pch_panel_set_backlight(struct drm_device *dev, u32 level)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;
	u32 val = I915_READ(BLC_PWM_CPU_CTL) & ~BACKLIGHT_DUTY_CYCLE_MASK;
	I915_WRITE(BLC_PWM_CPU_CTL, val | level);
}

void
intel_panel_actually_set_backlight(struct drm_device *dev, u32 level)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;
	u32 tmp;

	DRM_DEBUG("set backlight PWM = %d\n", level);

	if (HAS_PCH_SPLIT(dev_priv))
		return intel_pch_panel_set_backlight(dev, level);

	if (is_backlight_combination_mode(dev)) {
		u32 max = intel_panel_get_max_backlight(dev);
		u8 lbpc;

		lbpc = level * 0xfe / max + 1;
		level /= lbpc;
		pci_conf_write(dev_priv->pc, dev_priv->tag, PCI_LBPC, lbpc);
	}

	tmp = I915_READ(BLC_PWM_CTL);
	if (INTEL_INFO(dev_priv)->gen < 4) 
		level <<= 1;
	tmp &= ~BACKLIGHT_DUTY_CYCLE_MASK;
	I915_WRITE(BLC_PWM_CTL, tmp | level);
}

void
intel_panel_set_backlight(struct drm_device *dev, u32 level)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;

	dev_priv->backlight_level = level;
	if (dev_priv->backlight_enabled)
		intel_panel_actually_set_backlight(dev, level);
}

void
intel_panel_disable_backlight(struct drm_device *dev)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;

	dev_priv->backlight_enabled = false;
	intel_panel_actually_set_backlight(dev, 0);
}

void
intel_panel_enable_backlight(struct drm_device *dev)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;

	if (dev_priv->backlight_level == 0)
		dev_priv->backlight_level = intel_panel_get_max_backlight(dev);

	dev_priv->backlight_enabled = true;
	intel_panel_actually_set_backlight(dev, dev_priv->backlight_level);
}

void
intel_panel_init_backlight(struct drm_device *dev)
{
	struct inteldrm_softc *dev_priv = dev->dev_private;

	dev_priv->backlight_level = intel_panel_get_backlight(dev);
	dev_priv->backlight_enabled = dev_priv->backlight_level != 0;
}

enum drm_connector_status
intel_panel_detect(struct drm_device *dev)
{
#if 0
	struct inteldrm_softc *dev_priv = dev->dev_private;
#endif

	if (i915_panel_ignore_lid)
		return i915_panel_ignore_lid > 0 ?
			connector_status_connected :
			connector_status_disconnected;

	/* opregion lid state on HP 2540p is wrong at boot up,
	 * appears to be either the BIOS or Linux ACPI fault */
#if 0
	/* Assume that the BIOS does not lie through the OpRegion... */
	if (dev_priv->opregion.lid_state)
		return ioread32(dev_priv->opregion.lid_state) & 0x1 ?
			connector_status_connected :
			connector_status_disconnected;
#endif

	return connector_status_unknown;
}

int
intel_panel_setup_backlight(struct drm_device *dev)
{
	intel_panel_init_backlight(dev);
	return 0;
}

void
intel_panel_destroy_backlight(struct drm_device *dev)
{
	return;
}
