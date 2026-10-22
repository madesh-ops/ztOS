#!/usr/bin/env python3
"""The Galaxy M20's changes to a mainline kernel tree.

  patch-kernel.py <linux source tree>

Three drivers that mainline already has learn the M20's variant of their
chip. Each change is an insertion next to an anchor that is asserted, and the
whole script can run again on a patched tree (it says "already" and moves on).

  phy-exynos5-usbdrd   "samsung,exynos7885-usbdrd-phy": the Exynos 850's PHY
                       code (the same "usb3p1" register layout, see
                       phy-exynos-usb3p1-reg.h in Samsung's M20 kernel) with
                       the PHY's PMU isolation register at 0x674 instead of
                       0x704 (exynos7885.dtsi: pmu_offset = <0x674>).
  himax_hx83112b       "himax,hx83112a": the M20's in-cell touch controller,
                       product id 0x83112a. Read with the HX83112B's event
                       format; whether the A's matches is the first thing to
                       look at if touches come out wrong.
  s3c2410_wdt          "samsung,exynos7885-wdt": the Exynos 7 watchdog with
                       the A53 cluster's bit (24) in the PMU registers
                       (drv_data_exynos8 in Samsung's M20 kernel).
"""
import sys
from pathlib import Path

tree = Path(sys.argv[1])


def read(path):
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def insert(rel, marker, anchor, text, after=True):
    path = tree / rel
    s = read(path)
    if marker in s:
        print(f"patch-kernel: {rel}: already")
        return
    if s.count(anchor) != 1:
        sys.exit(f"patch-kernel: {rel}: expected the anchor once, found {s.count(anchor)}:\n{anchor}")
    s = s.replace(anchor, anchor + text if after else text + anchor)
    write(path, s)
    print(f"patch-kernel: {rel}: done")


# --- USB PHY -------------------------------------------------------------------
PHY = "drivers/phy/samsung/phy-exynos5-usbdrd.c"
insert(PHY, "EXYNOS7885_USBDRD_PHY_CONTROL",
       "static const struct exynos5_usbdrd_phy_drvdata exynos850_usbdrd_phy = {",
       """/* Exynos 7885/7904 (Galaxy M20): the 850's PHY, isolated at PMU 0x674 */
#define EXYNOS7885_USBDRD_PHY_CONTROL	0x674

/*
 * The 7885's PHY controller registers sit behind CMU_FSYS gates of their own:
 * with only "phy" on, every register reads 0 and writes are lost (seen on the
 * M20). Samsung's driver turns on its 2.0 and both 3.0 PHY-controller clocks.
 */
static const char * const exynos7885_clk_names[] = {
	"phy", "phy30_0", "phy30_1",
};

""", after=False)
# The 7885's init: Samsung's order (phy_exynos_usbdrd3.c utmi_init) is
# isolation off, every clock on, then the PHY reset. Mainline's 850 init
# resets the PHY before power_on turns on its reference clock, which on the
# 850 is the always-running oscillator; on the 7885 it is PLL_USB, off until
# then, and the PHY's own PLL never starts (seen on the M20: no pull-up at all).
insert(PHY, "exynos7885_usbdrd_phy_ops",
       "static void exynos5_usbdrd_gs101_pipe3_init(struct exynos5_usbdrd_phy *phy_drd)\n",
       """static int exynos7885_usbdrd_phy_init(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos5_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);
	int ret;

	/* the reference clock (PLL_USB) stays on until exit */
	ret = clk_bulk_prepare_enable(phy_drd->drv_data->n_core_clks,
				      phy_drd->core_clks);
	if (ret)
		return ret;
	inst->phy_cfg->phy_isol(inst, false);

	ret = exynos850_usbdrd_phy_init(phy);
	if (ret)
		clk_bulk_disable_unprepare(phy_drd->drv_data->n_core_clks,
					   phy_drd->core_clks);
	return ret;
}

static int exynos7885_usbdrd_phy_exit(struct phy *phy)
{
	struct phy_usb_instance *inst = phy_get_drvdata(phy);
	struct exynos5_usbdrd_phy *phy_drd = to_usbdrd_phy(inst);
	int ret;

	ret = exynos850_usbdrd_phy_exit(phy);
	clk_bulk_disable_unprepare(phy_drd->drv_data->n_core_clks,
				   phy_drd->core_clks);
	return ret;
}

static const struct phy_ops exynos7885_usbdrd_phy_ops = {
	.init		= exynos7885_usbdrd_phy_init,
	.exit		= exynos7885_usbdrd_phy_exit,
	.power_on	= exynos5_usbdrd_phy_power_on,
	.power_off	= exynos5_usbdrd_phy_power_off,
	.owner		= THIS_MODULE,
};

""", after=False)
# The 7885's data: a copy of the 850's with its own PMU offset, clocks and ops.
path = tree / PHY
s = read(path)
if "exynos7885_usbdrd_phy = {" in s:
    print(f"patch-kernel: {PHY}: exynos7885 data: already")
else:
    start = s.index("static const struct exynos5_usbdrd_phy_drvdata exynos850_usbdrd_phy = {")
    end = s.index("\n};\n", start) + len("\n};\n")
    body = s[start:end]
    if body.count(".pmu_offset_usbdrd0_phy\t= EXYNOS5_USBDRD_PHY_CONTROL,") != 1:
        sys.exit("patch-kernel: exynos850_usbdrd_phy no longer sets pmu_offset_usbdrd0_phy as expected")
    if body.count("exynos5_clk_names") != 2:
        sys.exit("patch-kernel: exynos850_usbdrd_phy no longer uses exynos5_clk_names as expected")
    copy = (body.replace("exynos850_usbdrd_phy = {", "exynos7885_usbdrd_phy = {")
                .replace(".pmu_offset_usbdrd0_phy\t= EXYNOS5_USBDRD_PHY_CONTROL,",
                         ".pmu_offset_usbdrd0_phy\t= EXYNOS7885_USBDRD_PHY_CONTROL,")
                .replace("exynos5_clk_names", "exynos7885_clk_names")
                .replace("&exynos850_usbdrd_phy_ops", "&exynos7885_usbdrd_phy_ops"))
    write(path, s[:end] + "\n" + copy + s[end:])
    print(f"patch-kernel: {PHY}: exynos7885 data: done")
