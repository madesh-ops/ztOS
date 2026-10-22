// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-wlbt: stage 1 of Wi-Fi and Bluetooth on the Galaxy M20: boot the
 * Exynos 7904's wireless subsystem (Samsung SCSC "Maxwell", S612 radio) and
 * see its firmware run, before any of Samsung's SCSC stack is ported.
 *
 * The subsystem has its own Cortex-R4 (and an M4), boots from a 4 MB window of
 * DRAM (reserved in the DT as wifibt_if at 0xe9000000) and talks to the AP
 * through two mailboxes. The sequence is Samsung's (drivers/misc/samsung/scsc
 * in its M20 kernel: mxman.c, platform_mif.c, mif_reg_S5E7885.h):
 *
 *   1. mx140.bin to the start of the window, the rest cleared
 *   2. both mailboxes cleared and masked; ISSR0 = the firmware's entry point,
 *      ISSR1 = its config block (an offset in the window), ISSR2 = 0xbcdeedcb,
 *      ISSR3 = start-up flags
 *   3. PMU: boot from DRAM, the window's base and size, power on, reset off,
 *      start
 *
 * Nothing touches the hardware at probe. Writing 1 to the "boot" attribute
 * runs the sequence and watches for life (the firmware raising a mailbox
 * interrupt, or writing the panic record its header names, which it does
 * even when it rejects the empty config block); "boot" reads back what was
 * seen. Writing 0 puts the subsystem back into reset, as Samsung's driver
 * stops it.
 *
 * Mainline has neither of what Samsung's kernel also does around reset release:
 * exynos_acpm_set_flag() (ACPM sets BUCK2's voltage) and the modem's vote for
 * the shared regulator (exynos_pmu_shared_reg_enable()). If the core does not
 * start, those are the first suspects.
 *
 * The firmware is the stock one, from the VENDOR partition's etc/wifi/mx140.bin,
 * installed as /lib/firmware/scsc/mx140.bin.
 *
 * Bound by the device tree (compatible "zerone,m20-wlbt", in
 * devices/m20lte/kernel/exynos7904-m20lte.dts).
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#define FW_NAME			"scsc/mx140.bin"

/* mailbox (each of the R4's and the M4's) */
#define MBOX_INTCR0		0x00c
#define MBOX_INTMR0		0x010
#define MBOX_INTSR0		0x014
#define MBOX_INTMSR0		0x018
#define MBOX_INTCR1		0x020
#define MBOX_INTMR1		0x024
#define MBOX_ISSR(n)		(0x080 + 4 * (n))
#define MBOX_COUNT		8
#define MBOX2_MAGIC		0xbcdeedcb

/* PMU */
#define WIFI_CTRL_NS		0x0140
#define  WIFI_PWRON		BIT(1)
#define  WIFI_RESET_SET		BIT(2)
#define  WIFI_RESET_REQ_CLR	BIT(8)
#define WIFI_CTRL_S		0x0144
#define  WIFI_START		BIT(3)
#define WIFI_STAT		0x0148
#define CENTRAL_SEQ_WIFI_CFG	0x0380
#define CENTRAL_SEQ_WIFI_STATUS	0x0384
#define  SEQ_STATES		GENMASK(23, 16)
#define  SEQ_IN_RESET		0x80
#define RESET_AHEAD_WIFI	0x1360
#define CLEANY_BUS_WIFI		0x1364
#define LOGIC_RESET_WIFI	0x1368
#define TCXO_GATE_WIFI		0x136c
#define WIFI_DISABLE_ISO	0x1370
#define WIFI_RESET_ISO		0x1374
#define WIFI2AP_MEM_CONFIG0	0x7300		/* window size, 4 KB units */
#define WIFI2AP_MEM_CONFIG1	0x7304		/* window base >> 12 */
#define WIFI2AP_MEM_CONFIG2	0x7308
#define WIFI2AP_MEM_CONFIG3	0x730c
#define WLBT_BOOT_TEST_RST_CFG	0x7330		/* 0: boot from DRAM */

/* the mx140.bin header ("smxf"), FWHDR_02 in Samsung's fwhdr.c */
#define FWHDR_MAGIC_OFF		8
#define FWHDR_RUNTIME_LEN_OFF	36
#define FWHDR_ENTRY_OFF		40
#define FWHDR_R4_PANIC_OFF	176
#define FWHDR_MIN_LEN		188

struct zrn_wlbt {
	struct device *dev;
	void __iomem *r4;		/* the R4's mailbox */
	void __iomem *m4;		/* the M4's */
	struct regmap *pmu;
	phys_addr_t mem_phys;
	size_t mem_size;
	void *mem;			/* the window, write-combined */
	struct mutex lock;
	bool running;
	u32 panic_off;
	atomic_t mbox_irqs, wdog_irqs;
	char seen[160];
};

static void mbox_reset(struct zrn_wlbt *w)
{
	int i;

	for (i = 0; i < MBOX_COUNT; i++) {
		writel(0, w->r4 + MBOX_ISSR(i));
		writel(0, w->m4 + MBOX_ISSR(i));
	}
	/* everything masked, then everything cleared */
	writel(0xffff0000, w->r4 + MBOX_INTMR0);
	writel(0x0000ffff, w->r4 + MBOX_INTMR1);
	writel(0x0000ffff, w->m4 + MBOX_INTMR1);
	writel(0xffff0000, w->r4 + MBOX_INTCR0);
	writel(0x0000ffff, w->r4 + MBOX_INTCR1);
	writel(0x0000ffff, w->m4 + MBOX_INTCR1);
}

static irqreturn_t zrn_wlbt_mbox_irq(int irq, void *data)
{
	struct zrn_wlbt *w = data;
	u32 pending = readl(w->r4 + MBOX_INTMSR0) >> 16;

	atomic_inc(&w->mbox_irqs);
	dev_info(w->dev, "mailbox interrupt from the firmware: %#x, ISSR %08x %08x %08x %08x\n",
		 pending, readl(w->r4 + MBOX_ISSR(4)), readl(w->r4 + MBOX_ISSR(5)),
		 readl(w->r4 + MBOX_ISSR(6)), readl(w->r4 + MBOX_ISSR(7)));
	writel(pending << 16, w->r4 + MBOX_INTCR0);
	return IRQ_HANDLED;
}

static irqreturn_t zrn_wlbt_wdog_irq(int irq, void *data)
{
	struct zrn_wlbt *w = data;

	atomic_inc(&w->wdog_irqs);
	dev_warn(w->dev, "reset request from the subsystem (its watchdog)\n");
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_RESET_REQ_CLR, WIFI_RESET_REQ_CLR);
	return IRQ_HANDLED;
}

