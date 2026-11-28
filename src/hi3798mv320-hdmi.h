/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HI3798MV320_HDMI_H
#define HI3798MV320_HDMI_H

#include <linux/types.h>
#include <drm/drm_modes.h>

struct drm_connector;
struct mv320_hdmi;
struct platform_device;
struct regmap;

struct mv320_hdmi_mode {
	struct drm_display_mode timing;
	u32 tmds_khz, hpll0, hpll1, drive;
	u8 vic, vdp_div, hd0_div, ssc_div, tmds_div, phy_divsel;
	bool y420;
};

struct mv320_hdmi *mv320_hdmi_init(struct platform_device *pdev, struct regmap *crg,
				  struct drm_connector *connector);
void mv320_hdmi_hpd_enable(struct mv320_hdmi *hdmi);
void mv320_hdmi_hpd_disable(struct mv320_hdmi *hdmi);
bool mv320_hdmi_detect(struct mv320_hdmi *hdmi);
int mv320_hdmi_get_modes(struct mv320_hdmi *hdmi);
const struct mv320_hdmi_mode *mv320_hdmi_find_mode(const struct drm_display_mode *mode);
const struct mv320_hdmi_mode *mv320_hdmi_select_mode(struct mv320_hdmi *hdmi,
						   const struct drm_display_mode *mode);
void mv320_hdmi_disable(struct mv320_hdmi *hdmi);
int mv320_hdmi_prepare(struct mv320_hdmi *hdmi, const struct mv320_hdmi_mode *mode,
		       bool hdmi_sink);
int mv320_hdmi_enable(struct mv320_hdmi *hdmi, const struct mv320_hdmi_mode *mode,
		      bool hdmi_sink, bool scdc);

#endif
