// SPDX-License-Identifier: GPL-2.0-only
#include <drm/display/drm_scdc.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>

#include "hi3798-hdmi-scdc.h"

/* The resource starts at TX +0x38, above the AIAO-owned reset word. */
#define DDC_OUT		0x00
#define DDC_IN		0x04
#define DDC_COUNT	0x08
#define DDC_ADDR	0x0c
#define DDC_STATUS	0x10
#define DDC_CMD		0x14
#define DDC_CLEAR	0x18
#define DDC_REQ		0x30
#define DDC_ACK		0x34
#define DDC_ARB		0x38
#define DDC_NACK	BIT(0)
#define DDC_BUS_LOW	BIT(1)
#define DDC_BUSY	BIT(2)
#define DDC_EMPTY	BIT(5)
#define DDC_FIFO_COUNT	GENMASK(4, 0)
#define DDC_LENGTH	GENMASK(17, 8)

#define DDC_READ	2
#define DDC_SEG_READ	4
#define DDC_WRITE	7
#define DDC_FLUSH	9
#define DDC_ABORT	15
#define AON_HPD		0x28
#define AON_DDC_ERR	0x54
#define AON_DDC_PINS	0x5c
#define EDID_ADDR	0xa0
#define SCDC_ADDR	0xa8
#define EDID_BLOCK_SIZE	128
#define MAX_EDID_EXT	8
#define TMDS_CONFIG_MASK (SCDC_SCRAMBLING_ENABLE | SCDC_TMDS_BIT_CLOCK_RATIO_BY_40)
#define CHANNEL_LOCKS	GENMASK(3, 0)

static int ddc_transfer(struct hi3798_hdmi_sink *sink, u8 slave, u8 offset,
			u8 segment, u8 *data, unsigned int length, bool write)
{
	void __iomem *ddc = sink->ddc, *aon = sink->aon;
	u32 saved_addr, saved_count, arb, status, errors, count, value;
	unsigned int received = 0;
	int ret, idle = 0, release;
	u64 deadline;

	if (!(readl(aon + AON_HPD) & BIT(0)))
		return -ENOLINK;
	if ((readl(ddc + DDC_REQ) | readl(ddc + DDC_ACK)) & BIT(0))
		return -EBUSY;
	arb = readl(ddc + DDC_ARB);
	writel(readl(ddc + DDC_REQ) | BIT(0), ddc + DDC_REQ);
	ret = readl_poll_timeout(ddc + DDC_ACK, value, value & BIT(0), 50, 50000);
	if (ret)
		goto release;

	status = readl(ddc + DDC_STATUS);
	if ((status & DDC_BUSY) || !(status & DDC_EMPTY) ||
	    ((status | readl(aon + AON_DDC_ERR)) & (DDC_NACK | DDC_BUS_LOW)) ||
	    (readl(aon + AON_DDC_PINS) & 3) != 3) {
		ret = -EBUSY;
		goto release;
	}

	saved_addr = readl(ddc + DDC_ADDR);
	saved_count = readl(ddc + DDC_COUNT);
	writel(DDC_FLUSH, ddc + DDC_CMD);
	writel((saved_addr & ~GENMASK(23, 0)) | segment << 16 | offset << 8 | slave,
	       ddc + DDC_ADDR);
	writel((saved_count & ~DDC_LENGTH) | FIELD_PREP(DDC_LENGTH, length), ddc + DDC_COUNT);
	if (write) {
		/* The stock controller starts command 7 before filling the TX FIFO. */
		writel(DDC_WRITE, ddc + DDC_CMD);
		writel(*data, ddc + DDC_IN);
		usleep_range(1000, 1500);
	} else {
		writel(segment ? DDC_SEG_READ : DDC_READ, ddc + DDC_CMD);
	}
	deadline = ktime_get_ns() + 50000000;
	do {
		if (!(readl(aon + AON_HPD) & BIT(0))) {
			ret = -ENOLINK;
			break;
		}
		status = readl(ddc + DDC_STATUS);
		errors = status | readl(aon + AON_DDC_ERR);
		if (errors & DDC_BUS_LOW) {
			ret = -EIO;
			break;
		}
		if (errors & DDC_NACK) {
			ret = -ENXIO;
			break;
		}
		count = readl(ddc + DDC_COUNT) & DDC_FIFO_COUNT;
		if (!write) {
			if (count > length - received) {
				ret = -EOVERFLOW;
				break;
			}
			while (count--)
				data[received++] = readl(ddc + DDC_OUT);
		}
		if (!(status & DDC_BUSY) && (write ? !count : received == length)) {
			ret = 0;
			break;
		}
		ret = -ETIMEDOUT;
		usleep_range(40, 80);
	} while (ktime_get_ns() < deadline);

	if (ret) {
		writel(DDC_ABORT, ddc + DDC_CMD);
		idle = readl_poll_timeout(ddc + DDC_STATUS, value, !(value & DDC_BUSY),
					  50, 50000);
		if (!idle) {
			writel(DDC_FLUSH, ddc + DDC_CMD);
			errors = readl(ddc + DDC_STATUS) | readl(aon + AON_DDC_ERR);
			writel(((errors & DDC_NACK) << 1) | ((errors & DDC_BUS_LOW) >> 1),
			       ddc + DDC_CLEAR);
		}
	}
	if (!idle) {
		writel(saved_count & ~DDC_FIFO_COUNT, ddc + DDC_COUNT);
		writel(saved_addr, ddc + DDC_ADDR);
	}

release:
	writel(readl(ddc + DDC_REQ) & ~BIT(0), ddc + DDC_REQ);
	release = readl_poll_timeout(ddc + DDC_ACK, value, !(value & BIT(0)), 50, 50000);
	status = readl(ddc + DDC_STATUS);
	if (!release && (readl(ddc + DDC_ARB) != arb || !(status & DDC_EMPTY) ||
			 (status & DDC_BUSY) ||
			 ((status | readl(aon + AON_DDC_ERR)) & (DDC_NACK | DDC_BUS_LOW)) ||
			 (readl(aon + AON_DDC_PINS) & 3) != 3))
		release = -EIO;
	return idle ?: (release ?: ret);
}