# The M20's high-speed PHY tuning, as Samsung's kernel sets it for device mode
# (usb_hs_tune in exynos7904-m20lte_common.dtsi, written to HSP_TUNE 0x58 by
# phy_exynos_usb_v3p1_tune). The reset values leave the host unable to read
# the phone's descriptors (seen on the first boot).
insert(PHY, "exynos7885_tunes_utmi_postinit",
       "static const struct exynos5_usbdrd_phy_drvdata exynos850_usbdrd_phy = {",
       """static const struct exynos5_usbdrd_phy_tuning exynos7885_tunes_utmi_postinit[] = {
	PHY_TUNING_ENTRY_PHY(EXYNOS850_DRD_HSPPARACON,
			     (HSPPARACON_TXVREF | HSPPARACON_TXRISE |
			      HSPPARACON_TXRES | HSPPARACON_TXPREEMPPULSE |
			      HSPPARACON_TXPREEMPAMP | HSPPARACON_TXHSXV |
			      HSPPARACON_TXFSLS | HSPPARACON_SQRX |
			      HSPPARACON_OTG | HSPPARACON_COMPDIS),
			     (FIELD_PREP_CONST(HSPPARACON_TXVREF, 0xd) |
			      FIELD_PREP_CONST(HSPPARACON_TXRISE, 3) |
			      FIELD_PREP_CONST(HSPPARACON_TXRES, 3) |
			      FIELD_PREP_CONST(HSPPARACON_TXPREEMPPULSE, 0) |
			      FIELD_PREP_CONST(HSPPARACON_TXPREEMPAMP, 3) |
			      FIELD_PREP_CONST(HSPPARACON_TXHSXV, 3) |
			      FIELD_PREP_CONST(HSPPARACON_TXFSLS, 3) |
			      FIELD_PREP_CONST(HSPPARACON_SQRX, 7) |
			      FIELD_PREP_CONST(HSPPARACON_OTG, 2) |
			      FIELD_PREP_CONST(HSPPARACON_COMPDIS, 7))),
	/*
	 * phy_exynos_usb_v3p1_pipe_ovrd, which Samsung's driver runs right
	 * after the UTMI init: the 7885's DWC3 is a USB 3 controller on a
	 * USB 2-only port, so its PIPE3 side is forced idle and the SS PMA
	 * powered down. Without it the link waits on a PHY that is not there.
	 */
	PHY_TUNING_ENTRY_PHY(EXYNOS850_DRD_LINKCTRL,
			     (LINKCTRL_FORCE_PIPE_EN | LINKCTRL_FORCE_PHYSTATUS |
			      LINKCTRL_FORCE_RXELECIDLE),
			     (LINKCTRL_FORCE_PIPE_EN | LINKCTRL_FORCE_RXELECIDLE)),
	PHY_TUNING_ENTRY_PHY(EXYNOS850_DRD_SECPMACTL,
			     SECPMACTL_PMA_LOW_PWR, SECPMACTL_PMA_LOW_PWR),
	PHY_TUNING_ENTRY_LAST
};

static const struct exynos5_usbdrd_phy_tuning *exynos7885_tunes[PTS_MAX] = {
	[PTS_UTMI_POSTINIT] = exynos7885_tunes_utmi_postinit,
};

""", after=False)
insert(PHY, ".phy_tunes\t\t= exynos7885_tunes,",
       ".pmu_offset_usbdrd0_phy\t= EXYNOS7885_USBDRD_PHY_CONTROL,\n",
       "\t.phy_tunes\t\t= exynos7885_tunes,\n")
insert(PHY, '"samsung,exynos7885-usbdrd-phy"',
       """		.compatible = "samsung,exynos7870-usbdrd-phy",
		.data = &exynos7870_usbdrd_phy
	}, {""",
       """
		.compatible = "samsung,exynos7885-usbdrd-phy",
		.data = &exynos7885_usbdrd_phy
	}, {""")

# --- touch ---------------------------------------------------------------------
TS = "drivers/input/touchscreen/himax_hx83112b.c"
insert(TS, "hx83112a_chip",
       "static const struct himax_chip hx83112b_chip = {",
       """static const struct himax_chip hx83112a_chip = {
	.id = 0x83112a,
	.sense_on = true,
	.check_id = himax_check_product_id,
	.read_events = himax_read_events,
};

""", after=False)
# "Sense on", as Samsung's driver does it for the M20's HX83112A
# (himax_mcu_sense_on, then himax_mcu_pin_reset). The bootloader leaves the
# touch firmware stopped (central state 0x0c, also under Samsung's own kernel)
# with its stop request in the FW control register; a reset alone brings it
# back up stopped, and it never raises an interrupt (seen on the M20). Clearing
# that register first, then the reset, then 50 ms for the firmware to reload.
insert(TS, "\tbool sense_on;",
       "struct himax_chip {\n\tu32 id;\n",
       "\tbool sense_on;\t\t/* clear the FW stop request before reset */\n")
insert(TS, "HIMAX_REG_ADDR_FW_CTRL",
       "static void himax_reset(struct himax_ts_data *ts)\n",
       """#define HIMAX_REG_ADDR_FW_CTRL			0x9000005c

static int himax_sense_on(struct himax_ts_data *ts)
{
	u32 cmd[2] = { HIMAX_REG_ADDR_FW_CTRL, 0 };
	int error;

	error = himax_bus_enable_burst(ts);
	if (error)
		return error;

	return regmap_bulk_write(ts->regmap, HIMAX_AHB_ADDR_BYTE_0, cmd,
				 ARRAY_SIZE(cmd));
}

""", after=False)
# The reset line is requested released, not asserted, so that the write above
# reaches a running chip; himax_reset() pulses it right after either way.
path = tree / TS
s = read(path)
old_rst = 'ts->gpiod_rst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);'
if 'devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW)' in s:
    print(f"patch-kernel: {TS}: reset requested released: already")
