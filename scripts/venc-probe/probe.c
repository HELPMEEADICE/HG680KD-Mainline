// SPDX-License-Identifier: GPL-2.0-only
/* HG680KD RAM image only. One 320x192 H.264 CAVLC IDR per manual request. */

#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/vmalloc.h>

#define WIDTH		320
#define HEIGHT		192
#define STRIDE		ALIGN(WIDTH, 64)
#define Y_SIZE		(STRIDE * ALIGN(HEIGHT, 16))
#define INPUT_SIZE	(Y_SIZE * 3 / 2)
#define STREAM_CAP	PAGE_ALIGN(4 * WIDTH * HEIGHT + 4096)
#define STREAM_HEAD	64
#define GUARD		0xa5
#define STREAM_FILL	0xcd
#define PT_SIZE		SZ_4M
#define BIT_EOF		BIT(0)
#define BIT_SLICE	BIT(1)
#define BIT_TIMEOUT	BIT(6)
#define BIT_ERRORS	(BIT(2) | BIT(3))

/* Preserve the factory 720p channel allocation shape for this first IDR. */
#define REC_Y		0xe1000
#define REC_SIZE	(REC_Y * 3 / 2)
#define AUX_T		25600
#define AUX_M		204800
#define AUX_P		2592
#define AUX_A		327680
#define AUX_B		3200
#define AUX_C		6400
#define T_OFFSET	(2 * REC_SIZE)
#define M_OFFSET	(T_OFFSET + 2 * AUX_T)
#define P_OFFSET	(M_OFFSET + 2 * AUX_M)
#define A_OFFSET	(P_OFFSET + 3 * AUX_P)
#define B_OFFSET	(A_OFFSET + AUX_A)
#define C_OFFSET	(B_OFFSET + AUX_B)
#define WORK_SIZE	(C_OFFSET + AUX_C)

enum venc_area {
	SOURCE, WORK, PARAMS, POINTERS, STREAM, ERR_READ, ERR_WRITE, NUM_AREAS
};

static const size_t area_size[NUM_AREAS] = {
	[SOURCE] = INPUT_SIZE, [WORK] = WORK_SIZE, [PARAMS] = PAGE_SIZE,
	[POINTERS] = PAGE_SIZE, [STREAM] = STREAM_HEAD + STREAM_CAP,
	[ERR_READ] = PAGE_SIZE, [ERR_WRITE] = PAGE_SIZE,
};

struct venc_buffer {
	u8 *cpu;
	dma_addr_t dma;
	size_t size;
};

struct venc_snapshot {
	u32 irq, error, bus, length, tail, pic, stat0, stat1;
	u32 fault_s, fault_ns, fault_addr[4];
};

struct venc_probe {
	struct device *dev, *crg;
	void __iomem *core, *smmu;
	struct reset_control *smmu_reset, *core_reset;
	int irq;
	struct completion done;
	struct mutex lock;
	struct venc_snapshot event;
	u32 irq_count;
	struct venc_buffer area[NUM_AREAS], pt;
	u8 *input, *output;
	size_t staged, bytesused, window_size, headers_size;
	u8 headers[64];
	bool quarantined;
};

struct venc_reg {
	u16 offset;
	u32 value;
};

struct venc_bits {
	u8 data[24];
	unsigned int count;
};

#include "r002-registers.h"

int hg680kd_venc_crg(struct device *dev, bool start, u32 saved[2], u32 actual[2]);

static void put_bits(struct venc_bits *bits, u32 value, unsigned int count)
{
	while (count--) {
		unsigned int pos = bits->count++;

		bits->data[pos / 8] |= ((value >> count) & 1) << (7 - pos % 8);
	}
}

static void put_ue(struct venc_bits *bits, unsigned int value)
{
	unsigned int width = fls(value + 1);

	put_bits(bits, 0, width - 1);
	put_bits(bits, value + 1, width);
}

