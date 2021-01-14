// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2020, Jiaxun Yang <jiaxun.yang@flygoat.com>
 *			Jianmin Lv <lvjianmin@loongson.cn>
 *  Loongson HyperTransport Interrupt Vector support
 */

#define pr_fmt(fmt) "htvec: " fmt

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/syscore_ops.h>

/* Registers */
#define HTVEC_EN_OFF		0x20
#define HTVEC_REG_COUNT_V0	2
#define HTVEC_REG_COUNT_V1	4
#define VEC_COUNT_PER_REG	64
#define VEC_REG_IDX(irq_id)	((irq_id) / VEC_COUNT_PER_REG)
#define VEC_REG_BIT(irq_id)	((irq_id) % VEC_COUNT_PER_REG)

struct htvec {
	void __iomem		*base;
	struct irq_domain	*htvec_domain;
	u64			saved_vec_en[HTVEC_REG_COUNT_V1];
	int			vec_reg_count;
	struct fwnode_handle	*domain_handle;
} *htvec_priv;

static void htvec_irq_dispatch(struct irq_desc *desc)
{
	int i;
	u64 pending;
	bool handled = false;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct htvec *priv = irq_desc_get_handler_data(desc);
	int cpu = smp_processor_id();
	chained_irq_enter(chip, desc);

	for (i = 0; i < priv->vec_reg_count; i++) {
		pending = readq(priv->base + 8 * i);
		writeq(pending, priv->base + 8 * i);
		while (pending) {
			int bit = __ffs(pending);
			int virq = irq_linear_revmap(priv->htvec_domain,
					bit + VEC_COUNT_PER_REG * i);
			if (virq > 0) handle_virq(virq, cpu);
			pending &= ~BIT(bit);
			handled = true;
		}
	}

	chained_irq_exit(chip, desc);
}

static void htvec_ack_irq(struct irq_data *d)
{
}

static void htvec_mask_irq(struct irq_data *d)
{
}

static void htvec_unmask_irq(struct irq_data *d)
{
}

static struct irq_chip htvec_irq_chip = {
	.name			= "LOONGSON_HTVEC",
	.irq_mask		= htvec_mask_irq,
	.irq_unmask		= htvec_unmask_irq,
	.irq_ack		= htvec_ack_irq,
	.irq_set_affinity	= def_set_irq_affinity,
};

static int htvec_domain_translate(struct irq_domain *d,
					struct irq_fwspec *fwspec,
					unsigned long *hwirq,
					unsigned int *type)
{
	if (fwspec->param_count < 1)
		return -EINVAL;
	*hwirq = fwspec->param[0];
	*type = IRQ_TYPE_NONE;
	return 0;
}

static int htvec_domain_alloc(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	unsigned int type, i;
	unsigned long hwirq = 0;
	struct htvec *priv = domain->host_data;

	htvec_domain_translate(domain, arg, &hwirq, &type);
	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(domain, virq + i, hwirq + i, &htvec_irq_chip,
					priv, handle_edge_irq, NULL, NULL);
	}

	return 0;
}

static void htvec_domain_free(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

		irq_set_handler(virq + i, NULL);
		irq_domain_reset_irq_data(d);
	}
}

static const struct irq_domain_ops htvec_domain_ops = {
	.translate	= htvec_domain_translate,
	.alloc		= htvec_domain_alloc,
	.free		= htvec_domain_free,
};

static void htvec_reset(struct htvec *priv)
{
	u32 idx;

	/* Clear IRQ cause registers, mask all interrupts */
	for (idx = 0; idx < priv->vec_reg_count; idx++) {
		writeq_relaxed((u64)-1, priv->base + 8 * idx);
		writeq_relaxed((u64)-1, priv->base + HTVEC_EN_OFF + 8 * idx);
	}
}

int htvec_init(u64 base_addr, struct fwnode_handle *parent_handle, struct fwnode_handle **handle)
{
	int err, parent_irq[HTVEC_REG_COUNT_V1 * 2], num_parents = 0, i;
	int num_vec;
	struct irq_fwspec fwspec;

	htvec_priv = kzalloc(sizeof(*htvec_priv), GFP_KERNEL);
	if (!htvec_priv)
		return -ENOMEM;

	htvec_priv->base = (void __iomem *)base_addr;
	if (loongson_cpu_has_msi256)
		num_parents = HTVEC_REG_COUNT_V1 * 2;
	else
		num_parents = HTVEC_REG_COUNT_V0 * 2;

	htvec_priv->vec_reg_count = num_parents >> 1;
	num_vec = htvec_priv->vec_reg_count * VEC_COUNT_PER_REG;
	/* Interrupt may come from any of the 4 interrupt line */
	for (i = 0; i < num_parents; i++) {
		parent_irq[i] = LOONGSON_CPU_HT1_VEC + i;
	}

	htvec_priv->domain_handle = irq_domain_alloc_fwnode(htvec_priv->base);
	if (!htvec_priv->domain_handle) {
		pr_err("Unable to allocate domain handle\n");
		err = -ENOMEM;
		goto iounmap_base;
	}

	/* Setup IRQ domain */
	htvec_priv->htvec_domain = irq_domain_create_linear(htvec_priv->domain_handle, num_vec,
					&htvec_domain_ops, htvec_priv);
	if (!htvec_priv->htvec_domain) {
		pr_err("loongson-htvec: cannot add IRQ domain\n");
		err = -ENOMEM;
		goto iounmap_base;
	}

	htvec_reset(htvec_priv);

	for (i = 0; i < num_parents; i++) {
		fwspec.fwnode = parent_handle;
		fwspec.param[0] = parent_irq[i];
		fwspec.param_count = 1;
		parent_irq[i] = irq_create_fwspec_mapping(&fwspec);
		irq_set_chained_handler_and_data(parent_irq[i],
						 htvec_irq_dispatch, htvec_priv);
	}
#ifdef CONFIG_LOONGSON_PCH_MSI
	pch_msi_init(htvec_priv->htvec_domain, false, PCH_IRQ_ROUTE_HT);
#endif
	if (handle)
		*handle = htvec_priv->domain_handle;
	return 0;

iounmap_base:
	iounmap(htvec_priv->base);
	htvec_priv->domain_handle = NULL;
	kfree(htvec_priv);

	return err;
}

#ifdef CONFIG_PM
static void htvec_irq_resume(void)
{
	int i;
	for (i = 0; i < htvec_priv->vec_reg_count; i++) {
		writeq(htvec_priv->saved_vec_en[i],
				htvec_priv->base + HTVEC_EN_OFF + 8 * i);
	}
}

static int htvec_irq_suspend(void)
{
	int i;
	for (i = 0; i < htvec_priv->vec_reg_count; i++) {
		htvec_priv->saved_vec_en[i] = readq(htvec_priv->base + HTVEC_EN_OFF + 8 * i);
	}
	return 0;
}

#else
#define htvec_irq_suspend NULL
#define htvec_irq_resume NULL
#endif

static struct syscore_ops htvec_irq_syscore_ops = {
	.suspend = htvec_irq_suspend,
	.resume = htvec_irq_resume,
};

static int __init htvec_init_syscore_ops(void)
{
	if (htvec_priv)
		register_syscore_ops(&htvec_irq_syscore_ops);
	return 0;
}
device_initcall(htvec_init_syscore_ops);