elif s.count(old_rst) == 1:
    write(path, s.replace(old_rst, 'ts->gpiod_rst = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);'))
    print(f"patch-kernel: {TS}: reset requested released: done")
else:
    sys.exit(f"patch-kernel: {TS}: the reset GPIO request changed")
s = read(path)
probe_reset = "\thimax_reset(ts);\n\n\tif (ts->chip->check_id) {"
if "error = himax_sense_on(ts);" in s:
    print(f"patch-kernel: {TS}: sense on in probe: already")
elif s.count(probe_reset) == 1:
    write(path, s.replace(probe_reset, """	if (ts->chip->sense_on) {
		error = himax_sense_on(ts);
		if (error)
			dev_warn(dev, "Failed to clear the FW stop request: %d\\n",
				 error);
		msleep(20);
	}

	himax_reset(ts);

	if (ts->chip->sense_on)
		msleep(50);	/* the firmware reloads from flash */

	if (ts->chip->check_id) {"""))
    print(f"patch-kernel: {TS}: sense on in probe: done")
else:
    sys.exit(f"patch-kernel: {TS}: the probe's reset changed")
# Even after sense on and a reset, the firmware sits waiting until the host
# writes the high-sensitivity (glove) flag; Samsung's driver does that on every
# resume (himax_mcu_set_HSEN_enable(0)). Without it the chip never scans: no
# interrupts, and the event stack keeps its power-up contents (seen on the M20,
# and under Samsung's kernel with its driver unbound after a screen off/on).
insert(TS, "HIMAX_REG_ADDR_HSEN",
       "static void himax_reset(struct himax_ts_data *ts)\n",
       """#define HIMAX_REG_ADDR_HSEN			0x10007f14

static int himax_hsen_off(struct himax_ts_data *ts)
{
	u32 cmd[2] = { HIMAX_REG_ADDR_HSEN, 0 };
	int error;

	error = himax_bus_enable_burst(ts);
	if (error)
		return error;

	return regmap_bulk_write(ts->regmap, HIMAX_AHB_ADDR_BYTE_0, cmd,
				 ARRAY_SIZE(cmd));
}

""", after=False)
s = read(path)
probe_input = "\terror = himax_input_register(ts);\n"
if "error = himax_hsen_off(ts);" in s:
    print(f"patch-kernel: {TS}: HSEN off in probe: already")
elif s.count(probe_input) == 1:
    write(path, s.replace(probe_input, """	if (ts->chip->sense_on) {
		error = himax_hsen_off(ts);
		if (error)
			dev_warn(dev, "Failed to start touch scanning: %d\\n", error);
	}

""" + probe_input))
    print(f"patch-kernel: {TS}: HSEN off in probe: done")
else:
    sys.exit(f"patch-kernel: {TS}: the probe's input registration changed")
# The HX83112A's firmware leaves unused finger slots at 0x0000, not the
# 0xffff the driver expects. The driver walks the slots counting down the
# finger count, so once the first of two fingers lifts its empty slot read as
# a touch at (0, 0) and the real finger after it was dropped: a phantom touch
# in the corner and a jumping pinch zoom (seen in Chromium on the M20). No
# finger lands exactly on the corner pixel, so (0, 0) means an empty slot.
path = tree / TS
s = read(path)
old_inv = "\tif (x == HIMAX_INVALID_COORD || y == HIMAX_INVALID_COORD)\n"
new_inv = "\tif (x == HIMAX_INVALID_COORD || y == HIMAX_INVALID_COORD || (!x && !y))\n"
if new_inv in s:
    print(f"patch-kernel: {TS}: empty slots at (0, 0): already")
elif s.count(old_inv) == 1:
    write(path, s.replace(old_inv, new_inv))
    print(f"patch-kernel: {TS}: empty slots at (0, 0): done")
else:
    sys.exit(f"patch-kernel: {TS}: the invalid-point check changed")
insert(TS, '"hx83112a", .driver_data',
       "static const struct i2c_device_id himax_ts_id[] = {\n",
       '\t{ .name = "hx83112a", .driver_data = (kernel_ulong_t)&hx83112a_chip },\n')
insert(TS, '"himax,hx83112a"',
       "static const struct of_device_id himax_of_match[] = {\n",
       '\t{ .compatible = "himax,hx83112a", .data = &hx83112a_chip },\n')

