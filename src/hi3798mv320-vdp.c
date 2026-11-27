// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/scatterlist.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_drv.h>
#include <drm/drm_dumb_buffers.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_vblank.h>

#include "hi3798mv320-tiled.h"

#define VDP_INT_STATUS		0x0004
#define VDP_INT_CLEAR		0x0008
#define VDP_INT_MASK		0x000c
#define HD0_VT1			BIT(0)
#define HD0_TIMING_IRQS		GENMASK(2, 0)
#define HD0_UNDERFLOW		BIT(3)
#define VDP_FETCH_UNDERFLOW	0x0050
#define V0_UNDERFLOW		BIT(0)
#define G0_UNDERFLOW		BIT(8)
#define HD0_SCANOUT_IRQS	(HD0_VT1 | HD0_UNDERFLOW)
#define HD0_MIX_BG		0x0200
#define HD0_MIX_PRIO		0x0208
#define HD0_MIX_VP0_GP0		0x21
#define V0_LINK_CTRL		0x0c00
#define V0_ZME1_HY_COEF		0x0e04
#define V0_ZME1_VY_COEF		0x0e0c
#define V0_ZME1_HC_COEF		0x0e14
#define V0_ZME1_VC_COEF		0x0e1c
#define V0_ZME_COEF_UPDATE	0x0f00
#define V0_ZME1_COEF_UPDATES	GENMASK(3, 0)
#define V0_ZME_TABLE_BYTES	256
#define V0_ZME_TABLE_PHASES	18
#define V0_ZME_TAP_BITS		10
#define V0_ZME_COEF_WORD_TAPS	12
#define V0_CTRL			0x2000
#define V0_UPDATE		0x2004
#define V0_ACTIVE_CTRL		0x2018
#define V0_DISP_FIRST		0x2080
#define V0_DISP_LAST		0x2084
#define V0_VIDEO_FIRST		0x2088
#define V0_VIDEO_LAST		0x208c
#define V0_ZME1_HCTRL		0x22c0
#define V0_ZME1_HY_PHASE	0x22c4
#define V0_ZME1_HC_PHASE	0x22c8
#define V0_ZME1_VCTRL		0x22d8
#define V0_ZME1_VRATIO		0x22dc
#define V0_ZME1_VPHASE		0x22e0
#define V0_ZME1_VPHASE_BOTTOM	0x22e4
#define V0_OUT_SIZE		0x22e8
#define V0_IFIR_CTRL		0x2600
#define V0_IFIR_MODE		GENMASK(2, 1)
#define V0_IFIR_FACTORY_MODE	3
#define V0_IFIR_COEF0		0x2604
#define V0_IFIR_COEF_MASK	GENMASK(9, 0)
#define V0_ZME_V_LUMA_ENABLE	BIT(31)
#define V0_ZME_V_CHROMA_ENABLE	BIT(30)
#define V0_ZME_V_INPUT_420	BIT(19)
#define V0_ZME_V_CLOCK		BIT(0)
#define V0_ZME_V_LUMA_FIR	BIT(24)
#define V0_ZME_H_ENABLE		GENMASK(31, 30)
#define V0_ZME_H_LUMA_FIR	BIT(26)
#define V0_ZME_H_UNITY		BIT(20)
#define V0_ZME_V_UNITY		BIT(12)
#define V0_MIN_SCALE		(DRM_PLANE_NO_SCALING / 8)
#define V0_MAX_SCALE		(DRM_PLANE_NO_SCALING * 2)
#define VP0_CTRL		0x4000
#define VP0_UPDATE		0x4004
#define VP0_IN_SIZE		0x4010
#define VP0_DISP_FIRST		0x4080
#define VP0_DISP_LAST		0x4084
#define VP0_VIDEO_FIRST		0x4088
#define VP0_VIDEO_LAST		0x408c
#define VP0_ALPHA		0x4094
#define VP0_MUTE		0x4098
#define VP0_MIX_BG		0x4100
#define VP0_MIX_PRIO		0x4108
#define G0_CTRL			0x5000
#define G0_UPDATE		0x5004
#define G0_DISP_SIZE		0x500c
#define G0_ACTIVE_CTRL		0x5018
#define G0_DISP_FIRST		0x5080
#define G0_DISP_LAST		0x5084
#define G0_VIDEO_FIRST		0x5088
#define G0_VIDEO_LAST		0x508c
#define G0_INPUT_SIZE		0x509c
#define G0_GLOBAL_ALPHA		GENMASK(7, 0)
#define G0_FDR_BLEND		0x8a08
#define G0_PIXEL_ALPHA_255	BIT(0)
#define G0_PIXEL_ALPHA_ENABLE	BIT(1)
#define G0_PREMULTIPLIED	BIT(10)
#define G0_FDR_BYPASS		0x8a20
#define G0_FDR_FORMAT		0x8a40
#define G0_FDR_ARGB8888		0x68
#define G0_FDR_SRC_SIZE		0x8a44
#define G0_FDR_CROP		0x8a48
#define G0_FDR_IN_SIZE		0x8a4c
#define G0_FDR_ADDR_HI		0x8a50
#define G0_FDR_ADDR_LO		0x8a54
#define G0_FDR_STRIDE		0x8a60
#define G0_FDR_ACTIVE_ADDR	0x8b38
#define GP0_IN_SIZE		0x6810
#define GP0_MIX_PRIO		0x6908
#define GP0_ZME_HCTRL		0x6a04
#define GP0_ZME_VCTRL		0x6a14
#define LAYER_ENABLE		BIT(31)
#define V0_FDR_CTRL		0x8200
#define V0_FDR_MRG		BIT(19)
#define V0_FDR_IS_MUTED		BIT(16)
#define V0_FDR_CHROMA_COPY	BIT(13)
#define V0_FDR_ORDER		0x8210
#define V0_FDR_BYPASS		0x8220
#define V0_FDR_FORMAT		0x8240
#define V0_FDR_TILED_VU		(BIT(3) | 2)
#define V0_FDR_WIDTH_8_PLUS_2	BIT(8)
#define V0_FDR_SRC_SIZE		0x8244
#define V0_FDR_CROP		0x8248
#define V0_FDR_IN_SIZE		0x824c
#define V0_FDR_Y_HI		0x8250
#define V0_FDR_Y_LO		0x8254
#define V0_FDR_VU_HI		0x8258
#define V0_FDR_VU_LO		0x825c
#define V0_FDR_Y2_HI		0x8280
#define V0_FDR_Y2_LO		0x8284
#define V0_FDR_VU2_HI		0x8288
#define V0_FDR_VU2_LO		0x828c
#define V0_FDR_PITCH		0x8290
#define V0_FDR_PITCH2		0x8294
#define V0_FDR_ACTIVE_Y		0x8338
#define HD0_CTRL		0xc200
#define HD0_UPDATE		BIT(0)
#define HD0_VACTIVE		0xc208
#define HD0_HACTIVE		0xc210
#define HD0_VT			0xc228
#define HD0_VT1_LINE		GENMASK(12, 0)
#define HD0_VT1_BACK_PORCH	12
#define HD0_STATE		0xc2a0
#define HD0_IN_BLANK		BIT(1)
#define VDP_SMMU_CTRL		0xe000
#define V0_CSC_CTRL		0xb000
#define V0_CSC_ENABLE		BIT(4)
#define V0_CSC_CLOCK		BIT(1)
#define V0_CSC_GLOBAL_ENABLE	BIT(0)
#define V0_HDR_ENABLE		BIT(6)
#define V0_CSC_COEF		0xb004
#define V0_CSC_COEF_MASK		GENMASK(14, 0)
#define V0_CSC_SCALE		0xb04c
#define V0_CSC_SCALE_Q12	4
#define V0_CSC_IN_DC		0xb050
#define V0_CSC_OUT_DC		0xb05c
#define V0_CSC_DC_MASK		GENMASK(10, 0)
#define V0_CSC_CLIP_MIN		0xb080
#define V0_CSC_CLIP_MAX		0xb084

