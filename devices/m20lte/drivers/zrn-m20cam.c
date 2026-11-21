// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-m20cam: the Galaxy M20's rear camera (Samsung S5K3L6) as /dev/zrn-cam, RAW
 * with no ISP, in the protocol of the S9's camera/driver/zrn-cam-live.h, so
 * ztmetro/camera (ztCamera) runs unchanged:
 *
 *	write "rear"		power up and stream 1052x780 at 30 fps; "stop" ends it
 *	write "rearfull"	the same at 4128x3096, 30 fps, for a still
 *	write "af N"		focus, 0 (infinity) .. 511 (macro); the DW9808 takes N * 2
 *	write "exp C G"		exposure lines and analog gain code (0x20 = 1x)
 *	read			struct zrn_cam_frame, the newest finished frame
 *	mmap			frames of 16-bit Bayer (10 bits low), info.area bytes apart
 *	ioctl ZRN_CAM_DMABUF	the same memory as a DMA-BUF, for the GPU
 *
 * "front", "frontfull" and the aperture commands answer -ENODEV: the front camera
 * (S5K4HA) and the wide one (S5K5E9) are not done yet.
 *
 * Every step was first done from user space: devices/m20lte/camera/ (README.md).
 * Power: the S2MPB03 camera PMIC (0x56) on gpc2-0/1, the sensor (0x2d) on
 * gpc1-2/3 and the DW9808 (0x0c) on gpc1-6/7, all as i2c-gpio adapters made
 * here; reset gpg3-4, AF supply gpg1-5. MCLK2 26 MHz from CMU_TOP on gpc0-2.
 * Receiver CSIS1 (0x14410000, Samsung's v5.0 register layout, VERSION
 * 0x05010001), its PHY (PMU 0x680, SYSREG_CAM 0x840 bit 8), the CSIS-DMA common
 * block (0x14440000) and DMA channel 0 (on the 7885, DMA_N_DISABLE = 1 enables).
 * The camera SysMMU (0x14540000) is secure: never touched; DMA is not translated.
 *
 * A thread polls the receiver; no interrupt is requested. It stops by itself
 * when the last reader stops reading for LIVE_IDLE_S, after LIVE_MAX_S, at
 * LIVE_HOT_MC on cpu-thermal, on repeated receiver errors, and at unload.
 * Nothing loads this module at boot.
 */
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_data/i2c-gpio.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thermal.h>
#include <linux/uaccess.h>

#include "zrn-m20cam-tables.h"

/* --- the protocol, as zrn-cam-live.h --- */
#define ZRN_CAM_DMABUF	_IO('Z', 1)
struct zrn_cam_frame {
	__u32 seq, index, w, h, stride, bayer;	/* bayer: 0 GB/RG, 1 GR/BG (this sensor) */
	__u32 errors, stopped;
	__u32 area;
};
enum { LS_RUNNING, LS_ASKED, LS_IDLE, LS_TIME, LS_HOT, LS_ERRORS, LS_FAILED };

#define PV_W		1052
#define PV_H		780
#define PV_FLL		3264		/* frame length lines: 816 is 120 fps, this is 30 */
#define FULL_W		4128
#define FULL_H		3096
#define FULL_FLL	0x0cc1
#define STRIDE(w)	ALIGN((w) * 2, 64)	/* observed: 1052 pixels -> 2112 bytes */
#define LIVE_BUFS	4
#define LIVE_FULL_BUFS	2
#define LIVE_FULL_AREA	((size_t)STRIDE(FULL_W) * FULL_H)
#define LIVE_SIZE	(LIVE_FULL_BUFS * LIVE_FULL_AREA)	/* >= ztCamera's RAW_POOL */
#define LIVE_IDLE_S	3
#define LIVE_MAX_S	600
#define LIVE_HOT_MC	75000		/* cpu-thermal */
#define LIVE_ERR_MAX	30
#define LIVE_ERR_WINDOW_S 5
#define LIVE_ERR_LOGS	8
#define SETTLE		26		/* Samsung's for 1196 Mbps; 18-44 all clean here */
#define RAW10		0x2b

static bool live_tp;
module_param(live_tp, bool, 0644);	/* the sensor's colour bars instead of the room */