/* Back into reset, as platform_mif_pmu_reset(.., 2) and platform_mif_reset(.., 1) */
static int zrn_wlbt_stop(struct zrn_wlbt *w)
{
	u32 val;
	int ret;

	regmap_update_bits(w->pmu, RESET_AHEAD_WIFI, 0x3, 0);
	regmap_update_bits(w->pmu, CLEANY_BUS_WIFI, 0x1, 0);
	regmap_update_bits(w->pmu, LOGIC_RESET_WIFI, 0x3, 0);
	regmap_update_bits(w->pmu, TCXO_GATE_WIFI, 0x1, 0);
	regmap_update_bits(w->pmu, WIFI_DISABLE_ISO, 0x1, 1);
	regmap_update_bits(w->pmu, WIFI_RESET_ISO, 0x1, 0);
	regmap_update_bits(w->pmu, CENTRAL_SEQ_WIFI_CFG, BIT(16), 0);
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_RESET_SET, WIFI_RESET_SET);

	ret = regmap_read_poll_timeout(w->pmu, CENTRAL_SEQ_WIFI_STATUS, val,
				       FIELD_GET(SEQ_STATES, val) == SEQ_IN_RESET,
				       1000, 500000);
	if (ret)
		dev_err(w->dev, "the sequencer did not reach reset: status %#x\n", val);

	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG0, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG1, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG2, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG3, 0);
	w->running = false;
	return ret;
}

