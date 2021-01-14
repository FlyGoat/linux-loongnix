/*
 *  Copyright (C) 2007, Lemote Inc. & Institute of Computing Technology
 *  Copyright (C) 2020, Loongson Technology Corporation Limited, Inc.
 *  Author: Zhu Yinbo, zhuyinbo@loongson.cn
 *
 *  This program is free software; you can redistribute	 it and/or modify it
 *  under  the terms of	 the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the	License, or (at your
 *  option) any later version.
 */
#include <linux/export.h>
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/of_fdt.h>
#include <linux/pci.h>

#include <asm/prom.h>
#include <asm/bootinfo.h>

extern int hw_coherentio;

void __init plat_mem_setup(void)
{
	if (loongson_fdt_blob)
		__dt_setup_arch(loongson_fdt_blob);
}

void __init device_tree_init(void)
{
	if (!initial_boot_params)
		return;

	if (early_init_dt_verify(initial_boot_params))
		unflatten_and_copy_device_tree();
}

static int __init setup_dma_ops(void)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, "ls,nbus");

	if (!np) {
		pr_err("Oops: No Loongson NBus found!\n");
		hw_coherentio = 0;
		pr_info("Assume Hardware DOES NOT support coherent IO!\n");
		goto no_np;
	}

	if (of_property_read_bool(np, "dma-coherent")) {

		hw_coherentio = 1;
		pr_info("Hardware support coherent IO!\n");

	} else {

		hw_coherentio = 0;
		pr_info("Hardware DOES NOT support coherent IO!\n");

		 /*it should not be called from any formal release kernel,*/
		 /*since it should be called from bootloader*/
		/*set_io_noncoherent();*/
	}

	if (of_property_read_bool(np, "pci-probe-only")) {
		pci_add_flags(PCI_PROBE_ONLY);
	}

	of_node_put(np);
no_np:
	return 0;
}

arch_initcall(setup_dma_ops);
