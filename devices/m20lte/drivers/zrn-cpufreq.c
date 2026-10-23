// SPDX-License-Identifier: GPL-2.0-only
/*
 * zrn-cpufreq: CPU frequency scaling on the Galaxy M20 (Exynos 7904).
 *
 * Two domains, as Samsung's kernel has them: the Cortex-A73 pair (CPUs 6-7,
 * CMU_CPUCL0, supplied by the S2MPU08's BUCK2) and the six Cortex-A53s (CPUs
 * 0-3 on CMU_CPUCL1 and 4-5 on CMU_CPUCL2, each cluster with its own PLL but
 * both on BUCK3). The bootloader leaves them at 1352 and 1014 MHz.
 *
 * The levels and voltages are the bootloader's ECT for this chip: the PLL
 * settings from its PLL tables, the voltages from ASV table 3 (the one its
 * MINMAX entries pick for table version 20), speed group 9, both capped where
 * MINMAX caps them (1768 and 1586 MHz). Other speed groups need other
 * voltages, so the driver reads the ASV fuses first and stays out of the way
 * unless they are this chip's; it also checks that the bucks hold the table's
 * voltage for the boot level.
 *
 * The voltage goes up before the clock and down after it. The PMIC is behind
 * the APM: ACPM PMIC messages on channel 2, as for the GPU's BUCK5.
 *
 * A PLL is changed the way the Galaxy Tab A 10.1 2019 port found the
 * pll_1417x needs (github.com/Mollomm1/linux-exynos7885, branch gta3xl,
 * "clk: samsung: pll: ..."): with interrupts off, park the cluster on its
 * switch-user clock (the 26 MHz oscillator here), restart the PLL at once
 * with the dividers, enable and output select in one write, wait for lock,
 * unpark. A restart that does not lock (about one in fifteen) is deselected,
 * unparked onto the oscillator and tried again, up to five times.
 */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

/* CMU_CPUCLn */
#define PLL_CON0_CPUCL0			0x0140
#define PLL_CON0_CPUCL12		0x0120
#define MUX_CLK_CPUCL_PLL		0x1000	/* bit 0: 1 = switch-user (park) */
#define MUX_BUSY			BIT(16)
#define PLL_ENABLE			BIT(31)
#define PLL_LOCKED			BIT(29)
#define PLL_MUX_SEL			BIT(4)
#define PLL_PMS_MASK			(GENMASK(25, 16) | GENMASK(13, 8) | GENMASK(2, 0))
#define PLL_PMS(m, p, s)		(((m) << 16) | ((p) << 8) | (s))
#define PLL_TRIES			5
#define PLL_LOCK_US			200

/* S2MPU08 over ACPM */
#define ACPM_TYPE_PMIC			1
#define S2MPU08_B2CTRL2			0x17	/* vdd_cpucl0 */
#define S2MPU08_B3CTRL2			0x19	/* vdd_cpucl1_2 */
#define BUCK_MIN_UV			300000
#define BUCK_STEP_UV			6250
#define BUCK_RAMP_UV_US			12000

/* ASV fuses, read through EL3 */
#define SMC_CMD_REG			((unsigned long)-101)
#define SMC_REG_ID_SFR_R(a)		((0x3UL << 30) | ((a) >> 2))
#define ASV_FUSE0			0x10009000
#define ASV_FUSE3			0x1000900c

struct zrn_level {
	unsigned int khz;
	u32 pms;
	unsigned int uv;
};

#define L(mhz, m, p, s, uv) { (mhz) * 1000, PLL_PMS(m, p, s), uv }

/* fastest first */
static const struct zrn_level a73_levels[] = {
	L(1768, 204, 3, 0, 962500),
	L(1664, 192, 3, 0, 918750),
	L(1560, 180, 3, 0, 875000),
	L(1352, 156, 3, 0, 806250),
	L(1144, 132, 3, 0, 743750),
	L(936, 216, 3, 1, 687500),
};