static size_t append_nal(u8 *dst, u8 type, struct venc_bits *rbsp)
{
	static const u8 prefix[] = { 0, 0, 0, 1 };
	unsigned int zeros = 0, i;
	size_t used = sizeof(prefix);

	put_bits(rbsp, 1, 1);
	memcpy(dst, prefix, sizeof(prefix));
	dst[used++] = type;
	for (i = 0; i < DIV_ROUND_UP(rbsp->count, 8); i++) {
		u8 byte = rbsp->data[i];

		if (zeros == 2 && byte <= 3) {
			dst[used++] = 3;
			zeros = 0;
		}
		dst[used++] = byte;
		zeros = byte ? 0 : zeros + 1;
	}
	return used;
}

static void make_headers(struct venc_probe *v, u32 *slice, u32 *lengths)
{
	struct venc_bits sps = {}, pps = {}, hdr = {};
	size_t used;

	put_bits(&sps, 66, 8);		/* Baseline */
	put_bits(&sps, 0xc0, 8);		/* Constrained Baseline */
	put_bits(&sps, 20, 8);		/* Level 2.0 */
	put_ue(&sps, 0);			/* SPS id */
	put_ue(&sps, 0);			/* 4-bit frame_num */
	put_ue(&sps, 2);			/* POC type 2 */
	put_ue(&sps, 1);			/* One reference frame */
	put_bits(&sps, 0, 1);		/* Disallow frame_num gaps */
	put_ue(&sps, WIDTH / 16 - 1);
	put_ue(&sps, HEIGHT / 16 - 1);
	put_bits(&sps, 0xc, 4);		/* Progressive, direct 8x8, no crop/VUI */
	used = append_nal(v->headers, 0x67, &sps);

	put_ue(&pps, 0);			/* PPS id */
	put_ue(&pps, 0);			/* SPS id */
	put_bits(&pps, 0, 2);		/* CAVLC, no bottom-field POC */
	put_ue(&pps, 0);			/* One slice group */
	put_ue(&pps, 0);
	put_ue(&pps, 0);			/* Default reference list sizes */
	put_bits(&pps, 0, 3);		/* No weighted prediction */
	put_ue(&pps, 0);			/* Initial QP 26, hardware adds delta */
	put_ue(&pps, 0);			/* Initial QS 26 */
	put_ue(&pps, 0);			/* Chroma QP offset */
	put_bits(&pps, 4, 3);		/* Deblocking fields present */
	v->headers_size = used + append_nal(v->headers + used, 0x68, &pps);

	/* R002 inserts first_mb_in_slice and the QP/deblocking fields itself. */
	put_ue(&hdr, 2);			/* I slice */
	put_ue(&hdr, 0);			/* PPS id */
	put_bits(&hdr, 0, 4);		/* frame_num */
	put_ue(&hdr, 0);			/* idr_pic_id */
	*slice = get_unaligned_be32(hdr.data) >> (32 - hdr.count);
	/* Factory also loads one zero reorder bit and two zero IDR marking bits. */
	*lengths = ((hdr.count - 1) << 16) | (1 - 1) << 8 | (2 - 1);
}

static int alloc_buffer(struct venc_probe *v, struct venc_buffer *b, size_t size)
{
	b->size = size;
	b->cpu = dma_alloc_coherent(v->dev, size, &b->dma, GFP_KERNEL);
	if (!b->cpu)
		return -ENOMEM;
	if (!b->dma || b->dma + size - 1 > U32_MAX || !IS_ALIGNED(b->dma, PAGE_SIZE))
		return -ERANGE;
	return 0;
}

static void free_buffer(struct venc_probe *v, struct venc_buffer *b)
{
	if (b->cpu)
		dma_free_coherent(v->dev, b->size, b->cpu, b->dma);
	*b = (struct venc_buffer){};
}

static u32 bus_addr(struct venc_probe *v, enum venc_area id, size_t offset)
{
	return v->area[id].dma + PAGE_SIZE + offset;
}

static u8 *payload(struct venc_probe *v, enum venc_area id)
{
	return v->area[id].cpu + PAGE_SIZE;
}