static int scdc_read(struct hi3798_hdmi_sink *sink, u8 reg, u8 *value)
{
	return ddc_transfer(sink, SCDC_ADDR, reg, 0, value, 1, false);
}

static int scdc_write(struct hi3798_hdmi_sink *sink, u8 reg, u8 value)
{
	return ddc_transfer(sink, SCDC_ADDR, reg, 0, &value, 1, true);
}

static int read_edid(struct hi3798_hdmi_sink *sink, unsigned int block, u8 *edid)
{
	u8 checksum = 0;
	unsigned int i;
	int ret;

	ret = ddc_transfer(sink, EDID_ADDR, (block & 1) * EDID_BLOCK_SIZE, block >> 1,
			   edid, EDID_BLOCK_SIZE, false);
	if (ret)
		return ret;
	for (i = 0; i < EDID_BLOCK_SIZE; i++)
		checksum += edid[i];
	return checksum ? -EBADMSG : 0;
}

static void cta_caps(struct hi3798_hdmi_sink *sink, const u8 *edid, u8 vic)
{
	unsigned int i, j, end = edid[2], svd_index = 0;
	const u8 *map = NULL, *payload;
	unsigned int map_len = 0;
	u64 matches = 0;

	if (edid[0] != 0x02 || edid[1] < 3 || end < 4 || end > 127)
		return;
	for (i = 4; i < end;) {
		unsigned int tag = edid[i] >> 5, len = edid[i] & 31;
		const u8 *forum = NULL;

		payload = edid + i + 1;
		i += len + 1;
		if (i > end || !len)
			break;
		if (tag == 2) {
			for (j = 0; j < len; j++, svd_index++) {
				/* Native SVDs use 129–192. This probe uses VICs 16, 95 and 97. */
				if (payload[j] != vic && !(vic <= 64 && payload[j] == vic + 128))
					continue;
				sink->rgb = true;
				if (svd_index < 64)
					matches |= BIT_ULL(svd_index);
			}
		} else if (tag == 3 && len >= 7 && !memcmp(payload, "\x03\x0c\x00", 3)) {
			sink->max_tmds_khz = max(sink->max_tmds_khz, payload[6] * 5000U);
		} else if (tag == 3 && len >= 7 && !memcmp(payload, "\xd8\x5d\xc4", 3)) {
			forum = payload + 3;
		} else if (tag == 7 && payload[0] == 0x79 && len >= 7) {
			forum = payload + 3;
		} else if (tag == 7 && payload[0] == 0x0e) {
			for (j = 1; j < len; j++)
				sink->y420 |= payload[j] == vic;
		} else if (tag == 7 && payload[0] == 0x0f) {
			map = payload + 1;
			map_len = len - 1;
		}
		if (forum) {
			sink->max_tmds_khz = max(sink->max_tmds_khz, forum[1] * 5000U);
			sink->scdc |= !!(forum[2] & BIT(7));
		}
	}
	if (map && !map_len && matches)
		sink->y420 = true;
	for (j = 0; j < min(map_len, 8U); j++)
		sink->y420 |= !!((matches >> (8 * j)) & map[j]);
}