/* --- the hardware --- */
static void __iomem *top, *pmu, *sysreg, *csis, *dmac, *pin;
#define TOP_CAM_GATE	0x200c
#define TOP_CIS2_MUX	0x1010
#define TOP_CIS2_DIV	0x1818
#define TOP_CIS2_GATE	0x2018
#define PIN_GPC0_CON	0x1c0
#define PIN_GPC0_PUD	0x1c8
#define PMU_PHY_ISO	0x680
#define SYSREG_PHY	0x840
#define CSIS_VERSION	0x000
#define CSIS_CMN	0x004
#define CSIS_SRC0	0x014
#define CSIS_SRC1	0x01c
#define CSIS_CFG0	0x040
#define CSIS_RES0	0x044
#define CSIS_FRM_CNT0	0x100
#define CSIS_PHY_CMN	0x704
#define DMA0_CTRL	0x1000
#define DMA0_FMT	0x1004
#define DMA0_ADDR(n)	(0x1010 + 4 * (n))
#define DMA0_RESOL	0x1040
#define DMA_DATA_CTRL	0x1400
#define DMA_INT_SRC	0x1408
#define DMA_CMN_CTRL	0x1414
#define DMAC_ARB	0x0
#define DMAC_CTRL	0x8
#define DMAC_SPLIT	0xc
#define DMAC_MATRIX	0x10

static struct platform_device *ctl_pdev, *bus_pdev[3];
static struct device *ctl_dev;
static struct i2c_adapter *bus[3];		/* 0 PMIC, 1 sensor, 2 focus */
static struct gpio_desc *reset_gpio, *afen_gpio;
static DEFINE_MUTEX(lock);
static struct wakeup_source *stream_ws;

#define BUS_ID0	40	/* i2c-gpio adapters 40..42 */