static int alloc_areas(struct venc_probe *v, bool identity)
{
	__le32 *pte;
	int i, ret;
	size_t offset;

	for (i = 0; i < NUM_AREAS; i++) {
		ret = alloc_buffer(v, &v->area[i], PAGE_ALIGN(area_size[i]) + 2 * PAGE_SIZE);
		if (ret)
			return ret;
		memset(v->area[i].cpu, GUARD, v->area[i].size);
		memset(payload(v, i), i == STREAM ? STREAM_FILL : 0, area_size[i]);
		dev_info(v->dev, "DMA area %d %08x +%zu\n", i, bus_addr(v, i, 0), area_size[i]);
	}
	memset(payload(v, STREAM), GUARD, STREAM_HEAD);
	memcpy(payload(v, SOURCE), v->input, INPUT_SIZE);
	memcpy(payload(v, PARAMS), v->headers, v->headers_size);
	if (!identity)
		return 0;

	ret = alloc_buffer(v, &v->pt, PT_SIZE);
	if (ret)
		return ret;
	pte = (__le32 *)v->pt.cpu;
	memset(pte, 0, PT_SIZE);
	for (i = 0; i < NUM_AREAS; i++)
		for (offset = 0; offset < area_size[i]; offset += PAGE_SIZE) {
			u32 addr = bus_addr(v, i, offset);

			pte[addr >> 12] = cpu_to_le32(addr | 1);
		}
	return 0;
}

static void program_core(struct venc_probe *v, u32 slice, u32 lengths)
{
	const u32 ref = bus_addr(v, WORK, 0);
	const u32 rec = bus_addr(v, WORK, REC_SIZE);
	const struct venc_reg dynamic[] = {
		{ 0x104, bus_addr(v, SOURCE, 0) }, { 0x108, bus_addr(v, SOURCE, Y_SIZE) },
		{ 0x11c, bus_addr(v, WORK, M_OFFSET) },
		{ 0x120, bus_addr(v, WORK, M_OFFSET) },
		{ 0x124, bus_addr(v, WORK, M_OFFSET + AUX_M) },
		{ 0x128, bus_addr(v, WORK, M_OFFSET + AUX_M) },
		{ 0x12c, bus_addr(v, POINTERS, 16) }, { 0x130, bus_addr(v, POINTERS, 0) },
		{ 0x134, bus_addr(v, WORK, P_OFFSET + AUX_P) },
		{ 0x138, bus_addr(v, WORK, P_OFFSET) },
		{ 0x13c, bus_addr(v, WORK, C_OFFSET) },
		{ 0x140, bus_addr(v, WORK, B_OFFSET) },
		{ 0x144, bus_addr(v, WORK, A_OFFSET) },
		{ 0x164, bus_addr(v, WORK, T_OFFSET + AUX_T) },
		{ 0x168, bus_addr(v, WORK, T_OFFSET) },
		{ 0x174, ref }, { 0x178, ref + REC_Y },
		{ 0x17c, ref }, { 0x180, ref + REC_Y },
		{ 0x184, ref }, { 0x188, ref + REC_Y },
		{ 0x18c, ref }, { 0x190, ref + REC_Y },
		{ 0x194, rec }, { 0x198, rec + REC_Y },
		{ 0x19c, rec }, { 0x1a0, rec + REC_Y },
		/* R002 ListBuf stores slot lengths before slot DMA addresses. */
		{ 0x27c, STREAM_CAP }, { 0x2bc, bus_addr(v, STREAM, STREAM_HEAD) },
		{ 0x2fc, bus_addr(v, PARAMS, 0) }, { 0x454, slice }, { 0x474, lengths },
	};
	unsigned int i, j;

	for (i = 0; i < ARRAY_SIZE(r002_idr); i++) {
		u32 val = r002_idr[i].value;
		u16 reg = r002_idr[i].offset;

		for (j = 0; j < ARRAY_SIZE(dynamic); j++)
			if (reg == dynamic[j].offset) {
				val = dynamic[j].value;
				break;
			}
		if (reg >= 0x304 && reg < 0x304 + sizeof(v->headers))
			val = get_unaligned_le32(v->headers + reg - 0x304);
		writel_relaxed(val, v->core + reg);
	}
}

