// SPDX-License-Identifier: GPL-2.0-only
/*
 * zrn-s2mpu08-g3d: the Mali-G71's supply on the Galaxy M20, the S2MPU08
 * PMIC's BUCK5 (vdd_g3d), as a regulator.
 *
 * The PMIC sits behind the APM, so its registers are read and written with
 * ACPM PMIC messages (channel 2, as Samsung's tree and the Galaxy Tab A 10.1
 * 2019 port use). Only the output voltage is handled: B5CTRL2 (0x1d) selects
 * 300 mV + n x 6.25 mV, Samsung's range, under which the bootloader's setting
 * reads back as the ECT table's voltage. Switching the buck on and off stays
 * with the PMU and the bootloader.
 *
 * Whether the kernel may change the voltage is the device tree's call: with
 * no regulator-min/max-microvolt the regulator core only reads it.
 *
 * After s2mpu08-acpm-buck.c of the Galaxy Tab A 10.1 2019 port
 * (github.com/Mollomm1/linux-exynos7885, branch gta3xl), for 7.1's ACPM
 * interface. No module alias: loaded by hand while ACPM is brought up.
 */
#include <linux/firmware/samsung/exynos-acpm-protocol.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define ACPM_TYPE_PMIC		1
#define S2MPU08_B5CTRL2		0x1d
#define BUCK_MIN_UV		300000
#define BUCK_STEP_UV		6250
#define BUCK_RAMP_UV_US		12000

struct zrn_g3d {
	struct acpm_handle *acpm;
	unsigned int chan;
};

static int zrn_g3d_get_voltage_sel(struct regulator_dev *rdev)
{
	struct zrn_g3d *g = rdev_get_drvdata(rdev);
	u8 val;
	int ret;

	ret = g->acpm->ops.pmic_ops.read_reg(g->acpm, g->chan, ACPM_TYPE_PMIC,
					     S2MPU08_B5CTRL2, 0, &val);
	return ret ?: val;
}

static int zrn_g3d_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	struct zrn_g3d *g = rdev_get_drvdata(rdev);

	return g->acpm->ops.pmic_ops.write_reg(g->acpm, g->chan, ACPM_TYPE_PMIC,
					       S2MPU08_B5CTRL2, 0, sel);
}

static const struct regulator_ops zrn_g3d_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = zrn_g3d_get_voltage_sel,
	.set_voltage_sel = zrn_g3d_set_voltage_sel,
	.set_voltage_time_sel = regulator_set_voltage_time_sel,
};

static const struct regulator_desc zrn_g3d_desc = {
	.name = "buck5",
	.ops = &zrn_g3d_ops,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.min_uV = BUCK_MIN_UV,
	.uV_step = BUCK_STEP_UV,
	.n_voltages = 256,
	.ramp_delay = BUCK_RAMP_UV_US,
};

static int zrn_g3d_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regulator_config config = { };
	struct of_phandle_args args;
	struct regulator_dev *rdev;
	struct zrn_g3d *g;
	int ret;

	g = devm_kzalloc(dev, sizeof(*g), GFP_KERNEL);
	if (!g)
		return -ENOMEM;

	ret = of_parse_phandle_with_fixed_args(dev->of_node, "samsung,acpm-ipc",
					       1, 0, &args);
	if (ret)
		return dev_err_probe(dev, ret, "no samsung,acpm-ipc\n");
	g->chan = args.args[0];
	g->acpm = devm_acpm_get_by_node(dev, args.np);
	of_node_put(args.np);
	if (IS_ERR_OR_NULL(g->acpm))
		return dev_err_probe(dev, g->acpm ? PTR_ERR(g->acpm) : -ENODEV,
				     "no ACPM\n");

	config.dev = dev;
	config.driver_data = g;
	config.of_node = dev->of_node;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node, &zrn_g3d_desc);
	if (!config.init_data)
		return -ENOMEM;

	rdev = devm_regulator_register(dev, &zrn_g3d_desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev), "cannot register\n");

	ret = zrn_g3d_get_voltage_sel(rdev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot read BUCK5 over ACPM\n");
	dev_info(dev, "BUCK5 (vdd_g3d) reads %u uV\n", BUCK_MIN_UV + ret * BUCK_STEP_UV);
	return 0;
}

static const struct of_device_id zrn_g3d_of_match[] = {
	{ .compatible = "samsung,s2mpu08-acpm-buck5" },
	{ }
};

static struct platform_driver zrn_g3d_driver = {
	.driver = {
		.name = "zrn-s2mpu08-g3d",
		.of_match_table = zrn_g3d_of_match,
	},
	.probe = zrn_g3d_probe,
};
module_platform_driver(zrn_g3d_driver);

MODULE_DESCRIPTION("Galaxy M20: the GPU's supply (S2MPU08 BUCK5) over ACPM");
MODULE_LICENSE("GPL");
