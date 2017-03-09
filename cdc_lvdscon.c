/*
 * cdc_lvdscon.c  --  CDC Display Controller LVDS Connector
 *
 * Copyright (C) 2016 TES Electronic Solutions GmbH
 * Author: Christian Thaler <christian.thaler@tes-dst.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include "cdc_regs.h"
#include "cdc_drv.h"
#include "cdc_kms.h"
#include "cdc_encoder.h"


struct cdc_lvds_connector {
        struct cdc_connector connector;

        struct {
                unsigned int width_mm;          /* Panel width in mm */
                unsigned int height_mm;         /* Panel height in mm */
                struct videomode mode;
        } panel;
};


#define to_cdc_lvds_connector(c) \
        container_of(c, struct cdc_lvds_connector, connector.connector)


static int cdc_lvds_connector_get_modes(struct drm_connector *connector)
{
  struct drm_display_mode *mode;
  struct cdc_lvds_connector *lvdscon = to_cdc_lvds_connector(connector);
  struct cdc_device *cdc = lvdscon->connector.cdc;

  dev_dbg(cdc->dev, "%s\n", __func__);

  mode = drm_mode_create(connector->dev);
  if(mode == NULL)
  {
    dev_err(cdc->dev, "could not create mode\n");
      return 0;
  }

  mode->type = DRM_MODE_TYPE_PREFERRED | DRM_MODE_TYPE_DRIVER;
  drm_display_mode_from_videomode(&lvdscon->panel.mode, mode);

  drm_mode_probed_add(connector, mode);

  return 1;
}


static const struct drm_connector_helper_funcs connector_helper_funcs = {
  .get_modes = cdc_lvds_connector_get_modes,
  .best_encoder = cdc_connector_best_encoder,
};


/*******************************************************************************
 * drm_connector_funcs
 */

static void cdc_lvds_connector_destroy(struct drm_connector *connector) {
  struct cdc_lvds_connector *lvdscon = to_cdc_lvds_connector(connector);
  struct cdc_device *cdc = lvdscon->connector.cdc;

  dev_dbg(cdc->dev, "%s\n", __func__);

  drm_connector_unregister(connector);
  drm_connector_cleanup(connector);
}


static enum drm_connector_status cdc_lvds_connector_detect(struct drm_connector *connector, bool force) {
  struct cdc_lvds_connector *lvdscon = to_cdc_lvds_connector(connector);
  struct cdc_device *cdc = lvdscon->connector.cdc;

  dev_dbg(cdc->dev, "%s\n", __func__);

  return connector_status_connected;
}


static const struct drm_connector_funcs connector_funcs = {
  .dpms = drm_atomic_helper_connector_dpms,
  .reset = drm_atomic_helper_connector_reset,
  .detect = cdc_lvds_connector_detect,
  .fill_modes = drm_helper_probe_single_connector_modes,
  .destroy = cdc_lvds_connector_destroy,
  .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
  .atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};


int cdc_lvds_connector_init(struct cdc_device *cdc, struct cdc_encoder *enc, struct device_node *np)
{
  struct drm_encoder *encoder = cdc_encoder_to_drm_encoder(enc);
  struct drm_connector *connector;
  struct cdc_lvds_connector *lvdscon;
  struct display_timing timing;
  int ret;

  dev_dbg(cdc->dev, "%s\n", __func__);

  lvdscon = devm_kzalloc(cdc->dev, sizeof(*lvdscon), GFP_KERNEL);
  if(lvdscon == NULL)
    return -ENOMEM;

  lvdscon->connector.cdc = cdc;

  ret = of_get_display_timing(np, "panel-timing", &timing);
  if(ret < 0)
    return ret;

  videomode_from_timing(&timing, &lvdscon->panel.mode);

  /* We have to carry DE polarity and pixel clock polarity separate,
   * since DRM does not offer any flags for it.
   */
  if(lvdscon->panel.mode.flags & DISPLAY_FLAGS_DE_LOW)
    cdc->neg_blank = true;
  else
    cdc->neg_blank = false;

  if(lvdscon->panel.mode.flags & DISPLAY_FLAGS_PIXDATA_NEGEDGE)
      cdc->neg_pixclk = true;
    else
      cdc->neg_pixclk = false;

  of_property_read_u32(np, "width-mm", &lvdscon->panel.width_mm);
  of_property_read_u32(np, "height-mm", &lvdscon->panel.height_mm);

  connector = &lvdscon->connector.connector;

  connector->display_info.width_mm  = lvdscon->panel.width_mm;
  connector->display_info.height_mm = lvdscon->panel.height_mm;
  connector->interlace_allowed      = false;
  connector->doublescan_allowed     = false;
  connector->polled                 = 0;

  ret = drm_connector_init(cdc->ddev, connector, &connector_funcs, DRM_MODE_CONNECTOR_LVDS);
  if(ret < 0) {
    dev_err(cdc->dev, "Error initializing connector: %d\n", ret);
    return ret;
  }

  drm_connector_helper_add(connector, &connector_helper_funcs);

  ret = drm_connector_register(connector);
  if(ret < 0)
  {
    dev_err(cdc->dev, "Error adding connector to sysfs (%d)\n", ret);
    return ret;
  }

  connector->dpms = DRM_MODE_DPMS_OFF;
  drm_object_property_set_value(&connector->base, cdc->ddev->mode_config.dpms_property, DRM_MODE_DPMS_OFF);

  ret = drm_mode_connector_attach_encoder(connector, encoder);
  if(ret < 0) {
    dev_err(cdc->dev, "Error attaching encoder and connector: %d\n", ret);
    return ret;
  }

  lvdscon->connector.encoder = enc;

  return 0;
}
