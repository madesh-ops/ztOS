// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-s2mu004-fg: the Galaxy M20's battery, as the S2MU004 fuel gauge
 * measures it.
 *
 * The gauge is its own I2C device (i2c_0, 0x3b; battery_data_m20lte_common.dtsi)
 * and keeps counting by itself once Android or the bootloader has set it up,
 * so this only reads it: no charger control, no gauge calibration, no
 * battery model. What it reports, from Samsung's s2mu004_fuelgauge.c:
 *
 *   0x04 RVBAT    voltage:  (word * 1000) >> 13 mV
 *   0x06 RCUR_CC  current:  two's complement, (word * 1000) >> 12 mA,
 *                           negative while discharging
 *   0x08 RSOC     charge:   (word * 10000) >> 14 in 0.01 %, the gauge's raw
 *                           state of charge (Samsung's "raw soc"; its UI
 *                           percentage is this rescaled)
 *
 * Status is read from the sign of the current, since nothing here knows
 * about the charger: a full battery on the cable shows as "Not charging".
 *
 * Bound by the device tree (compatible "zerone,s2mu004-fuelgauge").
 */
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/power_supply.h>

#define REG_RVBAT	0x04
#define REG_RCUR_CC	0x06
#define REG_RSOC	0x08

/* the M20's battery: EB-BG580ABU, 5000 mAh, 4.4 V */
#define DESIGN_UAH	5000000
#define DESIGN_MAX_UV	4400000

struct zrn_fg {
	struct i2c_client *client;
	struct mutex lock;
};

static int fg_word(struct zrn_fg *fg, u8 reg, u16 *out)
{
	u8 buf[2];
	int ret;

	mutex_lock(&fg->lock);
	ret = i2c_smbus_read_i2c_block_data(fg->client, reg, 2, buf);
	mutex_unlock(&fg->lock);
	if (ret < 0)
		return ret;
	if (ret != 2)
		return -EIO;
	*out = buf[0] | (buf[1] << 8);
	return 0;
}

static int fg_voltage_uv(struct zrn_fg *fg, int *uv)
{
	u16 w;
	int ret = fg_word(fg, REG_RVBAT, &w);

	if (!ret)
		*uv = ((w * 1000) >> 13) * 1000;
	return ret;
}

static int fg_current_ua(struct zrn_fg *fg, int *ua)
{
	u16 w;
	int ret = fg_word(fg, REG_RCUR_CC, &w);

	if (ret)
		return ret;
	if (w & 0x8000)		/* charging: Samsung's driver negates this */
		*ua = ((((~w) & 0xffff) + 1) * 1000 >> 12) * 1000;
	else
		*ua = -(((w & 0x7fff) * 1000 >> 12) * 1000);
	return 0;
}

static int fg_capacity(struct zrn_fg *fg, int *percent)
{
	u16 w;
	int ret = fg_word(fg, REG_RSOC, &w);

	if (ret)
		return ret;
	if (w & 0x8000)		/* below empty */
		*percent = 0;
	else
		*percent = clamp(((w & 0x7fff) * 10000 >> 14) / 100, 0, 100);
	return 0;
}

static int fg_get_property(struct power_supply *psy, enum power_supply_property psp,
			   union power_supply_propval *val)
{
	struct zrn_fg *fg = power_supply_get_drvdata(psy);
	int ret = 0, ua;

	switch (psp) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		ret = fg_current_ua(fg, &ua);
		if (!ret)
			val->intval = ua > 20000 ? POWER_SUPPLY_STATUS_CHARGING :
				      ua < -20000 ? POWER_SUPPLY_STATUS_DISCHARGING :
				      POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = fg_voltage_uv(fg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = fg_current_ua(fg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		ret = fg_capacity(fg, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = DESIGN_UAH;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = DESIGN_MAX_UV;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static const enum power_supply_property fg_props[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_SCOPE,
};

static const struct power_supply_desc fg_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = fg_props,
	.num_properties = ARRAY_SIZE(fg_props),
	.get_property = fg_get_property,
};

static int zrn_fg_probe(struct i2c_client *client)
{
	struct power_supply_config cfg = {};
	struct power_supply *psy;
	struct zrn_fg *fg;
	int uv, percent, ret;

	fg = devm_kzalloc(&client->dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;
	fg->client = client;
	mutex_init(&fg->lock);

	ret = fg_voltage_uv(fg, &uv);
	if (ret)
		return dev_err_probe(&client->dev, ret, "the S2MU004 gauge does not answer at %#x\n",
				     client->addr);
	fg_capacity(fg, &percent);
	dev_info(&client->dev, "battery %d mV, %d %%\n", uv / 1000, percent);

	cfg.drv_data = fg;
	cfg.fwnode = dev_fwnode(&client->dev);
	psy = devm_power_supply_register(&client->dev, &fg_desc, &cfg);
	return PTR_ERR_OR_ZERO(psy);
}

static const struct of_device_id zrn_fg_of_match[] = {
	{ .compatible = "zerone,s2mu004-fuelgauge" },
	{ }
};
MODULE_DEVICE_TABLE(of, zrn_fg_of_match);

static const struct i2c_device_id zrn_fg_id[] = {
	{ "zrn-s2mu004-fg" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zrn_fg_id);

static struct i2c_driver zrn_fg_driver = {
	.driver = {
		.name = "zrn-s2mu004-fg",
		.of_match_table = zrn_fg_of_match,
	},
	.probe = zrn_fg_probe,
	.id_table = zrn_fg_id,
};
module_i2c_driver(zrn_fg_driver);

MODULE_DESCRIPTION("Galaxy M20 battery readings from the S2MU004 fuel gauge");
MODULE_LICENSE("GPL");