# --- simpledrm scanout of GPU-rendered buffers ---------------------------------
# On the M20 the screen is simpledrm's, the GPU is Panfrost's: wlroots
# allocates simpledrm dumb buffers, Panfrost renders into them, and simpledrm
# copies each damaged rectangle to the real framebuffer with the CPU. The
# Mali is not cache coherent and the dumb buffers are cached in the kernel's
# mapping, so the copy read stale lines from earlier frames (dashed streaks
# across moving tiles, seen on the M20). drm_gem_fb_begin_cpu_access() only
# syncs imported buffers; these are simpledrm's own, so clean and invalidate
# the rows each rectangle reads before copying them. Clean as well as
# invalidate, so a CPU renderer's unwritten lines are kept.
SYSFB = "drivers/gpu/drm/sysfb/drm_sysfb_modeset.c"
insert(SYSFB, "drm_sysfb_sync_rows_for_cpu",
       "void drm_sysfb_plane_helper_atomic_update(struct drm_plane *plane, struct drm_atomic_state *state)\n",
       """#include <drm/drm_gem.h>

#ifdef CONFIG_ARM64
#include <asm/cacheflush.h>

static void drm_sysfb_sync_rows_for_cpu(const struct iosys_map *src,
					const struct drm_framebuffer *fb,
					const struct drm_rect *clip)
{
	unsigned long start, end;

	if (src->is_iomem || !src->vaddr || drm_gem_is_imported(fb->obj[0]))
		return;
	start = (unsigned long)src->vaddr + fb->offsets[0] +
		(unsigned long)clip->y1 * fb->pitches[0];
	end = (unsigned long)src->vaddr + fb->offsets[0] +
	      (unsigned long)clip->y2 * fb->pitches[0];
	dcache_clean_inval_poc(start, end);
}
#else
static void drm_sysfb_sync_rows_for_cpu(const struct iosys_map *src,
					const struct drm_framebuffer *fb,
					const struct drm_rect *clip)
{
}
#endif

/*
 * The scanout runs on its own from the framebuffer the copy below writes
 * into, so a copy that the panel's scan crosses shows half of the old frame
 * and half of the new one: a tear. Where the device tree names the display
 * controller's line counter ("zerone,scanout-line" = <register mask lines
 * line-ns>: the M20's DSIM LINK0), wait until the scan is just above the
 * topmost changed row and copy from there: the copy is faster than the scan,
 * so the panel only ever reads rows already written, all of one frame.
 */
static struct {
	void __iomem *reg;
	u32 mask, lines, line_ns;
	bool probed;
} drm_sysfb_beam;

static void drm_sysfb_beam_probe(struct drm_device *dev)
{
	u32 v[4];

	if (drm_sysfb_beam.probed)
		return;
	drm_sysfb_beam.probed = true;
	if (!dev->dev->of_node ||
	    of_property_read_u32_array(dev->dev->of_node, "zerone,scanout-line", v, 4) ||
	    !v[2] || !v[3])
		return;
	drm_sysfb_beam.reg = ioremap(v[0], 4);
	drm_sysfb_beam.mask = v[1];
	drm_sysfb_beam.lines = v[2];
	drm_sysfb_beam.line_ns = v[3];
	if (drm_sysfb_beam.reg)
		drm_info(dev, "copies follow the scanout (line counter at %#x)\\n", v[0]);
}

static void drm_sysfb_beam_wait(int row)
{
	u32 lines = drm_sysfb_beam.lines, ahead;
	ktime_t end = ktime_add_ms(ktime_get(), 2 * 17);

	if (!drm_sysfb_beam.reg || row < 0 || row >= lines)
		return;
	for (;;) {
		u32 line = readl(drm_sysfb_beam.reg) & drm_sysfb_beam.mask;

		/* lines until the scan reaches the row */
		ahead = (row + lines - min(line, lines - 1)) % lines;
		if (ahead <= 16 || ktime_after(ktime_get(), end))
			return;
		if (ahead > 64) {
			unsigned long us = (ahead - 48) * drm_sysfb_beam.line_ns / 1000;

			usleep_range(us, us + 50);
		} else {
			cpu_relax();
		}
	}
}

""", after=False)
for header in ("#include <linux/delay.h>\n", "#include <linux/io.h>\n", "#include <linux/of.h>\n"):
    insert(SYSFB, header, "#include <linux/slab.h>\n", header)

# The copy itself: the rows synced first, then the wait for the scan, then the
# copy, timed, so a copy slower than the scan (which would tear again) shows.
path = tree / SYSFB
s = read(path)
old_loop = """	drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &damage) {
		struct iosys_map dst = sysfb->fb_addr;
		struct drm_rect dst_clip = plane_state->dst;

		if (!drm_rect_intersect(&dst_clip, &damage))
			continue;

		iosys_map_incr(&dst, drm_fb_clip_offset(dst_pitch, dst_format, &dst_clip));
		blit_to_crtc(&dst, &dst_pitch, shadow_plane_state->data, fb, &damage,
			     &shadow_plane_state->fmtcnv_state);
	}
"""
new_loop = """	drm_sysfb_beam_probe(dev);
	{
		int top = INT_MAX;

		drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
		drm_atomic_for_each_plane_damage(&iter, &damage) {
			drm_sysfb_sync_rows_for_cpu(&shadow_plane_state->data[0], fb, &damage);
			top = min(top, damage.y1);
		}
		if (top != INT_MAX)
			drm_sysfb_beam_wait(top);
	}

	{
		ktime_t t0 = ktime_get();
		unsigned int rows = 0;

		drm_atomic_helper_damage_iter_init(&iter, old_plane_state, plane_state);
		drm_atomic_for_each_plane_damage(&iter, &damage) {
			struct iosys_map dst = sysfb->fb_addr;
			struct drm_rect dst_clip = plane_state->dst;

			if (!drm_rect_intersect(&dst_clip, &damage))
				continue;

			iosys_map_incr(&dst, drm_fb_clip_offset(dst_pitch, dst_format, &dst_clip));
			blit_to_crtc(&dst, &dst_pitch, shadow_plane_state->data, fb, &damage,
				     &shadow_plane_state->fmtcnv_state);
			rows += drm_rect_height(&damage);
		}
		if (drm_sysfb_beam.reg && rows > 64 &&
		    ktime_to_ns(ktime_sub(ktime_get(), t0)) > (s64)rows * drm_sysfb_beam.line_ns)
			dev_info_ratelimited(dev->dev, "copied %u rows in %lld us: slower than the scanout\\n",
					     rows, ktime_to_us(ktime_sub(ktime_get(), t0)));
	}
"""
if "drm_sysfb_beam_wait(top);" in s:
    print(f"patch-kernel: {SYSFB}: copies follow the scanout: already")
elif s.count(old_loop) == 1:
    write(path, s.replace(old_loop, new_loop))
    print(f"patch-kernel: {SYSFB}: copies follow the scanout: done")
else:
    sys.exit(f"patch-kernel: {SYSFB}: the damage loop changed")

# --- speaker amplifier -----------------------------------------------------------
# The M20's speaker amp is an NXP TFA9896 (revision 0x1b96 read back on i2c_3,
# 0x34). tfa989x runs these chips with their CoolFlux DSP bypassed, which needs
# no firmware container; the 9896 only adds its own power-on settings, taken
# from Samsung's driver (tfa9896_specific() in sound/soc/codecs/tfa9896/
# tfa_init.c, the same for revisions 0x1b96, 0x2b96 and 0x3b96). Unused until
# the kernel has sound (SND is off in build-boot.sh: ABOX has no driver yet).
AMP = "sound/soc/codecs/tfa989x.c"
insert(AMP, "TFA9896_REVISION",
       "#define TFA9897_REVISION		0x97\n",
       "#define TFA9896_REVISION		0x96\n", after=False)
