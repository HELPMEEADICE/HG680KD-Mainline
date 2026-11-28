// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <sound/core.h>
#include <sound/pcm.h>

#define AIAO_IRQ_EN		0x0000
#define AIAO_IRQ_STATUS		0x0004
#define AIAO_VERSION		0x000c
#define AIAO_RESET_READY	0x0030
#define AIAO_DMA_BURST		0x0034
#define AIAO_TX2_MCLK		0x0150
#define AIAO_TX2_CLOCK		0x0154
#define AIAO_TX2		0x2200
#define AIAO_TX2_IRQ		BIT(18)

#define TX_INTERFACE		0x00
#define TX_CONTROL		0x04
#define TX_DMA_BASE		0x80
#define TX_DMA_SIZE		0x84
#define TX_WRITE_PTR		0x88
#define TX_READ_PTR		0x8c
#define TX_WATERMARK		0x90
#define TX_PERIOD		0x94
#define TX_IRQ_EN		0xa0
#define TX_IRQ_STATUS		0xa8
#define TX_IRQ_CLEAR		0xac
#define TX_PERIOD_IRQ		BIT(0)
#define TX_STOPPED		BIT(29)
#define TX_START		BIT(28)
#define TX_VOLUME		GENMASK(14, 8)
#define TX_MUTE			BIT(0)

#define CRG_AIAO		0x118
#define CRG_AIAO_GATE		BIT(0)
#define CRG_AIAO_RESET		BIT(4)
#define CRG_AIAO_CLOCKS		(BIT(22) | BIT(17))
#define CRG_AIAO_CLKSEL		GENMASK(16, 12)

#define HDMI_AUD_CTRL		0x00
#define HDMI_AUD_I2S		0x04
#define HDMI_AUD_CHST0		0x0c
#define HDMI_AUD_CHST4		0x10
#define HDMI_AUD_ACR		0x40
#define HDMI_AUD_N		0x48
#define HDMI_AUD_CTS		0x4c
#define HDMI_AUD_MUTE		BIT(1)
#define HDMI_AUDIO_RESETS	GENMASK(8, 6)
#define HDMI_AVI_HEADER		0x18
#define HDMI_AVI_PAYLOAD0	0x1c
#define HDMI_AVI_PAYLOAD1	0x20
#define HDMI_AUDIO_HEADER	0x3c
#define HDMI_AUDIO_PAYLOAD	0x40
#define HDMI_AUDIO_INFO_EN	0x1ac
#define HDMI_GCP_AVMUTE		0x20c

#define DMA_BUFFER_MAX		SZ_64K
#define TX_RING_GUARD_FRAMES	8

struct mv320_aiao {
	void __iomem *regs, *tx, *hdmi, *packets, *hdmi_reset;
	struct regmap *crg;
	struct snd_pcm_substream *stream;
	unsigned int write_ptr;
	int irq;
	bool running, stopping, held_tail;
};

static void mv320_update(void __iomem *reg, u32 mask, u32 value)
{
	writel((readl(reg) & ~mask) | (value & mask), reg);
}

static int mv320_aiao_power_on(struct mv320_aiao *aiao)
{
	u32 value;
	int ret;

	ret = regmap_read(aiao->crg, CRG_AIAO, &value);
	if (ret)
		return ret;
	if (!(value & CRG_AIAO_GATE)) {
		ret = regmap_update_bits(aiao->crg, CRG_AIAO,
					 CRG_AIAO_GATE | CRG_AIAO_RESET,
					 CRG_AIAO_GATE | CRG_AIAO_RESET);
		if (ret)
			return ret;
		usleep_range(1000, 2000);
	}
	ret = regmap_update_bits(aiao->crg, CRG_AIAO, CRG_AIAO_RESET, 0);
	if (ret)
		return ret;
	ret = readl_poll_timeout(aiao->regs + AIAO_RESET_READY, value,
				 value & BIT(0), 10, 1000);
	if (ret)
		return ret;
	ret = regmap_update_bits(aiao->crg, CRG_AIAO, CRG_AIAO_CLOCKS | CRG_AIAO_CLKSEL,
				 CRG_AIAO_CLOCKS);
	if (ret)
		return ret;
	if (readl(aiao->regs + AIAO_VERSION) != 0x03070001)
		return -ENODEV;

	mv320_update(aiao->regs + AIAO_DMA_BURST, GENMASK(2, 0), 2);
	/* 48 kHz, 64 BCLK per frame, MCLK/BCLK = 4. TX2 is the HDMI I2S port. */
	mv320_update(aiao->regs + AIAO_TX2_MCLK, GENMASK(26, 0), 0x234f72);
	mv320_update(aiao->regs + AIAO_TX2_CLOCK, GENMASK(11, 0) | BIT(13), 0x133);
	writel(0xe4280014, aiao->tx + TX_INTERFACE);
	writel(0, aiao->tx + TX_IRQ_EN);
	writel(~0U, aiao->tx + TX_IRQ_CLEAR);
	return 0;
}

