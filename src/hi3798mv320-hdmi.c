// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/hdmi.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/unaligned.h>

#include <drm/display/drm_scdc.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_probe_helper.h>

#include "hi3798mv320-hdmi.h"

#define AON_HPD_STATUS		0x28
#define AON_IRQ_ENABLE		0x30
#define AON_IRQ_STATUS		0x34
#define AON_HPD			BIT(0)
#define AON_HPD_EVENTS		GENMASK(1, 0)
#define AON_DDC_ERR		0x54
#define AON_DDC_PINS		0x5c

#define DDC_OUT			0x38
#define DDC_IN			0x3c
#define DDC_COUNT		0x40
#define DDC_SLAVE_OFFSET	0x44
#define DDC_STATUS		0x48
#define DDC_CMD			0x4c
#define DDC_CLEAR		0x50
#define DDC_REQ			0x68
#define DDC_ACK			0x6c
#define DDC_ARB			0x70
#define DDC_NACK		BIT(0)
#define DDC_BUS_LOW		BIT(1)
#define DDC_BUSY		BIT(2)
#define DDC_EMPTY		BIT(5)
#define DDC_FIFO_COUNT		GENMASK(4, 0)
#define DDC_LENGTH		GENMASK(17, 8)
#define DDC_READ		2
#define DDC_SEG_READ		4
#define DDC_WRITE		7
#define DDC_FLUSH		9
#define DDC_ABORT		15
#define EDID_ADDR		0xa0
#define SCDC_ADDR		0xa8
#define SCDC_TMDS_MASK		(SCDC_SCRAMBLING_ENABLE | SCDC_TMDS_BIT_CLOCK_RATIO_BY_40)

#define CRG_HPLL0		0x28
#define CRG_HPLL1		0x2c
#define CRG_HPLL_PD		BIT(20)
#define CRG_HD0			0xd8
#define CRG_TX			0x10c
#define CRG_PHY			0x110
#define CRG_PLL_LOCK		0x150
#define CRG_HPLL_LOCK		BIT(3)
#define CRG_VDP			0x34c
#define CRG_PIXEL_GATE		BIT(16)
#define CRG_PHY_GATE		BIT(0)
#define CRG_PHY_RESET		GENMASK(5, 4)

#define PHY_POWER		0x64
#define PHY_CHANNEL_PD		GENMASK(3, 0)
#define PHY_DRIVER_PD		GENMASK(8, 5)
#define PHY_TERM		GENMASK(18, 11)
#define PHY_OUTPUT		(PHY_CHANNEL_PD | PHY_DRIVER_PD | PHY_TERM)
#define PHY_LDO_PD		GENMASK(10, 9)
#define PHY_RXSENSE_PD		BIT(19)
#define PHY_PLL_PD		BIT(20)
#define PHY_DIVSEL		GENMASK(22, 21)
#define PHY_LOCK_STATUS		0x98
#define PHY_PLL_LOCK		BIT(20)

#define TX_RESET		0x10
#define TX_AUDIO_RESETS		GENMASK(8, 6)
#define AUD_CTRL		0x00
#define AUD_MUTE		BIT(1)
#define AUD_I2S			0x04
#define AUD_CHST0		0x0c
#define AUD_CHST4		0x10
#define AUD_ACR			0x40
#define AUD_N			0x48
#define AUD_SW_CTS		0x4c
#define AUD_HW_CTS		0x50
#define AUD_ACR_VALUE		GENMASK(19, 0)

#define PKT_STABLE		0x04
#define PKT_AVI_HEADER		0x18
#define PKT_AUDIO_HEADER	0x3c
#define PKT_DRM_HEADER		0xa8
#define PKT_AVI_ENABLE		0x1a4
#define PKT_AUDIO_ENABLE	0x1ac
#define PKT_DRM_ENABLE		0x1b4
#define PKT_GCP_ENABLE		0x1b8
#define PKT_ENABLE_REPEAT	GENMASK(1, 0)
#define PKT_LINK_MODE		0x208
#define PKT_GCP			0x20c
#define PKT_ENCODER		0x260
#define PKT_HDMI2		BIT(0)
#define PKT_SCRAMBLE		BIT(1)
#define PKT_ENCODER_FLAGS	(BIT(5) | BIT(2) | PKT_SCRAMBLE | PKT_HDMI2)
#define AVMUTE_SET		BIT(0)
#define AVMUTE_CLEAR		BIT(1)

