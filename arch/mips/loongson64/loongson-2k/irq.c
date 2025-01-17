/* SPDX-License-Identifier: GPL-2.0 */
/*
 * =====================================================================================
 *
 *       Filename:  irq.c
 *
 *    Description:  irq handle
 *
 *        Version:  1.0
 *        Created:  03/16/2017 10:52:40 AM
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  hp (Huang Pei), huangpei@loongson.cn
 *        Company:  Loongson Corp.
 *
 * =====================================================================================
 */
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/spinlock_types.h>
#include <asm/irq_cpu.h>
#include <loongson_cpu.h>
#include <loongson.h>
#include <loongson-2k.h>

DEFINE_RAW_SPINLOCK(ls2k_irq_lock);

static struct irq_domain *ls2k_pic_irq_domain;

#define LS2K_INTISR_REG(i)	TO_UNCAC(0x1fe11420 | (((i) > 31) ? 0x40 : 0))
#define LS2K_INTEN_REG(i)	TO_UNCAC(0x1fe11424 | (((i) > 31) ? 0x40 : 0))
#define LS2K_INTENSET_REG(i)	TO_UNCAC(0x1fe11428 | (((i) > 31) ? 0x40 : 0))
#define LS2K_INTENCLR_REG(i)	TO_UNCAC(0x1fe1142c | (((i) > 31) ? 0x40 : 0))
#define LS2K_INTPOL_REG(i)	TO_UNCAC(0x1fe11430 | (((i) > 31) ? 0x40 : 0))
#define LS2K_INTEDGE_REG(i)	TO_UNCAC(0x1fe11434 | (((i) > 31) ? 0x40 : 0))
#define LS2K_INTBOUNCE_REG(i)	TO_UNCAC(0x1fe11438 | (((i) > 31) ? 0x40 : 0))
#define LS2K_INTAUTO_REG(i)	TO_UNCAC(0x1fe1143c | (((i) > 31) ? 0x40 : 0))

#define LS2K_IRQ_ROUTE_REG(i)	TO_UNCAC((0x1fe11400 | (((i) > 31) ? 0x40 : 0))\
				+ ((i) % 32))

#define LS2K_PIC_IRQ_LINE   1  /* IP3 */
#define LS2K_PIC_HWIRQ_BASE 8  /* should keep consistent with dts  */

#ifdef CONFIG_SMP
extern void ls64_ipi_interrupt(struct pt_regs *regs);
#endif

struct fwnode_handle *liointc_handle;

static void ls2k_pic_set_irq_route(int irq, struct cpumask *core_mask, int line)
{
	unsigned long bounce_reg = LS2K_INTBOUNCE_REG(irq);
	unsigned long auto_reg = LS2K_INTAUTO_REG(irq);
	unsigned long *mask;
	unsigned int weight;
	unsigned int reg_val;

	if (irq > 63) {
		printk(KERN_ERR "%s: interrupt number error\n", __func__);
		return;
	}

	mask = cpumask_bits(core_mask);
	ls64_conf_write8((1 << (line+4)) | (*mask & 0xf), (void *)LS2K_IRQ_ROUTE_REG(irq));

	reg_val = ls64_conf_read32((void *)auto_reg);
	reg_val &= ~(1 << (irq % 32)); /* auto always set to 0 */
	ls64_conf_write32(reg_val, (void *)auto_reg);

	reg_val = ls64_conf_read32((void *)bounce_reg);

	weight = cpumask_weight(core_mask);
	if (weight > 1)
		reg_val |= (1 << (irq % 32));
	else
		reg_val &= ~(1 << (irq % 32));
	ls64_conf_write32(reg_val, (void *)bounce_reg);
}

static int ls2k_pic_irq_handler(void)
{
	unsigned long pending, enabled;
	u32 reg_val;
	int irq;
	unsigned int virq;

	pending = readl((void *)LS2K_INTISR_REG(0));
	reg_val = readl((void *)LS2K_INTISR_REG(32));
	pending |= (unsigned long)reg_val << 32;

	enabled = readl((void *)LS2K_INTEN_REG(0));
	reg_val = readl((void *)LS2K_INTEN_REG(32));
	enabled |= (unsigned long)reg_val << 32;

	pending &= enabled;
	if (!pending)
		spurious_interrupt();

	while (pending) {
		irq = fls64(pending) - 1;
		virq = irq_linear_revmap(ls2k_pic_irq_domain, irq + LS2K_PIC_HWIRQ_BASE);
		do_IRQ(virq);
		pending &= ~BIT(irq);
	}
	return 0;
}
static unsigned long ls2k_irq_count;

void mach_irq_dispatch(unsigned int pending)
{
	ls2k_irq_count++;
	if (pending & CAUSEF_IP2)
		do_IRQ(MIPS_CPU_IRQ_BASE + 2);
	if (pending & CAUSEF_IP3)
		ls2k_pic_irq_handler();
#ifdef CONFIG_SMP
	if (pending & CAUSEF_IP6)
		ls64_ipi_interrupt(NULL);
#endif
	if (pending & CAUSEF_IP7)
		do_IRQ(MIPS_CPU_IRQ_BASE + 7);
}

