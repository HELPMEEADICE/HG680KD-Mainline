// SPDX-License-Identifier: GPL-2.0-only
/* HG680KD RAM-only diagnostic. All MMIO writes require a debugfs command. */
#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/seq_buf.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define HPD_LEVEL       0x28
#define AON_ENABLE      0x30
#define AON_EVENT       0x34
#define AON_DDC_ERR     0x54
#define AON_DDC_PINS    0x5c
#define HPD_EVENTS      GENMASK(1, 0)
#define HPD_CONNECTED   BIT(0)
#define DDC_LINES_HIGH  GENMASK(1, 0)
#define DDC_ERRORS      GENMASK(1, 0)

/* The resource starts at TX +0x38, beyond the AIAO reset register. */
#define DDC_FIFO        (0x38 - 0x38)
#define DDC_COUNT       (0x40 - 0x38)
#define DDC_ADDR        (0x44 - 0x38)
#define DDC_STATUS      (0x48 - 0x38)
#define DDC_CMD         (0x4c - 0x38)
#define DDC_CLEAR       (0x50 - 0x38)
#define DDC_REQ         (0x68 - 0x38)
#define DDC_ACK         (0x6c - 0x38)
#define DDC_ARB         (0x70 - 0x38)
#define DDC_BUSY        BIT(2)
#define DDC_EMPTY       BIT(5)
#define DDC_RX_COUNT    GENMASK(4, 0)
#define DDC_LENGTH      GENMASK(17, 8)
#define DDC_OWN         BIT(0)
#define DDC_READ        2
#define DDC_SEG_READ    4  /* E-DDC segment pointer at 7-bit address 0x30 */
#define DDC_FLUSH       9
#define DDC_ABORT       15
#define EDID_SIZE       128
#define EDID_MAX_BLOCKS 256

#define CEC_GATE        BIT(19)
#define CEC_RESET       BIT(20)
#define OSC_GATE        BIT(21)
#define APB_GATE        BIT(22)
#define APB_RESET       BIT(23)
#define CEC_CTRL        0x08
#define CEC_RX          0x0c
#define CEC_INIT        0x1c
#define CEC_DEST        0x20
#define CEC_CAPTURE     0x24
#define CEC_TX_OPCODE   0x28
#define CEC_RX_MSG      0x38
#define CEC_POLL        BIT(4)
#define CEC_MANUAL      BIT(19)
#define CEC_RETRIES     GENMASK(28, 26)
#define CEC_OPERANDS    GENMASK(25, 22)
#define CEC_RX_CLEAR    BIT(30)
#define CEC_TX_FLUSH    BIT(14)
#define CEC_ACTIONS     (CEC_RX_CLEAR | BIT(29) | CEC_MANUAL | BIT(15) | CEC_TX_FLUSH)
#define CEC_CTRL_DYNAMIC (CEC_ACTIONS | GENMASK(13, 11) | GENMASK(3, 0))
#define CEC_NACK_HDR    BIT(9)
#define CEC_PASSTHRU    BIT(18)
#define CEC_FAIL        BIT(6)
#define CEC_DONE        BIT(11)
#define CEC_RX_EVENT    BIT(12)
#define CEC_EVENTS      GENMASK(13, 4)
#define CEC_TERMINAL    (CEC_DONE | CEC_FAIL)

struct probe_irq {
	void __iomem *aon;
	spinlock_t lock;
	u32 mask;
	u32 events;
	unsigned int hits;
	int irq;
	bool requested;
};

struct hdmi_probe {
	struct device *dev;
	struct dentry *root;
	struct mutex lock;
	void __iomem *ddc;
	void __iomem *aon;
	void __iomem *cec;
	void __iomem *crg;
	struct probe_irq hpd_irq, cec_irq;
	struct seq_buf result;
	char report[EDID_MAX_BLOCKS * 80 + 1024];
	size_t edid_len;
	u8 edid[EDID_SIZE * EDID_MAX_BLOCKS];
};

static void replace_bits(void __iomem *reg, u32 mask, u32 value)
{
	u32 old = readl(reg);
	u32 new = (old & ~mask) | (value & mask);

	if (old != new)
		writel(new, reg);
}

static u32 consume_events(struct probe_irq *line, bool from_irq)
{
	unsigned long flags;
	u32 pending, events;

	spin_lock_irqsave(&line->lock, flags);
	pending = readl(line->aon + AON_EVENT) & line->mask;
	if (pending) {
		writel(pending, line->aon + AON_EVENT);
		readl(line->aon + AON_EVENT);
		line->events |= pending;
		line->hits += from_irq;
	}
	events = from_irq ? pending : line->events;
	spin_unlock_irqrestore(&line->lock, flags);
	return events;
}