/* (each table ends in a flexible array: three objects, not an array of them) */
static struct gpiod_lookup_table bus_gpios0 = { .dev_id = "i2c-gpio.40", .table = {
	GPIO_LOOKUP_IDX("gpc2", 0, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
	GPIO_LOOKUP_IDX("gpc2", 1, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN), { } } };
static struct gpiod_lookup_table bus_gpios1 = { .dev_id = "i2c-gpio.41", .table = {
	GPIO_LOOKUP_IDX("gpc1", 2, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
	GPIO_LOOKUP_IDX("gpc1", 3, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN), { } } };
static struct gpiod_lookup_table bus_gpios2 = { .dev_id = "i2c-gpio.42", .table = {
	GPIO_LOOKUP_IDX("gpc1", 6, NULL, 0, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN),
	GPIO_LOOKUP_IDX("gpc1", 7, NULL, 1, GPIO_ACTIVE_HIGH | GPIO_OPEN_DRAIN), { } } };
static struct gpiod_lookup_table *bus_gpios[3] = { &bus_gpios0, &bus_gpios1, &bus_gpios2 };
static struct gpiod_lookup_table ctl_gpios = {
	.dev_id = "zrn-m20cam", .table = {
		GPIO_LOOKUP("gpg3", 4, "reset", GPIO_ACTIVE_HIGH),
		GPIO_LOOKUP("gpg1", 5, "afen", GPIO_ACTIVE_HIGH), { } },
};
static const struct i2c_gpio_platform_data bus_pdata = { .udelay = 2, .timeout = HZ / 10 };

/* --- I2C --- */
static int pmic_write(u8 reg, u8 val)
{
	u8 b[2] = { reg, val };
	struct i2c_msg m = { .addr = 0x56, .len = 2, .buf = b };

	return i2c_transfer(bus[0], &m, 1) == 1 ? 0 : -EIO;
}

static int cis_write(u16 reg, u16 val, int size)
{
	u8 b[4] = { reg >> 8, reg & 0xff, size == 2 ? val >> 8 : val, val & 0xff };
	struct i2c_msg m = { .addr = 0x2d, .len = 2 + size, .buf = b };

	return i2c_transfer(bus[1], &m, 1) == 1 ? 0 : -EIO;
}
#define cis_write16(r, v)	cis_write(r, v, 2)
#define cis_write8(r, v)	cis_write(r, v, 1)

static int cis_read(u16 reg, u8 *val, int n)
{
	u8 a[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg m[2] = { { .addr = 0x2d, .len = 2, .buf = a },
				{ .addr = 0x2d, .flags = I2C_M_RD, .len = n, .buf = val } };

	return i2c_transfer(bus[1], m, 2) == 2 ? 0 : -EIO;
}

static int cis_table(const struct cis_reg *t, size_t n)
{
	size_t i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = cis_write(t[i].reg, t[i].val, t[i].size);
		if (ret)
			return ret;
	}
	return 0;
}

static int af_write(u8 reg, const u8 *v, int n)
{
	u8 b[3] = { reg };
	struct i2c_msg m = { .addr = 0x0c, .len = 1 + n, .buf = b };

	memcpy(b + 1, v, n);
	return i2c_transfer(bus[2], &m, 1) == 1 ? 0 : -EIO;
}

/* --- power, clock, PHY, receiver --- */
static void rmw(void __iomem *r, u32 clear, u32 set)
{
	writel((readl(r) & ~clear) | set, r);
}

static void mclk(bool on)
{
	if (on) {
		rmw(top + TOP_CIS2_MUX, 1, 0);		/* OSCCLK 26 MHz */
		rmw(top + TOP_CIS2_DIV, 0x1f, 0);	/* /1 */
		writel(0x300000, top + TOP_CIS2_GATE);	/* MANUAL | CG_VAL */
		rmw(pin + PIN_GPC0_PUD, 0xf << 8, 0);
		rmw(pin + PIN_GPC0_CON, 0xf << 8, 2 << 8);	/* gpc0-2: MCLK */
	} else {
		rmw(pin + PIN_GPC0_CON, 0xf << 8, 0);
		rmw(pin + PIN_GPC0_PUD, 0xf << 8, 1 << 8);
		writel(0x200000, top + TOP_CIS2_GATE);
	}
}

static void power_off(void)
{
	if (reset_gpio)
		gpiod_set_value_cansleep(reset_gpio, 0);
	usleep_range(5000, 6000);
	mclk(false);
	if (afen_gpio)
		gpiod_set_value_cansleep(afen_gpio, 0);
	pmic_write(0x05, 0x2c);				/* LDO3 VDDIO off */
	pmic_write(0x04, 0x1c);				/* LDO2 DVDD off */
	pmic_write(0x08, 0x28);				/* LDO6 AVDD off */
}

/* fimc-is-device-module-3l6.c's order */
static int power_on(void)
{
	int ret;

	gpiod_set_value_cansleep(reset_gpio, 0);
	ret = pmic_write(0x08, 0xa8);			/* LDO6 AVDD 2.8 V */
	usleep_range(1000, 1100);
	if (!ret)
		ret = pmic_write(0x04, 0x9c);		/* LDO2 DVDD 1.05 V */
	usleep_range(5000, 5500);
	if (!ret)
		ret = pmic_write(0x05, 0xac);		/* LDO3 VDDIO 1.8 V */
	usleep_range(5000, 5500);
	if (ret)
		return ret;
	gpiod_set_value_cansleep(afen_gpio, 1);
	usleep_range(12000, 13000);
	mclk(true);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(reset_gpio, 1);
	usleep_range(5000, 6000);
	return 0;
}

static void csis_on(u32 w, u32 h, dma_addr_t first)
{
	int i;

	writel(0x300000, top + TOP_CAM_GATE);		/* CAM bus clock held */
	rmw(pmu + PMU_PHY_ISO, 0, 1);			/* PHY isolation bypass */
	rmw(sysreg + SYSREG_PHY, BIT(8), 0);		/* PHY reset, released */
	rmw(sysreg + SYSREG_PHY, 0, BIT(8));
	rmw(csis + CSIS_CMN, 0, BIT(1));		/* software reset */
	for (i = 0; i < 100 && (readl(csis + CSIS_CMN) & BIT(1)); i++)
		udelay(10);
	/* settle, S_CLKSETTLECTL 1, S_BYTE_CLK_ENABLE (always on the 7885), 4 data lanes */
	rmw(csis + CSIS_PHY_CMN, 0xffff009f, (SETTLE << 24) | (1 << 16) | BIT(7) | (0xf << 1));
	rmw(csis + CSIS_CMN, (3 << 8) | (3 << 10) | BIT(12), 3 << 8);	/* 4 lanes, ch0 only */
	rmw(csis + CSIS_CFG0, 0x3fff, (RAW10 << 2) | BIT(12));
	writel((h << 16) | w, csis + CSIS_RES0);
	/* CSIS-DMA common, "DMA1 only" (csi_hw_s_dma_common_dynamic) */
	rmw(dmac + DMAC_ARB, 3, 2);
	rmw(dmac + DMAC_CTRL, 0, BIT(1));
	writel((320 << 16) | 4, dmac + DMAC_SPLIT);
	writel(0, dmac + DMAC_MATRIX);
	writel(0, csis + DMA0_FMT);			/* 2D, unpacked, legacy, RAW10 */
	writel(w, csis + DMA0_RESOL);
	rmw(csis + DMA_DATA_CTRL, 1, 1);
	for (i = 0; i < 8; i++)
		writel(lower_32_bits(first), csis + DMA0_ADDR(i));
	writel(0, csis + DMA_CMN_CTRL);
	writel(0xffffffff, csis + DMA_INT_SRC);
	writel(0xffffffff, csis + CSIS_SRC0);
	writel(0xffffffff, csis + CSIS_SRC1);
	writel(BIT(1) | BIT(0), csis + DMA0_CTRL);	/* UPDT_PTR_EN, enable (7885) */
	rmw(csis + CSIS_CMN, 0, 0xf << 16);		/* shadow update */
	rmw(csis + CSIS_PHY_CMN, 0, 1);			/* clock lane */
	rmw(csis + CSIS_CMN, 0, 1);			/* CSI on */
}

static void csis_off(void)
{
	writel(0, csis + DMA0_CTRL);
	rmw(csis + CSIS_PHY_CMN, 0x1f, 0);
	rmw(csis + CSIS_CMN, 1, 0);
	rmw(dmac + DMAC_CTRL, BIT(1), 0);
	rmw(sysreg + SYSREG_PHY, BIT(8), 0);
	rmw(pmu + PMU_PHY_ISO, 1, 0);
	writel(0x200000, top + TOP_CAM_GATE);
}

/* --- focus --- */
static bool af_up;
static int af_at;

static int af_position(int pos)
{
	u8 v[2] = { pos >> 8, pos & 0xff };

	return af_write(0x03, v, 2);
}

/* fimc-is-actuator-dw9808.c's init, without calibration data */
static int af_goto(int pos)
{
	u8 v;
	int ret = 0;

	if (!af_up) {
		v = 0x01; ret = af_write(0x02, &v, 1);
		v = 0x00; if (!ret) ret = af_write(0x02, &v, 1);
		usleep_range(5000, 5500);
		v = 0x02; if (!ret) ret = af_write(0x02, &v, 1);	/* ring control */
		v = 0x61; if (!ret) ret = af_write(0x06, &v, 1);	/* SAC3, Tvib x1 */
		v = 0x36; if (!ret) ret = af_write(0x07, &v, 1);
		v = 0xff; if (!ret) ret = af_write(0x0a, &v, 1);
		if (ret)
			return ret;
		af_up = true;
		af_at = 0;
	}
	ret = af_position(pos);
	if (!ret)
		af_at = pos;
	return ret;
}

/* land the lens in steps, not with a click */
static void af_down(void)
{
	if (!af_up)
		return;
	while (af_at > 80) {
		af_at -= 80;
		af_position(af_at);
		msleep(10);
	}
	af_position(0);
	msleep(30);
	af_up = false;
}

/* --- streaming --- */
static struct task_struct *live_task;
static DECLARE_COMPLETION(live_done);
static void *live_buf;
static dma_addr_t live_dma;
static struct zrn_cam_frame live_info;
static DEFINE_SPINLOCK(live_slock);
static unsigned long live_read_at;
static bool live_stop_asked;
static size_t live_area;
static unsigned int live_bufs, live_fll;

static void live_addr(unsigned int i)
{
	unsigned int n;

	for (n = 0; n < 8; n++)
		writel(lower_32_bits(live_dma + i * live_area), csis + DMA0_ADDR(n));
}

static int cpu_mc(void)
{
	static struct thermal_zone_device *tz;
	int t = 0;

	if (!tz) {
		tz = thermal_zone_get_zone_by_name("cpu-thermal");
		if (IS_ERR(tz)) {
			tz = NULL;
			return 0;
		}
	}
	return thermal_zone_get_temp(tz, &t) ? 0 : t;
}

static int live_thread(void *d)
{
	unsigned long start = jiffies, last_temp = jiffies, err_from = jiffies;
	unsigned int cur = 0, why = LS_RUNNING, errs = 0, recent = 0, logged = 0;

	while (why == LS_RUNNING) {
		u32 s0 = readl(csis + CSIS_SRC0), s1 = readl(csis + CSIS_SRC1);
		u32 dm = readl(csis + DMA_INT_SRC);

		if (s0)
			writel(s0, csis + CSIS_SRC0);
		if (s1)
			writel(s1, csis + CSIS_SRC1);
		if (dm)
			writel(dm, csis + DMA_INT_SRC);
		/* SRC0: ID, CRC, ECC, wrong config, overflow, lost FE/FS, SOT; SRC1: the
		 * PHY's code errors; DMA: error and OTF overlap */
		if ((s0 & 0x000fff1f) || (s1 & 0xf0) || (dm & BIT(12)) || (dm & 0x00f00000)) {
			errs++;
			recent++;
			if (logged++ < LIVE_ERR_LOGS)
				pr_info("zrn-m20cam: error %u at frame %u: src0 %08x src1 %08x dma %08x\n",
					errs, live_info.seq, s0, s1, dm);
		}
		if (time_after(jiffies, err_from + LIVE_ERR_WINDOW_S * HZ)) {
			err_from = jiffies;
			recent = 0;
		}
		if (dm & BIT(8)) {	/* a frame finished: publish it, the next goes elsewhere */
			spin_lock(&live_slock);
			live_info.seq++;
			live_info.index = cur;
			live_info.errors = errs;
			spin_unlock(&live_slock);
			cur = (cur + 1) % live_bufs;
			live_addr(cur);
		}
		if (READ_ONCE(live_stop_asked))
			why = LS_ASKED;
		else if (time_after(jiffies, READ_ONCE(live_read_at) + LIVE_IDLE_S * HZ))
			why = LS_IDLE;
		else if (time_after(jiffies, start + LIVE_MAX_S * HZ))
			why = LS_TIME;
		else if (recent > LIVE_ERR_MAX)
			why = LS_ERRORS;
		else if (time_after(jiffies, last_temp + 2 * HZ)) {
			last_temp = jiffies;
			if (cpu_mc() >= LIVE_HOT_MC)
				why = LS_HOT;
		}
		usleep_range(1000, 1500);
	}
	mutex_lock(&lock);
	writel(0, csis + DMA0_CTRL);
	cis_write16(0x0100, 0x0000);
	msleep(50);
	csis_off();
	af_down();
	power_off();
	__pm_relax(stream_ws);
	pr_info("zrn-m20cam: stopped (%u): %u frames, %u errors\n", why, live_info.seq, errs);
	spin_lock(&live_slock);
	live_info.stopped = why;
	spin_unlock(&live_slock);
	live_task = NULL;
	mutex_unlock(&lock);
	complete_all(&live_done);
	return 0;
}

static int live_start(bool full)
{
	u32 w = full ? FULL_W : PV_W, h = full ? FULL_H : PV_H;
	struct task_struct *t;
	u8 id[2];
	int ret;

	mutex_lock(&lock);
	if (live_task) {
		ret = -EBUSY;
		goto out;
	}
	if (readl(csis + CSIS_VERSION) != 0x05010001) {
		ret = -ENODEV;
		goto out;
	}
	live_area = (size_t)STRIDE(w) * h;
	live_bufs = full ? LIVE_FULL_BUFS : LIVE_BUFS;
	live_fll = full ? FULL_FLL : PV_FLL;
	ret = power_on();
	if (ret)
		goto off;
	ret = cis_read(0x0000, id, 2);
	if (!ret && (id[0] << 8 | id[1]) != 0x30c6)
		ret = -ENODEV;
	if (!ret)
		ret = cis_table(tb_3l6_global, ARRAY_SIZE(tb_3l6_global));
	if (!ret)
		ret = full ? cis_table(tb_3l6_4128x3096, ARRAY_SIZE(tb_3l6_4128x3096))
			   : cis_table(tb_3l6_1052x780, ARRAY_SIZE(tb_3l6_1052x780));
	if (!ret)
		ret = cis_write16(0x0340, live_fll);
	if (!ret)
		ret = cis_write16(0x0202, 0x0320);		/* a start for AE: 800 lines */
	if (!ret)
		ret = cis_write16(0x0204, 0x0080);		/* 4x */
	if (!ret && live_tp)
		ret = cis_write16(0x0600, 0x0002);		/* colour bars */
	if (ret)
		goto off;
	__pm_stay_awake(stream_ws);
	csis_on(w, h, live_dma);
	/* sensor_3l6_cis_stream_on */
	cis_write16(0x3892, 0x3600);
	cis_write16(0x3c1e, 0x0100);
	usleep_range(3000, 3100);
	ret = cis_write16(0x0100, 0x0100);
	usleep_range(3000, 3100);
	cis_write16(0x3c1e, 0x0000);
	if (ret) {
		csis_off();
		__pm_relax(stream_ws);
		goto off;
	}
	memset(&live_info, 0, sizeof(live_info));
	live_info.w = w;
	live_info.h = h;
	live_info.stride = STRIDE(w);
	live_info.bayer = 1;		/* GR/BG */
	live_info.area = live_area;
	live_stop_asked = false;
	live_read_at = jiffies;
	reinit_completion(&live_done);
	t = kthread_create(live_thread, NULL, "zrn-m20cam");
	if (IS_ERR(t)) {
		ret = PTR_ERR(t);
		cis_write16(0x0100, 0x0000);
		csis_off();
		__pm_relax(stream_ws);
		goto off;
	}
	live_task = t;
	wake_up_process(t);
	pr_info("zrn-m20cam: streaming %ux%u\n", w, h);
	goto out;
off:
	af_down();
	power_off();
	live_info.stopped = LS_FAILED;
	pr_info("zrn-m20cam: start failed: %d\n", ret);
out:
	mutex_unlock(&lock);
	return ret;
}

static void live_stop(void)
{
	WRITE_ONCE(live_stop_asked, true);
	if (READ_ONCE(live_task))
		wait_for_completion_timeout(&live_done, 3 * HZ);
}

static ssize_t live_write(struct file *f, const char __user *ubuf, size_t len, loff_t *pos)
{
	char cmd[32];
	size_t n = min(len, sizeof(cmd) - 1);
	u32 c, g;
	int ret = 0;

	if (copy_from_user(cmd, ubuf, n))
		return -EFAULT;
	cmd[n] = 0;
	strim(cmd);
	if (!strcmp(cmd, "rear") || !strcmp(cmd, "rearfull")) {
		live_stop();
		ret = live_start(strstr(cmd, "full") != NULL);
	} else if (!strcmp(cmd, "front") || !strcmp(cmd, "frontfull") ||
		   !strcmp(cmd, "ap15") || !strcmp(cmd, "ap24")) {
		ret = -ENODEV;
	} else if (sscanf(cmd, "af %u", &c) == 1) {
		mutex_lock(&lock);
		ret = live_task && c <= 511 ? af_goto(c * 2) : -EINVAL;
		mutex_unlock(&lock);
	} else if (!strcmp(cmd, "stop")) {
		live_stop();
	} else if (sscanf(cmd, "exp %u %u", &c, &g) == 2) {
		mutex_lock(&lock);
		if (live_task && c && g >= 0x20 && g <= 0x200) {
			c = min(c, live_fll - 8);
			cis_write8(0x0104, 1);		/* grouped: both from one frame */
			cis_write16(0x0202, c);
			cis_write16(0x0204, g);
			cis_write8(0x0104, 0);
		} else {
			ret = -EINVAL;
		}
		mutex_unlock(&lock);
	} else {
		ret = -EINVAL;
	}
	return ret ? ret : len;
}

static ssize_t live_read(struct file *f, char __user *ubuf, size_t len, loff_t *pos)
{
	struct zrn_cam_frame i;

	if (len < sizeof(i))
		return -EINVAL;
	WRITE_ONCE(live_read_at, jiffies);
	spin_lock(&live_slock);
	i = live_info;
	spin_unlock(&live_slock);
	return copy_to_user(ubuf, &i, sizeof(i)) ? -EFAULT : sizeof(i);
}

static int live_mmap(struct file *f, struct vm_area_struct *vma)
{
	if (vma->vm_pgoff || vma->vm_end - vma->vm_start > PAGE_ALIGN(LIVE_SIZE))
		return -EINVAL;
	return dma_mmap_coherent(ctl_dev, vma, live_buf, live_dma, vma->vm_end - vma->vm_start);
}

static int live_release(struct inode *i, struct file *f)
{
	live_stop();
	return 0;
}

static struct sg_table *zc_map(struct dma_buf_attachment *at, enum dma_data_direction dir)
{
	struct sg_table *sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	int ret;