int hi3798_sink_probe(struct hi3798_hdmi_sink *sink, u8 vic, bool y420, u32 tmds_khz)
{
	static const u8 header[] = { 0, 255, 255, 255, 255, 255, 255, 0 };
	unsigned int i, blocks;
	u8 edid[EDID_BLOCK_SIZE];
	int ret;

	*sink = (struct hi3798_hdmi_sink) { .ddc = sink->ddc, .aon = sink->aon,
					   .max_tmds_khz = 165000 };
	ret = read_edid(sink, 0, edid);
	if (ret)
		return ret;
	if (memcmp(edid, header, sizeof(header)))
		return -EBADMSG;
	blocks = edid[126];
	if (blocks > MAX_EDID_EXT)
		return -E2BIG;
	for (i = 1; i <= blocks; i++) {
		ret = read_edid(sink, i, edid);
		if (ret)
			return ret;
		cta_caps(sink, edid, vic);
	}

	sink->version_error = scdc_read(sink, SCDC_SINK_VERSION, &sink->version);
	if (sink->version_error && (sink->version_error != -ENXIO || sink->scdc))
		return sink->version_error;
	if ((y420 ? !sink->y420 : !sink->rgb) || tmds_khz > sink->max_tmds_khz ||
	    (tmds_khz > 340000 && (!sink->scdc || !sink->version)))
		return -EOPNOTSUPP;
	return 0;
}

int hi3798_sink_configure(struct hi3798_hdmi_sink *sink, bool high)
{
	u8 flags = high ? TMDS_CONFIG_MASK : 0;
	int ret;

	if (sink->version_error == -ENXIO)
		return high ? -EOPNOTSUPP : 0;
	if (sink->version) {
		ret = scdc_write(sink, SCDC_SOURCE_VERSION, 1);
		if (ret)
			return ret;
	} else if (sink->scdc || high) {
		return -EPROTO;
	}
	ret = scdc_read(sink, SCDC_TMDS_CONFIG, &sink->config_before);
	if (ret)
		return ret;
	ret = scdc_write(sink, SCDC_TMDS_CONFIG, (sink->config_before & ~TMDS_CONFIG_MASK) | flags);
	if (ret)
		return ret;
	sink->configured = true;
	/* HDMI requires 1–100 ms from a clock ratio write to starting the stream. */
	usleep_range(1000, 2000);
	return 0;
}

int hi3798_sink_verify(struct hi3798_hdmi_sink *sink, bool high)
{
	u64 deadline = ktime_get_ns() + 250000000;
	u8 flags = high ? TMDS_CONFIG_MASK : 0;
	int ret;

	if (!sink->configured)
		return high ? -EIO : 0;
	ret = scdc_read(sink, SCDC_TMDS_CONFIG, &sink->config_after);
	if (ret)
		return ret;
	if ((sink->config_after & TMDS_CONFIG_MASK) != flags)
		return -EIO;
	do {
		ret = scdc_read(sink, SCDC_SCRAMBLER_STATUS, &sink->scrambler);
		if (ret)
			return ret;
		ret = scdc_read(sink, SCDC_STATUS_FLAGS_0, &sink->status);
		if (ret)
			return ret;
		if (!!(sink->scrambler & SCDC_SCRAMBLING_STATUS) == high &&
		    (!high || (sink->status & CHANNEL_LOCKS) == CHANNEL_LOCKS)) {
			sink->verified = true;
			return 0;
		}
		usleep_range(5000, 10000);
	} while (ktime_get_ns() < deadline);
	return -ETIMEDOUT;
}
