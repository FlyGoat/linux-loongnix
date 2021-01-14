// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2020, Jiaxun Yang <jiaxun.yang@flygoat.com>
 *			Jianmin Lv <lvjianmin@loongson.cn>
 *  Loongson PCH PIC support
 */

#define pr_fmt(fmt) "pch-pic: " fmt

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/syscore_ops.h>
#include <loongson.h>

/* Registers */
#define PCH_PIC_MASK		0x20
#define PCH_PIC_HTMSI_EN	0x40
#define PCH_PIC_EDGE		0x60
#define PCH_PIC_CLR		0x80
#define PCH_PIC_AUTO0		0xc0
#define PCH_PIC_AUTO1		0xe0
#define PCH_INT_ROUTE(irq)	(0x100 + irq)
#define PCH_INT_HTVEC(irq)	(0x200 + irq)
#define PCH_PIC_POL		0x3e0
#define PCH_PIC_STS		0x3a0

#define LIOINTC_PIN_SYSINT0	0
#define PIC_COUNT_PER_REG	64
#define PIC_REG_COUNT		1
#define PIC_COUNT		(PIC_COUNT_PER_REG * PIC_REG_COUNT)
#define GUEST_PIC_COUNT		32
#define PIC_REG_IDX(irq_id)	((irq_id) / PIC_COUNT_PER_REG)
#define PIC_REG_BIT(irq_id)	((irq_id) % PIC_COUNT_PER_REG)
#define PCH_PIC_SIZE		0x400
#define PCH_PIC_GSI_BASE	64
#define LS7A_VZ_PCH_REG_BASE		0xe0010000000UL

static int pch_cnt;
struct pch_pic {
	void __iomem		*base;
	struct fwnode_handle	*domain_handle;
	struct irq_domain	*pic_domain;
	u64			saved_vec_en[PIC_REG_COUNT];
	u64			saved_vec_edge[PIC_REG_COUNT];
	u64			saved_vec_pol[PIC_REG_COUNT];
	int			model;
	int			gsi_base;
	raw_spinlock_t		pic_lock;
} *pch_pic_priv[MAX_PCH_PICS];

static void pch_pic_bitset(struct pch_pic *priv, int offset, int bit)
{
	u64 reg;
	unsigned long flags;
	void __iomem *addr = priv->base + offset + PIC_REG_IDX(bit) * 8;

	raw_spin_lock_irqsave(&priv->pic_lock, flags);
	reg = readq(addr);
	reg |= BIT(PIC_REG_BIT(bit));
	writeq(reg, addr);
	raw_spin_unlock_irqrestore(&priv->pic_lock, flags);
}

static void pch_pic_bitclr(struct pch_pic *priv, int offset, int bit)
{
	u64 reg;
	unsigned long flags;
	void __iomem *addr = priv->base + offset + PIC_REG_IDX(bit) * 8;

	raw_spin_lock_irqsave(&priv->pic_lock, flags);
	reg = readq(addr);
	reg &= ~BIT(PIC_REG_BIT(bit));
	writeq(reg, addr);
	raw_spin_unlock_irqrestore(&priv->pic_lock, flags);
}

static void pch_pic_mask_irq(struct irq_data *d)
{
	struct pch_pic *priv = irq_data_get_irq_chip_data(d);

	pch_pic_bitset(priv, PCH_PIC_MASK, d->hwirq);
	irq_chip_mask_parent(d);
}

static void pch_pic_unmask_irq(struct irq_data *d)
{
	struct pch_pic *priv = irq_data_get_irq_chip_data(d);
	u32 idx = PIC_REG_IDX(d->hwirq);

	irq_chip_unmask_parent(d);
	writeq(BIT(PIC_REG_BIT(d->hwirq)),
			priv->base + PCH_PIC_CLR + idx * 8);
	pch_pic_bitclr(priv, PCH_PIC_MASK, d->hwirq);
}

static void pch_line_mask_irq(struct irq_data *d)
{
	struct pch_pic *priv = irq_data_get_irq_chip_data(d);

	pch_pic_bitset(priv, PCH_PIC_MASK, d->hwirq);
}