	if (!sgt)
		return ERR_PTR(-ENOMEM);
	ret = dma_get_sgtable(ctl_dev, sgt, live_buf, live_dma, LIVE_SIZE);
	if (!ret)
		ret = dma_map_sgtable(at->dev, sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
	if (ret) {
		sg_free_table(sgt);
		kfree(sgt);
		return ERR_PTR(ret);
	}
	return sgt;
}

static void zc_unmap(struct dma_buf_attachment *at, struct sg_table *sgt, enum dma_data_direction dir)
{
	dma_unmap_sgtable(at->dev, sgt, dir, DMA_ATTR_SKIP_CPU_SYNC);
	sg_free_table(sgt);
	kfree(sgt);
}

static void zc_release(struct dma_buf *b)
{
}

static int zc_mmap(struct dma_buf *b, struct vm_area_struct *vma)
{
	return dma_mmap_coherent(ctl_dev, vma, live_buf, live_dma, vma->vm_end - vma->vm_start);
}

static const struct dma_buf_ops zc_ops = {
	.map_dma_buf = zc_map, .unmap_dma_buf = zc_unmap, .release = zc_release, .mmap = zc_mmap,
};

static long live_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	DEFINE_DMA_BUF_EXPORT_INFO(info);
	struct dma_buf *b;
	int fd;

