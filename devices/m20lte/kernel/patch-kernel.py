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