static int mv320_hdmi_setup(struct mv320_aiao *aiao)
{
	/* The board DTS is for the firmware's VIC 97, 8-bit YCbCr 4:2:0 link. */
	if (readl(aiao->packets + HDMI_AVI_HEADER) != 0x000d0282 ||
	    FIELD_GET(GENMASK(7, 0), readl(aiao->packets + HDMI_AVI_PAYLOAD1)) != 97 ||
	    FIELD_GET(GENMASK(15, 13), readl(aiao->packets + HDMI_AVI_PAYLOAD0)) != 3 ||
	    readl(aiao->packets + HDMI_GCP_AVMUTE) & BIT(0))
		return -ENODEV;

	/* I2S, 2-channel layout, 16-bit words and IEC 60958 48 kHz status. */
	mv320_update(aiao->hdmi + HDMI_AUD_CTRL, GENMASK(8, 0), 0xf3);
	mv320_update(aiao->hdmi + HDMI_AUD_I2S, GENMASK(15, 0), 0x200);
	writel(0x02000000, aiao->hdmi + HDMI_AUD_CHST0);
	mv320_update(aiao->hdmi + HDMI_AUD_CHST4, GENMASK(7, 0), 0xd2);
	/* 297 MHz TMDS, 48 kHz: CTS = TMDS * N / (128 * Fs). */
	mv320_update(aiao->hdmi + HDMI_AUD_N, GENMASK(19, 0), 6144);
	mv320_update(aiao->hdmi + HDMI_AUD_CTS, GENMASK(19, 0), 297000);
	mv320_update(aiao->hdmi + HDMI_AUD_ACR, GENMASK(4, 0), 7);

	mv320_update(aiao->hdmi_reset, HDMI_AUDIO_RESETS, HDMI_AUDIO_RESETS);
	udelay(10);
	mv320_update(aiao->hdmi_reset, HDMI_AUDIO_RESETS, 0);

	/* CTA Audio InfoFrame. Two channels, rate and size from channel status. */
	writel(0x000a0184, aiao->packets + HDMI_AUDIO_HEADER);
	writel(0x00000170, aiao->packets + HDMI_AUDIO_PAYLOAD);
	writel(0, aiao->packets + HDMI_AUDIO_PAYLOAD + 4);
	writel(0, aiao->packets + HDMI_AUDIO_PAYLOAD + 8);
	writel(0, aiao->packets + HDMI_AUDIO_PAYLOAD + 12);
	mv320_update(aiao->packets + HDMI_AUDIO_INFO_EN, GENMASK(1, 0), 3);
	return 0;
}

static const struct snd_pcm_hardware mv320_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_48000,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = DMA_BUFFER_MAX,
	.period_bytes_min = 1920,
	.period_bytes_max = 8192,
	.periods_min = 2,
	.periods_max = 16,
};

static int mv320_pcm_open(struct snd_pcm_substream *ss)
{
	struct snd_pcm_runtime *rt = ss->runtime;
	int ret;

	rt->hw = mv320_pcm_hw;
	ret = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret)
		return ret;
	return snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 32);
}

static int mv320_pcm_close(struct snd_pcm_substream *ss)
{
	return 0;
}

static int mv320_pcm_prepare(struct snd_pcm_substream *ss)
{
	struct mv320_aiao *aiao = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	unsigned int size = snd_pcm_lib_buffer_bytes(ss);
	unsigned int period = snd_pcm_lib_period_bytes(ss);

	if (upper_32_bits(rt->dma_addr + size - 1))
		return -ERANGE;
	aiao->write_ptr = 0;
	aiao->held_tail = false;
	writel(TX_MUTE | FIELD_PREP(TX_VOLUME, 0x79), aiao->tx + TX_CONTROL);
	writel(lower_32_bits(rt->dma_addr), aiao->tx + TX_DMA_BASE);
	writel(size, aiao->tx + TX_DMA_SIZE);
	writel(0, aiao->tx + TX_READ_PTR);
	writel(0, aiao->tx + TX_WRITE_PTR);
	writel(period, aiao->tx + TX_WATERMARK);
	writel(period, aiao->tx + TX_PERIOD);
	writel(~0U, aiao->tx + TX_IRQ_CLEAR);
	return 0;
}

