// SPDX-License-Identifier: GPL-2.0-only
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hdmi.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/unaligned.h>
#include <sound/pcm.h>

#include <linux/hi3798-hdmi-modeset-audio.h>

#define INT_STATUS		0x0004
#define INT_CLEAR		0x0008
#define INT_MASK		0x000c
#define VT1			BIT(0)
#define HD0_UNDERFLOW		BIT(3)
#define FETCH_UNDERFLOW		0x0050
#define DHD_CTRL		0xc200
#define DHD_ENABLE		BIT(31)
#define DHD_PROGRESSIVE		BIT(4)
#define DHD_UPDATE		BIT(0)
#define DHD_STATE		0xc2a0
#define DHD_IN_BLANK		BIT(1)
#define DHD_VT			0xc228

#define PHY_PD			0x64
#define PHY_CHANNEL_PD		GENMASK(3, 0)
#define PHY_DRIVER_PD		GENMASK(8, 5)
#define PHY_TERM_PD		GENMASK(18, 11)
#define PHY_TERM_RGB8		BIT(11)
#define PHY_LDO_PD		GENMASK(10, 9)
#define PHY_RXSENSE_PD		BIT(19)
#define PHY_PLL_PD		BIT(20)
#define PHY_DIVSEL		GENMASK(22, 21)
#define CRG_TX			0x10c
#define CRG_PHY			0x110
#define CRG_PIXEL_GATE		BIT(16)
#define CRG_PHY_GATE		BIT(0)
#define CRG_PHY_RESET		GENMASK(5, 4)

#define PKT_STABLE		0x04
#define PKT_AVI_HEADER		0x18
#define PKT_AVI_ENABLE		0x1a4
#define PKT_AUDIO_ENABLE	0x1ac
#define PKT_HDR_ENABLE		0x1b4
#define PKT_GCP_ENABLE		0x1b8
#define PKT_LINK_MODE		0x208
#define PKT_GCP			0x20c
#define PKT_ENCODER		0x260
#define AVMUTE_SET		BIT(0)
#define AVMUTE_CLEAR		BIT(1)
#define AUD_MUTE		BIT(1)
#define AUD_ACR			0x40
#define AUD_N			0x48
#define AUD_SW_CTS		0x4c
#define AUD_HW_CTS		0x50
#define AUD_RESETS		GENMASK(8, 6)
#define AUD_ACR_FIELDS		GENMASK(4, 0)
#define CTS_MASK		GENMASK(19, 0)

struct ram_mode {
	const char *name;
	u16 width, height, hfront, hsync, hback, vfront, vsync, vback;
	u32 pixel_khz, hpll0, hpll1, drive;
	u8 vic, hd0_div, tmds_div, phy_divsel;
};

static const struct ram_mode modes[] = {
	{
		.name = "1080p60", .width = 1920, .height = 1080,
		.hfront = 88, .hsync = 44, .hback = 148,
		.vfront = 4, .vsync = 5, .vback = 36, .pixel_khz = 148500,
		.hpll0 = 0x11800000, .hpll1 = 0x1031, .drive = 0x0075d75d,
		.vic = 16, .hd0_div = 0, .tmds_div = 1, .phy_divsel = 1,
	}, {
		.name = "2160p30", .width = 3840, .height = 2160,
		.hfront = 176, .hsync = 88, .hback = 296,
		.vfront = 8, .vsync = 10, .vback = 72, .pixel_khz = 297000,
		.hpll0 = 0x12000000, .hpll1 = 0x1063, .drive = 0x0079e79f,
		.vic = 95, .hd0_div = 3, .tmds_div = 0, .phy_divsel = 0,
	},
};

struct modeset {
	void __iomem *vdp, *video, *phy, *aon;
	struct regmap *crg;
	dma_addr_t bars_dma[ARRAY_SIZE(modes)];
	int irq, last_error;
	const char *step;
	const struct ram_mode *mode;
	spinlock_t stats_lock;
	u64 vt1, first_ns, last_ns, underflow;
	u64 transition[5];
	u32 fetch, pixel_count, tmds_count;
};

