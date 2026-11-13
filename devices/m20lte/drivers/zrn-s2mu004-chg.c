// SPDX-License-Identifier: GPL-2.0
/*
 * zrn-s2mu004-chg: the Galaxy M20's charger, the S2MU004 (i2c_1, 0x3d; the
 * fuel gauge is a separate device on i2c_0, see zrn-s2mu004-fg.c).
 *
 * Read-only for now: the charger keeps the settings the bootloader left. This
 * says whether a cable is in and what the charger is doing, and shows the
 * registers in debugfs, so that their values can be checked on the phone
 * before anything writes to a chip that decides how a battery is charged.
 *
 * It reads 0x0a-0x24 only. Below those are the chip's interrupt registers,
 * which a read clears. From Samsung's s2mu004_charger.c:
 *
 *   0x0a CHG_STATUS0  bits 7:5 CHGIN: 0xa0 or 0x60 is power at the input
 *   0x0b CHG_STATUS1  bit 7 self-discharge, bit 1 top-off, bit 0 done
 *
 * Bound by the device tree (compatible "zerone,s2mu004-charger").
 */
#include <linux/debugfs.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/power_supply.h>
#include <linux/seq_file.h>

#define REG_STATUS0	0x0a
#define REG_STATUS1	0x0b
#define REG_FIRST	0x0a
#define REG_LAST	0x24

#define CHGIN_MASK	0xe0
#define CHGIN_OK_A	0xa0
#define CHGIN_OK_B	0x60
#define STATUS1_SELF	0x80
#define STATUS1_TOPOFF	0x02
#define STATUS1_DONE	0x01

struct zrn_chg {
	struct i2c_client *client;
	struct dentry *debug;
};

static int chg_online(struct zrn_chg *chg, bool *online)
{
	int v = i2c_smbus_read_byte_data(chg->client, REG_STATUS0);

	if (v < 0)
		return v;
	v &= CHGIN_MASK;
	*online = v == CHGIN_OK_A || v == CHGIN_OK_B;
	return 0;
}

static int chg_status(struct zrn_chg *chg, int *status)
{
	bool online;
	int ret = chg_online(chg, &online);
	int s1;

	if (ret)
		return ret;
	s1 = i2c_smbus_read_byte_data(chg->client, REG_STATUS1);
	if (s1 < 0)
		return s1;
	if (s1 & STATUS1_SELF)
		*status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (s1 & (STATUS1_TOPOFF | STATUS1_DONE))
		*status = POWER_SUPPLY_STATUS_FULL;
	else if (online)
		*status = POWER_SUPPLY_STATUS_CHARGING;
	else
		*status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	return 0;
}

static int chg_get_property(struct power_supply *psy, enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct zrn_chg *chg = power_supply_get_drvdata(psy);
	bool online;
	int ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		ret = chg_online(chg, &online);
		if (ret)
			return ret;
		val->intval = online;
		return 0;
	case POWER_SUPPLY_PROP_STATUS:
		return chg_status(chg, &val->intval);
	default:
		return -EINVAL;
	}
}

static const enum power_supply_property chg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
};

static const struct power_supply_desc chg_desc = {
	.name = "s2mu004-charger",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = chg_props,
	.num_properties = ARRAY_SIZE(chg_props),
	.get_property = chg_get_property,
};

static int chg_regs_show(struct seq_file *m, void *unused)
{
	struct zrn_chg *chg = m->private;
	int reg;

	for (reg = REG_FIRST; reg <= REG_LAST; reg++) {
		int v = i2c_smbus_read_byte_data(chg->client, reg);

		if (v < 0)
			seq_printf(m, "%02x: error %d\n", reg, v);
		else
			seq_printf(m, "%02x: %02x\n", reg, v);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(chg_regs);

static int chg_probe(struct i2c_client *client)
{
	struct power_supply_config cfg = {};
	struct power_supply *psy;
	struct zrn_chg *chg;
	int v;

	chg = devm_kzalloc(&client->dev, sizeof(*chg), GFP_KERNEL);
	if (!chg)
		return -ENOMEM;
	chg->client = client;
	i2c_set_clientdata(client, chg);

	v = i2c_smbus_read_byte_data(client, REG_STATUS0);
	if (v < 0)
		return dev_err_probe(&client->dev, v, "no charger at 0x%02x\n", client->addr);

	cfg.drv_data = chg;
	cfg.fwnode = dev_fwnode(&client->dev);
	psy = devm_power_supply_register(&client->dev, &chg_desc, &cfg);
	if (IS_ERR(psy))
		return PTR_ERR(psy);

	chg->debug = debugfs_create_dir("zrn-s2mu004-chg", NULL);
	debugfs_create_file("regs", 0444, chg->debug, chg, &chg_regs_fops);

	dev_info(&client->dev, "charger, status0 0x%02x\n", v);
	return 0;
}

static void chg_remove(struct i2c_client *client)
{
	struct zrn_chg *chg = i2c_get_clientdata(client);

	debugfs_remove_recursive(chg->debug);
}

static const struct of_device_id chg_of_match[] = {
	{ .compatible = "zerone,s2mu004-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, chg_of_match);

static struct i2c_driver chg_driver = {
	.driver = {
		.name = "zrn-s2mu004-chg",
		.of_match_table = chg_of_match,
	},
	.probe = chg_probe,
	.remove = chg_remove,
};
module_i2c_driver(chg_driver);

MODULE_DESCRIPTION("S2MU004 charger, read-only, for the Galaxy M20");
MODULE_LICENSE("GPL");