#define UHD_MODE(clock) DRM_MODE("3840x2160", 0, clock, 3840, 4016, 4104, 4400, 0, \
				  2160, 2168, 2178, 2250, 0, \
				  DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC)

static const struct mv320_hdmi_mode mv320_hdmi_modes[] = {
	{
		.timing = { DRM_MODE("1920x1080", 0, 148500, 1920, 2008, 2052, 2200, 0,
				     1080, 1084, 1089, 1125, 0,
				     DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
		.tmds_khz = 148500, .hpll0 = 0x11800000, .hpll1 = 0x1031,
		.drive = 0x0075d75d, .vic = 16, .vdp_div = 1, .ssc_div = 9,
		.tmds_div = 1, .phy_divsel = 1,
	}, {
		.timing = { UHD_MODE(297000) },
		.tmds_khz = 297000, .hpll0 = 0x12000000, .hpll1 = 0x1063,
		.drive = 0x0079e79f, .vic = 95, .vdp_div = 1, .hd0_div = 3, .ssc_div = 9,
	}, {
		.timing = { UHD_MODE(594000) },
		.tmds_khz = 297000, .hpll0 = 0x12000000, .hpll1 = 0x1063,
		.drive = 0x0079e79f, .vic = 97, .hd0_div = 3, .ssc_div = 9, .y420 = true,
	}, {
		.timing = { UHD_MODE(594000) },
		.tmds_khz = 594000, .hpll0 = 0x12000000, .hpll1 = 0x1063,
		.drive = 0x00d34d2d, .vic = 97, .hd0_div = 3, .ssc_div = 4,
	},
};

struct mv320_hdmi {
	void __iomem *tx, *aon, *video, *audio, *packets, *phy;
	struct regmap *crg;
	struct drm_connector *connector;
	struct mutex ddc_lock;
	atomic_t hpd_generation;
	int edid_generation;
	int irq;
	bool output_enabled;
};

static void mv320_hdmi_update(void __iomem *base, u32 offset, u32 mask, u32 value)
{
	writel((readl(base + offset) & ~mask) | (value & mask), base + offset);
}

static bool mv320_hdmi_connected(struct mv320_hdmi *hdmi)
{
	/* RxSense shares the event IRQ. Only HPD establishes DDC presence. */
	return readl(hdmi->aon + AON_HPD_STATUS) & AON_HPD;
}

bool mv320_hdmi_detect(struct mv320_hdmi *hdmi)
{
	bool connected = mv320_hdmi_connected(hdmi);

	if (!connected)
		atomic_inc(&hdmi->hpd_generation);
	return connected;
}

static irqreturn_t mv320_hpd_irq(int irq, void *data)
{
	struct mv320_hdmi *hdmi = data;
	u32 events = readl(hdmi->aon + AON_IRQ_STATUS) & AON_HPD_EVENTS;

	if (!events)
		return IRQ_NONE;

	writel(events, hdmi->aon + AON_IRQ_STATUS);
	readl(hdmi->aon + AON_IRQ_STATUS);
	if (events & AON_HPD)
		atomic_inc(&hdmi->hpd_generation);
	drm_connector_helper_hpd_irq_event(hdmi->connector);
	return IRQ_HANDLED;
}

void mv320_hdmi_hpd_enable(struct mv320_hdmi *hdmi)
{
	writel(AON_HPD_EVENTS, hdmi->aon + AON_IRQ_STATUS);
	mv320_hdmi_update(hdmi->aon, AON_IRQ_ENABLE, AON_HPD_EVENTS, AON_HPD_EVENTS);
	enable_irq(hdmi->irq);
}

void mv320_hdmi_hpd_disable(struct mv320_hdmi *hdmi)
{
	mv320_hdmi_update(hdmi->aon, AON_IRQ_ENABLE, AON_HPD_EVENTS, 0);
	disable_irq(hdmi->irq);
}

struct mv320_hdmi *mv320_hdmi_init(struct platform_device *pdev, struct regmap *crg,
				  struct drm_connector *connector)
{
	struct device *dev = &pdev->dev;
	struct mv320_hdmi *hdmi;
	int ret;

	hdmi = devm_kzalloc(dev, sizeof(*hdmi), GFP_KERNEL);
	if (!hdmi)
		return ERR_PTR(-ENOMEM);

	hdmi->tx = devm_platform_ioremap_resource_byname(pdev, "tx");
	if (IS_ERR(hdmi->tx))
		return ERR_CAST(hdmi->tx);
	hdmi->aon = devm_platform_ioremap_resource_byname(pdev, "aon");
	if (IS_ERR(hdmi->aon))
		return ERR_CAST(hdmi->aon);
	hdmi->video = devm_platform_ioremap_resource_byname(pdev, "video");
	if (IS_ERR(hdmi->video))
		return ERR_CAST(hdmi->video);
	hdmi->audio = devm_platform_ioremap_resource_byname(pdev, "audio");
	if (IS_ERR(hdmi->audio))
		return ERR_CAST(hdmi->audio);
	hdmi->packets = devm_platform_ioremap_resource_byname(pdev, "packets");
	if (IS_ERR(hdmi->packets))
		return ERR_CAST(hdmi->packets);
	hdmi->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(hdmi->phy))
		return ERR_CAST(hdmi->phy);

	hdmi->crg = crg;
	hdmi->connector = connector;
	mutex_init(&hdmi->ddc_lock);
	hdmi->output_enabled = true; /* The bootloader can be driving the PHY. */
	hdmi->irq = platform_get_irq_byname(pdev, "hpd");
	if (hdmi->irq < 0)
		return ERR_PTR(hdmi->irq);
	mv320_hdmi_update(hdmi->aon, AON_IRQ_ENABLE, AON_HPD_EVENTS, 0);
	ret = devm_request_threaded_irq(dev, hdmi->irq, NULL, mv320_hpd_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "hi3798mv320-hpd", hdmi);
	if (ret)
		return ERR_PTR(ret);

	return hdmi;
}

static int mv320_ddc_transfer(struct mv320_hdmi *hdmi, u8 slave, u8 offset,
			      u8 segment, u8 *data, unsigned int length, bool write)
{
	void __iomem *tx = hdmi->tx, *aon = hdmi->aon;
	unsigned int received = 0;
	u32 saved_addr, saved_count, arb, status, errors, count, value;
	int ret, idle = 0, release;
	ktime_t deadline;

	lockdep_assert_held(&hdmi->ddc_lock);
	if (!mv320_hdmi_connected(hdmi))
		return -ENOLINK;
	if ((readl(tx + DDC_REQ) | readl(tx + DDC_ACK)) & BIT(0))
		return -EBUSY;

	arb = readl(tx + DDC_ARB);
	mv320_hdmi_update(tx, DDC_REQ, BIT(0), BIT(0));
	ret = readl_poll_timeout(tx + DDC_ACK, value, value & BIT(0), 50, 50000);
	if (ret)
		goto release;

	status = readl(tx + DDC_STATUS);
	if ((status & DDC_BUSY) || !(status & DDC_EMPTY) ||
	    (readl(aon + AON_DDC_PINS) & 3) != 3) {
		ret = -EBUSY;
		goto release;
	}

	saved_addr = readl(tx + DDC_SLAVE_OFFSET);
	saved_count = readl(tx + DDC_COUNT) & ~DDC_FIFO_COUNT;
	writel(DDC_FLUSH, tx + DDC_CMD);
	/* Clear stale NACK or bus-low after holding arbitration. */
	writel(3, tx + DDC_CLEAR);
	mv320_hdmi_update(tx, DDC_SLAVE_OFFSET, GENMASK(23, 0), segment << 16 | offset << 8 | slave);
	writel((saved_count & ~DDC_LENGTH) | FIELD_PREP(DDC_LENGTH, length), tx + DDC_COUNT);
	if (write) {
		/* The controller starts command 7 before filling the TX FIFO. */
		writel(DDC_WRITE, tx + DDC_CMD);
		writel(*data, tx + DDC_IN);
		usleep_range(1000, 1500);
	} else {
		writel(segment ? DDC_SEG_READ : DDC_READ, tx + DDC_CMD);
	}

	deadline = ktime_add_ms(ktime_get(), 50);
	do {
		if (!mv320_hdmi_connected(hdmi)) {
			ret = -ENOLINK;
			break;
		}
		status = readl(tx + DDC_STATUS);
		errors = status | readl(aon + AON_DDC_ERR);
		if (errors & (DDC_BUS_LOW | DDC_NACK)) {
			ret = errors & DDC_BUS_LOW ? -EIO : -ENXIO;
			break;
		}
		count = readl(tx + DDC_COUNT) & DDC_FIFO_COUNT;
		if (!write) {
			if (count > length - received) {
				ret = -EOVERFLOW;
				break;
			}
			while (count--)
				data[received++] = readl(tx + DDC_OUT);
		}
		if (!(status & DDC_BUSY) && (write ? !count : received == length)) {
			ret = 0;
			break;
		}
		ret = -ETIMEDOUT;
		usleep_range(40, 80);
	} while (ktime_before(ktime_get(), deadline));

	if (ret) {
		writel(DDC_ABORT, tx + DDC_CMD);
		idle = readl_poll_timeout(tx + DDC_STATUS, value, !(value & DDC_BUSY), 50, 50000);
		if (!idle) {
			writel(DDC_FLUSH, tx + DDC_CMD);
			writel(3, tx + DDC_CLEAR);
		}
	}
	if (!idle) {
		writel(saved_count, tx + DDC_COUNT);
		writel(saved_addr, tx + DDC_SLAVE_OFFSET);
	}

release:
	mv320_hdmi_update(tx, DDC_REQ, BIT(0), 0);
	release = readl_poll_timeout(tx + DDC_ACK, value, !(value & BIT(0)), 50, 50000);
	if (!release && readl(tx + DDC_ARB) != arb)
		release = -EIO;
	return idle ?: (release ?: ret);
}

static int mv320_edid_read(void *context, u8 *buf, unsigned int block, size_t len)
{
	struct mv320_hdmi *hdmi = context;

	return mv320_ddc_transfer(hdmi, EDID_ADDR, (block & 1) * EDID_LENGTH, block >> 1,
				  buf, len, false);
}

int mv320_hdmi_get_modes(struct mv320_hdmi *hdmi)
{
	struct drm_connector *connector = hdmi->connector;
	const struct drm_edid *edid;
	struct drm_display_mode *mode;
	int generation = atomic_read(&hdmi->hpd_generation);
	int count;

	/*
	 * Keep the parsed EDID stable across repeated GETCONNECTOR calls.
	 * HPD invalidates it even if the sink was reconnected before detection.
	 */
	if (!connector->edid_blob_ptr || generation != hdmi->edid_generation) {
		mutex_lock(&hdmi->ddc_lock);
		edid = drm_edid_read_custom(connector, mv320_edid_read, hdmi);
		mutex_unlock(&hdmi->ddc_lock);
		if (generation != atomic_read(&hdmi->hpd_generation)) {
			drm_edid_free(edid);
			edid = NULL;
		}
		if (!drm_edid_connector_update(connector, edid))
			hdmi->edid_generation = generation;
		drm_edid_free(edid);
	}

	count = drm_edid_connector_add_modes(connector);
	if (count) {
		/* fbdev uses a full-size dumb buffer. Prefer the safe G0 canvas. */
		list_for_each_entry(mode, &connector->probed_modes, head) {
			const struct mv320_hdmi_mode *link = mv320_hdmi_find_mode(mode);

			if (link && link->vic == 16) {
				drm_set_preferred_mode(connector, mode->hdisplay, mode->vdisplay);
				break;
			}
		}
		return count;
	}

	mode = drm_mode_duplicate(connector->dev, &mv320_hdmi_modes[0].timing);
	if (!mode)
		return 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static bool mv320_mode_matches(const struct drm_display_mode *mode,
			       const struct mv320_hdmi_mode *link)
{
	return drm_mode_match(mode, &link->timing,
			      DRM_MODE_MATCH_TIMINGS | DRM_MODE_MATCH_CLOCK | DRM_MODE_MATCH_FLAGS);
}

const struct mv320_hdmi_mode *mv320_hdmi_find_mode(const struct drm_display_mode *mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mv320_hdmi_modes); i++)
		if (mv320_mode_matches(mode, &mv320_hdmi_modes[i]))
			return &mv320_hdmi_modes[i];
	return NULL;
}