struct mv320_vdp {
	struct drm_device drm;
	struct drm_plane v0;
	struct drm_plane g0;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	struct drm_display_mode mode;
	unsigned int g0_scale_x, g0_scale_y;
	dma_addr_t zme_coeff_dma;
	bool zme_coeff_loaded;
	bool csc_dirty;
	void __iomem *regs;
	struct drm_pending_vblank_event *flip;
	atomic64_t irqs;
	atomic64_t blank_irqs;
	atomic64_t underflow_irqs;
	u32 last_irq_state;
	u32 unexpected_status;
};

static inline struct mv320_vdp *to_vdp(struct drm_device *drm)
{
	return container_of(drm, struct mv320_vdp, drm);
}

static void mv320_update_bits(struct mv320_vdp *vdp, u32 reg, u32 mask, u32 val)
{
	writel((readl(vdp->regs + reg) & ~mask) | val, vdp->regs + reg);
}

static const struct drm_format_info mv320_main10 = {
	.format = DRM_FORMAT_HI3798MV320_TILED10,
	.num_planes = 4,
	.char_per_block = { 1, 2 },
	.hsub = 2,
	.vsub = 2,
	.is_yuv = true,
};

static const struct drm_format_info *mv320_format_info(u32 fourcc, u64 modifier)
{
	if (fourcc == DRM_FORMAT_HI3798MV320_TILED10 && modifier == MV320_TILED_MODIFIER)
		return &mv320_main10;
	return NULL;
}

static bool mv320_format_supported(struct drm_plane *plane, u32 format, u64 modifier)
{
	if (plane == &to_vdp(plane->dev)->g0)
		return modifier == DRM_FORMAT_MOD_LINEAR &&
		       (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888);

	return modifier == MV320_TILED_MODIFIER &&
	       (format == DRM_FORMAT_NV21 || format == DRM_FORMAT_HI3798MV320_TILED10);
}

static struct drm_framebuffer *mv320_fb_create(struct drm_device *drm, struct drm_file *file,
					      const struct drm_format_info *info,
					      const struct drm_mode_fb_cmd2 *cmd)
{
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	bool main10 = cmd->pixel_format == DRM_FORMAT_HI3798MV320_TILED10;
	u32 stride = ALIGN(cmd->width, 256), y = stride * ALIGN(cmd->height, 64);
	u32 offsets[] = { 0, y, 3 * y / 2, 7 * y / 4 };
	u32 size = main10 ? 15 * y / 8 : 3 * y / 2;
	dma_addr_t dma;
	unsigned int i;

	if (mv320_format_supported(&to_vdp(drm)->g0, cmd->pixel_format, cmd->modifier[0]))
		return drm_gem_fb_create(drm, file, info, cmd);

	if (!mv320_format_supported(&to_vdp(drm)->v0, cmd->pixel_format, cmd->modifier[0]) ||
	    cmd->width < 32 || cmd->height < 32 || cmd->width > 4096 || cmd->height > 2176 ||
	    ((cmd->width | cmd->height) & 1))
		return ERR_PTR(-EINVAL);
	for (i = 0; i < (main10 ? 4 : 2); i++)
		if (cmd->handles[i] != cmd->handles[0] || cmd->offsets[i] != offsets[i] ||
		    cmd->pitches[i] != (i < 2 ? stride : stride / 4))
			return ERR_PTR(-EINVAL);

	fb = drm_gem_fb_create(drm, file, info, cmd);
	if (IS_ERR(fb))
		return fb;
	obj = drm_gem_fb_get_obj(fb, 0);
	for (i = 1; i < info->num_planes; i++) {
		if (drm_gem_fb_get_obj(fb, i) != obj) {
			drm_framebuffer_put(fb);
			return ERR_PTR(-EINVAL);
		}
	}
	dma = to_drm_gem_dma_obj(obj)->dma_addr;
	if (!obj->import_attach || obj->size < size || !IS_ALIGNED(dma, 256) ||
	    (u64)dma + size - 1 > U32_MAX) {
		drm_framebuffer_put(fb);
		return ERR_PTR(-EINVAL);
	}
	return fb;
}

static const struct drm_mode_config_funcs mv320_mode_funcs = {
	.fb_create = mv320_fb_create,
	.get_format_info = mv320_format_info,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int mv320_dumb_create(struct drm_file *file, struct drm_device *drm,
			     struct drm_mode_create_dumb *args)
{
	int ret = drm_mode_size_dumb(drm, args, 16, 0);

	if (ret)
		return ret;
	return drm_gem_dma_dumb_create_internal(file, drm, args);
}

static void mv320_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *drm = state->dev;

	drm_atomic_helper_commit_modeset_disables(drm, state);
	drm_atomic_helper_commit_modeset_enables(drm, state);
	drm_atomic_helper_commit_planes(drm, state, DRM_PLANE_COMMIT_ACTIVE_ONLY);
	drm_atomic_helper_fake_vblank(state);
	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_wait_for_flip_done(drm, state);
	drm_atomic_helper_cleanup_planes(drm, state);
}