static void snapshot(struct venc_probe *v, struct venc_snapshot *s)
{
	unsigned int i;

	s->irq = readl(v->core + 0x3000);
	s->error = readl(v->core + 0x3004);
	s->bus = readl(v->core + 0x3010);
	s->length = readl(v->core + 0x3844);
	s->tail = readl(v->core + 0x3848);
	s->pic = readl(v->core + 0x3810);
	s->stat0 = readl(v->core + 0x31c0);
	s->stat1 = readl(v->core + 0x31c4);
	s->fault_s = readl(v->smmu + 0x018);
	s->fault_ns = readl(v->smmu + 0x028);
	for (i = 0; i < ARRAY_SIZE(s->fault_addr); i++)
		s->fault_addr[i] = readl(v->smmu + 0x330 + 0x10 * i);
}

static void report(struct venc_probe *v, const char *phase, const struct venc_snapshot *s)
{
	dev_info(v->dev,
		 "%s 3000=%08x 3004=%08x 3010=%08x 3844=%08x 3848=%08x 3810=%08x 31c0=%08x 31c4=%08x fault=%08x/%08x addr=%08x/%08x/%08x/%08x\n",
		 phase, s->irq, s->error, s->bus, s->length, s->tail, s->pic,
		 s->stat0, s->stat1, s->fault_s, s->fault_ns, s->fault_addr[0],
		 s->fault_addr[1], s->fault_addr[2], s->fault_addr[3]);
}

static void ack(struct venc_probe *v)
{
	writel(U32_MAX, v->core + 0x004);
	writel(U32_MAX, v->smmu + 0x01c);
	writel(U32_MAX, v->smmu + 0x02c);
	readl(v->core + 0x3000);
}

static irqreturn_t venc_irq(int irq, void *data)
{
	struct venc_probe *v = data;
	struct venc_snapshot s;

	snapshot(v, &s);
	if (!(s.irq & (BIT_EOF | BIT_SLICE | BIT_TIMEOUT)) &&
	    !(s.error & BIT_ERRORS) && !((s.fault_s | s.fault_ns) & 0xf))
		return IRQ_NONE;
	v->irq_count++;
	v->event = s;
	ack(v);
	if ((s.irq & (BIT_EOF | BIT_TIMEOUT)) || (s.error & BIT_ERRORS) ||
	    ((s.fault_s | s.fault_ns) & 0xf)) {
		writel(0, v->core + 0x000);
		complete(&v->done);
	}
	return IRQ_HANDLED;
}

static int reset_venc(struct venc_probe *v)
{
	int ret;

	ret = reset_control_assert(v->smmu_reset);
	if (ret)
		return ret;
	udelay(5);
	ret = reset_control_deassert(v->smmu_reset);
	if (ret)
		return ret;
	udelay(10);
	ret = reset_control_assert(v->core_reset);
	if (ret)
		return ret;
	udelay(5);
	ret = reset_control_deassert(v->core_reset);
	udelay(10);
	return ret;
}

static bool guards_ok(struct venc_probe *v)
{
	unsigned int i;
	bool ok = true;

	for (i = 0; i < NUM_AREAS; i++) {
		const struct venc_buffer *b = &v->area[i];
		size_t end = PAGE_SIZE + area_size[i];

		if (memchr_inv(b->cpu, GUARD, PAGE_SIZE) ||
		    memchr_inv(b->cpu + end, GUARD, b->size - end)) {
			dev_err(v->dev, "DMA guard changed in area %u\n", i);
			ok = false;
		}
	}
	if (memchr_inv(payload(v, STREAM), GUARD, STREAM_HEAD) ||
	    memcmp(payload(v, SOURCE), v->input, INPUT_SIZE)) {
		dev_err(v->dev, "stream prefix or input changed\n");
		ok = false;
	}
	return ok;
}