const struct mv320_hdmi_mode *mv320_hdmi_select_mode(struct mv320_hdmi *hdmi,
						   const struct drm_display_mode *mode)
{
	const struct drm_display_info *info = &hdmi->connector->display_info;
	unsigned int max_tmds = info->max_tmds_clock ?: 165000;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mv320_hdmi_modes); i++) {
		const struct mv320_hdmi_mode *link = &mv320_hdmi_modes[i];

		if (!mv320_mode_matches(mode, link) || link->tmds_khz > max_tmds)
			continue;
		if (link->y420 ? !drm_mode_is_420(info, mode) : drm_mode_is_420_only(info, mode))
			continue;
		if (link->tmds_khz > 340000 &&
		    (!info->is_hdmi || !info->hdmi.scdc.supported))
			continue;
		return link;
	}
	return NULL;
}

static int mv320_scdc_configure(struct mv320_hdmi *hdmi, bool required, bool high)
{
	u8 version, value, config, flags = high ? SCDC_TMDS_MASK : 0;
	int ret;

	mutex_lock(&hdmi->ddc_lock);
	ret = mv320_ddc_transfer(hdmi, SCDC_ADDR, SCDC_SINK_VERSION, 0, &version, 1, false);
	if (ret == -ENXIO && !required) {
		ret = 0;
		goto unlock;
	}
	if (ret)
		goto unlock;
	if (version) {
		value = 1;
		ret = mv320_ddc_transfer(hdmi, SCDC_ADDR, SCDC_SOURCE_VERSION, 0, &value, 1, true);
		if (ret)
			goto unlock;
	} else if (required) {
		ret = -EPROTO;
		goto unlock;
	}

	ret = mv320_ddc_transfer(hdmi, SCDC_ADDR, SCDC_TMDS_CONFIG, 0, &value, 1, false);
	if (ret)
		goto unlock;
	config = (value & ~SCDC_TMDS_MASK) | flags;
	ret = mv320_ddc_transfer(hdmi, SCDC_ADDR, SCDC_TMDS_CONFIG, 0, &config, 1, true);
	if (ret)
		goto unlock;
	ret = mv320_ddc_transfer(hdmi, SCDC_ADDR, SCDC_TMDS_CONFIG, 0, &value, 1, false);
	if (!ret && (value & SCDC_TMDS_MASK) != flags)
		ret = -EIO;

unlock:
	mutex_unlock(&hdmi->ddc_lock);
	if (!ret)
		usleep_range(1000, 2000);
	return ret;
}