insert(AMP, "tfa9896_init",
       "static const struct tfa989x_rev tfa9897_rev = {",
       """static int tfa9896_init(struct regmap *regmap)
{
	static const struct reg_sequence seq[] = {
		{ 0x06, 0x000b },
		{ 0x07, 0x3e7f },
		{ 0x0a, 0x0d8a },
		/* Reduce slewrate by clearing iddqtestbst to avoid booster damage */
		{ TFA989X_CURRENTSENSE3, 0x0300 },
		{ 0x88, 0x0100 },
	};
	int ret;

	ret = regmap_multi_reg_write(regmap, seq, ARRAY_SIZE(seq));
	if (ret)
		return ret;

	/* Enable clipping */
	return regmap_clear_bits(regmap, TFA989X_CURRENTSENSE4, 0x1);
}

static const struct tfa989x_rev tfa9896_rev = {
	.rev	= TFA9896_REVISION,
	.init	= tfa9896_init,
};

""", after=False)
insert(AMP, '"nxp,tfa9896"',
       '\t{ .compatible = "nxp,tfa9897", .data = &tfa9897_rev },\n',
       '\t{ .compatible = "nxp,tfa9896", .data = &tfa9896_rev },\n', after=False)

# --- watchdog ------------------------------------------------------------------
WDT = "drivers/watchdog/s3c2410_wdt.c"
insert(WDT, "drv_data_exynos7885",
       "static const struct s3c2410_wdt_variant drv_data_exynos850_cl0 = {",
       """/* Exynos 7885/7904: the A53 cluster's watchdog, bit 24 in the PMU */
static const struct s3c2410_wdt_variant drv_data_exynos7885 = {
	.disable_reg = EXYNOS5_WDT_DISABLE_REG_OFFSET,
	.mask_reset_reg = EXYNOS5_WDT_MASK_RESET_REG_OFFSET,
	.mask_bit = 24,
	.rst_stat_reg = EXYNOS5_RST_STAT_REG_OFFSET,
	.rst_stat_bit = 24,
	.quirks = QUIRK_HAS_WTCLRINT_REG | QUIRK_HAS_PMU_MASK_RESET | \\
		  QUIRK_HAS_PMU_RST_STAT | QUIRK_HAS_PMU_AUTO_DISABLE,
};

""", after=False)
insert(WDT, '"samsung,exynos7885-wdt"',
       """	{ .compatible = "samsung,exynos7-wdt",
	  .data = &drv_data_exynos7 },
""",
       """	{ .compatible = "samsung,exynos7885-wdt",
	  .data = &drv_data_exynos7885 },
""")


def replace(rel, marker, old, new):
    path = tree / rel
    s = read(path)
    if marker in s:
        print(f"patch-kernel: {rel}: already")
        return
    if s.count(old) != 1:
        sys.exit(f"patch-kernel: {rel}: expected the text once, found {s.count(old)}:\n{old}")
    write(path, s.replace(old, new))
    print(f"patch-kernel: {rel}: done")


# --- ACPM -------------------------------------------------------------------------
# ACPM, the APM firmware's message protocol, is how the AP reaches the S2MPU08
# PMIC (the GPU's BUCK5, the codec) on this SoC. Mainline speaks it for gs101;
# the Exynos 7885/7904 has the same channel descriptors at SRAM offset 0x4b80
# (acpm_ipc in Samsung's exynos7885.dtsi). After the Galaxy Tab A 10.1 2019
# port (github.com/Mollomm1/linux-exynos7885, branch gta3xl), reduced to what
# reading a PMIC register needs:
#   - only the APM's polled message queues are used; its other descriptors
#     (notification and buffer channels, which need an interrupt handler this
#     driver does not have) are left alone;
#   - a descriptor is used only once everything it points at fits in the SRAM,
#     so a wrong layout fails the probe instead of writing into the APM's memory;
#   - no ACPM clocks (the 7885's CPU and GPU clocks are the CMUs');
#   - the 7885 is not in the module alias table: the module is only ever loaded
#     by hand while this is being brought up.
# The doorbell is drivers/zrn-acpm-mbox.c.
ACPM = "drivers/firmware/samsung/exynos-acpm.c"
insert(ACPM, "ACPM_EXYNOS7885_INITDATA_BASE",
       "#define ACPM_GS101_INITDATA_BASE\t0xa000\n",
       """#define ACPM_EXYNOS7885_INITDATA_BASE\t0x4b80
/* Exynos 7885: acpm_chan_shmem.reserved[2] is the channel's type */
#define ACPM_EXYNOS7885_CHAN_QUEUE\t1
""")
insert(ACPM, "@exynos7885:",
       " * @num_chans:\tnumber of channels available for this controller.\n",
       " * @sram_size:\tsize of the SRAM.\n"
       " * @exynos7885:\tthe Exynos 7885's layout (polled queues only, all checked).\n")
insert(ACPM, "bool exynos7885;",
       "\tstruct acpm_handle handle;\n\tu32 num_chans;\n",
       "\tresource_size_t sram_size;\n\tbool exynos7885;\n")
