/*
 * cdc_plane.c  --  CDC Display Controller Plane
 *
 * Copyright (C) 2017 TES Electronic Solutions GmbH
 * Author: Christian Thaler <christian.thaler@tes-dst.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>

#include "cdc_regs.h"
#include "cdc_drv.h"
#include "cdc_kms.h"
#include "cdc_plane.h"
#include "cdc_hw_helpers.h"

static struct cdc_plane *to_cdc_plane(struct drm_plane *p)
{
	return container_of(p, struct cdc_plane, plane);
}

void cdc_plane_setup_fb(struct cdc_plane *plane)
{
	struct cdc_device *cdc = plane->cdc;
	unsigned int layer = plane->hw_idx;
	struct drm_framebuffer *fb = plane->plane.state->fb;
	struct drm_gem_cma_object *gem;
	unsigned int byte_offset;
	dma_addr_t fb_addr;

	byte_offset = (plane->plane.state->src_y >> 16) * fb->pitches[0]
		+ (plane->plane.state->src_x >> 16) * (fb->format->cpp[0] * 8);
	gem = drm_fb_cma_get_gem_obj(fb, 0);
	fb_addr = gem->paddr + fb->offsets[0] + byte_offset;
	cdc_hw_setCBAddress(cdc, layer, fb_addr);
	if(cdc->dswz) {
		dswz_set_fb_addr(cdc->dswz, fb_addr);
	}
}

void cdc_plane_setup_window(struct drm_plane *plane)
{
	struct cdc_plane *cplane = to_cdc_plane(plane);
	struct cdc_device *cdc = cplane->cdc;
	unsigned int layer = cplane->hw_idx;
	const struct drm_display_mode *mode =
		&plane->state->crtc->state->adjusted_mode;
	int32_t x = plane->state->crtc_x;
	int32_t y = plane->state->crtc_y;
	int32_t w = plane->state->crtc_w;
	int32_t h = plane->state->crtc_h;

	/* Do clipping. CDC requires windows that lie inside of the screen. */
	if (x >= mode->hdisplay)
		x = mode->hdisplay - 1;
	if (y >= mode->vdisplay)
		y = mode->vdisplay - 1;
	if ((x + w) > mode->hdisplay)
		w -= x + w - mode->hdisplay;
	if ((y + h) > mode->vdisplay)
		h -= y + h - mode->vdisplay;

	dev_dbg(cdc->dev, "%s for layer %d (plane: 0x%p, cdc: 0x%p, crtc: 0x%p)\n",
		__func__, layer, plane, cdc, plane->crtc);
	dev_dbg(cdc->dev, "plane state crtc: %d\n", plane->state->crtc->base.id);
	dev_dbg(cdc->dev, "setWindow(%d,%d:%dx%d)@%dx%d\n", x, y, w, h, mode->hdisplay,
		mode->vdisplay);

	cdc->planes[layer].window_width = w;
	cdc->planes[layer].window_height = h;
	cdc->planes[layer].window_x = x;
	cdc->planes[layer].window_y = y;

	cdc_hw_setWindow(cdc, layer, x, y, w, h, plane->state->fb->pitches[0]);
}

int cdc_plane_disable(struct drm_plane *plane)
{
	struct cdc_plane *cplane = to_cdc_plane(plane);
	struct cdc_device *cdc = cplane->cdc;
	int layer;

	dev_dbg(cdc->dev, "%s (plane: %d)\n", __func__, cplane->hw_idx);

	if (!cplane->enabled) {
		return 0;
	}

	layer = cplane->hw_idx;
	cdc_hw_layer_setEnabled(cdc, layer, false);

	return 0;
}

static void cdc_plane_atomic_update(struct drm_plane *plane,
	struct drm_plane_state *old_state)
{
	struct cdc_plane *cplane = to_cdc_plane(plane);
	struct cdc_device *cdc = cplane->cdc;
	struct drm_plane_state *new_state = plane->state; /* todo: use cdc_plane_state_only */
	struct cdc_plane_state *old_cstate = to_cdc_plane_state(old_state);
	struct cdc_plane_state *new_cstate = to_cdc_plane_state(plane->state);
	int layer = cplane->hw_idx;

	dev_dbg(cdc->dev, "%s (plane: %d)\n", __func__, cplane->hw_idx);

	if (old_cstate->alpha != new_cstate->alpha) {
		dev_dbg(cdc->dev, "Plane %d: setting alpha to %u\n", layer,
			new_cstate->alpha);
		cdc_hw_layer_setConstantAlpha(cdc, layer, new_cstate->alpha);
	}