static int encode(struct venc_probe *v, bool identity)
{
	struct venc_snapshot s, after;
	u32 saved[2] = {}, actual[2], slice, lengths, ctrl, drain, padded, padding;
	const u8 *stream;
	unsigned long finished;
	ktime_t start, requested;
	s64 wait_us;
	int i, ret, restore;

	if (v->quarantined)
		return -EBUSY;
	if (v->staged != INPUT_SIZE)
		return -ENODATA;
	v->bytesused = 0;
	v->window_size = 0;
	make_headers(v, &slice, &lengths);
	ret = alloc_areas(v, identity);
	if (ret)
		goto free;
	ret = hg680kd_venc_crg(v->crg, true, saved, actual);
	if (ret == -EPROBE_DEFER)
		goto free;
	dev_info(v->dev, "CRG smmu/core saved %08x/%08x enabled %08x/%08x\n",
		 saved[0], saved[1], actual[0], actual[1]);
	if (ret)
		goto restore;
	ret = reset_venc(v);
	if (ret)
		goto restore;
	writel(0, v->core + 0x000);
	program_core(v, slice, lengths);

	ctrl = identity ? 0x8 : 0x9;
	writel(ctrl, v->smmu + 0x000);
	writel(0, v->smmu + 0x008);
	writel(0, v->smmu + 0x010);
	writel(0, v->smmu + 0x020);
	writel(0, v->smmu + 0x530);
	writel(v->pt.dma, v->smmu + 0x2ec);
	writel(bus_addr(v, ERR_READ, 0), v->smmu + 0x304);
	writel(bus_addr(v, ERR_WRITE, 0), v->smmu + 0x30c);
	ack(v);
	dev_info(v->dev,
		 "job %s SMMU=%08x TTBR=%08x source=%08x/%08x stride=%08x rec=%08x/%08x stream=%08x/%u params=%08x slice=%08x/%08x\n",
		 identity ? "identity" : "bypass", readl(v->smmu), readl(v->smmu + 0x2ec),
		 readl(v->core + 0x104), readl(v->core + 0x108), readl(v->core + 0x1f0),
		 readl(v->core + 0x194), readl(v->core + 0x198), readl(v->core + 0x2bc),
		 readl(v->core + 0x27c), readl(v->core + 0x2fc),
		 readl(v->core + 0x454), readl(v->core + 0x474));
	if (readl(v->smmu) != ctrl || readl(v->smmu + 0x2ec) != v->pt.dma ||
	    readl(v->core + 0x104) != bus_addr(v, SOURCE, 0) ||
	    readl(v->core + 0x2bc) != bus_addr(v, STREAM, STREAM_HEAD) ||
	    readl(v->core + 0x27c) != STREAM_CAP) {
		ret = -EIO;
		goto restore;
	}

	reinit_completion(&v->done);
	v->event = (struct venc_snapshot){};
	v->irq_count = 0;
	snapshot(v, &s);
	report(v, "before START", &s);
	enable_irq(v->irq);
	writel(BIT_EOF | BIT_TIMEOUT, v->core + 0x000);
	dma_wmb();
	start = ktime_get();
	writel(1, v->core + 0x008);
	drain = readl(v->core + 0x3010);
	finished = wait_for_completion_timeout(&v->done, msecs_to_jiffies(500));
	wait_us = ktime_us_delta(ktime_get(), start);
	writel(0, v->core + 0x000);
	disable_irq(v->irq);

	snapshot(v, &s);
	report(v, "IRQ", &v->event);
	report(v, "before drain", &s);
	dev_info(v->dev, "irq_count=%u start_3010=%08x wait_us=%lld\n",
		 v->irq_count, drain, wait_us);
	requested = ktime_get();
	writel(1, v->core + 0x014);
	ret = readl_poll_timeout(v->core + 0x3010, drain, drain, 10, 1000000);
	dev_info(v->dev, "drain request 3010=%08x elapsed_us=%lld status=%d\n",
		 drain, ktime_us_delta(ktime_get(), requested), ret);
	writel(0, v->core + 0x014);
	if (ret) {
		snapshot(v, &after);
		report(v, "quarantined", &after);
		v->quarantined = true;
		dev_err(v->dev, "retaining DMA buffers and CRG until board restart\n");
		return ret;
	}

	snapshot(v, &after);
	report(v, "after drain release", &after);
	dma_rmb();
	padded = v->event.length & GENMASK(28, 0);
	padding = v->event.tail & GENMASK(6, 0);
	stream = payload(v, STREAM) + STREAM_HEAD;
	if (padded <= STREAM_CAP) {
		v->window_size = min_t(size_t, padded + 64, STREAM_CAP) + STREAM_HEAD;
		memcpy(v->output, stream - STREAM_HEAD, v->window_size);
	}
	dev_info(v->dev, "DMA changed recY=%u recC=%u aux=%u params=%u rpwp=%u sink=%u\n",
		 !!memchr_inv(payload(v, WORK) + REC_SIZE, 0, REC_Y),
		 !!memchr_inv(payload(v, WORK) + REC_SIZE + REC_Y, 0, REC_Y / 2),
		 !!memchr_inv(payload(v, WORK) + T_OFFSET, 0, WORK_SIZE - T_OFFSET),
		 !!memcmp(payload(v, PARAMS), v->headers, sizeof(v->headers)),
		 !!memchr_inv(payload(v, POINTERS), 0, PAGE_SIZE),
		 !!memchr_inv(payload(v, ERR_WRITE), 0, PAGE_SIZE));
	if (!finished || !(v->event.irq & BIT_EOF) ||
	    ((v->event.irq | s.irq | after.irq) & BIT_TIMEOUT) ||
	    ((v->event.error | s.error | after.error) & BIT_ERRORS) ||
	    ((v->event.fault_s | v->event.fault_ns | s.fault_s | s.fault_ns |
	      after.fault_s | after.fault_ns) & 0xf) ||
	    !guards_ok(v) || padded <= padding || padded > STREAM_CAP ||
	    !(v->event.tail & BIT(31))) {
		ret = finished ? -EIO : -ETIMEDOUT;
	} else {
		v->bytesused = padded - padding;
		dev_info(v->dev, "slot0 padded=%u padding=%u bytesused=%zu\n",
			 padded, padding, v->bytesused);
		print_hex_dump(KERN_INFO, "venc head ", DUMP_PREFIX_OFFSET, 16, 1,
			       stream, min_t(size_t, 64, v->bytesused), false);
		print_hex_dump(KERN_INFO, "venc tail ", DUMP_PREFIX_OFFSET, 16, 1,
			       stream + v->bytesused - min_t(size_t, 64, v->bytesused),
			       min_t(size_t, 64, v->bytesused), false);
	}
	ack(v);
	restore = reset_venc(v);
	snapshot(v, &after);
	report(v, "after reset", &after);
	if (!ret)
		ret = restore;
restore:
	restore = hg680kd_venc_crg(v->crg, false, saved, actual);
	dev_info(v->dev, "CRG after smmu/core %08x/%08x status=%d\n",
		 actual[0], actual[1], restore);
	if (!ret)
		ret = restore;
free:
	if (ret)
		v->bytesused = 0;
	free_buffer(v, &v->pt);
	for (i = 0; i < NUM_AREAS; i++)
		free_buffer(v, &v->area[i]);
	return ret;
}