insert(ACPM, "acpm_exynos7885_chan_usable",
       "/**\n * acpm_channels_init() - initialize",
       """static bool acpm_sram_fits(struct acpm_info *acpm, u32 off, size_t len)
{
	return !(off & 3) && off <= acpm->sram_size &&
	       len <= acpm->sram_size - off;
}

/*
 * Exynos 7885: use a descriptor only if it is a polled message queue whose
 * id is its index (the doorbell rings the index) and whose queues and
 * indices all lie in the SRAM. Returns 1 to use it, 0 to leave it alone,
 * -EINVAL for a queue that makes no sense.
 */
static int acpm_exynos7885_chan_usable(struct acpm_info *acpm,
				       struct acpm_chan_shmem __iomem *c, int i)
{
	u32 id = readl(&c->id), type = readl(&c->reserved[2]);
	u32 qlen = readl(&c->qlen), mlen = readl(&c->mlen);
	u32 poll = readl(&c->poll_completion);
	u32 idx[4] = { readl(&c->rx_rear), readl(&c->rx_front),
		       readl(&c->tx_rear), readl(&c->tx_front) };
	size_t qsize = (size_t)qlen * mlen;
	int j;

	dev_info(acpm->dev, "descriptor %d: id %u type %u poll %u qlen %u mlen %u\\n",
		 i, id, type, poll, qlen, mlen);
	if (type != ACPM_EXYNOS7885_CHAN_QUEUE || !poll)
		return 0;
	if (id != i || qlen < 2 || qlen > 1024 || !mlen || mlen > 256 || mlen & 3 ||
	    !acpm_sram_fits(acpm, readl(&c->rx_base), qsize) ||
	    !acpm_sram_fits(acpm, readl(&c->tx_base), qsize))
		goto bad;
	for (j = 0; j < ARRAY_SIZE(idx); j++)
		if (!acpm_sram_fits(acpm, idx[j], sizeof(u32)) ||
		    readl(acpm->sram_base + idx[j]) >= qlen)
			goto bad;
	return 1;
bad:
	dev_err(acpm->dev, "descriptor %d does not fit the SRAM\\n", i);
	return -EINVAL;
}

""", after=False)
insert(ACPM, "implausible ACPM channel table",
       "\tacpm->chans = devm_kcalloc(dev, acpm->num_chans,",
       """\tif (acpm->exynos7885 &&
	    (!acpm->num_chans || acpm->num_chans > 16 ||
	     !acpm_sram_fits(acpm, readl(&shmem->chans),
			     acpm->num_chans * sizeof(struct acpm_chan_shmem))))
		return dev_err_probe(dev, -EINVAL, "implausible ACPM channel table\\n");
""", after=False)
insert(ACPM, "acpm_exynos7885_chan_usable(acpm, chan_shmem, i)",
       "\t\tachan->acpm = acpm;\n",
       """
		if (acpm->exynos7885) {
			ret = acpm_exynos7885_chan_usable(acpm, chan_shmem, i);
			if (ret < 0)
				return ret;
			if (!ret)
				continue;
		}
""")
insert(ACPM, "match_data->initdata_base == ACPM_EXYNOS7885_INITDATA_BASE",
       "\tacpm->shmem = acpm->sram_base + match_data->initdata_base;\n",
       """\tacpm->sram_size = size;
	acpm->exynos7885 = match_data->initdata_base == ACPM_EXYNOS7885_INITDATA_BASE;
	if (acpm->exynos7885 && !acpm_sram_fits(acpm, match_data->initdata_base,
						sizeof(struct acpm_shmem)))
		return dev_err_probe(dev, -EINVAL, "initdata beyond the SRAM\\n");
""", after=False)
insert(ACPM, "if (!match_data->acpm_clk_dev_name)",
       "\tacpm_clk_pdev = platform_device_register_data(dev,",
       """\t/* no ACPM clocks on the Exynos 7885 */
	if (!match_data->acpm_clk_dev_name)
		return devm_of_platform_populate(dev);

""", after=False)
insert(ACPM, "acpm_exynos7885 = {",
       "static const struct of_device_id acpm_match[] = {",
       """static const struct acpm_match_data acpm_exynos7885 = {
	.initdata_base = ACPM_EXYNOS7885_INITDATA_BASE,
};

""", after=False)
insert(ACPM, '"samsung,exynos7885-acpm-ipc"',
       "\t\t.data = &acpm_gs101,\n\t},\n",
       """\t{
		.compatible = "samsung,exynos7885-acpm-ipc",
		.data = &acpm_exynos7885,
	},
""")
# Samsung's exynos_acpm_set_flag(), which its WLBT driver sends just before
# the subsystem leaves reset: DVFS channel 0, { 7, 1, SET_FLAG (6), 0 }.
# Exported for zrn-wlbt, which looks it up only if this module is loaded.
insert(ACPM, "acpm_exynos7885_wlbt_flag",
       "/**\n * acpm_setup_ops() -",
       """static struct acpm_info *acpm_exynos7885_info;

int acpm_exynos7885_wlbt_flag(void);	/* used by zrn-wlbt, through symbol_get() */

/*
 * Exynos 7885: what exynos_acpm_set_flag() does in Samsung's kernel right
 * before WLBT leaves reset, so the APM sets the supply the subsystem needs.
 */
int acpm_exynos7885_wlbt_flag(void)
{
	u32 cmd[4] = { 7, 1, 6, 0 };
	struct acpm_xfer xfer = {
		.txd = cmd,
		.rxd = cmd,
		.txcnt = ARRAY_SIZE(cmd),
		.rxcnt = ARRAY_SIZE(cmd),
		.acpm_chan_id = 0,
	};

	if (!acpm_exynos7885_info)
		return -ENODEV;
	return acpm_do_xfer(&acpm_exynos7885_info->handle, &xfer);
}
EXPORT_SYMBOL_GPL(acpm_exynos7885_wlbt_flag);

""", after=False)
insert(ACPM, "acpm_exynos7885_info = acpm;",
       "\tplatform_set_drvdata(pdev, acpm);\n",
       "\tif (acpm->exynos7885)\n\t\tacpm_exynos7885_info = acpm;\n")
replace(ACPM, "acpm_autoload",
        "MODULE_DEVICE_TABLE(of, acpm_match);",
        """/* gs101 only: the Exynos 7885 is loaded by hand while it is brought up */
static const struct of_device_id acpm_autoload[] = {
	{ .compatible = "google,gs101-acpm-ipc" },
	{},
};
MODULE_DEVICE_TABLE(of, acpm_autoload);""")

