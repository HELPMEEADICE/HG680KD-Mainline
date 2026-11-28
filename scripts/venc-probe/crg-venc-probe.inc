/* SPDX-License-Identifier: GPL-2.0-only */
/* Included only by the CRG driver of the dedicated RAM image. */

#define VENC_CORE_CRG 0x08c
#define VENC_SMMU_CRG 0x32c
#define VENC_CORE_MASK (BIT(8) | BIT(4) | BIT(1) | BIT(0))
#define VENC_SMMU_MASK (BIT(4) | BIT(0))

int hg680kd_venc_crg(struct device *dev, bool start, u32 saved[2], u32 actual[2]);
int hg680kd_venc_crg(struct device *dev, bool start, u32 saved[2], u32 actual[2])
{
	struct hi3798mv320_crg_priv *priv = dev_get_drvdata(dev);
	unsigned long flags;
	u32 smmu, core;

	if (!priv || !priv->base)
		return -EPROBE_DEFER;

	spin_lock_irqsave(&priv->lock, flags);
	smmu = readl(priv->base + VENC_SMMU_CRG);
	core = readl(priv->base + VENC_CORE_CRG);
	if (start) {
		saved[0] = smmu;
		saved[1] = core;
		smmu |= BIT(0);
		core = (core & ~BIT(8)) | BIT(0) | BIT(1);
	} else {
		smmu = (smmu & ~VENC_SMMU_MASK) | (saved[0] & VENC_SMMU_MASK);
		core = (core & ~VENC_CORE_MASK) | (saved[1] & VENC_CORE_MASK);
	}
	writel(smmu, priv->base + VENC_SMMU_CRG);
	actual[0] = readl(priv->base + VENC_SMMU_CRG);
	writel(core, priv->base + VENC_CORE_CRG);
	actual[1] = readl(priv->base + VENC_CORE_CRG);
	spin_unlock_irqrestore(&priv->lock, flags);

	return ((actual[0] ^ smmu) & VENC_SMMU_MASK) ||
	       ((actual[1] ^ core) & VENC_CORE_MASK) ? -EIO : 0;
}