void mask_pic_irq(struct irq_data *data)
{
	unsigned long hwirq = data->hwirq - LS2K_PIC_HWIRQ_BASE;
	unsigned long reg = LS2K_INTENCLR_REG(hwirq);
	unsigned long flags;

	raw_spin_lock_irqsave(&ls2k_irq_lock, flags);
	ls64_conf_write32(1<<(hwirq%32), (void *)reg);
	raw_spin_unlock_irqrestore(&ls2k_irq_lock, flags);

}

void unmask_pic_irq(struct irq_data *data)
{
	unsigned long hwirq = data->hwirq - LS2K_PIC_HWIRQ_BASE;
	unsigned long reg = LS2K_INTENSET_REG(hwirq);
	unsigned long flags;

	raw_spin_lock_irqsave(&ls2k_irq_lock, flags);
	ls64_conf_write32(1<<(hwirq % 32), (void *)reg);
	raw_spin_unlock_irqrestore(&ls2k_irq_lock, flags);
}
static int ls2k_pic_set_affinity(struct irq_data *d, const struct cpumask *affinity,
		bool force)
{
	unsigned long hwirq = d->hwirq - LS2K_PIC_HWIRQ_BASE;
	cpumask_t tmask;
	unsigned long *mask;
	unsigned long flags;

	cpumask_and(&tmask, affinity, cpu_online_mask);
	cpumask_copy(d->common->affinity, &tmask);

	mask = cpumask_bits(&tmask);
	raw_spin_lock_irqsave(&ls2k_irq_lock, flags);

	ls2k_pic_set_irq_route(hwirq, &tmask, LS2K_PIC_IRQ_LINE);

	raw_spin_unlock_irqrestore(&ls2k_irq_lock, flags);

	irq_data_update_effective_affinity(d, &tmask);

	return IRQ_SET_MASK_OK_NOCOPY;
}

static struct irq_chip ls2k_pic = {
	.name		= "ls2k-pic",
	.irq_mask	= mask_pic_irq,
	.irq_unmask	= unmask_pic_irq,
	.irq_set_affinity = ls2k_pic_set_affinity,
};

static int ls2k_pic_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &ls2k_pic, handle_level_irq);
	return 0;
}

int ls2k_pic_irq_domain_xlate(struct irq_domain *d, struct device_node *node,
		const u32 *intspec, unsigned int intsize,
		unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))
		return -EINVAL;

	*out_hwirq = intspec[0];
	if (intsize > 1)
		*out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;
	else
		*out_type = IRQ_TYPE_NONE;
	return 0;
}

static const struct irq_domain_ops ls2k_pic_irq_domain_ops = {
	.map = ls2k_pic_map,
	.xlate = ls2k_pic_irq_domain_xlate,
};

void __init mach_init_irq(void)
{
	unsigned long reg, irq;
	struct cpumask core_mask;

	/* init mips cpu interrupt */
	irq_alloc_descs(MIPS_CPU_IRQ_BASE, MIPS_CPU_IRQ_BASE, 8, 0);
	mips_cpu_irq_init();
	set_c0_status(STATUSF_IP2 | STATUSF_IP3 | STATUSF_IP6);

	/* disable all peripheral interrupts */
	reg = LS2K_INTENCLR_REG(0);
	ls64_conf_write32(0xffffffff, (void *)reg);
	reg = LS2K_INTENCLR_REG(32);
	ls64_conf_write32(0xffffffff, (void *)reg);

	/* all peripheral interrupts route to core 0 IP3 */
	cpumask_clear(&core_mask);
	cpumask_set_cpu(0, &core_mask);

	for (irq = 0; irq < 64; irq++)
		ls2k_pic_set_irq_route(irq, &core_mask, LS2K_PIC_IRQ_LINE);

	irqchip_init();
}

asmlinkage void plat_irq_dispatch(void)
{
	unsigned int pending;

	pending = read_c0_cause() & read_c0_status() & ST0_IM;

	/* machine-specific plat_irq_dispatch */
	mach_irq_dispatch(pending);
}

void __init arch_init_irq(void)
{
	/*
	 * Clear all of the interrupts while we change the able around a bit.
	 * int-handler is not on bootstrap
	 */
	clear_c0_status(ST0_IM | ST0_BEV);

	/* no steer */
	LOONGSON_INTSTEER = 0;

	/*
	 * Mask out all interrupt by writing "1" to all bit position in
	 * the interrupt reset reg.
	 */
	LOONGSON_INTENCLR = ~0;

	/* machine specific irq init */
	mach_init_irq();
}

int ls2k_irq_of_init(struct device_node *of_node,
				struct device_node *parent)
{
	unsigned int virq;

	virq = irq_alloc_descs(LS2K_IRQ_BASE, LS2K_IRQ_BASE, 64, 0);
	ls2k_pic_irq_domain = irq_domain_add_legacy(of_node, 64, virq, LS2K_PIC_HWIRQ_BASE,
							&ls2k_pic_irq_domain_ops, NULL);
	if (!ls2k_pic_irq_domain)
		panic("Failed to add irqdomain for ls2k peripheral intc");

	return 0;
}
EXPORT_SYMBOL(ls2k_irq_of_init);

IRQCHIP_DECLARE(ls2k_pic, "loongson,2k-icu", ls2k_irq_of_init);