static void mv320_pcm_publish(struct snd_pcm_substream *ss)
{
	struct mv320_aiao *aiao = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	snd_pcm_sframes_t queued = snd_pcm_playback_hw_avail(rt);
	snd_pcm_sframes_t held = queued - (snd_pcm_sframes_t)rt->buffer_size +
				 TX_RING_GUARD_FRAMES;
	unsigned int pos;

	/*
	 * Equal byte offsets denote an empty hardware ring. ALSA may fill
	 * its entire ring, so withhold up to one DMA burst. Recompute after
	 * the PCM core has advanced hw_ptr, including at a period IRQ.
	 */
	aiao->held_tail = held > 0;
	pos = frames_to_bytes(rt, (rt->control->appl_ptr % rt->buffer_size +
			      rt->buffer_size - max(held, 0L)) % rt->buffer_size);
	if (pos == aiao->write_ptr)
		return;
	aiao->write_ptr = pos;
	dma_wmb();
	writel(pos, aiao->tx + TX_WRITE_PTR);
}

static int mv320_pcm_ack(struct snd_pcm_substream *ss)
{
	mv320_pcm_publish(ss);
	return 0;
}

static snd_pcm_uframes_t mv320_pcm_pointer(struct snd_pcm_substream *ss)
{
	struct mv320_aiao *aiao = snd_pcm_substream_chip(ss);
	unsigned int pos = readl(aiao->tx + TX_READ_PTR);

	if (pos >= snd_pcm_lib_buffer_bytes(ss))
		return SNDRV_PCM_POS_XRUN;
	return bytes_to_frames(ss->runtime, pos);
}

static int mv320_pcm_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct mv320_aiao *aiao = snd_pcm_substream_chip(ss);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		aiao->running = true;
		writel(TX_PERIOD_IRQ, aiao->tx + TX_IRQ_EN);
		mv320_update(aiao->regs + AIAO_IRQ_EN, AIAO_TX2_IRQ, AIAO_TX2_IRQ);
		mv320_update(aiao->tx + TX_CONTROL, TX_START | TX_MUTE, TX_START);
		if (aiao->hdmi)
			mv320_update(aiao->hdmi + HDMI_AUD_CTRL, HDMI_AUD_MUTE, 0);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
		if (aiao->hdmi)
			mv320_update(aiao->hdmi + HDMI_AUD_CTRL, HDMI_AUD_MUTE, HDMI_AUD_MUTE);
		mv320_update(aiao->tx + TX_CONTROL, TX_START | TX_MUTE, TX_MUTE);
		writel(0, aiao->tx + TX_IRQ_EN);
		mv320_update(aiao->regs + AIAO_IRQ_EN, AIAO_TX2_IRQ, 0);
		aiao->running = false;
		aiao->stopping = true;
		return 0;
	default:
		return -EINVAL;
	}
}

static int mv320_pcm_sync_stop(struct snd_pcm_substream *ss)
{
	struct mv320_aiao *aiao = snd_pcm_substream_chip(ss);
	u32 value;
	int ret;

	synchronize_irq(aiao->irq);
	if (!aiao->stopping)
		return 0;
	aiao->stopping = false;
	ret = readl_poll_timeout(aiao->tx + TX_CONTROL, value, value & TX_STOPPED,
				 50, 20000);
	if (ret) {
		dev_err(ss->pcm->card->dev, "TX2 stop timed out, resetting AIAO\n");
		regmap_update_bits(aiao->crg, CRG_AIAO, CRG_AIAO_RESET, CRG_AIAO_RESET);
		usleep_range(1000, 2000);
		ret = mv320_aiao_power_on(aiao);
	}
	return ret;
}

static const struct snd_pcm_ops mv320_pcm_ops = {
	.open = mv320_pcm_open,
	.close = mv320_pcm_close,
	.prepare = mv320_pcm_prepare,
	.trigger = mv320_pcm_trigger,
	.pointer = mv320_pcm_pointer,
	.ack = mv320_pcm_ack,
	.sync_stop = mv320_pcm_sync_stop,
};