static irqreturn_t probe_irq_handler(int irq, void *arg)
{
	return consume_events(arg, true) ? IRQ_HANDLED : IRQ_NONE;
}

static int arm_irq(struct hdmi_probe *p, struct probe_irq *line,
		   unsigned int intid, const char *name)
{
	struct irq_data *data = irq_get_irq_data(line->irq);
	int ret = 0;

	if (!data || irqd_to_hwirq(data) != intid ||
	    irqd_get_trigger_type(data) != IRQ_TYPE_LEVEL_HIGH)
		return -EINVAL;
	if (!line->requested) {
		ret = devm_request_irq(p->dev, line->irq, probe_irq_handler,
				       IRQF_NO_AUTOEN, name, line);
		line->requested = !ret;
	}
	seq_buf_printf(&p->result, "%s SPI=%u INTID=%u Linux IRQ=%d registration=%d\n",
		       name, intid - 32, intid, line->irq, ret);
	return ret;
}

static int probe_hpd(struct hdmi_probe *p, unsigned int ms)
{
	struct probe_irq *line = &p->hpd_irq;
	u32 saved = readl(p->aon + AON_ENABLE);
	u32 initial = readl(p->aon + AON_EVENT);
	u32 state = readl(p->aon + HPD_LEVEL);
	unsigned long until;
	unsigned int transitions = 0;
	int ret;

	seq_buf_printf(&p->result, "HPD initial level=%08x HPD=%u RxSense=%u enable=%08x event=%08x\n",
		       state, !!(state & BIT(0)), !!(state & BIT(1)), saved, initial);
	ret = arm_irq(p, line, 120, "hi3798-hdmi-hpd");
	if (ret)
		return ret;

	line->events = 0;
	line->hits = 0;
	enable_irq(line->irq);
	replace_bits(p->aon + AON_ENABLE, HPD_EVENTS, HPD_EVENTS);
	until = jiffies + msecs_to_jiffies(ms);
	do {
		u32 now = readl(p->aon + HPD_LEVEL);

		if ((state ^ now) & HPD_EVENTS) {
			if (transitions++ < 24)
				seq_buf_printf(&p->result, "HPD transition level=%08x -> %08x\n",
					       state, now);
		}
		state = now;
		msleep(20);
	} while (time_before(jiffies, until));
	replace_bits(p->aon + AON_ENABLE, HPD_EVENTS, saved);
	disable_irq(line->irq);

	/* Catch a latched event even if this interrupt never reached the GIC. */
	initial = consume_events(line, false);
	seq_buf_printf(&p->result, "HPD final level=%08x enable=%08x event=%08x observed=%08x transitions=%u IRQ delta=%u\n",
		       state, readl(p->aon + AON_ENABLE), readl(p->aon + AON_EVENT),
		       initial, transitions, line->hits);
	return 0;
}

static void ddc_snapshot(struct hdmi_probe *p, const char *when)
{
	seq_buf_printf(&p->result,
		       "DDC %s req=%08x ack=%08x arb=%08x status=%08x count=%08x addr=%08x aon_err=%08x pins=%08x\n",
		       when, readl(p->ddc + DDC_REQ), readl(p->ddc + DDC_ACK),
		       readl(p->ddc + DDC_ARB), readl(p->ddc + DDC_STATUS),
		       readl(p->ddc + DDC_COUNT), readl(p->ddc + DDC_ADDR),
		       readl(p->aon + AON_DDC_ERR), readl(p->aon + AON_DDC_PINS));
}