# --- GPU clocks ---------------------------------------------------------------------
# CMU_G3D (0x11400000), the Mali-G71's clocks: PLL_G3D, the CMU_TOP switching
# clock as the alternative, and the BUSD mux that clocks the core and its bus.
# Register layout and the PLL's levels (the bootloader's ECT, 343 to 845 MHz)
# from the Galaxy Tab A 10.1 2019 port (github.com/Mollomm1/linux-exynos7885,
# branch gta3xl), which follows Samsung's CAL tables.
#
# Only PLL, mux and divider registers are listed, so registering writes nothing
# (exynos_arm64_init_clocks() only touches gate registers). The GPU's rate
# requests (Panfrost's devfreq) go up to PLL_G3D, whose levels are the M20's
# own ECT's (sboot, mmcblk0boot0 at 0x44400: PLL_G3D 343 to 1300 MHz, the same
# P/M/S as here; MINMAX_dvfs_g3d caps this chip's table at 845 MHz). PLL_G3D's
# output is unstable until it relocks, so a notifier parks the GPU on the
# switching path for the change (BUSD mux), as Samsung's DVFS and the T510 port
# do. Its power domain is the bootloader's and stays on (pd_ignore_unused).
CLKH = "include/dt-bindings/clock/exynos7885.h"
insert(CLKH, "CLK_DOUT_G3D_SWITCH",
       "#define CLK_MOUT_SHARED1_PLL\t\t62\n",
       "#define CLK_GOUT_G3D_SWITCH\t\t63\n#define CLK_DOUT_G3D_SWITCH\t\t64\n")
insert(CLKH, "CLK_MOUT_G3D_BUSD",
       "#endif /* _DT_BINDINGS_CLOCK_EXYNOS_7885_H */",
       """/* CMU_G3D */
#define CLK_FOUT_G3D_PLL\t\t1
#define CLK_MOUT_G3D_PLL\t\t2
#define CLK_MOUT_G3D_SWITCH_USER\t3
#define CLK_MOUT_G3D_BUSD\t\t4
#define CLK_DOUT_G3D_BUSP\t\t5

""", after=False)

CLK = "drivers/clk/samsung/clk-exynos7885.c"
replace(CLK, "(CLK_DOUT_G3D_SWITCH + 1)",
        "#define CLKS_NR_TOP\t\t\t(CLK_MOUT_SHARED1_PLL + 1)",
        "#define CLKS_NR_TOP\t\t\t(CLK_DOUT_G3D_SWITCH + 1)")
insert(CLK, "CLKS_NR_G3D",
       "#define CLKS_NR_FSYS\t\t\t(CLK_FSYS_USB30DRD_REF_CLK + 1)\n",
       "#define CLKS_NR_G3D\t\t\t(CLK_DOUT_G3D_BUSP + 1)\n")
insert(CLK, "CLK_CON_DIV_CLKCMU_G3D_SWITCH",
       "#define CLK_CON_DIV_CLKCMU_FSYS_USB30DRD\t0x1854\n",
       "#define CLK_CON_DIV_CLKCMU_G3D_SWITCH\t\t0x1858\n")
insert(CLK, "CLK_CON_GAT_GATE_CLKCMU_G3D_SWITCH",
       "#define CLK_CON_GAT_GATE_CLKCMU_FSYS_USB30DRD\t0x2054\n",
       "#define CLK_CON_GAT_GATE_CLKCMU_G3D_SWITCH\t0x2058\n")
# the divider is saved with CMU_TOP's registers; the gate is not listed, so it
# keeps the bootloader's mode
insert(CLK, "\tCLK_CON_DIV_CLKCMU_G3D_SWITCH,\n",
       "\tCLK_CON_DIV_CLKCMU_FSYS_USB30DRD,\n",
       "\tCLK_CON_DIV_CLKCMU_G3D_SWITCH,\n")