static const struct zrn_level a53_levels[] = {
	L(1586, 183, 3, 0, 962500),
	L(1482, 171, 3, 0, 912500),
	L(1352, 156, 3, 0, 850000),
	L(1248, 144, 3, 0, 818750),
	L(1144, 132, 3, 0, 781250),
	L(1014, 117, 3, 0, 737500),
	L(676, 156, 3, 1, 637500),
	L(546, 126, 3, 1, 600000),
};

struct zrn_domain {
	const char *name;
	const char *cpus;		/* cpulist */
	const struct zrn_level *levels;
	unsigned int nr_levels;
	u8 buck_reg;
	unsigned int nr_cmus;
	void __iomem *cmu[2];
	u32 pll_con0;
	struct cpufreq_frequency_table *table;
	unsigned int cur;		/* index into levels */
	cpumask_var_t mask;
};

static struct zrn_domain domains[] = {
	{ .name = "A53", .cpus = "0-5", .levels = a53_levels,
	  .nr_levels = ARRAY_SIZE(a53_levels), .buck_reg = S2MPU08_B3CTRL2,
	  .nr_cmus = 2, .pll_con0 = PLL_CON0_CPUCL12 },
	{ .name = "A73", .cpus = "6-7", .levels = a73_levels,
	  .nr_levels = ARRAY_SIZE(a73_levels), .buck_reg = S2MPU08_B2CTRL2,
	  .nr_cmus = 1, .pll_con0 = PLL_CON0_CPUCL0 },
};

static struct acpm_handle *acpm;
static unsigned int acpm_chan;
static struct device *zrn_dev;

static int buck_get_uv(struct zrn_domain *d)
{
	u8 val;
	int ret = acpm->ops.pmic_ops.read_reg(acpm, acpm_chan, ACPM_TYPE_PMIC,
					      d->buck_reg, 0, &val);

	return ret ?: BUCK_MIN_UV + val * BUCK_STEP_UV;
}

static int buck_set_uv(struct zrn_domain *d, unsigned int old_uv, unsigned int uv)
{
	unsigned int sel = DIV_ROUND_UP(uv - BUCK_MIN_UV, BUCK_STEP_UV);
	int ret;

	ret = acpm->ops.pmic_ops.write_reg(acpm, acpm_chan, ACPM_TYPE_PMIC,
					   d->buck_reg, 0, sel);
	if (ret)
		return ret;
	/* the ramp, when rising, plus a little to settle */
	if (uv > old_uv)
		udelay(DIV_ROUND_UP(uv - old_uv, BUCK_RAMP_UV_US) + 10);
	return 0;
}

static void park(void __iomem *cmu, bool on)
{
	u32 v = readl(cmu + MUX_CLK_CPUCL_PLL) & ~BIT(0);

	writel(v | (on ? BIT(0) : 0), cmu + MUX_CLK_CPUCL_PLL);
	readl_poll_timeout_atomic(cmu + MUX_CLK_CPUCL_PLL, v, !(v & MUX_BUSY), 1, 100);
}

/* Change one cluster's PLL; false if it never locked (it is then left
 * deselected: the cluster runs from the oscillator) */
static bool pll_set(void __iomem *cmu, u32 con0_off, u32 pms)
{
	void __iomem *con0 = cmu + con0_off;
	unsigned long flags;
	bool locked = false;
	u32 v;

	for (int i = 0; i < PLL_TRIES && !locked; i++) {
		local_irq_save(flags);
		park(cmu, true);
		v = readl(con0) & ~(PLL_ENABLE | PLL_MUX_SEL | PLL_PMS_MASK);
		writel(v, con0);
		readl(con0);
		writel(v | pms | PLL_ENABLE | PLL_MUX_SEL, con0);
		locked = !readl_poll_timeout_atomic(con0, v, v & PLL_LOCKED, 1, PLL_LOCK_US);
		if (!locked)
			writel(readl(con0) & ~(PLL_ENABLE | PLL_MUX_SEL), con0);
		park(cmu, false);
		local_irq_restore(flags);
	}
	return locked;
}