static void pch_line_unmask_irq(struct irq_data *d)
{
	struct pch_pic *priv = irq_data_get_irq_chip_data(d);

	pch_pic_bitclr(priv, PCH_PIC_MASK, d->hwirq);
}
static int pch_pic_set_type(struct irq_data *d, unsigned int type)
{
	struct pch_pic *priv = irq_data_get_irq_chip_data(d);
	int ret = 0;

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		pch_pic_bitset(priv, PCH_PIC_EDGE, d->hwirq);
		pch_pic_bitclr(priv, PCH_PIC_POL, d->hwirq);
		break;
	case IRQ_TYPE_EDGE_FALLING:
		pch_pic_bitset(priv, PCH_PIC_EDGE, d->hwirq);
		pch_pic_bitset(priv, PCH_PIC_POL, d->hwirq);
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		pch_pic_bitclr(priv, PCH_PIC_EDGE, d->hwirq);
		pch_pic_bitclr(priv, PCH_PIC_POL, d->hwirq);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		pch_pic_bitclr(priv, PCH_PIC_EDGE, d->hwirq);
		pch_pic_bitset(priv, PCH_PIC_POL, d->hwirq);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static unsigned int pch_line_startup(struct irq_data *d)
{
	create_ipi_dirq(d->irq);
	pch_line_unmask_irq(d);
	return 0;
}

static void pch_line_shutdown(struct irq_data *d)
{
	pch_line_mask_irq(d);
	destroy_ipi_dirq(d->irq);
}
static unsigned int pch_pic_startup(struct irq_data *d)
{
	create_ipi_dirq(d->irq);
	pch_pic_unmask_irq(d);
	return 0;
}

static void pch_pic_shutdown(struct irq_data *d)
{
	pch_pic_mask_irq(d);
	destroy_ipi_dirq(d->irq);
}

static struct irq_chip pch_line_irq_chip = {
	.name			= "PCH-PIC-LINE",
	.irq_mask		= pch_line_mask_irq,
	.irq_unmask		= pch_line_unmask_irq,
	.irq_set_type		= pch_pic_set_type,
	.irq_startup		= pch_line_startup,
	.irq_shutdown		= pch_line_shutdown,
	.irq_set_affinity	= def_set_irq_affinity,
	.flags			= IRQCHIP_SKIP_SET_WAKE,
};

static struct irq_chip pch_pic_irq_chip = {
	.name			= "PCH-PIC-HT",
	.irq_mask		= pch_pic_mask_irq,
	.irq_unmask		= pch_pic_unmask_irq,
	.irq_ack		= irq_chip_ack_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_set_type		= pch_pic_set_type,
	.irq_startup		= pch_pic_startup,
	.irq_shutdown		= pch_pic_shutdown,
	.flags			= IRQCHIP_SKIP_SET_WAKE,
};

static void pch_handle_irq(struct irq_desc *desc)
{
	struct pch_pic *priv = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int cpu = smp_processor_id();
	u64 pending;

	chained_irq_enter(chip, desc);

	pending = readq(priv->base + PCH_PIC_STS);

	if (!pending)
		spurious_interrupt();

	while (pending) {
		int bit = __ffs(pending);
		int virq = irq_linear_revmap(priv->pic_domain, bit);
		if (virq > 0) handle_virq(virq, cpu);
		pending &= ~BIT(bit);
	}
	chained_irq_exit(chip, desc);
}

static int pch_pic_domain_translate(struct irq_domain *d,
					struct irq_fwspec *fwspec,
					unsigned long *hwirq,
					unsigned int *type)
{
	struct pch_pic *priv = d->host_data;
	if (fwspec->param_count < 1)
		return -EINVAL;
	*hwirq = fwspec->param[0] - priv->gsi_base;
	*type = IRQ_TYPE_NONE;
	return 0;
}

static int pch_line_alloc(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	unsigned int type, i;
	unsigned long hwirq = 0;
	struct htvec *priv = domain->host_data;

	pch_pic_domain_translate(domain, arg, &hwirq, &type);
	for (i = 0; i < nr_irqs; i++) {
		irq_domain_set_info(domain, virq + i, hwirq + i, &pch_line_irq_chip,
					priv, handle_level_irq, NULL, NULL);
	}

	irq_set_noprobe(virq);
	return 0;
}

static int pch_pic_alloc(struct irq_domain *domain, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	int err;
	unsigned int type;
	struct irq_fwspec fwspec;
	struct pch_pic *priv = domain->host_data;
	unsigned long hwirq = 0;

	pch_pic_domain_translate(domain, arg, &hwirq, &type);

	fwspec.fwnode = domain->parent->fwnode;
	fwspec.param_count = 1;
	fwspec.param[0] = hwirq;
	err = irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
	if (err)
		return err;
	irq_domain_set_info(domain, virq, hwirq,
				&pch_pic_irq_chip, priv,
				handle_level_irq, NULL, NULL);
	irq_set_noprobe(virq);

	return 0;
}
static const struct irq_domain_ops pch_line_domain_ops = {
	.translate	= pch_pic_domain_translate,
	.alloc		= pch_line_alloc,
	.free		= irq_domain_free_irqs_parent,
};
static const struct irq_domain_ops pch_pic_domain_ops = {
	.translate	= pch_pic_domain_translate,
	.alloc		= pch_pic_alloc,
	.free		= irq_domain_free_irqs_parent,
};

static void pch_pic_reset(struct pch_pic *priv)
{
	int i;

	for (i = 0; i < PIC_COUNT; i++) {
		if (priv->model != PCH_IRQ_ROUTE_LINE) {
			/* Write vector ID */
			writeb(i, priv->base + PCH_INT_HTVEC(i));
		}
		/* Hardcode route to HT0 Lo */
		writeb(1, priv->base + PCH_INT_ROUTE(i));
	}

	for (i = 0; i < PIC_REG_COUNT; i++) {
		/* Clear IRQ cause registers, mask all interrupts */
		writeq_relaxed((u64)-1, priv->base + PCH_PIC_MASK + 8 * i);
		writeq_relaxed((u64)-1, priv->base + PCH_PIC_CLR + 8 * i);
		/* Clear auto bounce, we don't need that */
		writeq_relaxed(0, priv->base + PCH_PIC_AUTO0 + 8 * i);
		writeq_relaxed(0, priv->base + PCH_PIC_AUTO1 + 8 * i);
		if (priv->model != PCH_IRQ_ROUTE_LINE) {
			/* Enable HTMSI transformer */
			writeq_relaxed((u64)-1, priv->base + PCH_PIC_HTMSI_EN + 8 * i);
		}
	}
}

struct fwnode_handle *pch_irqdomain_handle(int id)
{
	return pch_pic_priv[id] ? pch_pic_priv[id]->domain_handle : NULL;
}

int pch_pic_init(u64 base_addr, struct fwnode_handle *parent_handle,
		int model, int gsi_base, struct device_node *of_node)
{
	struct pch_pic *priv;
	struct device_node __maybe_unused *np;
	int err;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	raw_spin_lock_init(&priv->pic_lock);
	if (model == PCH_IRQ_ROUTE_EXT_GUEST) {
		priv->base = ioremap(base_addr | LS7A_VZ_PCH_REG_BASE,
							PCH_PIC_SIZE);
	} else {
		priv->base = ioremap(base_addr, PCH_PIC_SIZE);
	}
	if (!priv->base) {
		err = -ENOMEM;
		goto free_priv;
	}

	if (!of_node) {
		priv->domain_handle = irq_domain_alloc_fwnode(priv->base);
		if (!priv->domain_handle) {
			pr_err("Unable to allocate domain handle\n");
			err = -ENOMEM;
			goto iounmap_base;
		}
	} else {
		priv->domain_handle = of_node_to_fwnode(of_node);
	}

	priv->model = model;
	priv->gsi_base = gsi_base;
	if (priv->model == PCH_IRQ_ROUTE_LINE) {
		priv->pic_domain = irq_domain_create_linear(priv->domain_handle,
							PIC_COUNT,
							&pch_line_domain_ops,
							priv);
	} else if (priv->model == PCH_IRQ_ROUTE_HT ||
			priv->model == PCH_IRQ_ROUTE_EXT) {
		struct irq_domain *parent_domain = irq_find_matching_fwnode(
							parent_handle, DOMAIN_BUS_ANY);
		if (!parent_domain) {
			pr_err("Failed to find the parent domain\n");
			err = -ENODEV;
			goto iounmap_base;
		}
		if (priv->model == PCH_IRQ_ROUTE_EXT)
			pch_pic_irq_chip.name = "PCH-PIC-EXT";

		priv->pic_domain = irq_domain_create_hierarchy(parent_domain, 0,
							PIC_COUNT,
							priv->domain_handle,
							&pch_pic_domain_ops,
							priv);
	} else if (priv->model == PCH_IRQ_ROUTE_EXT_GUEST) {
		struct irq_domain *parent_domain = irq_find_matching_fwnode(
						parent_handle, DOMAIN_BUS_ANY);
		if (!parent_domain) {
			pr_err("Failed to find the parent domain\n");
			err = -ENODEV;
			goto iounmap_base;
		}
		pch_pic_irq_chip.name = "PCH-PIC-EXT";

		priv->pic_domain = irq_domain_create_hierarchy(parent_domain, 0,
							GUEST_PIC_COUNT,
							priv->domain_handle,
							&pch_pic_domain_ops,
							priv);

	}

	if (!priv->pic_domain) {
		pr_err("Failed to create IRQ domain\n");
		err = -ENOMEM;
		goto iounmap_base;
	}
	if (!pch_cnt)
		irq_set_default_host(priv->pic_domain);

	if (priv->model == PCH_IRQ_ROUTE_LINE) {
		struct irq_fwspec fwspec;
		int parent_irq;
		fwspec.fwnode = parent_handle;
		fwspec.param[0] = LIOINTC_PIN_SYSINT0;
		fwspec.param_count = 1;
		parent_irq = irq_create_fwspec_mapping(&fwspec);
		irq_set_chained_handler_and_data(parent_irq,
						 pch_handle_irq, priv);
	}
	pch_pic_reset(priv);
#ifdef CONFIG_LOONGSON_PCH_LPC
	if (!pch_cnt) {
		np = of_find_compatible_node(NULL, NULL, "simple-bus");
		if (np) {
			if (of_property_read_bool(np, "enable-lpc-irq"))
				pch_lpc_init(priv->domain_handle);
		} else {
			pch_lpc_init(priv->domain_handle);
		}
	}
#endif
	pch_pic_priv[pch_cnt++] = priv;
	return 0;

iounmap_base:
	iounmap(priv->base);
free_priv:
	kfree(priv);

	return err;
}
static int pch_pic_of_init(struct device_node *node,
				struct device_node *parent)
{
	return 0;
}

#ifdef CONFIG_PM
static void pch_pic_resume(void)
{
	int i, j;
	for (j = 0; j < pch_cnt; j++) {
		pch_pic_reset(pch_pic_priv[j]);
		for (i = 0; i < PIC_REG_COUNT; i++) {
			writeq(pch_pic_priv[j]->saved_vec_en[i],
					pch_pic_priv[j]->base + PCH_PIC_MASK + 8 * i);
			writeq(pch_pic_priv[j]->saved_vec_edge[i],
					pch_pic_priv[j]->base + PCH_PIC_EDGE + 8 * i);
			writeq(pch_pic_priv[j]->saved_vec_pol[i],
					pch_pic_priv[j]->base + PCH_PIC_POL + 8 * i);
		}
	}
}

static int pch_pic_suspend(void)
{
	int i, j;
	for (j = 0; j < pch_cnt; j++)
		for (i = 0; i < PIC_REG_COUNT; i++) {
			pch_pic_priv[j]->saved_vec_en[i] =
				readq(pch_pic_priv[j]->base + PCH_PIC_MASK + 8 * i);
			pch_pic_priv[j]->saved_vec_edge[i] =
				readq(pch_pic_priv[j]->base + PCH_PIC_EDGE + 8 * i);
			pch_pic_priv[j]->saved_vec_pol[i] =
				readq(pch_pic_priv[j]->base + PCH_PIC_POL + 8 * i);
		}
	return 0;
}

#else
#define pch_pic_suspend NULL
#define pch_pic_resume NULL
#endif

static struct syscore_ops pch_pic_syscore_ops = {
	.suspend =  pch_pic_suspend,
	.resume =  pch_pic_resume,
};

static int __init pch_pic_init_syscore_ops(void)
{
	if (pch_cnt > 0)
		register_syscore_ops(&pch_pic_syscore_ops);
	return 0;
}
device_initcall(pch_pic_init_syscore_ops);
IRQCHIP_DECLARE(pch_pic, "loongson,ls7a-interrupt-controller", pch_pic_of_init);
