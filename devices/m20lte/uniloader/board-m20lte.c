/* SPDX-License-Identifier: GPL-2.0 */
/*
 * uniLoader board for the Samsung Galaxy M20 (m20lte), for ztOS.
 *
 * Built in place of uniLoader's board-a30.c with a30_defconfig: the Galaxy
 * A30 is the same Exynos 7904 with the same 1080 x 2340 bootloader
 * framebuffer at 0xec000000, so its load addresses and framebuffer settings
 * are the M20's too. What this file adds is ztOS's way back (as
 * kernel/uniloader-failsafe.py does for the S9), written first thing after
 * sboot hands over:
 *
 *   PMU INFORM2 (0x11c80808) = 0x12345678   "this was a reset", not a cold boot
 *   PMU INFORM3 (0x11c8080c) = ZTOS_INFORM3 where sboot goes after a reset:
 *                                           0x12345674 TWRP, 0x12345670 BOOT
 *   with ZTOS_WDT: the A53 cluster watchdog (0x10060000) armed for 30 s,
 *   its reset unmasked in the PMU (0x408/0x40c bit 24), for the kernel's
 *   s3c2410_wdt to take over.
 *
 * Copyright (c) 2022, David Wronek <w.david0@protonmail.com>
 * Copyright (c) 2024, Ivaylo Ivanov <ivo.ivanov.ivanov1@gmail.com>
 */

#include <board.h>
#include <util.h>
#include <drivers/framework.h>
#include <lib/simplefb.h>
#include <soc/exynos7885.h>

/* set by devices/m20lte/scripts/build-boot.sh */
#define ZTOS_INFORM3	0x12345674
#define ZTOS_WDT	0

#define PMU_BASE	0x11c80000
#define WDT_BASE	0x10060000

int a30_init(void)
{
	/* ztOS failsafe: fallback boot target, then (optionally) the watchdog */
	*(volatile unsigned int *)(PMU_BASE + 0x808) = 0x12345678;
	*(volatile unsigned int *)(PMU_BASE + 0x80c) = ZTOS_INFORM3;
	if (ZTOS_WDT) {
		*(volatile unsigned int *)(PMU_BASE + 0x408) &= ~(1u << 24);
		*(volatile unsigned int *)(PMU_BASE + 0x40c) &= ~(1u << 24);
		*(volatile unsigned int *)(WDT_BASE + 0x0) = 0;
		/* 26 MHz / 256 / 128 = 793 Hz: 23805 counts is 30 s */
		*(volatile unsigned int *)(WDT_BASE + 0x4) = 23805;
		*(volatile unsigned int *)(WDT_BASE + 0x8) = 23805;
		*(volatile unsigned int *)(WDT_BASE + 0x0) = 0xff39;
	}

	/* Allow framebuffer to be written to */
	*(int*) (DECON_F_BASE + HW_SW_TRIG_CONTROL) = 0x1281;
	return 0;
}

static struct video_info m20lte_fb = {
	.format = FB_FORMAT_ARGB8888,
	.width = 1080,
	.height = 2340,
	.stride = 4,
	.address = (void *)0xec000000
};

static const struct device m20lte_devices[] = {
	{ "simplefb", &m20lte_fb, "fb" },
};

struct board_data board_ops = {
	.name = "samsung-m20lte",
	.ops = {
		.early_init = a30_init,
	},
	.devices = m20lte_devices,
	.num_devices = ARRAY_SIZE(m20lte_devices),
	.quirks = 0
};