static void update(void __iomem *base, u32 offset, u32 mask, u32 value)
{
	writel((readl(base + offset) & ~mask) | (value & mask), base + offset);
}

static irqreturn_t modeset_irq(int irq, void *data)
{
	struct modeset *test = data;
	u32 status = readl(test->vdp + INT_STATUS) & (VT1 | HD0_UNDERFLOW);
	unsigned long flags;

	if (!status)
		return IRQ_NONE;
	writel(status, test->vdp + INT_CLEAR);
	spin_lock_irqsave(&test->stats_lock, flags);
	test->fetch |= readl(test->vdp + FETCH_UNDERFLOW);
	if (status & HD0_UNDERFLOW)
		test->underflow++;
	if (status & VT1) {
		test->last_ns = ktime_get_ns();
		if (!test->vt1++)
			test->first_ns = test->last_ns;
	}
	spin_unlock_irqrestore(&test->stats_lock, flags);
	return IRQ_HANDLED;
}

static int prepare_bars(struct modeset *test, struct device *dev)
{
	static const u32 colors[] = {
		0xffebebeb, 0xffebeb10, 0xff10ebeb, 0xff10eb10,
		0xffeb10eb, 0xffeb1010, 0xff1010eb, 0xff101010,
	};
	unsigned int i, x, y;

	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		const struct ram_mode *mode = &modes[i];
		unsigned int bar_width = mode->width / ARRAY_SIZE(colors);
		size_t bytes = mode->width * mode->height * sizeof(u32);
		u32 *bars = dmam_alloc_coherent(dev, bytes, &test->bars_dma[i], GFP_KERNEL);

		if (!bars)
			return -ENOMEM;
		for (y = 0; y < mode->height; y++)
			for (x = 0; x < ARRAY_SIZE(colors); x++)
				memset32(bars + y * mode->width + x * bar_width, colors[x], bar_width);
	}
	dma_wmb();
	return 0;
}

static void configure_bars(struct modeset *test, const struct ram_mode *mode)
{
	void __iomem *vdp = test->vdp;
	u32 size = ((mode->height - 1) << 16) | (mode->width - 1);

	/* G0 feeds limited-range RGB to the GP0 mixer and HDMI TX. */
	update(vdp, 0xd500, BIT(0), 0);
	writel(0x04010040, vdp + 0x0200);
	writel(0x2, vdp + 0x0208);
	writel(0x1, vdp + 0x6908);
	update(vdp, 0x8a08, BIT(10) | GENMASK(1, 0), BIT(0));
	update(vdp, 0x8a20, BIT(0), BIT(0));
	writel(0x68, vdp + 0x8a40);
	writel(size, vdp + 0x8a44);
	writel(0, vdp + 0x8a48);
	writel(size, vdp + 0x8a4c);
	writel(0, vdp + 0x8a50);
	writel(lower_32_bits(test->bars_dma[mode - modes]), vdp + 0x8a54);
	writel(mode->width * sizeof(u32) / 16, vdp + 0x8a60);
	writel(size, vdp + 0x500c);
	writel(0, vdp + 0x5080);
	writel(size, vdp + 0x5084);
	writel(0, vdp + 0x5088);
	writel(size, vdp + 0x508c);
	writel(size, vdp + 0x509c);
	writel(size, vdp + 0x680c);
	writel(size, vdp + 0x6810);
	writel((mode->height << 16) | mode->width, vdp + 0x6824);
	writel(0, vdp + 0x6880);
	writel(size, vdp + 0x6884);
	writel(0, vdp + 0x6888);
	writel(size, vdp + 0x688c);
	writel(size, vdp + 0x689c);
	writel(BIT(16) | (mode->width - 1), vdp + 0x6a00);
	writel(BIT(20), vdp + 0x6a04);
	writel(mode->height - 1, vdp + 0x6a10);
	writel(BIT(12), vdp + 0x6a14);
	writel(0, vdp + 0x6a18);
	update(vdp, 0x5000, BIT(31) | GENMASK(7, 0), BIT(31) | 0xff);
	writel(1, vdp + 0x5004);
	writel(1, vdp + 0x6804);
}