static int mv320_scdc_wait_lock(struct mv320_hdmi *hdmi)
{
	ktime_t deadline = ktime_add_ms(ktime_get(), 250);
	u8 scrambler, status;
	int ret;

	mutex_lock(&hdmi->ddc_lock);
	do {
		ret = mv320_ddc_transfer(hdmi, SCDC_ADDR, SCDC_SCRAMBLER_STATUS, 0,
					 &scrambler, 1, false);
		if (ret)
			break;
		ret = mv320_ddc_transfer(hdmi, SCDC_ADDR, SCDC_STATUS_FLAGS_0, 0,
					 &status, 1, false);
		if (ret || ((scrambler & SCDC_SCRAMBLING_STATUS) && (status & 0xf) == 0xf))
			break;
		ret = -ETIMEDOUT;
		usleep_range(5000, 10000);
	} while (ktime_before(ktime_get(), deadline));
	mutex_unlock(&hdmi->ddc_lock);
	return ret;
}

static int mv320_hdmi_clocks(struct mv320_hdmi *hdmi, const struct mv320_hdmi_mode *mode)
{
	struct regmap *crg = hdmi->crg;
	u32 value;
	int ret;

	/* Retain the TX APB/AON clocks for out-of-band HPD and DDC. */
	ret = regmap_update_bits(crg, CRG_PHY, CRG_PHY_GATE | CRG_PHY_RESET, CRG_PHY_RESET);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_TX, CRG_PIXEL_GATE, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_HD0, BIT(16) | GENMASK(19, 18),
				 BIT(16) | (mode->hd0_div << 18));
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_HPLL1, CRG_HPLL_PD, CRG_HPLL_PD);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(crg, CRG_PLL_LOCK, value, !(value & CRG_HPLL_LOCK),
				       10, 3000);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_HPLL0, GENMASK(30, 0), mode->hpll0);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_HPLL1, GENMASK(27, 0), mode->hpll1);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(crg, CRG_PLL_LOCK, value, value & CRG_HPLL_LOCK,
				       10, 20000);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_VDP, GENMASK(28, 24), mode->vdp_div << 24);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_TX, GENMASK(13, 10) | CRG_PIXEL_GATE,
				 (mode->ssc_div << 10) | CRG_PIXEL_GATE);
	if (ret)
		return ret;
	return regmap_update_bits(crg, CRG_PHY, GENMASK(8, 6) | CRG_PHY_RESET | CRG_PHY_GATE,
				  (mode->tmds_div << 6) | CRG_PHY_GATE);
}