insert(CLK, '"dout_g3d_switch", "gout_g3d_switch"',
       "\t    CLK_CON_DIV_CLKCMU_FSYS_USB30DRD, 0, 4),\n",
       """	DIV(CLK_DOUT_G3D_SWITCH, "dout_g3d_switch", "gout_g3d_switch",
	    CLK_CON_DIV_CLKCMU_G3D_SWITCH, 0, 3),
""")
insert(CLK, '"gout_g3d_switch", "dout_shared0_div2"',
       "\t     CLK_CON_GAT_GATE_CLKCMU_FSYS_USB30DRD, 21, 0, 0),\n",
       """	/* G3D: the GPU's clock while PLL_G3D relocks */
	GATE(CLK_GOUT_G3D_SWITCH, "gout_g3d_switch", "dout_shared0_div2",
	     CLK_CON_GAT_GATE_CLKCMU_G3D_SWITCH, 21, 0, 0),
""")
insert(CLK, "g3d_cmu_info",
       "static int __init exynos7885_cmu_probe",
       """/* ---- CMU_G3D ------------------------------------------------------------- */

/* Register Offset definitions for CMU_G3D (0x11400000) */
#define PLL_LOCKTIME_PLL_G3D			0x0000
#define PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER	0x0100
#define PLL_CON0_PLL_G3D			0x0120
#define CLK_CON_MUX_MUX_CLK_G3D_BUSD		0x1004
#define CLK_CON_DIV_DIV_CLK_G3D_BUSP		0x1800

/* no gate registers: registering CMU_G3D writes nothing */
static const unsigned long g3d_clk_regs[] __initconst = {
	PLL_LOCKTIME_PLL_G3D,
	PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER,
	PLL_CON0_PLL_G3D,
	CLK_CON_MUX_MUX_CLK_G3D_BUSD,
	CLK_CON_DIV_DIV_CLK_G3D_BUSP,
};

/* the GPU's levels, with the bootloader ECT's P/M/S */
static const struct samsung_pll_rate_table g3d_pll_rates[] __initconst = {
	PLL_35XX_RATE(26 * MHZ, 845000000U, 130, 4, 0),
	PLL_35XX_RATE(26 * MHZ, 676000000U, 104, 4, 0),
	PLL_35XX_RATE(26 * MHZ, 545000000U, 545, 13, 1),
	PLL_35XX_RATE(26 * MHZ, 450000000U, 450, 13, 1),
	PLL_35XX_RATE(26 * MHZ, 343000000U, 343, 13, 1),
};

static const struct samsung_pll_clock g3d_pll_clks[] __initconst = {
	PLL(pll_1417x, CLK_FOUT_G3D_PLL, "fout_g3d_pll", "oscclk",
	    PLL_LOCKTIME_PLL_G3D, PLL_CON0_PLL_G3D, g3d_pll_rates),
};

PNAME(mout_g3d_pll_p)		= { "oscclk", "fout_g3d_pll" };
PNAME(mout_g3d_switch_user_p)	= { "oscclk", "dout_g3d_switch" };
PNAME(mout_g3d_busd_p)		= { "mout_g3d_pll", "mout_g3d_switch_user" };

static const struct samsung_mux_clock g3d_mux_clks[] __initconst = {
	MUX_F(CLK_MOUT_G3D_PLL, "mout_g3d_pll", mout_g3d_pll_p,
	      PLL_CON0_PLL_G3D, 4, 1,
	      CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT, 0),
	MUX(CLK_MOUT_G3D_SWITCH_USER, "mout_g3d_switch_user",
	    mout_g3d_switch_user_p, PLL_CON0_MUX_CLKCMU_G3D_SWITCH_USER, 4, 1),
	/*
	 * Clocks both the Mali core and the G3D data bus. Rate requests go to
	 * PLL_G3D; the switch to the other parent is the notifier's alone.
	 */
	MUX_F(CLK_MOUT_G3D_BUSD, "mout_g3d_busd", mout_g3d_busd_p,
	      CLK_CON_MUX_MUX_CLK_G3D_BUSD, 0, 1,
	      CLK_SET_RATE_PARENT | CLK_SET_RATE_NO_REPARENT, 0),
};

static const struct samsung_div_clock g3d_div_clks[] __initconst = {
	DIV(CLK_DOUT_G3D_BUSP, "dout_g3d_busp", "mout_g3d_busd",
	    CLK_CON_DIV_DIV_CLK_G3D_BUSP, 0, 3),
};

static const struct samsung_cmu_info g3d_cmu_info __initconst = {
	.pll_clks		= g3d_pll_clks,
	.nr_pll_clks		= ARRAY_SIZE(g3d_pll_clks),
	.mux_clks		= g3d_mux_clks,
	.nr_mux_clks		= ARRAY_SIZE(g3d_mux_clks),
	.div_clks		= g3d_div_clks,
	.nr_div_clks		= ARRAY_SIZE(g3d_div_clks),
	.nr_clk_ids		= CLKS_NR_G3D,
	.clk_regs		= g3d_clk_regs,
	.nr_clk_regs		= ARRAY_SIZE(g3d_clk_regs),
};

#define G3D_BUSD_SEL_SWITCH	BIT(0)
#define G3D_MUX_BUSY		BIT(16)

static void __iomem *g3d_busd_reg;
static bool g3d_parked;

static void exynos7885_g3d_busd_select(bool use_switch)
{
	u32 val = readl(g3d_busd_reg);

	if (use_switch)
		val |= G3D_BUSD_SEL_SWITCH;
	else
		val &= ~G3D_BUSD_SEL_SWITCH;
	writel(val, g3d_busd_reg);

	if (readl_poll_timeout_atomic(g3d_busd_reg, val, !(val & G3D_MUX_BUSY), 1, 100))
		pr_warn("exynos7885: G3D BUSD mux stays busy\\n");
}

/*
 * PLL_G3D's output is unstable until it locks again: run the GPU from the
 * switching path meanwhile (Samsung's DVFS does the same). That path is slower
 * than any PLL_G3D level, so it is safe at the old level's voltage and the new.
 */
static int exynos7885_g3d_pll_notifier(struct notifier_block *nb,
				       unsigned long event, void *data)
{
	switch (event) {
	case PRE_RATE_CHANGE:
		g3d_parked = !(readl(g3d_busd_reg) & G3D_BUSD_SEL_SWITCH);
		if (g3d_parked)
			exynos7885_g3d_busd_select(true);
		break;
	case POST_RATE_CHANGE:
	case ABORT_RATE_CHANGE:
		if (g3d_parked)
			exynos7885_g3d_busd_select(false);
		g3d_parked = false;
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block exynos7885_g3d_pll_nb = {
	.notifier_call = exynos7885_g3d_pll_notifier,
};

static int exynos7885_g3d_register_notifier(struct device *dev)
{
	struct of_phandle_args args = {
		.np = dev->of_node,
		.args_count = 1,
		.args = { CLK_FOUT_G3D_PLL },
	};
	struct clk *pll;

	g3d_busd_reg = devm_of_iomap(dev, dev->of_node, 0, NULL);
	if (IS_ERR(g3d_busd_reg))
		return PTR_ERR(g3d_busd_reg);
	g3d_busd_reg += CLK_CON_MUX_MUX_CLK_G3D_BUSD;

	pll = of_clk_get_from_provider(&args);
	if (IS_ERR(pll))
		return PTR_ERR(pll);
	return clk_notifier_register(pll, &exynos7885_g3d_pll_nb);
}

""", after=False)
insert(CLK, "exynos7885_g3d_register_notifier(dev);",
       "\texynos_arm64_register_cmu(dev, dev->of_node, info);\n",
       "\tif (info == &g3d_cmu_info)\n\t\treturn exynos7885_g3d_register_notifier(dev);\n")
insert(CLK, "#include <linux/iopoll.h>",
       "#include <linux/clk-provider.h>\n",
       "#include <linux/clk.h>\n#include <linux/iopoll.h>\n#include <linux/of_address.h>\n", after=False)
insert(CLK, '"samsung,exynos7885-cmu-g3d"',
       "\t\t.data = &fsys_cmu_info,\n",
       """	}, {
		.compatible = "samsung,exynos7885-cmu-g3d",
		.data = &g3d_cmu_info,
""")