static void configure_timing(struct modeset *test, const struct ram_mode *mode)
{
	void __iomem *vdp = test->vdp;

	/* Factory HD0 uses inclusive active counts and folds sync into back porch. */
	writel(((mode->vback + mode->vsync - 1) << 16) | (mode->height - 1), vdp + 0xc208);
	update(vdp, 0xc20c, GENMASK(15, 0), mode->vfront - 1);
	writel(((mode->hback + mode->hsync - 1) << 16) | (mode->width - 1), vdp + 0xc210);
	update(vdp, 0xc214, GENMASK(15, 0), mode->hfront - 1);
	update(vdp, 0xc220, GENMASK(23, 0), ((mode->vsync - 1) << 16) | (mode->hsync - 1));
	update(vdp, DHD_VT, GENMASK(12, 0), mode->height + 12);
	update(vdp, 0xc004, GENMASK(14, 13), BIT(13));
	update(vdp, 0xc600, BIT(6), BIT(6));
}

static int configure_clocks(struct modeset *test, const struct ram_mode *mode)
{
	struct regmap *crg = test->crg;
	u32 value;
	int ret;

	/* Gate the serial clock while retaining the TX APB/AON clocks. */
	ret = regmap_update_bits(crg, CRG_PHY, CRG_PHY_GATE | CRG_PHY_RESET, CRG_PHY_RESET);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_TX, CRG_PIXEL_GATE, 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, 0xd8, BIT(16), BIT(16));
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, 0x28, GENMASK(30, 0), mode->hpll0);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, 0x2c, GENMASK(27, 0), mode->hpll1);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, 0x34c, GENMASK(28, 24), BIT(24));
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, 0xd8, GENMASK(19, 18), mode->hd0_div << 18);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_TX, GENMASK(13, 10) | CRG_PIXEL_GATE,
				 (9 << 10) | CRG_PIXEL_GATE);
	if (ret)
		return ret;
	ret = regmap_update_bits(crg, CRG_PHY, GENMASK(8, 6) | CRG_PHY_GATE,
				 (mode->tmds_div << 6) | CRG_PHY_GATE);
	if (ret)
		return ret;
	/* Require the boot CRG lock bitmap before releasing the PHY reset. */
	ret = regmap_read_poll_timeout(crg, 0x150, value, (value & 0x7f) == 0x7f, 100, 20000);
	if (ret)
		return ret;
	return regmap_update_bits(crg, CRG_PHY, CRG_PHY_RESET, 0);
}

static void configure_phy(struct modeset *test, const struct ram_mode *mode)
{
	void __iomem *phy = test->phy;

	update(phy, PHY_PD, PHY_LDO_PD | PHY_RXSENSE_PD | PHY_PLL_PD | PHY_DIVSEL |
	       PHY_CHANNEL_PD | PHY_DRIVER_PD | PHY_TERM_PD,
	       FIELD_PREP(PHY_DIVSEL, mode->phy_divsel) |
	       PHY_CHANNEL_PD | PHY_DRIVER_PD | PHY_TERM_PD);
	update(phy, 0x10, BIT(0), BIT(0));
	update(phy, 0x10, BIT(0), 0);
	usleep_range(1000, 1500);

	/* 40-bit 1/10 pattern, fixed feedback and SSC disabled for both rows. */
	update(phy, 0x50, GENMASK(18, 9), FIELD_PREP(GENMASK(18, 9), 0x1f));
	update(phy, 0x54, GENMASK(29, 0), 0x01f07c1f);
	update(phy, 0xb0, BIT(27) | BIT(24) | GENMASK(23, 0), 0x640000);
	update(phy, 0xb8, GENMASK(23, 0), 0);
	update(phy, 0x90, GENMASK(19, 0), 0);
	update(phy, 0x8c, BIT(19) | GENMASK(17, 14) | GENMASK(11, 5),
	       FIELD_PREP(GENMASK(17, 14), 9) | FIELD_PREP(GENMASK(11, 5), 0x35));
	update(phy, 0xb0, BIT(24), BIT(24));

	/* MV320 EC factory spec rows c177e588/c177e5f4, converted by cur2hex. */
	update(phy, 0x68, GENMASK(23, 0), mode->drive);
	update(phy, 0x6c, GENMASK(17, 0), 0);
	update(phy, 0x70, GENMASK(11, 0), 0);
	update(phy, 0x74, GENMASK(17, 6), 0x0002da00);
	writel(0x20202060, phy + 0x78);
}