static const struct drm_mode_config_helper_funcs mv320_mode_helpers = {
	.atomic_commit_tail = mv320_commit_tail,
};

static int mv320_plane_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;
	const struct drm_display_mode *m;

	if (!p->crtc || !p->fb)
		return 0;
	crtc_state = drm_atomic_get_new_crtc_state(state, p->crtc);
	m = &crtc_state->mode;
	if (p->src_x || p->src_y || p->src_w != p->fb->width << 16 ||
	    p->src_h != p->fb->height << 16 || p->crtc_x < 0 || p->crtc_y < 0 ||
	    ((p->crtc_x | p->crtc_y | p->crtc_w | p->crtc_h) & 1) || p->crtc_w > m->hdisplay ||
	    p->crtc_h > m->vdisplay || p->crtc_x > m->hdisplay - p->crtc_w ||
	    p->crtc_y > m->vdisplay - p->crtc_h)
		return -EINVAL;
	return drm_atomic_helper_check_plane_state(p, crtc_state, V0_MIN_SCALE,
						   V0_MAX_SCALE, true, false);
}

static void mv320_zme_linear_coefficients(u8 *table, unsigned int taps)
{
	unsigned int phase;

	/* The factory DMA layout packs 12 signed 10-bit taps per 128-bit word. */
	for (phase = 0; phase < V0_ZME_TABLE_PHASES; phase++) {
		unsigned int fraction = min(phase, 16U) * 32;
		unsigned int weights[] = { min(512U - fraction, 511U), min(fraction, 511U) };
		unsigned int i, bit;

		for (i = 0; i < ARRAY_SIZE(weights); i++) {
			unsigned int tap = phase * taps + taps / 2 - 1 + i;
			unsigned int start = tap / V0_ZME_COEF_WORD_TAPS * 128 +
					     tap % V0_ZME_COEF_WORD_TAPS * V0_ZME_TAP_BITS;

			for (bit = 0; bit < V0_ZME_TAP_BITS; bit++)
				if (weights[i] & BIT(bit))
					table[(start + bit) / 8] |= BIT((start + bit) % 8);
		}
	}
}

static void mv320_v0_scale(struct mv320_vdp *vdp, const struct drm_plane_state *p)
{
	u32 hr = div_u64((u64)p->fb->width * V0_ZME_H_UNITY, p->crtc_w);
	u32 vr = p->fb->height * V0_ZME_V_UNITY / p->crtc_h;
	u32 hc = hr | (hr != V0_ZME_H_UNITY ? V0_ZME_H_ENABLE : 0);
	u32 vc = V0_ZME_V_LUMA_ENABLE | V0_ZME_V_CHROMA_ENABLE |
		 V0_ZME_V_INPUT_420 | V0_ZME_V_CLOCK;

	if (hr < V0_ZME_H_UNITY)
		hc |= V0_ZME_H_LUMA_FIR;
	if (vr < V0_ZME_V_UNITY)
		vc |= V0_ZME_V_LUMA_FIR;
	if (!vdp->zme_coeff_loaded && ((hc & V0_ZME_H_LUMA_FIR) || (vc & V0_ZME_V_LUMA_FIR))) {
		dma_addr_t dma = vdp->zme_coeff_dma;

		writel(dma, vdp->regs + V0_ZME1_HY_COEF);
		writel(dma + V0_ZME_TABLE_BYTES, vdp->regs + V0_ZME1_HC_COEF);
		writel(dma + 2 * V0_ZME_TABLE_BYTES, vdp->regs + V0_ZME1_VY_COEF);
		writel(dma + 3 * V0_ZME_TABLE_BYTES, vdp->regs + V0_ZME1_VC_COEF);
		writel(V0_ZME1_COEF_UPDATES, vdp->regs + V0_ZME_COEF_UPDATE);
		vdp->zme_coeff_loaded = true;
	}

	/* Y and C share ratios. Nearest-line C preserves 4:2:0 to 4:2:2. */
	writel(hc, vdp->regs + V0_ZME1_HCTRL);
	writel(0, vdp->regs + V0_ZME1_HY_PHASE);
	writel(0, vdp->regs + V0_ZME1_HC_PHASE);
	writel(vc, vdp->regs + V0_ZME1_VCTRL);
	writel(vr, vdp->regs + V0_ZME1_VRATIO);
	writel((u16)-512, vdp->regs + V0_ZME1_VPHASE);
	writel((u16)-512, vdp->regs + V0_ZME1_VPHASE_BOTTOM);
	writel(((p->crtc_h - 1) << 16) | (p->crtc_w - 1), vdp->regs + V0_OUT_SIZE);
}

static void mv320_v0_color(struct mv320_vdp *vdp, const struct drm_plane_state *p)
{
	/*
	 * YCbCr encoding conversion in Q12. The mixer and inherited AVI
	 * consume limited BT.709. The matrix keeps R'G'B' code values through
	 * the decode and re-encode, without a transfer function or gamut LUT.
	 */
	static const s16 matrices[DRM_COLOR_ENCODING_MAX][DRM_COLOR_RANGE_MAX][9] = {
		[DRM_COLOR_YCBCR_BT601] = {
			[DRM_COLOR_YCBCR_LIMITED_RANGE] = { 4096, -472, -852, 0, 4172, 468, 0, 308, 4200 },
			[DRM_COLOR_YCBCR_FULL_RANGE] = { 3507, -415, -746, 0, 3654, 411, 0, 269, 3678 },
		},
		[DRM_COLOR_YCBCR_BT709] = {
			[DRM_COLOR_YCBCR_FULL_RANGE] = { 3507, 0, 0, 0, 3588, 0, 0, 0, 3588 },
		},
		[DRM_COLOR_YCBCR_BT2020] = {
			[DRM_COLOR_YCBCR_LIMITED_RANGE] = { 4096, 73, -381, 0, 4113, 210, 0, -47, 4083 },
			[DRM_COLOR_YCBCR_FULL_RANGE] = { 3507, 64, -334, 0, 3602, 184, 0, -41, 3576 },
		},
	};
	const s16 *matrix = matrices[p->color_encoding][p->color_range];
	const u32 enable = V0_CSC_ENABLE | V0_CSC_GLOBAL_ENABLE;
	bool bypass = p->color_encoding == DRM_COLOR_YCBCR_BT709 &&
		      p->color_range == DRM_COLOR_YCBCR_LIMITED_RANGE;
	unsigned int i;

	vdp->csc_dirty = true;
	if (bypass) {
		mv320_update_bits(vdp, V0_CSC_CTRL, enable, 0);
		return;
	}

	for (i = 0; i < 9; i++)
		writel(matrix[i] & V0_CSC_COEF_MASK, vdp->regs + V0_CSC_COEF + i * 4);
	writel(V0_CSC_SCALE_Q12, vdp->regs + V0_CSC_SCALE);
	writel(p->color_range == DRM_COLOR_YCBCR_FULL_RANGE ? 0 : -64 & V0_CSC_DC_MASK,
	       vdp->regs + V0_CSC_IN_DC);
	writel(-512 & V0_CSC_DC_MASK, vdp->regs + V0_CSC_IN_DC + 4);
	writel(-512 & V0_CSC_DC_MASK, vdp->regs + V0_CSC_IN_DC + 8);
	writel(64, vdp->regs + V0_CSC_OUT_DC);
	writel(512, vdp->regs + V0_CSC_OUT_DC + 4);
	writel(512, vdp->regs + V0_CSC_OUT_DC + 8);
	writel(0, vdp->regs + V0_CSC_CLIP_MIN);
	writel(1023, vdp->regs + V0_CSC_CLIP_MAX);
	mv320_update_bits(vdp, V0_CSC_CTRL, enable | V0_CSC_CLOCK, enable | V0_CSC_CLOCK);
}