static u32 pll_pms(struct zrn_domain *d, unsigned int c)
{
	return readl(d->cmu[c] + d->pll_con0) & PLL_PMS_MASK;
}

static struct zrn_domain *domain_of(unsigned int cpu)
{
	for (int i = 0; i < ARRAY_SIZE(domains); i++) {
		if (cpumask_test_cpu(cpu, domains[i].mask))
			return &domains[i];
	}
	return NULL;
}

static int zrn_target_index(struct cpufreq_policy *policy, unsigned int index)
{
	struct zrn_domain *d = policy->driver_data;
	const struct zrn_level *from = &d->levels[d->cur], *to = &d->levels[index];
	bool ok = true;
	int ret;

	if (to->uv > from->uv) {
		ret = buck_set_uv(d, from->uv, to->uv);
		if (ret)
			return ret;
	}
	for (unsigned int c = 0; c < d->nr_cmus; c++) {
		if (!pll_set(d->cmu[c], d->pll_con0, to->pms)) {
			dev_err(zrn_dev, "%s cluster %u: PLL did not lock at %u kHz\n",
				d->name, c, to->khz);
			ok = false;
		}
	}
	if (!ok) {
		/* a cluster is on the oscillator: keep the voltage high enough
		 * for whichever level it goes back to */
		return -EIO;
	}
	if (to->uv < from->uv) {
		ret = buck_set_uv(d, from->uv, to->uv);
		if (ret)
			dev_warn(zrn_dev, "%s: voltage not lowered: %d\n", d->name, ret);
	}
	d->cur = index;
	return 0;
}

static unsigned int zrn_get(unsigned int cpu)
{
	struct zrn_domain *d = domain_of(cpu);

	return d ? d->levels[d->cur].khz : 0;
}

static int zrn_init(struct cpufreq_policy *policy)
{
	struct zrn_domain *d = domain_of(policy->cpu);

	if (!d)
		return -ENODEV;
	policy->driver_data = d;
	policy->freq_table = d->table;
	policy->cpuinfo.transition_latency = 300 * NSEC_PER_USEC;
	/*
	 * A change costs an ACPM message to the PMIC and a PLL restart with its
	 * cluster parked on the oscillator, interrupts off. schedutil's default
	 * from the latency, 450 us, made the A53s change about 100 times a
	 * second at light load, the stalls showing as jank; 10 ms makes it
	 * about 14.
	 */
	policy->transition_delay_us = 10 * USEC_PER_MSEC;
	cpumask_copy(policy->cpus, d->mask);
	return 0;
}

static struct cpufreq_driver zrn_cpufreq_driver = {
	.name		= "zrn-cpufreq",
	.flags		= CPUFREQ_NEED_INITIAL_FREQ_CHECK | CPUFREQ_IS_COOLING_DEV,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= zrn_target_index,
	.get		= zrn_get,
	.init		= zrn_init,
};

static u32 asv_fuse(unsigned long addr)
{
	struct arm_smccc_res res;

	arm_smccc_smc(SMC_CMD_REG, SMC_REG_ID_SFR_R(addr), 0, 0, 0, 0, 0, 0, &res);
	return res.a2;
}