static void configure_video(struct modeset *test, void __iomem *packets, const u8 *avi)
{
	void __iomem *video = test->video;
	unsigned int offset;

	/* Pass the G0 RGB channels through at 8 bpc. */
	update(video, 0x134, BIT(3) | BIT(0), 0);
	update(video, 0x180, GENMASK(3, 0), BIT(3));
	update(video, 0x18c, BIT(10) | BIT(0), 0);
	/* The VDP drives R/G/B. Select G/B/R into the TX Y/Cb/Cr lanes. */
	update(video, 0x1a8, GENMASK(28, 21) | GENMASK(8, 0), 0x11);
	update(packets, PKT_HDR_ENABLE, GENMASK(1, 0), 0);
	update(packets, 0x00, BIT(16) | GENMASK(1, 0), 0);
	update(packets, PKT_LINK_MODE, BIT(1) | BIT(0), BIT(0));
	update(packets, PKT_ENCODER, BIT(1) | BIT(0), 0);
	writel(avi[0] | (avi[1] << 8) | (avi[2] << 16), packets + PKT_AVI_HEADER);
	/* Each slot holds seven packed bytes in a pair of 32-bit words. */
	for (offset = 0; offset < 4; offset++) {
		const u8 *payload = avi + 3 + offset * 7;

		writel(get_unaligned_le32(payload), packets + 0x1c + offset * 8);
		writel(get_unaligned_le24(payload + 4), packets + 0x20 + offset * 8);
	}
}

