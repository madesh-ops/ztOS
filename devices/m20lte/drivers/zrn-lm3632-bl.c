// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-lm3632-bl: the Galaxy M20's backlight, for a kernel with no display
 * driver.
 *
 * On the M20 the backlight LEDs are driven by a TI LM3632 on i2c_4 (0x11),
 * which also makes the panel's +/- bias rails. Samsung's kernel sets its
 * LED current to full (LM3632_INIT in hx83112a_m20_param.h: 0x04 = 0x07,
 * 0x05 = 0xff) and does the dimming in the panel, with the DCS brightness
 * command (0x51) over DSI, the panel's PWM output then scaling the LM3632
 * (0x09 = 0x41: PWM input on). Mainline has no DSI for this SoC, so the
 * panel cannot be told anything; this dims with the LM3632's own 11-bit
 * current code instead, which works the same with the panel's PWM left at
 * whatever the bootloader set.
 *
 *	/sys/class/backlight/lm3632-bl/brightness	0 .. 2047
 *
 * Only the two brightness registers are ever written: register 0x0a also
 * switches the display bias rails, and turning those off blanks the panel
 * until the next boot. The LM3632 latches a new code when its MSB is
 * written, so the LSB goes first.
 *
 * Bound by the device tree (compatible "zerone,lm3632-backlight", in
 * devices/m20lte/kernel/exynos7904-m20lte.dts).
 */
#include <linux/backlight.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>

#define REG_BRT_LSB	0x04	/* bits 2:0 */
#define REG_BRT_MSB	0x05	/* bits 10:3 */
#define BRT_MAX		2047

static int lm3632_bl_update(struct backlight_device *bd)
{
	struct i2c_client *client = bl_get_data(bd);
	int level = backlight_get_brightness(bd);
	int ret;

	ret = i2c_smbus_write_byte_data(client, REG_BRT_LSB, level & 0x7);
	if (ret)
		return ret;
	return i2c_smbus_write_byte_data(client, REG_BRT_MSB, level >> 3);
}

static const struct backlight_ops lm3632_bl_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = lm3632_bl_update,
};

static int lm3632_bl_probe(struct i2c_client *client)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = BRT_MAX,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	struct backlight_device *bd;
	int lsb, msb;

	/* Start from what the bootloader left: no flash of full brightness. */
	lsb = i2c_smbus_read_byte_data(client, REG_BRT_LSB);
	msb = i2c_smbus_read_byte_data(client, REG_BRT_MSB);
	if (lsb < 0 || msb < 0)
		return dev_err_probe(&client->dev, lsb < 0 ? lsb : msb,
				     "the LM3632 does not answer at %#x\n", client->addr);
	props.brightness = (msb << 3) | (lsb & 0x7);

	bd = devm_backlight_device_register(&client->dev, "lm3632-bl", &client->dev,
					    client, &lm3632_bl_ops, &props);
	if (IS_ERR(bd))
		return PTR_ERR(bd);
	dev_info(&client->dev, "backlight at %d of %d\n", props.brightness, BRT_MAX);
	return 0;
}

static const struct of_device_id lm3632_bl_of_match[] = {
	{ .compatible = "zerone,lm3632-backlight" },
	{ }
};
MODULE_DEVICE_TABLE(of, lm3632_bl_of_match);

static const struct i2c_device_id lm3632_bl_id[] = {
	{ "zrn-lm3632-bl" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, lm3632_bl_id);

static struct i2c_driver lm3632_bl_driver = {
	.driver = {
		.name = "zrn-lm3632-bl",
		.of_match_table = lm3632_bl_of_match,
	},
	.probe = lm3632_bl_probe,
	.id_table = lm3632_bl_id,
};
module_i2c_driver(lm3632_bl_driver);

MODULE_DESCRIPTION("Galaxy M20 backlight through the LM3632's own brightness code");
MODULE_LICENSE("GPL");
