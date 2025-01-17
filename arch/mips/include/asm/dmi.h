/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2019 Loongson Technology Corp.
 * Author: Weihui Wang, wangweihui@loongson.cn
 *	   Yinglu Yang, yangyinglu@loongson.cn
 */

#ifndef _ASM_DMI_H__
#define _ASM_DMI_H__
#include <linux/efi.h>
#include <linux/slab.h>

#define dmi_early_remap		early_ioremap
#define dmi_early_unmap		early_iounmap
#define dmi_remap	dmi_ioremap
#define dmi_unmap	dmi_iounmap
#define dmi_alloc(l)	alloc_bootmem(l)

void __init __iomem *dmi_ioremap(u64 phys_addr, unsigned long size)
{
    return ((void *)TO_CAC(phys_addr));
}
void dmi_iounmap(volatile void __iomem *addr)
{

}
#endif /* _ASM_DMI_H */