static ssize_t input_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct venc_probe *v = file->private_data;
	ssize_t ret;

	mutex_lock(&v->lock);
	if (!*pos)
		v->staged = 0;
	if (*pos != v->staged || len > INPUT_SIZE - v->staged) {
		ret = -EINVAL;
	} else {
		ret = simple_write_to_buffer(v->input, INPUT_SIZE, pos, buf, len);
		if (ret > 0)
			v->staged = *pos;
	}
	mutex_unlock(&v->lock);
	return ret;
}

static ssize_t run_write(struct file *file, const char __user *buf, size_t len, loff_t *pos)
{
	struct venc_probe *v = file->private_data;
	char mode[16];
	bool identity;
	int ret;

	if (*pos || !len || len >= sizeof(mode))
		return -EINVAL;
	if (copy_from_user(mode, buf, len))
		return -EFAULT;
	mode[len] = 0;
	identity = sysfs_streq(mode, "identity");
	if (!identity && !sysfs_streq(mode, "bypass"))
		return -EINVAL;
	mutex_lock(&v->lock);
	ret = encode(v, identity);
	mutex_unlock(&v->lock);
	if (ret)
		return ret;
	*pos += len;
	return len;
}

static ssize_t stream_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct venc_probe *v = file->private_data;
	ssize_t ret;

	mutex_lock(&v->lock);
	ret = simple_read_from_buffer(buf, len, pos, v->output + STREAM_HEAD, v->bytesused);
	mutex_unlock(&v->lock);
	return ret;
}