/* Find the level the domain is at, and check the bucks hold its voltage */
static int domain_setup(struct device *dev, struct zrn_domain *d)
{
	u32 pms = pll_pms(d, 0);
	int uv, cur = -1;

	for (unsigned int c = 1; c < d->nr_cmus; c++) {
		if (pll_pms(d, c) != pms)
			return dev_err_probe(dev, -EINVAL, "%s clusters at different rates\n", d->name);
	}
	for (unsigned int i = 0; i < d->nr_levels; i++) {
		if (d->levels[i].pms == pms)
			cur = i;
	}
	if (cur < 0)
		return dev_err_probe(dev, -EINVAL, "%s PLL %#x is no level of ours\n", d->name, pms);

	uv = buck_get_uv(d);
	if (uv < 0)
		return dev_err_probe(dev, uv, "%s: cannot read its buck\n", d->name);
	if (uv != d->levels[cur].uv)
		return dev_err_probe(dev, -EINVAL,
				     "%s at %u kHz on %d uV, the table says %u: not this chip's table\n",
				     d->name, d->levels[cur].khz, uv, d->levels[cur].uv);
	d->cur = cur;

	d->table = devm_kcalloc(dev, d->nr_levels + 1, sizeof(*d->table), GFP_KERNEL);
	if (!d->table)
		return -ENOMEM;
	for (unsigned int i = 0; i < d->nr_levels; i++) {
		d->table[i].driver_data = i;
		d->table[i].frequency = d->levels[i].khz;
	}
	d->table[d->nr_levels].frequency = CPUFREQ_TABLE_END;

	if (!zalloc_cpumask_var(&d->mask, GFP_KERNEL))
		return -ENOMEM;
	if (cpulist_parse(d->cpus, d->mask))
		return -EINVAL;
	dev_info(dev, "%s (CPUs %s): %u kHz at %d uV, %u to %u kHz\n", d->name, d->cpus,
		 d->levels[cur].khz, uv, d->levels[d->nr_levels - 1].khz, d->levels[0].khz);
	return 0;
}

static int zrn_cpufreq_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct of_phandle_args args;
	u32 fuse0 = asv_fuse(ASV_FUSE0), fuse3 = asv_fuse(ASV_FUSE3);
	unsigned int n = 0;
	int ret;

	/* speed group 9 for both clusters, ASV table version 20 */
	if (fuse0 != 0x00090009 || (fuse3 & 0x7f) != 0x14)
		return dev_err_probe(dev, -ENODEV,
				     "ASV fuses %#x/%#x: no voltage table for this chip\n",
				     fuse0, fuse3);

	ret = of_parse_phandle_with_fixed_args(dev->of_node, "samsung,acpm-ipc", 1, 0, &args);
	if (ret)
		return dev_err_probe(dev, ret, "no samsung,acpm-ipc\n");
	acpm_chan = args.args[0];
	acpm = devm_acpm_get_by_node(dev, args.np);
	of_node_put(args.np);
	if (IS_ERR_OR_NULL(acpm))
		return dev_err_probe(dev, acpm ? PTR_ERR(acpm) : -EPROBE_DEFER, "no ACPM\n");

	/* reg: CMU_CPUCL1, CMU_CPUCL2 (the A53s), CMU_CPUCL0 (the A73s) */
	for (int i = 0; i < ARRAY_SIZE(domains); i++) {
		for (unsigned int c = 0; c < domains[i].nr_cmus; c++) {
			domains[i].cmu[c] = devm_platform_ioremap_resource(pdev, n++);
			if (IS_ERR(domains[i].cmu[c]))
				return PTR_ERR(domains[i].cmu[c]);
		}
	}
	for (int i = 0; i < ARRAY_SIZE(domains); i++) {
		ret = domain_setup(dev, &domains[i]);
		if (ret)
			return ret;
	}

	zrn_dev = dev;
	ret = cpufreq_register_driver(&zrn_cpufreq_driver);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register\n");
	return 0;
}

static void zrn_cpufreq_remove(struct platform_device *pdev)
{
	cpufreq_unregister_driver(&zrn_cpufreq_driver);
	for (int i = 0; i < ARRAY_SIZE(domains); i++)
		free_cpumask_var(domains[i].mask);
}

static const struct of_device_id zrn_cpufreq_of_match[] = {
	{ .compatible = "zrn,m20-cpufreq" },
	{ }
};
MODULE_DEVICE_TABLE(of, zrn_cpufreq_of_match);

static struct platform_driver zrn_cpufreq_platform_driver = {
	.driver = {
		.name = "zrn-cpufreq",
		.of_match_table = zrn_cpufreq_of_match,
	},
	.probe = zrn_cpufreq_probe,
	.remove = zrn_cpufreq_remove,
};
module_platform_driver(zrn_cpufreq_platform_driver);

MODULE_DESCRIPTION("Galaxy M20: CPU frequency scaling (Exynos 7904 clusters, S2MPU08 BUCK2/3)");
MODULE_LICENSE("GPL");