static int ddc_read_block(struct hdmi_probe *p, unsigned int n, u8 *dst)
{
	u32 status, count, addr = 0xa0 | ((n & 1) * EDID_SIZE << 8) | (n >> 1 << 16);
	unsigned long until = jiffies + msecs_to_jiffies(250);
	unsigned int read = 0;
	u8 checksum = 0;

	writel(DDC_FLUSH, p->ddc + DDC_CMD);
	replace_bits(p->ddc + DDC_ADDR, GENMASK(23, 0), addr);
	replace_bits(p->ddc + DDC_COUNT, DDC_LENGTH, FIELD_PREP(DDC_LENGTH, EDID_SIZE));
	writel(n < 2 ? DDC_READ : DDC_SEG_READ, p->ddc + DDC_CMD);
	do {
		if (!(readl(p->aon + HPD_LEVEL) & HPD_CONNECTED))
			return -ENOLINK;
		status = readl(p->ddc + DDC_STATUS);
		if ((status | readl(p->aon + AON_DDC_ERR)) & DDC_ERRORS)
			return -EREMOTEIO;
		count = readl(p->ddc + DDC_COUNT) & DDC_RX_COUNT;
		if (count > EDID_SIZE - read)
			return -EOVERFLOW;
		while (count--) {
			dst[read] = readl(p->ddc + DDC_FIFO);
			checksum += dst[read++];
		}
		if (read == EDID_SIZE && !(status & DDC_BUSY)) {
			seq_buf_printf(&p->result, "EDID block=%u segment=%u offset=0x%02x tag=%02x checksum=%02x\n",
				       n, n >> 1, (n & 1) * EDID_SIZE, dst[0], checksum);
			return checksum ? -EBADMSG : 0;
		}
		usleep_range(40, 80);
	} while (time_before(jiffies, until));
	return -ETIMEDOUT;
}

static int probe_edid(struct hdmi_probe *p)
{
	static const u8 header[] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
	unsigned int n, blocks = 1;
	u32 addr, count, arb, val, status;
	int ret, idle = 0, release;

	p->edid_len = 0;
	ddc_snapshot(p, "initial");
	if (!(readl(p->aon + HPD_LEVEL) & HPD_CONNECTED))
		return -ENOLINK;
	if ((readl(p->ddc + DDC_REQ) | readl(p->ddc + DDC_ACK)) & DDC_OWN)
		return -EBUSY;
	arb = readl(p->ddc + DDC_ARB);
	replace_bits(p->ddc + DDC_REQ, DDC_OWN, DDC_OWN);
	ret = readl_poll_timeout(p->ddc + DDC_ACK, val, val & DDC_OWN, 50, 50000);
	if (ret)
		goto release;

	ddc_snapshot(p, "acquired");
	status = readl(p->ddc + DDC_STATUS);
	if ((status & DDC_BUSY) || !(status & DDC_EMPTY) ||
	    ((status | readl(p->aon + AON_DDC_ERR)) & DDC_ERRORS) ||
	    (readl(p->aon + AON_DDC_PINS) & DDC_LINES_HIGH) != DDC_LINES_HIGH) {
		ret = -EBUSY;
		goto release;
	}

	addr = readl(p->ddc + DDC_ADDR);
	count = readl(p->ddc + DDC_COUNT);
	for (n = 0; n < blocks; n++) {
		u8 *data = p->edid + n * EDID_SIZE;

		ret = ddc_read_block(p, n, data);
		if (ret)
			break;
		p->edid_len += EDID_SIZE;
		if (n == 0) {
			if (memcmp(data, header, sizeof(header))) {
				ret = -EBADMSG;
				break;
			}
			blocks = 1 + data[126];
		}
		/* CTA HDMI Forum EDID Extension Override Data Block. */
		if (n == 1 && data[0] == 0x02 && data[2] >= 7 &&
		    data[4] == 0xe2 && data[5] == 0x78)
			blocks = max(blocks, 1U + data[6]);
	}
	ddc_snapshot(p, ret ? "transaction-error" : "read-complete");
	if (ret) {
		writel(DDC_ABORT, p->ddc + DDC_CMD);
		idle = readl_poll_timeout(p->ddc + DDC_STATUS, val, !(val & DDC_BUSY),
					 50, 50000);
		if (!idle) {
			writel(DDC_FLUSH, p->ddc + DDC_CMD);
			status = readl(p->ddc + DDC_STATUS) | readl(p->aon + AON_DDC_ERR);
			if (status & DDC_ERRORS)
				writel(((status & BIT(0)) << 1) | ((status & BIT(1)) >> 1),
				       p->ddc + DDC_CLEAR);
		}
	}
	if (!idle) {
		replace_bits(p->ddc + DDC_COUNT, DDC_LENGTH, count);
		writel(addr, p->ddc + DDC_ADDR);
	}

release:
	replace_bits(p->ddc + DDC_REQ, DDC_OWN, 0);
	release = readl_poll_timeout(p->ddc + DDC_ACK, val, !(val & DDC_OWN), 50, 50000);
	ddc_snapshot(p, "released");
	status = readl(p->ddc + DDC_STATUS);
	if (!release && ((readl(p->ddc + DDC_REQ) & DDC_OWN) ||
			 readl(p->ddc + DDC_ARB) != arb || !(status & DDC_EMPTY) ||
			 (status & DDC_BUSY) ||
			 ((status | readl(p->aon + AON_DDC_ERR)) & DDC_ERRORS) ||
			 (readl(p->aon + AON_DDC_PINS) & DDC_LINES_HIGH) != DDC_LINES_HIGH))
		release = -EIO;
	seq_buf_printf(&p->result, "EDID blocks=%zu expected=%u release=%d abort_idle=%d\n",
		       p->edid_len / EDID_SIZE, blocks, release, idle);
	return ret ?: (idle ?: release);
}

