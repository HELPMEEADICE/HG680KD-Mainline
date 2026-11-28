/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef HI3798_HDMI_SCDC_H
#define HI3798_HDMI_SCDC_H

#include <linux/types.h>

struct hi3798_hdmi_sink {
	void __iomem *ddc;
	void __iomem *aon;
	u32 max_tmds_khz;
	bool rgb, y420, scdc;
	int version_error;
	u8 version, config_before, config_after, scrambler, status;
	bool configured, verified;
};

int hi3798_sink_probe(struct hi3798_hdmi_sink *sink, u8 vic, bool y420, u32 tmds_khz);
int hi3798_sink_configure(struct hi3798_hdmi_sink *sink, bool high);
int hi3798_sink_verify(struct hi3798_hdmi_sink *sink, bool high);

#endif
