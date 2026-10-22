// SPDX-License-Identifier: GPL-2.0-only
/*
 * zrn-acpm-mbox: the Exynos 7885/7904's AP-to-APM doorbell, which ACPM
 * (drivers/firmware/samsung/exynos-acpm.c, patched by patch-kernel.py) rings
 * after queueing a message in the APM's SRAM.
 *
 * Channel n is bit n + 16 of INTGR0 (Samsung's acpm_ipc, mailbox at
 * 0x11c40000). The APM clears the bit, visible in INTSR0, once it has taken
 * the message; a channel whose previous doorbell is still pending is not
 * rung again, so a request can never be folded into the one before it.
 *
 * Only transmits: ACPM polls its queues for the answers, so the APM's
 * interrupt to the AP stays unrequested.
 *
 * After the Galaxy Tab A 10.1 2019 port's exynos7885-acpm-mbox.c
 * (github.com/Mollomm1/linux-exynos7885, branch gta3xl). No module alias:
 * the module is loaded by hand while ACPM is brought up on the M20.
 */
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mailbox/exynos-message.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define INTGR0		0x08
#define INTSR0		0x14
#define CHANNELS	16
#define DOORBELL(id)	BIT((id) + 16)
#define IDLE_TIMEOUT_US	2000

struct zrn_acpm_mbox {
	void __iomem *regs;
	struct mbox_controller controller;
	struct mbox_chan channels[CHANNELS];
};

static int zrn_acpm_mbox_send(struct mbox_chan *chan, void *data)
{
	struct zrn_acpm_mbox *mbox = dev_get_drvdata(chan->mbox->dev);
	struct exynos_mbox_msg *msg = data;
	u32 pending;

	if (!msg || msg->chan_type != EXYNOS_MBOX_CHAN_TYPE_DOORBELL ||
	    msg->chan_id >= CHANNELS)
		return -EINVAL;

	/* called under the controller's spinlock: busy-wait, briefly */
	if (readl_poll_timeout_atomic(mbox->regs + INTSR0, pending,
				      !(pending & DOORBELL(msg->chan_id)), 1,
				      IDLE_TIMEOUT_US)) {
		dev_err_ratelimited(chan->mbox->dev,
				    "channel %u: the APM has not taken the last message\n",
				    msg->chan_id);
		return -EBUSY;
	}
	writel(DOORBELL(msg->chan_id), mbox->regs + INTGR0);
	return 0;
}

static const struct mbox_chan_ops zrn_acpm_mbox_ops = {
	.send_data = zrn_acpm_mbox_send,
};

/* ACPM asks for one channel per message queue, with no arguments */
static struct mbox_chan *zrn_acpm_mbox_xlate(struct mbox_controller *ctl,
					     const struct of_phandle_args *sp)
{
	int i;

	if (sp->args_count)
		return ERR_PTR(-EINVAL);
	for (i = 0; i < ctl->num_chans; i++)
		if (!ctl->chans[i].cl)
			return &ctl->chans[i];
	return ERR_PTR(-EBUSY);
}

static int zrn_acpm_mbox_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct zrn_acpm_mbox *mbox;
	int i;

	mbox = devm_kzalloc(dev, sizeof(*mbox), GFP_KERNEL);
	if (!mbox)
		return -ENOMEM;

	mbox->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(mbox->regs))
		return PTR_ERR(mbox->regs);

	mbox->controller.dev = dev;
	mbox->controller.ops = &zrn_acpm_mbox_ops;
	mbox->controller.of_xlate = zrn_acpm_mbox_xlate;
	mbox->controller.chans = mbox->channels;
	mbox->controller.num_chans = CHANNELS;
	for (i = 0; i < CHANNELS; i++)
		mbox->channels[i].mbox = &mbox->controller;

	platform_set_drvdata(pdev, mbox);
	return devm_mbox_controller_register(dev, &mbox->controller);
}

static const struct of_device_id zrn_acpm_mbox_of_match[] = {
	{ .compatible = "samsung,exynos7885-acpm-mbox" },
	{ }
};

static struct platform_driver zrn_acpm_mbox_driver = {
	.probe = zrn_acpm_mbox_probe,
	.driver = {
		.name = "zrn-acpm-mbox",
		.of_match_table = zrn_acpm_mbox_of_match,
	},
};
module_platform_driver(zrn_acpm_mbox_driver);

MODULE_DESCRIPTION("Galaxy M20: the AP-to-APM doorbell that ACPM rings");
MODULE_LICENSE("GPL");