static irqreturn_t mv320_aiao_irq(int irq, void *data)
{
	struct mv320_aiao *aiao = data;
	struct snd_pcm_substream *ss = aiao->stream;
	unsigned long flags;
	u32 status;

	if (!(readl(aiao->regs + AIAO_IRQ_STATUS) & AIAO_TX2_IRQ))
		return IRQ_NONE;
	snd_pcm_stream_lock_irqsave(ss, flags);
	status = readl(aiao->tx + TX_IRQ_STATUS);
	writel(status, aiao->tx + TX_IRQ_CLEAR);
	if (aiao->running && (status & TX_PERIOD_IRQ)) {
		snd_pcm_period_elapsed_under_stream_lock(ss);
		if (aiao->running && aiao->held_tail)
			mv320_pcm_publish(ss);
	}
	snd_pcm_stream_unlock_irqrestore(ss, flags);
	return IRQ_HANDLED;
}

static int mv320_aiao_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct mv320_aiao *aiao;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	ret = snd_devm_card_new(dev, -1, "MV320HDMI", THIS_MODULE, sizeof(*aiao), &card);
	if (ret)
		return ret;
	aiao = card->private_data;
	aiao->regs = devm_platform_ioremap_resource_byname(pdev, "aiao");
	if (IS_ERR(aiao->regs))
		return PTR_ERR(aiao->regs);
	aiao->tx = aiao->regs + AIAO_TX2;
	/* Older DTBs delegate the firmware link's audio registers to AIAO. */
	if (platform_get_resource_byname(pdev, IORESOURCE_MEM, "hdmi-audio")) {
		aiao->hdmi_reset = devm_platform_ioremap_resource_byname(pdev, "hdmi-audio-reset");
		if (IS_ERR(aiao->hdmi_reset))
			return PTR_ERR(aiao->hdmi_reset);
		aiao->hdmi = devm_platform_ioremap_resource_byname(pdev, "hdmi-audio");
		if (IS_ERR(aiao->hdmi))
			return PTR_ERR(aiao->hdmi);
		aiao->packets = devm_platform_ioremap_resource_byname(pdev, "hdmi-packets");
		if (IS_ERR(aiao->packets))
			return PTR_ERR(aiao->packets);
	}
	if (IS_ERR(aiao->packets))
		return PTR_ERR(aiao->packets);
	aiao->crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	if (IS_ERR(aiao->crg))
		return PTR_ERR(aiao->crg);
	aiao->irq = platform_get_irq(pdev, 0);
	if (aiao->irq < 0)
		return aiao->irq;

	ret = mv320_aiao_power_on(aiao);
	if (ret)
		return dev_err_probe(dev, ret, "cannot initialize TX2\n");
	if (aiao->hdmi) {
		ret = mv320_hdmi_setup(aiao);
		if (ret)
			return dev_err_probe(dev, ret, "expected firmware HDMI VIC 97 YCbCr 4:2:0\n");
	}

	ret = snd_pcm_new(card, "HDMI PCM", 0, 1, 0, &pcm);
	if (ret)
		return ret;
	strscpy(card->driver, "hi3798mv320");
	strscpy(card->shortname, "Hi3798MV320 HDMI");
	strscpy(card->longname, "Hi3798MV320 AIAO TX2 HDMI LPCM");
	strscpy(pcm->name, "HDMI LPCM");
	pcm->private_data = aiao;
	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK, &mv320_pcm_ops);
	ret = snd_pcm_set_managed_buffer_all(pcm, SNDRV_DMA_TYPE_DEV, dev,
					     DMA_BUFFER_MAX, DMA_BUFFER_MAX);
	if (ret)
		return ret;
	aiao->stream = pcm->streams[SNDRV_PCM_STREAM_PLAYBACK].substream;
	ret = devm_request_irq(dev, aiao->irq, mv320_aiao_irq, 0, dev_name(dev), aiao);
	if (ret)
		return ret;
	card->sync_irq = aiao->irq;
	return snd_card_register(card);
}

static const struct of_device_id mv320_aiao_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-aiao" },
	{ }
};
MODULE_DEVICE_TABLE(of, mv320_aiao_of_match);

static struct platform_driver mv320_aiao_driver = {
	.probe = mv320_aiao_probe,
	.driver = {
		.name = "hi3798mv320-aiao",
		.of_match_table = mv320_aiao_of_match,
	},
};
module_platform_driver(mv320_aiao_driver);

MODULE_DESCRIPTION("Hi3798MV320 AIAO HDMI LPCM playback");
MODULE_LICENSE("GPL");