static void mv320_plane_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mv320_vdp *vdp = to_vdp(plane->dev);
	struct drm_plane_state *old = drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = p->fb;
	bool main10 = fb->format->format == DRM_FORMAT_HI3798MV320_TILED10;
	dma_addr_t dma = to_drm_gem_dma_obj(drm_gem_fb_get_obj(fb, 0))->dma_addr;
	u32 size = ((fb->height - 1) << 16) | (fb->width - 1);
	u32 start = (p->crtc_y << 16) | p->crtc_x;
	u32 end = ((p->crtc_y + p->crtc_h - 1) << 16) | (p->crtc_x + p->crtc_w - 1);

	mv320_update_bits(vdp, V0_FDR_CTRL, V0_FDR_MRG | V0_FDR_IS_MUTED | V0_FDR_CHROMA_COPY, 0);
	mv320_update_bits(vdp, V0_FDR_ORDER, BIT(4), 0);
	mv320_update_bits(vdp, V0_FDR_BYPASS, GENMASK(1, 0), GENMASK(1, 0));
	writel(V0_FDR_TILED_VU | (main10 ? V0_FDR_WIDTH_8_PLUS_2 : 0),
	       vdp->regs + V0_FDR_FORMAT);
	writel(size, vdp->regs + V0_FDR_SRC_SIZE);
	writel(0, vdp->regs + V0_FDR_CROP);
	writel(size, vdp->regs + V0_FDR_IN_SIZE);
	writel(0, vdp->regs + V0_FDR_Y_HI);
	writel(dma, vdp->regs + V0_FDR_Y_LO);
	writel(0, vdp->regs + V0_FDR_VU_HI);
	writel(dma + fb->offsets[1], vdp->regs + V0_FDR_VU_LO);
	writel(0, vdp->regs + V0_FDR_Y2_HI);
	writel(main10 ? dma + fb->offsets[2] : 0, vdp->regs + V0_FDR_Y2_LO);
	writel(0, vdp->regs + V0_FDR_VU2_HI);
	writel(main10 ? dma + fb->offsets[3] : 0, vdp->regs + V0_FDR_VU2_LO);
	writel((fb->pitches[1] << 16) | fb->pitches[0], vdp->regs + V0_FDR_PITCH);
	writel((fb->pitches[3] << 16) | fb->pitches[2], vdp->regs + V0_FDR_PITCH2);
	writel(start, vdp->regs + V0_DISP_FIRST);
	writel(end, vdp->regs + V0_DISP_LAST);
	writel(start, vdp->regs + V0_VIDEO_FIRST);
	writel(end, vdp->regs + V0_VIDEO_LAST);
	mv320_v0_scale(vdp, p);
	if (!old->visible || old->color_encoding != p->color_encoding ||
	    old->color_range != p->color_range ||
	    drm_atomic_crtc_needs_modeset(drm_atomic_get_new_crtc_state(state, p->crtc)))
		mv320_v0_color(vdp, p);
	mv320_update_bits(vdp, V0_CTRL, LAYER_ENABLE, LAYER_ENABLE);
}

static void mv320_plane_disable(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mv320_vdp *vdp = to_vdp(plane->dev);

	mv320_update_bits(vdp, V0_CTRL, LAYER_ENABLE, 0);
}

static const struct drm_plane_helper_funcs mv320_plane_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = mv320_plane_check,
	.atomic_update = mv320_plane_update,
	.atomic_disable = mv320_plane_disable,
};

static int mv320_g0_check(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mv320_vdp *vdp = to_vdp(plane->dev);
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state;
	const struct drm_display_mode *mode;
	struct drm_framebuffer *fb = p->fb;
	dma_addr_t addr;
	u64 end;

	if (!p->crtc || !fb)
		return 0;
	if (!vdp->g0_scale_x || !vdp->g0_scale_y)
		return -EINVAL;
	crtc_state = drm_atomic_get_new_crtc_state(state, p->crtc);
	mode = &crtc_state->mode;
	addr = to_drm_gem_dma_obj(drm_gem_fb_get_obj(fb, 0))->dma_addr + fb->offsets[0];
	end = (u64)addr + (u64)fb->pitches[0] * (fb->height - 1) + fb->width * 4;
	if (!IS_ALIGNED(addr, 16) || !IS_ALIGNED(fb->pitches[0], 16) ||
	    fb->pitches[0] / 16 > U16_MAX || end - 1 > U32_MAX ||
	    p->src_x || p->src_y || p->src_w != fb->width << 16 ||
	    p->src_h != fb->height << 16 || p->crtc_x < 0 || p->crtc_y < 0 ||
	    p->crtc_x % vdp->g0_scale_x || p->crtc_y % vdp->g0_scale_y ||
	    p->crtc_w != fb->width * vdp->g0_scale_x ||
	    p->crtc_h != fb->height * vdp->g0_scale_y ||
	    p->crtc_w > mode->hdisplay || p->crtc_h > mode->vdisplay ||
	    p->crtc_x > mode->hdisplay - p->crtc_w || p->crtc_y > mode->vdisplay - p->crtc_h)
		return -EINVAL;

	return drm_atomic_helper_check_plane_state(p, crtc_state, DRM_PLANE_NO_SCALING / 2,
						   DRM_PLANE_NO_SCALING, true, false);
}