	if (cmd != ZRN_CAM_DMABUF)
		return -ENOTTY;
	info.ops = &zc_ops;
	info.priv = live_buf;
	info.size = LIVE_SIZE;
	info.flags = O_RDWR | O_CLOEXEC;
	b = dma_buf_export(&info);
	if (IS_ERR(b))
		return PTR_ERR(b);
	fd = dma_buf_fd(b, O_CLOEXEC);
	if (fd < 0)
		dma_buf_put(b);
	return fd;
}

static const struct file_operations live_fops = {
	.owner = THIS_MODULE, .write = live_write, .read = live_read,
	.mmap = live_mmap, .release = live_release, .unlocked_ioctl = live_ioctl,
};
static struct miscdevice live_misc = {
	.minor = MISC_DYNAMIC_MINOR, .name = "zrn-cam", .fops = &live_fops, .mode = 0660,
};

/* --- module --- */
static void unmap_all(void)
{
	void __iomem **m[] = { &top, &pmu, &sysreg, &csis, &dmac, &pin };
	int i;

	for (i = 0; i < ARRAY_SIZE(m); i++)
		if (*m[i]) {
			iounmap(*m[i]);
			*m[i] = NULL;
		}
}

static void buses_down(void)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (bus[i]) {
			i2c_put_adapter(bus[i]);
			bus[i] = NULL;
		}
		if (bus_pdev[i]) {
			platform_device_unregister(bus_pdev[i]);
			bus_pdev[i] = NULL;
		}
		gpiod_remove_lookup_table(bus_gpios[i]);
	}
}