static int mv320_hdmi_phy(struct mv320_hdmi *hdmi, const struct mv320_hdmi_mode *mode)
{
	void __iomem *phy = hdmi->phy;
	bool high = mode->tmds_khz > 340000;
	u32 value;

	mv320_hdmi_update(phy, PHY_POWER, PHY_LDO_PD | PHY_RXSENSE_PD | PHY_PLL_PD | PHY_DIVSEL |
			  PHY_OUTPUT, FIELD_PREP(PHY_DIVSEL, mode->phy_divsel) | PHY_OUTPUT);
	mv320_hdmi_update(phy, 0x10, BIT(0), BIT(0));
	mv320_hdmi_update(phy, 0x10, BIT(0), 0);
	usleep_range(1000, 1500);

	/* Factory 40-bit clock patterns for 1/10 and 1/40. */
	mv320_hdmi_update(phy, 0x50, GENMASK(18, 9), high ? 0 : (0x1f << 9));
	mv320_hdmi_update(phy, 0x54, GENMASK(29, 0), high ? 0x000fffff : 0x01f07c1f);
	mv320_hdmi_update(phy, 0xb0, BIT(27) | BIT(24) | GENMASK(23, 0), 0x640000);
	mv320_hdmi_update(phy, 0xb8, GENMASK(23, 0), 0);
	mv320_hdmi_update(phy, 0x90, GENMASK(19, 0), 0);
	mv320_hdmi_update(phy, 0x8c, BIT(19) | GENMASK(17, 14) | GENMASK(11, 5),
			  (9 << 14) | (0x35 << 5));
	mv320_hdmi_update(phy, 0xb0, BIT(24), BIT(24));

	/* Drive, pre-emphasis and termination from the MV320 EC PHY rows. */
	mv320_hdmi_update(phy, 0x68, GENMASK(23, 0), mode->drive);
	mv320_hdmi_update(phy, 0x6c, GENMASK(17, 0), 0);
	mv320_hdmi_update(phy, 0x70, GENMASK(11, 0), 0);
	mv320_hdmi_update(phy, 0x74, GENMASK(17, 6), high ? 0 : 0x0002da00);
	writel(high ? 0x60606060 : 0x20202060, phy + 0x78);
	return readl_poll_timeout(phy + PHY_LOCK_STATUS, value, value & PHY_PLL_LOCK, 10, 20000);
}