static void cec_snapshot(struct hdmi_probe *p, const char *when)
{
	u32 ctrl = readl(p->cec + CEC_CTRL);

	seq_buf_printf(&p->result,
		       "CEC %s crg=%08x r00=%08x r04=%08x ctrl=%08x bus_owner=%x rx=%08x ping=%08x cdc=%08x initiator=%08x dest=%08x capture=%08x tx=%08x aon_enable=%08x aon_event=%08x\n",
		       when, readl(p->crg), readl(p->cec), readl(p->cec + 4), ctrl,
		       ctrl & 0xf, readl(p->cec + CEC_RX), readl(p->cec + 0x10),
		       readl(p->cec + 0x14), readl(p->cec + CEC_INIT), readl(p->cec + CEC_DEST),
		       readl(p->cec + CEC_CAPTURE), readl(p->cec + CEC_TX_OPCODE),
		       readl(p->aon + AON_ENABLE), readl(p->aon + AON_EVENT));
}

static u32 cec_send(struct hdmi_probe *p, u32 ctrl, unsigned int dest, bool poll)
{
	struct probe_irq *line = &p->cec_irq;
	unsigned int before;
	u32 events;
	unsigned long flags;
	void __iomem *trigger = p->cec + (poll ? CEC_DEST : CEC_CTRL);
	u32 base;

	spin_lock_irqsave(&line->lock, flags);
	line->events = 0;
	before = line->hits;
	writel(CEC_TERMINAL, p->aon + AON_EVENT);
	spin_unlock_irqrestore(&line->lock, flags);

	replace_bits(p->cec + CEC_DEST, GENMASK(4, 0), dest);
	if (!poll)
		replace_bits(p->cec + CEC_TX_OPCODE, GENMASK(7, 0), 0x83);
	base = poll ? readl(trigger) : ctrl;
	writel(base | (poll ? CEC_POLL : CEC_MANUAL), trigger);
	usleep_range(1000, 1500);
	writel(base, trigger);
	/* Allow IRQ dispatch before polling, then retain a snapshot on no delivery. */
	read_poll_timeout(READ_ONCE, events, events & CEC_TERMINAL, 1000, 350000,
			  false, line->events);
	events = consume_events(line, false);
	seq_buf_printf(&p->result,
		       "CEC %s 4->%u result=%s events=%08x IRQ delta=%u r04=%08x ctrl=%08x rx=%08x\n",
		       poll ? "Polling" : "Give Physical Address (0x83)", dest,
		       (events & CEC_TERMINAL) == CEC_DONE ? "ACK" :
		       (events & CEC_TERMINAL) == CEC_FAIL ?
		       (poll ? "NACK" : "TX_FAIL") :
		       events & CEC_TERMINAL ? "AMBIGUOUS" : "TIMEOUT",
		       events, READ_ONCE(line->hits) - before, readl(p->cec + 4),
		       readl(p->cec + CEC_CTRL), readl(p->cec + CEC_RX));
	return events;
}

