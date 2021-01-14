/*
 * reset.c - Add reboot support for loongson-2K.
 *
 * Copyright (C) 2020, Loongson Technology Corporation Limited, Inc.
 *
 * Authors Ming Wang <wangming01@loongson.cn>
 *
 * This program is free software; you can redistribute	it and/or modify it
 * under  the terms of	the GNU General	 Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kexec.h>
#include <linux/pm.h>
#include <linux/slab.h>
#include <acpi/reboot.h>
#include <linux/acpi.h>
#include <asm/bootinfo.h>
#include <asm/idle.h>
#include <asm/reboot.h>

#include <linux/efi.h>
#include <loongson-2k.h>
#include <boot_param.h>

#include <asm/delay.h>
#include <linux/sched/debug.h>

enum ACPI_Sx {
	ACPI_S3 = 5,
	ACPI_S4 = 6,
	ACPI_S5 = 7,
};

#define S_GPMCR		0
#define R_GPMCR		0x4
#define RTC_GPMCR	0x8
#define PM1_STS		0xc
#define PM1_SR		0x10
#define PM1_CTR		0x14
#define PM1_TIMER	0x18
#define PM_PCTR		0x1c

#define GPE0_STS	0x28
#define GPE0_SR		0x2c
#define RST_CTR		0x30
#define WD_CR		0x34
#define WD_TIMER	0x38

#define THER_SCR	0x4c
#define G_RTC_1		0x50
#define G_RTC_2		0x54

#define DPM_CfG		0x400
#define DPM_STS		0x404
#define DPM_CTR		0x408

static void ls2k_pm(enum ACPI_Sx sx)
{
	unsigned long base;
	unsigned int acpi_ctrl;
	base = CKSEG1ADDR(APB_BASE) + ACPI_OFF;

	acpi_ctrl = readl((void *)(base + PM1_STS));
	acpi_ctrl &= 0xffffffff;
	writel(acpi_ctrl, (void *)(base + PM1_STS));

	acpi_ctrl = readl((void *)(base + GPE0_STS));
	acpi_ctrl &= 0xffffffff;
	writel(acpi_ctrl, (void *)(base + GPE0_STS));

	/*GMAC0_EN and GMAC1_EN*/
	acpi_ctrl = readl((void *)(base + GPE0_SR));
	acpi_ctrl |= 0x00000060;
	writel(acpi_ctrl, (void *)(base + GPE0_SR));

	/*WOL_BAT_EN*/
	acpi_ctrl = readl((void *)(base + RTC_GPMCR));
	acpi_ctrl |= 0x00000080;
	writel(acpi_ctrl, (void *)(base + RTC_GPMCR));

	/*USB_GMAC_OK set 1*/
	acpi_ctrl = readl((void *)(base + R_GPMCR));
	acpi_ctrl |= 0x00000080;
	writel(acpi_ctrl, (void *)(base + R_GPMCR));

	acpi_ctrl = ((sx << 10) | (1 << 13));
	writel(acpi_ctrl, (void *)(base + PM1_CTR));
}

static inline void loongson_reboot(void)
{
	unsigned long base;
	base = CKSEG1ADDR(APB_BASE) + ACPI_OFF;

	writel(1, (void *)(base + RST_CTR));

	while (1) {
		;
	}
}

static void loongson_restart(char *command)
{
	loongson_reboot();
}

static void loongson_poweroff(void)
{
	ls2k_pm(ACPI_S5);
	while (1) {
		;
	}
}

static void loongson_halt(void)
{
	pr_notice("\n\n** You can safely turn off the power now **\n\n");
	while (1) {
		if (cpu_wait)
			cpu_wait();
	}
}

static int __init mips_reboot_setup(void)
{
	_machine_restart = loongson_restart;
	_machine_halt = loongson_halt;
	pm_power_off = loongson_poweroff;

	return 0;
}

arch_initcall(mips_reboot_setup);