static int zrn_wlbt_boot(struct zrn_wlbt *w)
{
	const struct firmware *fw;
	u32 entry, runtime, conf_off, ctrl, stat, seq;
	u8 before[64];
	int ret, i;

	regmap_read(w->pmu, WIFI_CTRL_NS, &ctrl);
	if (ctrl & WIFI_PWRON)
		return -EBUSY;

	ret = request_firmware(&fw, FW_NAME, w->dev);
	if (ret)
		return dev_err_probe(w->dev, ret, "no %s (VENDOR's etc/wifi/mx140.bin)\n", FW_NAME);
	if (fw->size < FWHDR_MIN_LEN || memcmp(fw->data + FWHDR_MAGIC_OFF, "smxf", 4)) {
		ret = -EINVAL;
		dev_err(w->dev, "%s: not a Maxwell firmware image\n", FW_NAME);
		goto out;
	}
	entry = get_unaligned_le32(fw->data + FWHDR_ENTRY_OFF);
	runtime = get_unaligned_le32(fw->data + FWHDR_RUNTIME_LEN_OFF);
	w->panic_off = get_unaligned_le32(fw->data + FWHDR_R4_PANIC_OFF);
	/* the config block after the runtime area, page aligned, as miframman places it */
	conf_off = ALIGN(runtime, SZ_4K);
	if (fw->size > w->mem_size || conf_off >= w->mem_size ||
	    w->panic_off + sizeof(before) > w->mem_size) {
		ret = -E2BIG;
		goto out;
	}
	dev_info(w->dev, "%s: %zu bytes, entry %#x, runtime %#x, config at %#x, panic record at %#x\n",
		 FW_NAME, fw->size, entry, runtime, conf_off, w->panic_off);

	/* 1 */
	memset(w->mem, 0, w->mem_size);
	memcpy(w->mem, fw->data, fw->size);
	memcpy(before, w->mem + w->panic_off, sizeof(before));

	/* 2 */
	mbox_reset(w);
	writel(entry, w->r4 + MBOX_ISSR(0));
	writel(conf_off, w->r4 + MBOX_ISSR(1));
	writel(MBOX2_MAGIC, w->r4 + MBOX_ISSR(2));
	writel(0, w->r4 + MBOX_ISSR(3));
	/* let the firmware's interrupts to the AP through (the upper half of INTMR0) */
	writel(0x0000ffff, w->r4 + MBOX_INTMR0);
	wmb();

	/* 3 */
	atomic_set(&w->mbox_irqs, 0);
	atomic_set(&w->wdog_irqs, 0);
	regmap_write(w->pmu, WLBT_BOOT_TEST_RST_CFG, 0);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG1, (w->mem_phys & 0xffffc000) >> 12);
	regmap_write(w->pmu, WIFI2AP_MEM_CONFIG0, w->mem_size >> 12);
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_PWRON, WIFI_PWRON);
	regmap_update_bits(w->pmu, WIFI_CTRL_NS, WIFI_RESET_SET, 0);
	regmap_update_bits(w->pmu, WIFI_CTRL_S, WIFI_START, WIFI_START);
	w->running = true;

	/* 4: watch for two seconds */
	for (i = 0; i < 200; i++) {
		if (atomic_read(&w->mbox_irqs) ||
		    memcmp(before, w->mem + w->panic_off, sizeof(before)))
			break;
		msleep(10);
	}
	regmap_read(w->pmu, WIFI_STAT, &stat);
	regmap_read(w->pmu, CENTRAL_SEQ_WIFI_STATUS, &seq);
	snprintf(w->seen, sizeof(w->seen),
		 "%s after %d ms: mailbox irqs %d, watchdog irqs %d, panic record %s, WIFI_STAT %#x, sequencer %#lx\n",
		 atomic_read(&w->mbox_irqs) ||
		 memcmp(before, w->mem + w->panic_off, sizeof(before)) ? "alive" : "no sign of life",
		 i * 10, atomic_read(&w->mbox_irqs), atomic_read(&w->wdog_irqs),
		 memcmp(before, w->mem + w->panic_off, sizeof(before)) ? "written" : "untouched",
		 stat, FIELD_GET(SEQ_STATES, seq));
	dev_info(w->dev, "%s", w->seen);