static int probe_cec(struct hdmi_probe *p)
{
	struct probe_irq *line = &p->cec_irq;
	static const u8 regs[] = { CEC_CTRL, CEC_INIT, CEC_DEST, CEC_CAPTURE, CEC_TX_OPCODE };
	u32 saved[ARRAY_SIZE(regs)], ctrl, events, val;
	u32 clocks = readl(p->crg);
	u32 enable = readl(p->aon + AON_ENABLE);
	int i, ret;

	seq_buf_printf(&p->result, "CEC initial crg=%08x aon_enable=%08x aon_event=%08x\n",
		       clocks, enable, readl(p->aon + AON_EVENT));
	if ((clocks & (OSC_GATE | APB_GATE | APB_RESET)) != (OSC_GATE | APB_GATE))
		return -EHOSTDOWN;
	if (!(readl(p->aon + HPD_LEVEL) & HPD_CONNECTED))
		return -ENOLINK;
	ret = arm_irq(p, line, 179, "hi3798-hdmi-cec");
	if (ret)
		return ret;

	replace_bits(p->crg, CEC_GATE, CEC_GATE);
	readl(p->crg);
	udelay(2);
	replace_bits(p->crg, CEC_RESET, 0);
	readl(p->crg);
	udelay(2);
	cec_snapshot(p, "ready");
	for (i = 0; i < ARRAY_SIZE(regs); i++)
		saved[i] = readl(p->cec + regs[i]);
	if ((saved[0] & CEC_ACTIONS) || (saved[2] & CEC_POLL) ||
	    (readl(p->cec + CEC_RX) & GENMASK(7, 0)) ||
	    ((clocks & (CEC_GATE | CEC_RESET)) == CEC_GATE &&
	     ((enable | readl(p->aon + AON_EVENT)) & CEC_EVENTS))) {
		ret = -EBUSY;
		goto restore_clock;
	}

	ctrl = saved[0] & ~(CEC_ACTIONS | CEC_RETRIES | CEC_OPERANDS |
			    CEC_PASSTHRU | CEC_NACK_HDR);
	writel(ctrl, p->cec + CEC_CTRL);
	replace_bits(p->cec + CEC_INIT, GENMASK(3, 0), 4);
	replace_bits(p->cec + CEC_CAPTURE, GENMASK(15, 0), 0);
	line->hits = 0;
	line->events = 0;
	writel(CEC_EVENTS, p->aon + AON_EVENT);
	enable_irq(line->irq);
	replace_bits(p->aon + AON_ENABLE, CEC_EVENTS, CEC_EVENTS);

	events = cec_send(p, ctrl, 4, true);
	if ((events & CEC_TERMINAL) == CEC_DONE) {
		/* Address 4 is occupied. Leave that device's address alone. */
		ret = -EADDRINUSE;
	} else if ((events & CEC_TERMINAL) == CEC_FAIL) {
		msleep(20);
		events = cec_send(p, ctrl, 0, true);
		msleep(20);
		replace_bits(p->cec + CEC_CAPTURE, GENMASK(15, 0), BIT(4));
		if (events & CEC_TERMINAL)
			events = cec_send(p, ctrl, 0, false);
		if (!(events & CEC_TERMINAL))
			ret = -ETIMEDOUT;
		msleep(150);
		events = consume_events(line, false);
		if (events & CEC_RX_EVENT)
			seq_buf_printf(&p->result, "CEC RX header=%08x data0=%08x data1=%08x\n",
				       readl(p->cec + CEC_RX), readl(p->cec + CEC_RX_MSG),
				       readl(p->cec + CEC_RX_MSG + 4));
	} else {
		ret = -ETIMEDOUT;
	}

	replace_bits(p->aon + AON_ENABLE, CEC_EVENTS, 0);
	disable_irq(line->irq);
	events = consume_events(line, false);
	cec_snapshot(p, "after-transmit");
	seq_buf_printf(&p->result, "CEC consumed=%08x total IRQ delta=%u\n", events, line->hits);
	writel(ctrl | CEC_TX_FLUSH | CEC_RX_CLEAR, p->cec + CEC_CTRL);
	usleep_range(1000, 1500);
	for (i = ARRAY_SIZE(regs) - 1; i >= 0; i--)
		writel(saved[i], p->cec + regs[i]);
	consume_events(line, false);
	replace_bits(p->aon + AON_ENABLE, CEC_EVENTS, enable);
	cec_snapshot(p, "controller-restored");
	for (i = 0; i < ARRAY_SIZE(regs); i++) {
		val = readl(p->cec + regs[i]) ^ saved[i];
		if (regs[i] == CEC_CTRL)
			val &= ~CEC_CTRL_DYNAMIC;
		if (val)
			ret = -EIO;
	}
	if (readl(p->aon + AON_ENABLE) != enable ||
	    (readl(p->aon + AON_EVENT) & CEC_EVENTS))
		ret = -EIO;
	seq_buf_printf(&p->result, "CEC line voltage requires an external pin measurement\n");

restore_clock:
	replace_bits(p->crg, CEC_RESET, clocks);
	replace_bits(p->crg, CEC_GATE, clocks);
	val = readl(p->crg);
	seq_buf_printf(&p->result, "CEC final crg=%08x expected=%08x aon_enable=%08x aon_event=%08x\n",
		       val, clocks, readl(p->aon + AON_ENABLE), readl(p->aon + AON_EVENT));
	return val != clocks ? -EIO : ret;
}

