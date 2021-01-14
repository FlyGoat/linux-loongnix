// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2020, Jianmin Lv <lvjianmin@loongson.cn>
 *  Loongson LPC support
 */

#define pr_fmt(fmt) "lpc: " fmt

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <loongson-pch.h>
#include <linux/syscore_ops.h>

/* Registers */
#define LPC_PARENT_IRQ		83
#define LPC_COUNT		16
struct pch_lpc {
	struct fwnode_handle	*domain_handle;
	struct irq_domain	*lpc_domain;
	u32			saved_reg_ctl;
	u32			saved_reg_ena;
	u32			saved_reg_pol;
	raw_spinlock_t		lpc_lock;
} *pch_lpc_priv;


static void ack_lpc_irq(struct irq_data *d)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&pch_lpc_priv->lpc_lock, flags);
	writel(0x1 << d->irq, LS7A_LPC_INT_CLR);
	raw_spin_unlock_irqrestore(&pch_lpc_priv->lpc_lock, flags);
}
static void mask_lpc_irq(struct irq_data *d)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&pch_lpc_priv->lpc_lock, flags);
	writel(readl(LS7A_LPC_INT_ENA) & (~(0x1 << (d->irq))), LS7A_LPC_INT_ENA);
	raw_spin_unlock_irqrestore(&pch_lpc_priv->lpc_lock, flags);
}

static void mask_ack_lpc_irq(struct irq_data *d)
{
}
static void unmask_lpc_irq(struct irq_data *d)
{
	unsigned long flags;
	raw_spin_lock_irqsave(&pch_lpc_priv->lpc_lock, flags);
	writel(readl(LS7A_LPC_INT_ENA) | (0x1 << (d->irq)),
			LS7A_LPC_INT_ENA);
	raw_spin_unlock_irqrestore(&pch_lpc_priv->lpc_lock, flags);
}

static struct irq_chip pch_lpc_irq_chip = {
	.name			= "LPC",
	.irq_mask		= mask_lpc_irq,
	.irq_unmask		= unmask_lpc_irq,
	.irq_ack		= ack_lpc_irq,
	.irq_mask_ack		= mask_ack_lpc_irq,
	.irq_eoi		= unmask_lpc_irq,
};

static void pch_handle_irq(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 pending;

	chained_irq_enter(chip, desc);

	pending = readl(LS7A_LPC_INT_ENA);
	pending &= readl(LS7A_LPC_INT_STS);

	if (!pending)
		spurious_interrupt();

	while (pending) {
		int bit = __ffs(pending);
		generic_handle_irq(bit);
		pending &= ~BIT(bit);
	}
	chained_irq_exit(chip, desc);
}

static int pch_lpc_map(struct irq_domain *d, unsigned int irq,
			irq_hw_number_t hw)
{
	irq_set_chip_and_handler(irq, &pch_lpc_irq_chip, handle_level_irq);
	return 0;
}

static const struct irq_domain_ops pch_lpc_domain_ops = {
	.map = pch_lpc_map,
	.xlate = irq_domain_xlate_onecell,
};

static void pch_lpc_reset(struct pch_lpc *priv)
{
	/* Enable the LPC interrupt, bit31: en  bit30: edge */
	writel(0x80000000, LS7A_LPC_INT_CTL);
	writel(0, LS7A_LPC_INT_ENA);

	/* clear all 18-bit interrpt bit */
	writel(0x3ffff, LS7A_LPC_INT_CLR);
}

int pch_lpc_init(struct fwnode_handle *domain_handle)
{
	struct pch_lpc *priv;
	struct irq_fwspec fwspec;
	int err, parent_irq;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	raw_spin_lock_init(&priv->lpc_lock);

	priv->domain_handle = irq_domain_alloc_fwnode(priv);
	if (!priv->domain_handle) {
		pr_err("Unable to allocate domain handle\n");
		err = -ENOMEM;
		goto iounmap_base;
	}
	priv->lpc_domain = irq_domain_add_legacy(NULL, LPC_COUNT, 0, 0,
						&pch_lpc_domain_ops,
						priv);
	if (!priv->lpc_domain) {
		pr_err("Failed to create IRQ domain\n");
		err = -ENOMEM;
		goto iounmap_base;
	}
	pch_lpc_reset(priv);

	fwspec.fwnode = NULL;
	fwspec.param[0] = LPC_PARENT_IRQ;
	fwspec.param_count = 1;
	parent_irq = irq_create_fwspec_mapping(&fwspec);
	irq_set_chained_handler_and_data(parent_irq,
						 pch_handle_irq, priv);
	pch_lpc_priv = priv;
	return 0;

iounmap_base:
	kfree(priv);

	return err;
}

#ifdef CONFIG_PM
static void pch_lpc_resume(void)
{
	writel(pch_lpc_priv->saved_reg_pol, LS7A_LPC_INT_POL);
	writel(pch_lpc_priv->saved_reg_ctl, LS7A_LPC_INT_CTL);
	writel(pch_lpc_priv->saved_reg_ena, LS7A_LPC_INT_ENA);
}

static int pch_lpc_suspend(void)
{
	pch_lpc_priv->saved_reg_ctl = readl(LS7A_LPC_INT_CTL);
	pch_lpc_priv->saved_reg_ena = readl(LS7A_LPC_INT_ENA);
	pch_lpc_priv->saved_reg_pol = readl(LS7A_LPC_INT_POL);
	return 0;
}

#else
#define pch_lpc_suspend NULL
#define pch_lpc_resume NULL
#endif

static struct syscore_ops pch_lpc_syscore_ops = {
	.suspend = pch_lpc_suspend,
	.resume = pch_lpc_resume,
};

static int __init pch_lpc_init_syscore_ops(void)
{
	if (pch_lpc_priv)
		register_syscore_ops(&pch_lpc_syscore_ops);
	return 0;
}
device_initcall(pch_lpc_init_syscore_ops);