static void mv320_g0_update(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	struct mv320_vdp *vdp = to_vdp(plane->dev);
	struct drm_plane_state *p = drm_atomic_get_new_plane_state(state, plane);
	struct drm_framebuffer *fb = p->fb;
	dma_addr_t addr = to_drm_gem_dma_obj(drm_gem_fb_get_obj(fb, 0))->dma_addr + fb->offsets[0];
	u32 size = ((fb->height - 1) << 16) | (fb->width - 1);
	u32 x = p->crtc_x / vdp->g0_scale_x, y = p->crtc_y / vdp->g0_scale_y;
	u32 start = (y << 16) | x;
	u32 end = ((y + fb->height - 1) << 16) | (x + fb->width - 1);
	u32 blend = G0_PIXEL_ALPHA_255;
	u32 alpha = DIV_ROUND_CLOSEST(p->alpha, 257);

	if (fb->format->has_alpha && p->pixel_blend_mode != DRM_MODE_BLEND_PIXEL_NONE) {
		blend |= G0_PIXEL_ALPHA_ENABLE;
		if (p->pixel_blend_mode == DRM_MODE_BLEND_PREMULTI)
			blend |= G0_PREMULTIPLIED;
	}

	mv320_update_bits(vdp, G0_FDR_BLEND, G0_PIXEL_ALPHA_255 | G0_PIXEL_ALPHA_ENABLE |
			  G0_PREMULTIPLIED, blend);
	mv320_update_bits(vdp, G0_FDR_BYPASS, BIT(0), BIT(0));
	writel(G0_FDR_ARGB8888, vdp->regs + G0_FDR_FORMAT);
	writel(size, vdp->regs + G0_FDR_SRC_SIZE);
	writel(0, vdp->regs + G0_FDR_CROP);
	writel(size, vdp->regs + G0_FDR_IN_SIZE);
	writel(0, vdp->regs + G0_FDR_ADDR_HI);
	writel(addr, vdp->regs + G0_FDR_ADDR_LO);
	writel(fb->pitches[0] / 16, vdp->regs + G0_FDR_STRIDE);
	writel(start, vdp->regs + G0_DISP_FIRST);
	writel(end, vdp->regs + G0_DISP_LAST);
	writel(start, vdp->regs + G0_VIDEO_FIRST);
	writel(end, vdp->regs + G0_VIDEO_LAST);
	writel(size, vdp->regs + G0_DISP_SIZE);
	writel(size, vdp->regs + G0_INPUT_SIZE);
	mv320_update_bits(vdp, G0_CTRL, LAYER_ENABLE | G0_GLOBAL_ALPHA, LAYER_ENABLE | alpha);
}

static void mv320_g0_disable(struct drm_plane *plane, struct drm_atomic_commit *state)
{
	mv320_update_bits(to_vdp(plane->dev), G0_CTRL, LAYER_ENABLE, 0);
}

static const struct drm_plane_helper_funcs mv320_g0_helpers = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = mv320_g0_check,
	.atomic_update = mv320_g0_update,
	.atomic_disable = mv320_g0_disable,
};

static const struct drm_plane_funcs mv320_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.format_mod_supported = mv320_format_supported,
};

static bool mv320_layer_latched(struct mv320_vdp *vdp, u32 ctrl_reg, u32 update,
			       u32 active_ctrl, u32 addr, u32 active_addr)
{
	u32 ctrl = readl(vdp->regs + ctrl_reg);

	return !(readl(vdp->regs + update) & 1) &&
	       !((ctrl ^ readl(vdp->regs + active_ctrl)) & LAYER_ENABLE) &&
	       (!(ctrl & LAYER_ENABLE) ||
		readl(vdp->regs + active_addr) == readl(vdp->regs + addr));
}

static irqreturn_t mv320_irq(int irq, void *data)
{
	struct mv320_vdp *vdp = data;
	u32 status = readl(vdp->regs + VDP_INT_STATUS);
	u32 scan;
	unsigned long flags;

	if (!(status & HD0_SCANOUT_IRQS))
		return IRQ_NONE;
	WRITE_ONCE(vdp->unexpected_status,
		   vdp->unexpected_status | (status & ~HD0_TIMING_IRQS));
	if (status & HD0_UNDERFLOW)
		atomic64_inc(&vdp->underflow_irqs);
	writel(status & HD0_SCANOUT_IRQS, vdp->regs + VDP_INT_CLEAR);
	if (!(status & HD0_VT1))
		return IRQ_HANDLED;
	readl_poll_timeout_atomic(vdp->regs + HD0_STATE, scan, scan & HD0_IN_BLANK, 1, 16);
	WRITE_ONCE(vdp->last_irq_state, scan);
	if (scan & HD0_IN_BLANK)
		atomic64_inc(&vdp->blank_irqs);
	atomic64_inc(&vdp->irqs);
	drm_crtc_handle_vblank(&vdp->crtc);
	spin_lock_irqsave(&vdp->drm.event_lock, flags);
	if (vdp->flip &&
	    !(readl(vdp->regs + VP0_UPDATE) & 1) &&
	    mv320_layer_latched(vdp, V0_CTRL, V0_UPDATE, V0_ACTIVE_CTRL,
				V0_FDR_Y_LO, V0_FDR_ACTIVE_Y) &&
	    mv320_layer_latched(vdp, G0_CTRL, G0_UPDATE, G0_ACTIVE_CTRL,
				G0_FDR_ADDR_LO, G0_FDR_ACTIVE_ADDR)) {
		drm_crtc_send_vblank_event(&vdp->crtc, vdp->flip);
		vdp->flip = NULL;
		drm_crtc_vblank_put(&vdp->crtc);
	}
	spin_unlock_irqrestore(&vdp->drm.event_lock, flags);
	return IRQ_HANDLED;
}

static int mv320_enable_vblank(struct drm_crtc *crtc)
{
	struct mv320_vdp *vdp = to_vdp(crtc->dev);

	writel(HD0_SCANOUT_IRQS, vdp->regs + VDP_INT_CLEAR);
	writel(HD0_SCANOUT_IRQS, vdp->regs + VDP_INT_MASK);
	return 0;
}

static void mv320_disable_vblank(struct drm_crtc *crtc)
{
	writel(0, to_vdp(crtc->dev)->regs + VDP_INT_MASK);
}