static ssize_t run_probe(struct file *file, const char __user *from, size_t len, loff_t *ppos)
{
	struct hdmi_probe *p = file->private_data;
	char cmd[16];
	int ret;

	if (!len || len >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, from, len))
		return -EFAULT;
	cmd[len] = '\0';
	strim(cmd);

	mutex_lock(&p->lock);
	seq_buf_clear(&p->result);
	if (!strcmp(cmd, "hpd"))
		ret = probe_hpd(p, 1000);
	else if (!strcmp(cmd, "watch"))
		ret = probe_hpd(p, 12000);
	else if (!strcmp(cmd, "edid"))
		ret = probe_edid(p);
	else if (!strcmp(cmd, "cec"))
		ret = probe_cec(p);
	else
		ret = -EINVAL;
	seq_buf_printf(&p->result, "%s return=%d\n", cmd, ret);
	mutex_unlock(&p->lock);
	return ret ? ret : (ssize_t)len;
}

static ssize_t read_report(struct file *file, char __user *to, size_t len, loff_t *pos)
{
	struct hdmi_probe *p = file->private_data;
	ssize_t ret;

	mutex_lock(&p->lock);
	ret = simple_read_from_buffer(to, len, pos, p->report, seq_buf_used(&p->result));
	mutex_unlock(&p->lock);
	return ret;
}

static ssize_t read_edid(struct file *file, char __user *to, size_t len, loff_t *pos)
{
	struct hdmi_probe *p = file->private_data;
	ssize_t ret;

	mutex_lock(&p->lock);
	ret = simple_read_from_buffer(to, len, pos, p->edid, p->edid_len);
	mutex_unlock(&p->lock);
	return ret;
}

static const struct file_operations command_fops = {
	.open = simple_open,
	.write = run_probe,
};

static const struct file_operations report_fops = {
	.open = simple_open,
	.read = read_report,
};

static const struct file_operations edid_fops = {
	.open = simple_open,
	.read = read_edid,
};

static int register_probe(struct platform_device *pdev)
{
	struct hdmi_probe *p;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = &pdev->dev;
	p->ddc = devm_platform_ioremap_resource(pdev, 0);
	p->aon = devm_platform_ioremap_resource(pdev, 1);
	p->cec = devm_platform_ioremap_resource(pdev, 2);
	p->crg = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(p->ddc) || IS_ERR(p->aon) || IS_ERR(p->cec) || IS_ERR(p->crg))
		return -ENXIO;
	p->hpd_irq.irq = platform_get_irq_byname(pdev, "hpd");
	p->cec_irq.irq = platform_get_irq_byname(pdev, "cec");
	if (p->hpd_irq.irq < 0)
		return p->hpd_irq.irq;
	if (p->cec_irq.irq < 0)
		return p->cec_irq.irq;
	p->hpd_irq.aon = p->cec_irq.aon = p->aon;
	p->hpd_irq.mask = HPD_EVENTS;
	p->cec_irq.mask = CEC_EVENTS;
	spin_lock_init(&p->hpd_irq.lock);
	spin_lock_init(&p->cec_irq.lock);
	mutex_init(&p->lock);
	seq_buf_init(&p->result, p->report, sizeof(p->report));
	seq_buf_printf(&p->result, "Write hpd, watch, edid or cec to run.\n");

	p->root = debugfs_create_dir("hi3798_hdmi_probe", NULL);
	if (IS_ERR(p->root))
		return PTR_ERR(p->root);
	platform_set_drvdata(pdev, p);
	debugfs_create_file("run", 0200, p->root, p, &command_fops);
	debugfs_create_file("report", 0400, p->root, p, &report_fops);
	debugfs_create_file("edid", 0400, p->root, p, &edid_fops);
	return 0;
}

static void remove_probe(struct platform_device *pdev)
{
	struct hdmi_probe *p = platform_get_drvdata(pdev);

	debugfs_remove_recursive(p->root);
}

static const struct of_device_id probe_match[] = {
	{ .compatible = "hisilicon,hg680kd-hdmi-ram-probe" },
	{ }
};

static struct platform_driver probe_driver = {
	.probe = register_probe,
	.remove = remove_probe,
	.driver = {
		.name = "hg680kd-hdmi-ram-probe",
		.of_match_table = probe_match,
	},
};
builtin_platform_driver(probe_driver);