static ssize_t window_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct venc_probe *v = file->private_data;
	ssize_t ret;

	mutex_lock(&v->lock);
	ret = simple_read_from_buffer(buf, len, pos, v->output, v->window_size);
	mutex_unlock(&v->lock);
	return ret;
}

static ssize_t headers_read(struct file *file, char __user *buf, size_t len, loff_t *pos)
{
	struct venc_probe *v = file->private_data;
	ssize_t ret;

	mutex_lock(&v->lock);
	ret = simple_read_from_buffer(buf, len, pos, v->headers, v->headers_size);
	mutex_unlock(&v->lock);
	return ret;
}

static const struct file_operations input_fops = { .open = simple_open, .write = input_write };
static const struct file_operations run_fops = { .open = simple_open, .write = run_write };
static const struct file_operations stream_fops = { .open = simple_open, .read = stream_read };
static const struct file_operations window_fops = { .open = simple_open, .read = window_read };
static const struct file_operations headers_fops = { .open = simple_open, .read = headers_read };

static void put_crg(void *data)
{
	put_device(data);
}

static void free_output(void *data)
{
	vfree(data);
}

static int venc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *crg;
	struct device_node *node;
	struct venc_probe *v;
	struct dentry *dir;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->dev = dev;
	v->core = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(v->core))
		return PTR_ERR(v->core);
	v->smmu = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(v->smmu))
		return PTR_ERR(v->smmu);
	node = of_parse_phandle(dev->of_node, "hisilicon,crg", 0);
	if (!node)
		return -EINVAL;
	crg = of_find_device_by_node(node);
	of_node_put(node);
	if (!crg)
		return -EPROBE_DEFER;
	v->crg = &crg->dev;
	ret = devm_add_action_or_reset(dev, put_crg, v->crg);
	if (ret)
		return ret;
	if (!dev_get_drvdata(v->crg))
		return -EPROBE_DEFER;
	v->smmu_reset = devm_reset_control_get_exclusive(dev, "smmu");
	if (IS_ERR(v->smmu_reset))
		return dev_err_probe(dev, PTR_ERR(v->smmu_reset), "SMMU reset\n");
	v->core_reset = devm_reset_control_get_exclusive(dev, "core");
	if (IS_ERR(v->core_reset))
		return dev_err_probe(dev, PTR_ERR(v->core_reset), "core reset\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	v->irq = platform_get_irq(pdev, 0);
	if (v->irq < 0)
		return v->irq;
	mutex_init(&v->lock);
	init_completion(&v->done);
	ret = devm_request_irq(dev, v->irq, venc_irq, IRQF_NO_AUTOEN, dev_name(dev), v);
	if (ret)
		return ret;
	v->input = devm_kmalloc(dev, INPUT_SIZE, GFP_KERNEL);
	v->output = vmalloc(STREAM_CAP + STREAM_HEAD);
	if (!v->output)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, free_output, v->output);
	if (ret || !v->input)
		return ret ?: -ENOMEM;

	dir = debugfs_create_dir("hi3798_venc_probe", NULL);
	if (IS_ERR(dir))
		return PTR_ERR(dir);
	debugfs_create_file("input", 0200, dir, v, &input_fops);
	debugfs_create_file("run", 0200, dir, v, &run_fops);
	debugfs_create_file("raw", 0400, dir, v, &stream_fops);
	debugfs_create_file("window", 0400, dir, v, &window_fops);
	debugfs_create_file("headers", 0400, dir, v, &headers_fops);
	return 0;
}

static const struct of_device_id venc_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-venc-ram-probe" },
	{}
};

static struct platform_driver venc_driver = {
	.probe = venc_probe,
	.driver = {
		.name = "hi3798-venc-ram-probe",
		.of_match_table = venc_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(venc_driver);