out:
	release_firmware(fw);
	return ret;
}

static ssize_t boot_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zrn_wlbt *w = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s", w->seen[0] ? w->seen : "not booted\n");
}

static ssize_t boot_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct zrn_wlbt *w = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;
	mutex_lock(&w->lock);
	if (on)
		ret = w->running ? -EBUSY : zrn_wlbt_boot(w);
	else
		ret = w->running ? zrn_wlbt_stop(w) : 0;
	mutex_unlock(&w->lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(boot);

static struct attribute *zrn_wlbt_attrs[] = {
	&dev_attr_boot.attr,
	NULL
};
ATTRIBUTE_GROUPS(zrn_wlbt);

static int zrn_wlbt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct reserved_mem *rmem;
	struct device_node *np;
	struct zrn_wlbt *w;
	int irq, ret;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = dev;
	mutex_init(&w->lock);
	platform_set_drvdata(pdev, w);

	w->r4 = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(w->r4))
		return PTR_ERR(w->r4);
	w->m4 = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(w->m4))
		return PTR_ERR(w->m4);

	w->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "samsung,syscon-phandle");
	if (IS_ERR(w->pmu))
		return dev_err_probe(dev, PTR_ERR(w->pmu), "no PMU\n");

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return dev_err_probe(dev, -ENODEV, "no memory-region\n");
	rmem = of_reserved_mem_lookup(np);
	of_node_put(np);
	if (!rmem)
		return dev_err_probe(dev, -ENODEV, "memory-region is not reserved\n");
	w->mem_phys = rmem->base;
	w->mem_size = rmem->size;
	w->mem = devm_memremap(dev, w->mem_phys, w->mem_size, MEMREMAP_WC);
	if (IS_ERR(w->mem))
		return dev_err_probe(dev, PTR_ERR(w->mem), "cannot map the window\n");

	irq = platform_get_irq_byname(pdev, "mbox");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, zrn_wlbt_mbox_irq, 0, "zrn-wlbt-mbox", w);
	if (ret)
		return ret;
	irq = platform_get_irq_byname(pdev, "wdog");
	if (irq < 0)
		return irq;
	ret = devm_request_irq(dev, irq, zrn_wlbt_wdog_irq, 0, "zrn-wlbt-wdog", w);
	if (ret)
		return ret;

	dev_info(dev, "Maxwell window %pa, %zu MB; boot with: echo 1 > %s/boot\n",
		 &w->mem_phys, w->mem_size >> 20, dev_name(dev));
	return 0;
}

static void zrn_wlbt_remove(struct platform_device *pdev)
{
	struct zrn_wlbt *w = platform_get_drvdata(pdev);

	mutex_lock(&w->lock);
	if (w->running)
		zrn_wlbt_stop(w);
	mutex_unlock(&w->lock);
}

static const struct of_device_id zrn_wlbt_of_match[] = {
	{ .compatible = "zerone,m20-wlbt" },
	{ }
};
MODULE_DEVICE_TABLE(of, zrn_wlbt_of_match);

static struct platform_driver zrn_wlbt_driver = {
	.driver = {
		.name = "zrn-wlbt",
		.of_match_table = zrn_wlbt_of_match,
		.dev_groups = zrn_wlbt_groups,
	},
	.probe = zrn_wlbt_probe,
	.remove = zrn_wlbt_remove,
};
module_platform_driver(zrn_wlbt_driver);

MODULE_DESCRIPTION("Galaxy M20: boot the Maxwell Wi-Fi/BT subsystem's firmware");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FW_NAME);