static void mv320_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	static const s16 ifir_coefficients[] = { -7, 12, -17, 25, -38, 58, -105, 328 };
	struct mv320_vdp *vdp = to_vdp(crtc->dev);
	u32 size = ((vdp->mode.vdisplay - 1) << 16) | (vdp->mode.hdisplay - 1);
	unsigned int i;

	/* Factory V0 IFIR mode and coefficients restore chroma ahead of ZME. */
	for (i = 0; i < ARRAY_SIZE(ifir_coefficients); i += 2) {
		u32 low = ifir_coefficients[i] & V0_IFIR_COEF_MASK;
		u32 high = ifir_coefficients[i + 1] & V0_IFIR_COEF_MASK;

		writel(low | (high << 16), vdp->regs + V0_IFIR_COEF0 + i * 2);
	}
	mv320_update_bits(vdp, V0_IFIR_CTRL, V0_IFIR_MODE,
			  FIELD_PREP(V0_IFIR_MODE, V0_IFIR_FACTORY_MODE));
	vdp->zme_coeff_loaded = false;

	mv320_update_bits(vdp, V0_LINK_CTRL, BIT(0), 0);
	/* VP0 mixes YCbCr for the inherited HDMI link. */
	mv320_update_bits(vdp, V0_CSC_CTRL, V0_CSC_ENABLE | V0_CSC_GLOBAL_ENABLE |
			  V0_HDR_ENABLE, 0);

	/* G0 coordinates are in the inherited GP0 canvas. */
	mv320_g0_disable(&vdp->g0, state);
	mv320_update_bits(vdp, HD0_MIX_PRIO, GENMASK(7, 0), HD0_MIX_VP0_GP0);

	/* V0 feeds HD0 through the VP0 video mixer. Slot 0 selects V0. */
	mv320_update_bits(vdp, VP0_MUTE, BIT(31), 0);
	writel(readl(vdp->regs + HD0_MIX_BG), vdp->regs + VP0_MIX_BG);
	mv320_update_bits(vdp, VP0_MIX_PRIO, GENMASK(7, 0), 1);
	mv320_update_bits(vdp, VP0_CTRL, LAYER_ENABLE | GENMASK(7, 0), LAYER_ENABLE | 0xff);
	mv320_update_bits(vdp, VP0_ALPHA, GENMASK(7, 0), 0xff);
	writel(size, vdp->regs + VP0_IN_SIZE);
	writel(0, vdp->regs + VP0_DISP_FIRST);
	writel(size, vdp->regs + VP0_DISP_LAST);
	writel(0, vdp->regs + VP0_VIDEO_FIRST);
	writel(size, vdp->regs + VP0_VIDEO_LAST);
	writel(1, vdp->regs + VP0_UPDATE);
	drm_crtc_vblank_on(crtc);
}