	// Setup the plane if a crtc is bound to it
	if (new_state->crtc) {
		/* plane setup */
		/* todo: find out what to change and only change that, like with the window */
		cdc_hw_setPixelFormat(cdc, layer,
			cdc_format_info(new_state->fb->format->format)->cdc_hw_format);

		// note: in the CDC default config, only CONS_ALPHA(_INV) and ALPHA_X_CONST_ALPHA(_INV) are available
		if ((layer != 0)
			&& (new_state->fb->format->format != DRM_FORMAT_XRGB8888)) {
			// Enable pixel alpha for overlay layers only
			cdc_hw_setBlendMode(cdc, layer,
				CDC_BLEND_PIXEL_ALPHA_X_CONST_ALPHA,
				CDC_BLEND_PIXEL_ALPHA_X_CONST_ALPHA_INV);
		} else {
			// No blending for primary layer and layers with XRGB8888 format (ignore the alpha value)
			cdc_hw_setBlendMode(cdc, layer, CDC_BLEND_CONST_ALPHA,
				CDC_BLEND_CONST_ALPHA_INV);
		}

		cdc_plane_setup_fb(cplane);

		if ((old_state->crtc_x != new_state->crtc_x)
			|| (old_state->crtc_y != new_state->crtc_y)
			|| (old_state->crtc_h != new_state->crtc_h)
			|| (old_state->crtc_w != new_state->crtc_w)) {
			cdc_plane_setup_window(plane);
		}

		if (!old_state->crtc)
			cdc_hw_layer_setEnabled(cdc, layer, true);

	} else if (old_state->crtc) {
		cdc_hw_layer_setEnabled(cdc, layer, false);
	}
}

static int cdc_plane_atomic_set_property(struct drm_plane *plane,
	struct drm_plane_state *state, struct drm_property *property, uint64_t val)
{
	struct cdc_plane_state *cstate = to_cdc_plane_state(state);
	struct cdc_plane *cplane = to_cdc_plane(plane);
	struct cdc_device *cdc = cplane->cdc;

	if (property == cdc->alpha)
		cstate->alpha = val;
	else
		return -EINVAL;

	return 0;
}

static int cdc_plane_atomic_get_property(struct drm_plane *plane,
	const struct drm_plane_state *state, struct drm_property *property,
	uint64_t *val)
{
	const struct cdc_plane_state
	*cstate =
		container_of(state, const struct cdc_plane_state, state);
	struct cdc_plane *cplane = to_cdc_plane(plane);
	struct cdc_device *cdc = cplane->cdc;

	if (property == cdc->alpha)
		*val = cstate->alpha;
	else
		return -EINVAL;

	return 0;
}

static void cdc_plane_reset(struct drm_plane *plane)
{
	struct cdc_plane_state *state;

	if (plane->state && plane->state->fb)
		drm_framebuffer_unreference(plane->state->fb);

	kfree(plane->state);
	plane->state = NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state == NULL)
		return;

	state->alpha = 255;

	plane->state = &state->state;
	plane->state->plane = plane;
}

static struct drm_plane_state *
	cdc_plane_atomic_duplicate_state(struct drm_plane *plane)
{
	struct cdc_plane_state *state;
	struct cdc_plane_state *copy;

	state = to_cdc_plane_state(plane->state);
	copy = kmemdup(state, sizeof(*state), GFP_KERNEL);
	if (copy == NULL)
		return NULL;

	if (copy->state.fb)
		drm_framebuffer_reference(copy->state.fb);

	return &copy->state;
}

static void cdc_plane_atomic_destroy_state(struct drm_plane *plane,
	struct drm_plane_state *state)
{
	if (state->fb)
		drm_framebuffer_unreference(state->fb);

	kfree(to_cdc_plane_state(state));
}

static const struct drm_plane_helper_funcs cdc_plane_helper_funcs = {
	.atomic_update = cdc_plane_atomic_update,
};

static const struct drm_plane_funcs cdc_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.atomic_set_property = cdc_plane_atomic_set_property,
	.atomic_get_property = cdc_plane_atomic_get_property,
	.reset = cdc_plane_reset,
	.atomic_duplicate_state = cdc_plane_atomic_duplicate_state,
	.atomic_destroy_state = cdc_plane_atomic_destroy_state,
};

static const uint32_t cdc_supported_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_ARGB1555,
};

int cdc_planes_init(struct cdc_device *cdc)
{
	int ret;
	int i;

	dev_dbg(cdc->dev, "%s\n", __func__);

	cdc->alpha = drm_property_create_range(cdc->ddev, 0, "alpha", 0, 255);
	if (cdc->alpha == NULL)
		return -ENOMEM;

	for (i = 0; i < cdc->hw.layer_count; ++i) {
		enum drm_plane_type type;
		struct cdc_plane *plane = &cdc->planes[i];

		if (i == 0)
			type = DRM_PLANE_TYPE_PRIMARY;
		else if (i == cdc->hw.layer_count - 1)
			type = DRM_PLANE_TYPE_CURSOR;
		else
			type = DRM_PLANE_TYPE_OVERLAY;

		dev_dbg(cdc->dev, "Initializing plane %d as %d type...\n", i, type);
		ret = drm_universal_plane_init(cdc->ddev, &plane->plane, 1,
			&cdc_plane_funcs, cdc_supported_formats,
			ARRAY_SIZE(cdc_supported_formats), NULL,
			type, NULL);
		if (ret < 0) {
			dev_err(cdc->dev, "could not initialize plane %d...\n", i);
			return ret;
		}

		drm_plane_helper_add(&plane->plane, &cdc_plane_helper_funcs);

		if (type != DRM_PLANE_TYPE_OVERLAY)
			continue;

		dev_dbg(cdc->dev, "Adding alpha property to plane %d...\n", i);
		drm_object_attach_property(&plane->plane.base, cdc->alpha, 255);
	}

	return 0;
}