static void mv320_hdmi_packet(struct mv320_hdmi *hdmi, u32 offset, const u8 *buf)
{
	void __iomem *packets = hdmi->packets + offset;
	unsigned int i;

	writel(get_unaligned_le24(buf), packets);
	for (i = 0; i < 4; i++) {
		const u8 *slot = buf + 3 + i * 7;

		writel(get_unaligned_le32(slot), packets + 4 + i * 8);
		writel(get_unaligned_le24(slot + 4), packets + 8 + i * 8);
	}
}

static int mv320_hdmi_infoframes(struct mv320_hdmi *hdmi, const struct mv320_hdmi_mode *mode)
{
	struct hdmi_avi_infoframe avi;
	struct hdmi_drm_infoframe sdr;
	u8 buf[31] = {};
	int ret;

	hdmi_avi_infoframe_init(&avi);
	avi.video_code = mode->vic;
	avi.colorspace = mode->y420 ? HDMI_COLORSPACE_YUV420 : HDMI_COLORSPACE_RGB;
	avi.picture_aspect = HDMI_PICTURE_ASPECT_16_9;
	avi.scan_mode = HDMI_SCAN_MODE_UNDERSCAN;
	avi.colorimetry = HDMI_COLORIMETRY_ITU_709;
	if (!mode->y420)
		avi.quantization_range = HDMI_QUANTIZATION_RANGE_LIMITED;
	ret = hdmi_avi_infoframe_pack(&avi, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	mv320_hdmi_packet(hdmi, PKT_AVI_HEADER, buf);

	hdmi_drm_infoframe_init(&sdr);
	sdr.eotf = HDMI_EOTF_TRADITIONAL_GAMMA_SDR;
	ret = hdmi_drm_infoframe_pack(&sdr, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	mv320_hdmi_packet(hdmi, PKT_DRM_HEADER, buf);
	return 0;
}

void mv320_hdmi_disable(struct mv320_hdmi *hdmi)
{
	mv320_hdmi_update(hdmi->audio, AUD_CTRL, AUD_MUTE, AUD_MUTE);
	if (hdmi->output_enabled) {
		mv320_hdmi_update(hdmi->packets, PKT_GCP, AVMUTE_SET | AVMUTE_CLEAR, AVMUTE_SET);
		mv320_hdmi_update(hdmi->packets, PKT_GCP_ENABLE, PKT_ENABLE_REPEAT, PKT_ENABLE_REPEAT);
		/* Allow one frame at the slowest supported mode for AVMUTE. */
		msleep(40);
		hdmi->output_enabled = false;
	}
	mv320_hdmi_update(hdmi->packets, PKT_AUDIO_ENABLE, PKT_ENABLE_REPEAT, 0);
	mv320_hdmi_update(hdmi->packets, PKT_AVI_ENABLE, PKT_ENABLE_REPEAT, 0);
	mv320_hdmi_update(hdmi->packets, PKT_DRM_ENABLE, PKT_ENABLE_REPEAT, 0);
	mv320_hdmi_update(hdmi->audio, AUD_ACR, BIT(0), 0);
	mv320_hdmi_update(hdmi->phy, PHY_POWER, PHY_OUTPUT, PHY_OUTPUT);
}

int mv320_hdmi_prepare(struct mv320_hdmi *hdmi, const struct mv320_hdmi_mode *mode,
		       bool hdmi_sink)
{
	void __iomem *video = hdmi->video;
	u32 value;
	int ret;

	mv320_hdmi_update(hdmi->phy, PHY_POWER, PHY_PLL_PD, PHY_PLL_PD);
	ret = readl_poll_timeout(hdmi->phy + PHY_LOCK_STATUS, value, !(value & PHY_PLL_LOCK),
				 10, 3000);
	if (ret)
		return ret;
	ret = mv320_hdmi_clocks(hdmi, mode);
	if (ret)
		return ret;
	ret = mv320_hdmi_phy(hdmi, mode);
	if (ret)
		return ret;

	/* GDM supplies BT.709 YCbCr for 4:2:0. Bypass the TX CSC. */
	mv320_hdmi_update(video, 0x134, GENMASK(11, 0), 0x440);
	mv320_hdmi_update(video, 0x180, GENMASK(3, 0), mode->y420 ? 7 : BIT(3));
	mv320_hdmi_update(video, 0x18c, BIT(10) | BIT(0), mode->y420 ? BIT(10) | BIT(0) : 0);
	mv320_hdmi_update(video, 0x1a8, GENMASK(28, 21) | GENMASK(8, 0), mode->y420 ? 0x88 : 0x11);
	mv320_hdmi_update(hdmi->packets, 0x00, BIT(16) | GENMASK(1, 0), 0);
	mv320_hdmi_update(hdmi->packets, PKT_LINK_MODE, GENMASK(1, 0), hdmi_sink ? 3 : 0);
	if (hdmi_sink)
		return mv320_hdmi_infoframes(hdmi, mode);
	mv320_hdmi_update(hdmi->packets, PKT_GCP_ENABLE, PKT_ENABLE_REPEAT, 0);
	return 0;
}

static int mv320_hdmi_audio(struct mv320_hdmi *hdmi, u32 tmds_khz)
{
	void __iomem *audio = hdmi->audio;
	u32 cts;
	int ret;

	/* I2S, S16_LE stereo, 48 kHz IEC 60958 status and software CTS. */
	mv320_hdmi_update(audio, AUD_CTRL, GENMASK(8, 0), 0xf3);
	mv320_hdmi_update(audio, AUD_I2S, GENMASK(15, 0), 0x200);
	writel(0x02000000, audio + AUD_CHST0);
	mv320_hdmi_update(audio, AUD_CHST4, GENMASK(7, 0), 0xd2);
	mv320_hdmi_update(audio, AUD_N, AUD_ACR_VALUE, 6144);
	mv320_hdmi_update(audio, AUD_SW_CTS, AUD_ACR_VALUE, tmds_khz);
	mv320_hdmi_update(audio, AUD_ACR, GENMASK(4, 0), 7);
	mv320_hdmi_update(hdmi->tx, TX_RESET, TX_AUDIO_RESETS, TX_AUDIO_RESETS);
	udelay(10);
	mv320_hdmi_update(hdmi->tx, TX_RESET, TX_AUDIO_RESETS, 0);

	ret = readl_poll_timeout(audio + AUD_HW_CTS, cts,
				 abs((int)(cts & AUD_ACR_VALUE) - (int)tmds_khz) <= 8, 1000, 180000);
	if (ret)
		return ret;

	/* Two channels, rate and sample size from IEC 60958 status. */
	writel(0x000a0184, hdmi->packets + PKT_AUDIO_HEADER);
	writel(0x00000170, hdmi->packets + PKT_AUDIO_HEADER + 4);
	writel(0, hdmi->packets + PKT_AUDIO_HEADER + 8);
	writel(0, hdmi->packets + PKT_AUDIO_HEADER + 12);
	writel(0, hdmi->packets + PKT_AUDIO_HEADER + 16);
	mv320_hdmi_update(hdmi->packets, PKT_AUDIO_ENABLE, PKT_ENABLE_REPEAT, PKT_ENABLE_REPEAT);
	mv320_hdmi_update(audio, AUD_CTRL, AUD_MUTE, 0);
	return 0;
}

int mv320_hdmi_enable(struct mv320_hdmi *hdmi, const struct mv320_hdmi_mode *mode,
		      bool hdmi_sink, bool scdc)
{
	const struct drm_display_mode *m = &mode->timing;
	bool high = mode->tmds_khz > 340000;
	u32 value;
	int ret;

	ret = readl_poll_timeout(hdmi->packets + PKT_STABLE, value, value & BIT(0), 1000, 180000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(hdmi->video + 0xc0, value,
				 (value & GENMASK(25, 0)) == (m->htotal << 13 | m->hdisplay),
				 1000, 180000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(hdmi->video + 0xc4, value,
				 (value & GENMASK(25, 0)) == (m->vtotal << 13 | m->vdisplay),
				 1000, 180000);
	if (ret)
		return ret;

	mv320_hdmi_update(hdmi->packets, PKT_ENCODER, PKT_ENCODER_FLAGS,
			  high ? PKT_HDMI2 | PKT_SCRAMBLE : 0);
	if (hdmi_sink && mv320_hdmi_connected(hdmi)) {
		ret = mv320_scdc_configure(hdmi, scdc, high);
		if (ret && (scdc || high))
			return ret;
	} else if (high) {
		return -ENOLINK;
	}

	mv320_hdmi_update(hdmi->phy, PHY_POWER, PHY_OUTPUT,
			  PHY_DRIVER_PD | (high ? FIELD_PREP(PHY_TERM, 0x55) : BIT(11)));
	hdmi->output_enabled = true;
	if (!hdmi_sink)
		return 0;
	if (high) {
		ret = mv320_scdc_wait_lock(hdmi);
		if (ret)
			return ret;
	}

	mv320_hdmi_update(hdmi->packets, PKT_AVI_ENABLE, PKT_ENABLE_REPEAT, PKT_ENABLE_REPEAT);
	mv320_hdmi_update(hdmi->packets, PKT_DRM_ENABLE, PKT_ENABLE_REPEAT, PKT_ENABLE_REPEAT);
	ret = mv320_hdmi_audio(hdmi, mode->tmds_khz);
	if (ret)
		return ret;
	mv320_hdmi_update(hdmi->packets, PKT_GCP, AVMUTE_SET | AVMUTE_CLEAR, AVMUTE_CLEAR);
	return 0;
}