static int wait_for_video(struct modeset *test, const struct ram_mode *mode, void __iomem *packets)
{
	void __iomem *video = test->video;
	u32 horizontal = ((mode->width + mode->hfront + mode->hsync + mode->hback) << 13) |
			 mode->width;
	u32 vertical = ((mode->height + mode->vfront + mode->vsync + mode->vback) << 13) |
		       mode->height;
	u32 value;
	int ret;

	ret = readl_poll_timeout(packets + PKT_STABLE, value, value & BIT(0), 1000, 180000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(video + 0xc0, value, (value & GENMASK(25, 0)) == horizontal,
				 1000, 180000);
	if (ret)
		return ret;
	ret = readl_poll_timeout(video + 0xc4, value, (value & GENMASK(25, 0)) == vertical,
				 1000, 180000);
	if (ret)
		return ret;
	update(video, 0xb8, BIT(8), 0);
	usleep_range(1000, 1500);
	update(video, 0xb8, BIT(8), BIT(8));
	usleep_range(1000, 1500);
	test->pixel_count = readl(video + 0xc8);
	test->tmds_count = readl(video + 0xcc);
	update(video, 0xb8, BIT(8), 0);
	if (abs((int)test->pixel_count - (int)mode->pixel_khz) > 8 ||
	    abs((int)test->tmds_count - (int)mode->pixel_khz) > 8)
		return -ERANGE;
	return 0;
}

static int configure_audio(struct hi3798_modeset_audio *audio, const struct ram_mode *mode)
{
	u32 cts;
	int ret, sample;

	update(audio->audio, AUD_N, CTS_MASK, 6144);
	update(audio->audio, AUD_SW_CTS, CTS_MASK, mode->pixel_khz);
	update(audio->audio, AUD_ACR, AUD_ACR_FIELDS, 7);
	update(audio->reset, 0, AUD_RESETS, AUD_RESETS);
	udelay(10);
	update(audio->reset, 0, AUD_RESETS, 0);
	/* Check consecutive hardware measurements against the nominal CTS. */
	for (sample = 0; sample < 5; sample++) {
		ret = readl_poll_timeout(audio->audio + AUD_HW_CTS, cts,
					 abs((int)(cts & CTS_MASK) - (int)mode->pixel_khz) <= 8,
					 1000, 180000);
		if (ret)
			return ret;
		usleep_range(3000, 4000);
	}
	update(audio->packets, PKT_AUDIO_ENABLE, GENMASK(1, 0), 3);
	return 0;
}

static int switch_mode(struct modeset *test, const struct ram_mode *mode)
{
	struct hi3798_modeset_audio *audio = &hi3798_ram_audio;
	struct hdmi_avi_infoframe frame;
	/* Header followed by four seven-byte payload slots including checksum. */
	u8 avi[31] = {};
	void __iomem *vdp = test->vdp, *packets = audio->packets;
	unsigned long flags;
	u32 value;
	int ret;

	if (!packets || !(readl(test->aon + 0x28) & BIT(0)))
		return -ENOLINK;
	if (SUBSTREAM_BUSY(audio->pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream))
		return -EBUSY;
	if (test->mode == mode)
		return 0;

	hdmi_avi_infoframe_init(&frame);
	frame.video_code = mode->vic;
	frame.colorspace = HDMI_COLORSPACE_RGB;
	frame.picture_aspect = HDMI_PICTURE_ASPECT_16_9;
	frame.scan_mode = HDMI_SCAN_MODE_UNDERSCAN;
	frame.colorimetry = HDMI_COLORIMETRY_ITU_709;
	frame.quantization_range = HDMI_QUANTIZATION_RANGE_LIMITED;
	ret = hdmi_avi_infoframe_pack(&frame, avi, sizeof(avi));
	if (ret < 0)
		return ret;

	test->mode = NULL;
	test->step = "quiesce";
	WRITE_ONCE(audio->silent, true);
	update(audio->audio, 0, AUD_MUTE, AUD_MUTE);
	update(packets, PKT_GCP, AVMUTE_SET | AVMUTE_CLEAR, AVMUTE_SET);
	update(packets, PKT_GCP_ENABLE, GENMASK(1, 0), 3);
	msleep(40);
	update(packets, PKT_AUDIO_ENABLE, GENMASK(1, 0), 0);
	update(packets, PKT_AVI_ENABLE, GENMASK(1, 0), 0);
	update(audio->audio, AUD_ACR, BIT(0), 0);
	update(test->phy, PHY_PD, PHY_CHANNEL_PD | PHY_DRIVER_PD | PHY_TERM_PD,
	       PHY_CHANNEL_PD | PHY_DRIVER_PD | PHY_TERM_PD);

	writel(HD0_UNDERFLOW, vdp + INT_MASK);
	synchronize_irq(test->irq);
	test->transition[0] = READ_ONCE(test->underflow);
	if (readl(vdp + DHD_CTRL) & DHD_ENABLE) {
		ret = readl_poll_timeout(vdp + DHD_STATE, value, value & DHD_IN_BLANK,
					 30, 45000);
		if (ret)
			return ret;
		update(vdp, DHD_CTRL, DHD_ENABLE | DHD_UPDATE, DHD_UPDATE);
		ret = readl_poll_timeout(vdp + DHD_CTRL, value, !(value & DHD_UPDATE),
					 100, 80000);
		if (ret)
			return ret;
	}
	test->transition[1] = READ_ONCE(test->underflow);
	update(test->phy, PHY_PD, PHY_PLL_PD, PHY_PLL_PD);

	test->step = "clocks";
	configure_timing(test, mode);
	ret = configure_clocks(test, mode);
	if (ret)
		return ret;

	test->transition[2] = READ_ONCE(test->underflow);
	test->step = "video";
	configure_phy(test, mode);
	configure_video(test, packets, avi);
	configure_bars(test, mode);
	test->transition[3] = READ_ONCE(test->underflow);
	spin_lock_irqsave(&test->stats_lock, flags);
	test->vt1 = test->first_ns = test->last_ns = 0;
	spin_unlock_irqrestore(&test->stats_lock, flags);
	writel(VT1, vdp + INT_CLEAR);
	writel(VT1 | HD0_UNDERFLOW, vdp + INT_MASK);
	update(vdp, DHD_CTRL, DHD_ENABLE | DHD_PROGRESSIVE | DHD_UPDATE,
	       DHD_ENABLE | DHD_PROGRESSIVE | DHD_UPDATE);

	ret = wait_for_video(test, mode, packets);
	if (ret)
		return ret;
	if (!(readl(test->aon + 0x28) & BIT(0)))
		return -ENOLINK;
	test->transition[4] = READ_ONCE(test->underflow);
	/* MV320 EC uses termination mode 2 for CLK and mode 3 for the data lanes. */
	update(test->phy, PHY_PD, PHY_CHANNEL_PD | PHY_TERM_PD, PHY_TERM_RGB8);
	update(packets, PKT_AVI_ENABLE, GENMASK(1, 0), 3);

	test->step = "audio";
	ret = configure_audio(audio, mode);
	if (ret) {
		update(test->phy, PHY_PD, PHY_CHANNEL_PD | PHY_TERM_PD,
		       PHY_CHANNEL_PD | PHY_TERM_PD);
		return ret;
	}
	update(packets, PKT_GCP, AVMUTE_SET | AVMUTE_CLEAR, AVMUTE_CLEAR);
	test->mode = mode;
	test->step = "active";
	WRITE_ONCE(audio->silent, false);
	/* The existing PCM trigger unmutes TX Audio when the first samples start. */
	return 0;
}

static void dump(struct seq_file *s, const char *name, void __iomem *base,
		 const u16 *offsets, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		seq_printf(s, "%s[%03x]=%08x\n", name, offsets[i], readl(base + offsets[i]));
}

static int report(struct seq_file *s, void *unused)
{
	struct modeset *test = s->private;
	struct hi3798_modeset_audio *audio = &hi3798_ram_audio;
	static const u16 crg[] = {0x28, 0x2c, 0xd8, 0x10c, 0x110, 0x150, 0x34c};
	static const u16 phy[] = {0x10, 0x50, 0x54, 0x64, 0x68, 0x6c, 0x70, 0x74, 0x78,
				  0x8c, 0x90, 0xb0, 0xb4, 0xb8, 0xbc};
	static const u16 vdp[] = {0xc004, 0xc200, 0xc208, 0xc20c, 0xc210, 0xc214, 0xc220,
				  0xc228, 0xc2a0, 0xc600, 0x5000, 0x5018, 0x509c, 0x6810,
				  0x8a54, 0x8b38, 0xd500, 0xe000};
	static const u16 video[] = {0xb8, 0xc0, 0xc4, 0x134, 0x180, 0x18c, 0x1a8};
	static const u16 packet[] = {0x00, 0x04, 0x18, 0x1c, 0x20, 0x3c, 0x40,
				     0x1a4, 0x1ac, 0x1b4, 0x208, 0x20c, 0x260};
	static const u16 acr[] = {0, 4, AUD_ACR, AUD_N, AUD_SW_CTS, AUD_HW_CTS};
	static const u16 tx2[] = {0, 4, 0x88, 0x8c, 0xa8};
	u64 vt1, first_ns, last_ns, underflow;
	unsigned long flags;
	u32 value, fetch;
	unsigned int i;

	mutex_lock(&audio->lock);
	spin_lock_irqsave(&test->stats_lock, flags);
	vt1 = test->vt1;
	first_ns = test->first_ns;
	last_ns = test->last_ns;
	underflow = test->underflow;
	fetch = test->fetch | readl(test->vdp + FETCH_UNDERFLOW);
	spin_unlock_irqrestore(&test->stats_lock, flags);
	seq_printf(s, "mode=%s step=%s error=%d hpd=%x\n",
		   test->mode ? test->mode->name : audio->silent ? "muted" : "firmware",
		   test->step, test->last_error, readl(test->aon + 0x28) & 3);
	seq_printf(s, "vt1=%llu first_ns=%llu last_ns=%llu underflow=%llu fetch=%08x\n",
		   vt1, first_ns, last_ns, underflow, fetch);
	seq_printf(s, "underflow_checkpoints=%llu/%llu/%llu/%llu/%llu\n",
		   test->transition[0], test->transition[1], test->transition[2],
		   test->transition[3], test->transition[4]);
	seq_printf(s, "pixel_count=%u tmds_count=%u\n", test->pixel_count, test->tmds_count);
	for (i = 0; i < ARRAY_SIZE(crg); i++)
		if (!regmap_read(test->crg, crg[i], &value))
			seq_printf(s, "crg[%03x]=%08x\n", crg[i], value);
	dump(s, "vdp", test->vdp, vdp, ARRAY_SIZE(vdp));
	dump(s, "phy", test->phy, phy, ARRAY_SIZE(phy));
	dump(s, "video", test->video, video, ARRAY_SIZE(video));
	if (audio->packets) {
		dump(s, "packet", audio->packets, packet, ARRAY_SIZE(packet));
		dump(s, "audio", audio->audio, acr, ARRAY_SIZE(acr));
		dump(s, "tx2", audio->tx2, tx2, ARRAY_SIZE(tx2));
	}
	mutex_unlock(&audio->lock);
	return 0;
}

static int modeset_open(struct inode *inode, struct file *file)
{
	return single_open(file, report, inode->i_private);
}

static ssize_t modeset_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct modeset *test = ((struct seq_file *)file->private_data)->private;
	char name[16];
	unsigned int i;
	int ret;

	if (!len || len >= sizeof(name))
		return -EINVAL;
	if (copy_from_user(name, buf, len))
		return -EFAULT;
	name[len] = 0;
	if (strlen(name) != len)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(modes); i++)
		if (sysfs_streq(name, modes[i].name))
			break;
	if (i == ARRAY_SIZE(modes))
		return -EINVAL;
	mutex_lock(&hi3798_ram_audio.lock);
	if (hi3798_ram_audio.pcm) {
		mutex_lock(&hi3798_ram_audio.pcm->open_mutex);
		ret = switch_mode(test, &modes[i]);
		mutex_unlock(&hi3798_ram_audio.pcm->open_mutex);
	} else {
		ret = -ENODEV;
	}
	test->last_error = ret;
	mutex_unlock(&hi3798_ram_audio.lock);
	return ret ? ret : len;
}