static void mv320_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mv320_vdp *vdp = to_vdp(crtc->dev);
	struct drm_crtc_state *s = drm_atomic_get_new_crtc_state(state, crtc);
	unsigned long flags;
	u32 ctrl;

	mv320_plane_disable(&vdp->v0, state);
	writel(1, vdp->regs + V0_UPDATE);
	mv320_g0_disable(&vdp->g0, state);
	writel(1, vdp->regs + G0_UPDATE);
	mv320_update_bits(vdp, VP0_CTRL, LAYER_ENABLE, 0);
	writel(1, vdp->regs + VP0_UPDATE);
	if (readl_poll_timeout(vdp->regs + V0_ACTIVE_CTRL, ctrl,
			       !(ctrl & LAYER_ENABLE), 100, 50000))
		drm_err(crtc->dev, "V0 disable did not latch\n");
	if (readl_poll_timeout(vdp->regs + G0_ACTIVE_CTRL, ctrl,
			       !(ctrl & LAYER_ENABLE), 100, 50000))
		drm_err(crtc->dev, "G0 disable did not latch\n");
	drm_crtc_vblank_off(crtc);
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (!s->active && s->event) {
		drm_crtc_send_vblank_event(crtc, s->event);
		s->event = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static void mv320_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_commit *state)
{
	struct mv320_vdp *vdp = to_vdp(crtc->dev);
	struct drm_crtc_state *s = drm_atomic_get_new_crtc_state(state, crtc);
	struct drm_pending_vblank_event *event = s->event;
	unsigned long flags;
	int ret = event && s->active ? drm_crtc_vblank_get(crtc) : -EINVAL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (s->active && vdp->csc_dirty) {
		writel(1, vdp->regs + VP0_UPDATE);
		vdp->csc_dirty = false;
	}
	if (s->active && drm_atomic_get_new_plane_state(state, &vdp->v0))
		writel(1, vdp->regs + V0_UPDATE);
	if (s->active && (s->active_changed || drm_atomic_get_new_plane_state(state, &vdp->g0)))
		writel(1, vdp->regs + G0_UPDATE);
	s->event = NULL;
	if (!ret)
		vdp->flip = event;
	else if (event)
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

static enum drm_mode_status mv320_mode_valid(struct drm_crtc *crtc,
					    const struct drm_display_mode *mode)
{
	return drm_mode_equal(mode, &to_vdp(crtc->dev)->mode) ? MODE_OK : MODE_BAD;
}

static const struct drm_crtc_helper_funcs mv320_crtc_helpers = {
	.mode_valid = mv320_mode_valid,
	.atomic_enable = mv320_crtc_enable,
	.atomic_disable = mv320_crtc_disable,
	.atomic_flush = mv320_crtc_flush,
};

static const struct drm_crtc_funcs mv320_crtc_funcs = {
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.destroy = drm_crtc_cleanup,
	.reset = drm_atomic_helper_crtc_reset,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
	.enable_vblank = mv320_enable_vblank,
	.disable_vblank = mv320_disable_vblank,
};

static int mv320_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode = drm_mode_duplicate(connector->dev, &to_vdp(connector->dev)->mode);

	if (!mode)
		return 0;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_connector_helper_funcs mv320_connector_helpers = {
	.get_modes = mv320_get_modes,
};

static const struct drm_connector_funcs mv320_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_encoder_funcs mv320_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static const struct drm_display_mode mv320_modes[] = {
	{ DRM_MODE("1920x1080", 0, 148500, 1920, 2008, 2052, 2200, 0,
		   1080, 1084, 1089, 1125, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	{ DRM_MODE("3840x2160", 0, 594000, 3840, 4016, 4104, 4400, 0,
		   2160, 2168, 2178, 2250, 0, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
};

static int mv320_firmware_mode(struct mv320_vdp *vdp, struct device *dev)
{
	struct regmap *crg = syscon_regmap_lookup_by_phandle(dev->of_node, "hisilicon,crg");
	unsigned int w, h, ht, vt, clk, work, i;
	u32 horizontal, vertical, sync;
	int ret;

	if (IS_ERR(crg))
		return PTR_ERR(crg);
	ret = regmap_read(crg, 0xd8, &clk);
	if (ret)
		return ret;
	ret = regmap_read(crg, 0x34c, &work);
	if (ret)
		return ret;
	if (!(clk & BIT(0)) || !(work & BIT(14)) ||
	    !(readl(vdp->regs + HD0_CTRL) & BIT(31)) ||
	    !(readl(vdp->regs + VDP_SMMU_CTRL) & BIT(0)))
		return dev_err_probe(dev, -ENODEV, "inactive firmware HD0 or VDP SMMU enabled\n");

	horizontal = readl(vdp->regs + HD0_HACTIVE);
	vertical = readl(vdp->regs + HD0_VACTIVE);
	sync = readl(vdp->regs + 0xc220);
	w = FIELD_GET(GENMASK(12, 0), horizontal) + 1;
	h = FIELD_GET(GENMASK(12, 0), vertical) + 1;
	ht = w + (horizontal >> 16) + (readl(vdp->regs + 0xc214) & 0xffff) + 2;
	vt = h + (vertical >> 16) + (readl(vdp->regs + 0xc20c) & 0xffff) + 2;
	for (i = 0; i < ARRAY_SIZE(mv320_modes); i++) {
		const struct drm_display_mode *m = &mv320_modes[i];

		/* Hardware back porch includes the sync pulse. */
		if (m->hdisplay == w && m->vdisplay == h && m->htotal == ht && m->vtotal == vt) {
			drm_mode_copy(&vdp->mode, m);
			dev_info(dev, "firmware HD0 %ux%u totals %ux%u CRG %08x/%08x sync %08x VT %08x\n",
				 w, h, ht, vt, clk, work, sync, readl(vdp->regs + HD0_VT));
			return 0;
		}
	}
	return dev_err_probe(dev, -EINVAL, "unsupported firmware HD0 %ux%u totals %ux%u\n",
			     w, h, ht, vt);
}

static ssize_t scanout_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mv320_vdp *vdp = platform_get_drvdata(to_platform_device(dev));
	static const u32 offsets[] = {
		VDP_INT_STATUS, VDP_INT_MASK, VDP_FETCH_UNDERFLOW,
		V0_ZME1_HY_COEF, V0_ZME1_HC_COEF, V0_ZME1_VY_COEF, V0_ZME1_VC_COEF,
		HD0_MIX_PRIO, V0_CTRL, V0_UPDATE, V0_ACTIVE_CTRL,
		V0_VIDEO_FIRST, V0_VIDEO_LAST, V0_OUT_SIZE, V0_ZME1_HCTRL, V0_ZME1_VCTRL,
		V0_ZME1_VRATIO, V0_IFIR_CTRL, V0_IFIR_COEF0, V0_IFIR_COEF0 + 4,
		V0_IFIR_COEF0 + 8, V0_IFIR_COEF0 + 12, VP0_CTRL, VP0_IN_SIZE,
		VP0_MIX_PRIO, G0_CTRL, G0_UPDATE, G0_ACTIVE_CTRL, G0_VIDEO_FIRST, G0_VIDEO_LAST,
		GP0_IN_SIZE, GP0_MIX_PRIO, GP0_ZME_HCTRL, GP0_ZME_VCTRL,
		G0_FDR_BLEND, G0_FDR_FORMAT, G0_FDR_ADDR_LO, G0_FDR_STRIDE, G0_FDR_ACTIVE_ADDR,
		V0_FDR_CTRL,
		V0_FDR_BYPASS, V0_FDR_FORMAT, V0_FDR_SRC_SIZE, V0_FDR_IN_SIZE, V0_FDR_Y_LO,
		V0_FDR_VU_LO, V0_FDR_Y2_LO, V0_FDR_VU2_LO, V0_FDR_PITCH, V0_FDR_PITCH2,
		0x82ec, 0x82f0, 0x8320, 0x8324, 0x8328, 0x832c, 0x8334, V0_FDR_ACTIVE_Y,
		V0_CSC_CTRL, V0_CSC_COEF, V0_CSC_COEF + 4, V0_CSC_COEF + 8,
		V0_CSC_SCALE, V0_CSC_IN_DC, V0_CSC_OUT_DC, HD0_VT, HD0_STATE, VDP_SMMU_CTRL,
	};
	ssize_t len = sysfs_emit(buf,
				 "vblank_irqs %lld in_blank %lld last_irq_state %08x unexpected_status %08x underflow_irqs %lld\n",
				 atomic64_read(&vdp->irqs), atomic64_read(&vdp->blank_irqs),
				 READ_ONCE(vdp->last_irq_state), READ_ONCE(vdp->unexpected_status),
				 atomic64_read(&vdp->underflow_irqs));
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		len += sysfs_emit_at(buf, len, "%04x %08x\n", offsets[i], readl(vdp->regs + offsets[i]));
	len += sysfs_emit_at(buf, len, "fetch_underflow %08x\n",
			    readl(vdp->regs + VDP_FETCH_UNDERFLOW) & (V0_UNDERFLOW | G0_UNDERFLOW));
	return len;
}
static DEVICE_ATTR_RO(scanout);

DEFINE_DRM_GEM_DMA_FOPS(mv320_fops);

static struct drm_gem_object *mv320_import(struct drm_device *drm,
					   struct dma_buf_attachment *attach, struct sg_table *sgt)
{
	struct scatterlist *sg;
	unsigned int i;

	for_each_sgtable_dma_sg(sgt, sg, i)
		if (sg_dma_is_swiotlb(sg))
			return ERR_PTR(-EINVAL);
	return drm_gem_dma_prime_import_sg_table(drm, attach, sgt);
}

static const struct drm_driver mv320_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &mv320_fops,
	.name = "hi3798mv320-vdp",
	.desc = "Hi3798MV320 V0 firmware handoff",
	.major = 1,
	.dumb_create = mv320_dumb_create,
	.gem_prime_import_sg_table = mv320_import,
};

static int mv320_probe(struct platform_device *pdev)
{
	static const u32 formats[] = { DRM_FORMAT_NV21, DRM_FORMAT_HI3798MV320_TILED10 };
	static const u64 modifiers[] = { MV320_TILED_MODIFIER, DRM_FORMAT_MOD_INVALID };
	static const u32 gfx_formats[] = { DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888 };
	static const u64 gfx_modifiers[] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID };
	struct device *dev = &pdev->dev;
	struct mv320_vdp *vdp;
	struct drm_device *drm;
	unsigned int gp_width, gp_height;
	u32 gp_size;
	u8 *coefficients;
	int irq, ret;

	if (device_iommu_mapped(dev))
		return dev_err_probe(dev, -EINVAL, "direct VDP DMA required\n");
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	vdp = devm_drm_dev_alloc(dev, &mv320_drm_driver, struct mv320_vdp, drm);
	if (IS_ERR(vdp))
		return PTR_ERR(vdp);
	drm = &vdp->drm;
	vdp->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(vdp->regs))
		return PTR_ERR(vdp->regs);
	ret = mv320_firmware_mode(vdp, dev);
	if (ret)
		return ret;
	gp_size = readl(vdp->regs + GP0_IN_SIZE);
	gp_width = (gp_size & U16_MAX) + 1;
	gp_height = (gp_size >> 16) + 1;
	if (gp_width == vdp->mode.hdisplay || gp_width * 2 == vdp->mode.hdisplay)
		vdp->g0_scale_x = vdp->mode.hdisplay / gp_width;
	if (gp_height == vdp->mode.vdisplay || gp_height * 2 == vdp->mode.vdisplay)
		vdp->g0_scale_y = vdp->mode.vdisplay / gp_height;
	coefficients = dmam_alloc_coherent(dev, 4 * V0_ZME_TABLE_BYTES, &vdp->zme_coeff_dma,
					  GFP_KERNEL);
	if (!coefficients)
		return -ENOMEM;
	mv320_zme_linear_coefficients(coefficients, 8);
	mv320_zme_linear_coefficients(coefficients + V0_ZME_TABLE_BYTES, 4);
	mv320_zme_linear_coefficients(coefficients + 2 * V0_ZME_TABLE_BYTES, 6);
	mv320_zme_linear_coefficients(coefficients + 3 * V0_ZME_TABLE_BYTES, 4);
	dma_wmb();
	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.funcs = &mv320_mode_funcs;
	drm->mode_config.helper_private = &mv320_mode_helpers;
	drm->mode_config.min_width = 32;
	drm->mode_config.min_height = 32;
	drm->mode_config.max_width = 4096;
	drm->mode_config.max_height = 2176;
	ret = drm_universal_plane_init(drm, &vdp->v0, BIT(0), &mv320_plane_funcs,
				       formats, ARRAY_SIZE(formats), modifiers, DRM_PLANE_TYPE_PRIMARY, "V0");
	if (ret)
		return ret;
	drm_plane_helper_add(&vdp->v0, &mv320_plane_helpers);
	ret = drm_plane_create_zpos_immutable_property(&vdp->v0, 0);
	if (ret)
		return ret;
	ret = drm_plane_create_color_properties(&vdp->v0,
			BIT(DRM_COLOR_YCBCR_BT601) | BIT(DRM_COLOR_YCBCR_BT709) |
			BIT(DRM_COLOR_YCBCR_BT2020),
			BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) | BIT(DRM_COLOR_YCBCR_FULL_RANGE),
			DRM_COLOR_YCBCR_BT709, DRM_COLOR_YCBCR_LIMITED_RANGE);
	if (ret)
		return ret;
	ret = drm_universal_plane_init(drm, &vdp->g0, BIT(0), &mv320_plane_funcs,
				       gfx_formats, ARRAY_SIZE(gfx_formats), gfx_modifiers,
				       DRM_PLANE_TYPE_OVERLAY, "G0");
	if (ret)
		return ret;
	drm_plane_helper_add(&vdp->g0, &mv320_g0_helpers);
	ret = drm_plane_create_zpos_immutable_property(&vdp->g0, 1);
	if (ret)
		return ret;
	ret = drm_plane_create_alpha_property(&vdp->g0);
	if (ret)
		return ret;
	ret = drm_plane_create_blend_mode_property(&vdp->g0, BIT(DRM_MODE_BLEND_PREMULTI) |
						   BIT(DRM_MODE_BLEND_COVERAGE) |
						   BIT(DRM_MODE_BLEND_PIXEL_NONE));
	if (ret)
		return ret;
	ret = drm_crtc_init_with_planes(drm, &vdp->crtc, &vdp->v0, NULL, &mv320_crtc_funcs, "HD0");
	if (ret)
		return ret;
	drm_crtc_helper_add(&vdp->crtc, &mv320_crtc_helpers);
	ret = drm_encoder_init(drm, &vdp->encoder, &mv320_encoder_funcs, DRM_MODE_ENCODER_TMDS, NULL);
	if (ret)
		return ret;
	vdp->encoder.possible_crtcs = drm_crtc_mask(&vdp->crtc);
	ret = drm_connector_init(drm, &vdp->connector, &mv320_connector_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret)
		return ret;
	drm_connector_helper_add(&vdp->connector, &mv320_connector_helpers);
	ret = drm_connector_attach_encoder(&vdp->connector, &vdp->encoder);
	if (ret)
		return ret;

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);
	writel(0, vdp->regs + VDP_INT_MASK);
	/*
	 * In the back porch the new FDR address can be confirmed, and the
	 * next commit has the rest of the frame to latch.
	 */
	mv320_update_bits(vdp, HD0_VT, HD0_VT1_LINE, HD0_VT1_BACK_PORCH);
	mv320_update_bits(vdp, HD0_CTRL, HD0_UPDATE, HD0_UPDATE);
	irq = platform_get_irq_byname(pdev, "vblank");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, mv320_irq, 0, dev_name(dev), vdp);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, vdp);
	ret = device_create_file(dev, &dev_attr_scanout);
	if (ret)
		return ret;
	ret = drm_dev_register(drm, 0);
	if (ret)
		device_remove_file(dev, &dev_attr_scanout);
	return ret;
}

static void mv320_remove(struct platform_device *pdev)
{
	struct mv320_vdp *vdp = platform_get_drvdata(pdev);

	device_remove_file(&pdev->dev, &dev_attr_scanout);
	drm_dev_unregister(&vdp->drm);
	drm_atomic_helper_shutdown(&vdp->drm);
}

static const struct of_device_id mv320_of_match[] = {
	{ .compatible = "hisilicon,hi3798mv320-vdp-handoff" },
	{ }
};
MODULE_DEVICE_TABLE(of, mv320_of_match);

static struct platform_driver mv320_platform_driver = {
	.probe = mv320_probe,
	.remove = mv320_remove,
	.driver = {
		.name = "hi3798mv320-vdp",
		.of_match_table = mv320_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mv320_platform_driver);

MODULE_DESCRIPTION("Hi3798MV320 VDP V0 DRM/KMS firmware handoff");
MODULE_LICENSE("GPL");