static int __init m20cam_init(void)
{
	int i, ret;

	top = ioremap(0x12060000, 0x3000);
	pmu = ioremap(0x11c80000, 0x1000);
	sysreg = ioremap(0x14510000, 0x1000);
	csis = ioremap(0x14410000, 0x2000);
	dmac = ioremap(0x14440000, 0x1000);
	pin = ioremap(0x139b0000, 0x1000);
	if (!top || !pmu || !sysreg || !csis || !dmac || !pin) {
		ret = -ENOMEM;
		goto err_map;
	}
	/* the camera power domain is the bootloader's and stays on; check before
	 * reading anything inside it (PMU CAM_STATUS 0x4084, outside this map) */
	ctl_pdev = platform_device_register_simple("zrn-m20cam", -1, NULL, 0);
	if (IS_ERR(ctl_pdev)) {
		ret = PTR_ERR(ctl_pdev);
		ctl_pdev = NULL;
		goto err_map;
	}
	ctl_dev = &ctl_pdev->dev;
	ret = dma_coerce_mask_and_coherent(ctl_dev, DMA_BIT_MASK(32));
	if (ret)
		goto err_dev;
	live_buf = dma_alloc_coherent(ctl_dev, LIVE_SIZE, &live_dma, GFP_KERNEL);
	if (!live_buf) {
		ret = -ENOMEM;
		goto err_dev;
	}
	gpiod_add_lookup_table(&ctl_gpios);
	reset_gpio = gpiod_get(ctl_dev, "reset", GPIOD_OUT_LOW);
	afen_gpio = IS_ERR(reset_gpio) ? NULL : gpiod_get(ctl_dev, "afen", GPIOD_OUT_LOW);
	if (IS_ERR_OR_NULL(reset_gpio) || IS_ERR_OR_NULL(afen_gpio)) {
		ret = IS_ERR(reset_gpio) ? PTR_ERR(reset_gpio) : afen_gpio ? PTR_ERR(afen_gpio) : -ENODEV;
		reset_gpio = afen_gpio = NULL;
		goto err_gpio;
	}
	request_module("i2c-gpio");
	for (i = 0; i < 3; i++) {
		gpiod_add_lookup_table(bus_gpios[i]);
		bus_pdev[i] = platform_device_register_data(NULL, "i2c-gpio", BUS_ID0 + i,
							    &bus_pdata, sizeof(bus_pdata));
		if (IS_ERR(bus_pdev[i])) {
			ret = PTR_ERR(bus_pdev[i]);
			bus_pdev[i] = NULL;
			goto err_bus;
		}
		bus[i] = i2c_get_adapter(BUS_ID0 + i);
		if (!bus[i]) {
			pr_err("zrn-m20cam: no i2c-gpio adapter %d (is i2c-gpio there?)\n", BUS_ID0 + i);
			ret = -ENODEV;
			goto err_bus;
		}
	}
	stream_ws = wakeup_source_register(NULL, "zrn-m20cam");
	ret = misc_register(&live_misc);
	if (ret)
		goto err_ws;
	pr_info("zrn-m20cam: rear camera on /dev/zrn-cam, %zu MiB of frames at %pad, CSIS1 %08x\n",
		(size_t)LIVE_SIZE >> 20, &live_dma, readl(csis + CSIS_VERSION));
	return 0;

err_ws:
	wakeup_source_unregister(stream_ws);
err_bus:
	buses_down();
err_gpio:
	if (reset_gpio)
		gpiod_put(reset_gpio);
	if (afen_gpio)
		gpiod_put(afen_gpio);
	gpiod_remove_lookup_table(&ctl_gpios);
	dma_free_coherent(ctl_dev, LIVE_SIZE, live_buf, live_dma);
err_dev:
	platform_device_unregister(ctl_pdev);
err_map:
	unmap_all();
	return ret;
}

static void __exit m20cam_exit(void)
{
	misc_deregister(&live_misc);
	live_stop();
	wakeup_source_unregister(stream_ws);
	buses_down();
	gpiod_put(reset_gpio);
	gpiod_put(afen_gpio);
	gpiod_remove_lookup_table(&ctl_gpios);
	dma_free_coherent(ctl_dev, LIVE_SIZE, live_buf, live_dma);
	platform_device_unregister(ctl_pdev);
	unmap_all();
}

module_init(m20cam_init);
module_exit(m20cam_exit);
MODULE_SOFTDEP("pre: i2c-gpio");
MODULE_DESCRIPTION("Galaxy M20 rear camera (S5K3L6) as /dev/zrn-cam, RAW through CSIS1");
MODULE_LICENSE("GPL");