static const struct file_operations modeset_fops = {
	.owner = THIS_MODULE,
	.open = modeset_open,
	.read = seq_read,
	.write = modeset_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int modeset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct modeset *test;
	struct dentry *file;
	int ret;

	test = devm_kzalloc(dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	test->vdp = devm_platform_ioremap_resource_byname(pdev, "vdp");
	if (IS_ERR(test->vdp))
		return PTR_ERR(test->vdp);
	test->video = devm_platform_ioremap_resource_byname(pdev, "video");
	if (IS_ERR(test->video))
		return PTR_ERR(test->video);
	test->phy = devm_platform_ioremap_resource_byname(pdev, "phy");
	if (IS_ERR(test->phy))
		return PTR_ERR(test->phy);
	test->aon = devm_platform_ioremap_resource_byname(pdev, "aon");
	if (IS_ERR(test->aon))
		return PTR_ERR(test->aon);
	test->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(test->crg))
		return PTR_ERR(test->crg);
	if (!(readl(test->vdp + DHD_CTRL) & DHD_ENABLE) ||
	    !(readl(test->vdp + 0xe000) & BIT(0)))
		return -ENODEV;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = prepare_bars(test, dev);
	if (ret)
		return ret;

	test->step = "inherited";
	spin_lock_init(&test->stats_lock);
	test->irq = platform_get_irq(pdev, 0);
	if (test->irq < 0)
		return test->irq;
	ret = devm_request_irq(dev, test->irq, modeset_irq, 0, dev_name(dev), test);
	if (ret)
		return ret;

	file = debugfs_create_file("hi3798_hdmi_modeset", 0600, NULL, test, &modeset_fops);
	if (IS_ERR(file))
		return PTR_ERR(file);
	writel(VT1 | HD0_UNDERFLOW, test->vdp + INT_MASK);
	return 0;
}

static const struct of_device_id modeset_match[] = {
	{ .compatible = "hisilicon,hg680kd-hdmi-ram-modeset" },
	{ }
};

static struct platform_driver modeset_driver = {
	.probe = modeset_probe,
	.driver = {
		.name = "hi3798-hdmi-ram-modeset",
		.of_match_table = modeset_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(modeset_driver);
