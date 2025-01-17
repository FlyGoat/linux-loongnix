// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2020, Jiaxun Yang <jiaxun.yang@flygoat.com>
 *			lvjianmin <lvjianmin@loongson.cn>
 *  Loongson PCH MSI support
 */

#define pr_fmt(fmt) "pch-msi: " fmt

#include <linux/irqchip.h>
#include <linux/msi.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <boot_param.h>

#define MSI_IRQ_NR_64	64
#define MSI_IRQ_NR_192	192
#define MSI_IRQ_NR_224	224

struct pch_msi_data {
	struct mutex	msi_map_lock;
	u32		irq_first;	/* The vector number that MSIs starts */
	u32		num_irqs;	/* The number of vectors for MSIs */
	unsigned long	*msi_map;
	struct fwnode_handle *domain_handle;
} *pch_msi_priv;

static void pch_msi_mask_msi_irq(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void pch_msi_unmask_msi_irq(struct irq_data *d)
{
	irq_chip_unmask_parent(d);
	pci_msi_unmask_irq(d);
}

static void pch_msi_compose_msi_msg(struct irq_data *data,
					struct msi_msg *msg)
{
	msg->address_hi = loongson_sysconf.msi_address_hi;
	msg->address_lo = loongson_sysconf.msi_address_lo;
	msg->data = data->hwirq;
}

static struct irq_chip pch_msi_irq_chip = {
	.name			= "PCH-MSI-HT",
	.irq_mask		= pch_msi_mask_msi_irq,
	.irq_unmask		= pch_msi_unmask_msi_irq,
	.irq_ack		= irq_chip_ack_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_compose_msi_msg	= pch_msi_compose_msi_msg,
};

static int pch_msi_allocate_hwirq(struct pch_msi_data *priv, int num_req)
{
	int first;

	mutex_lock(&priv->msi_map_lock);

	first = bitmap_find_free_region(priv->msi_map, priv->num_irqs,
					get_count_order(num_req));
	if (first < 0) {
		mutex_unlock(&priv->msi_map_lock);
		return -ENOSPC;
	}

	mutex_unlock(&priv->msi_map_lock);
	return priv->irq_first + first;
}

static void pch_msi_free_hwirq(struct pch_msi_data *priv,
				int hwirq, int num_req)
{
	int first = hwirq - priv->irq_first;

	mutex_lock(&priv->msi_map_lock);
	bitmap_release_region(priv->msi_map, first, get_count_order(num_req));
	mutex_unlock(&priv->msi_map_lock);
}


static int msi_domain_ops_init(struct irq_domain *domain,
				struct msi_domain_info *info,
				unsigned int virq, irq_hw_number_t hwirq,
				msi_alloc_info_t *arg)
{
	irq_domain_set_hwirq_and_chip(domain, virq, arg->hwirq, info->chip,
					info->chip_data);
	return 0;
}

static void pci_msi_domain_set_desc(msi_alloc_info_t *arg, struct msi_desc *desc)
{
	arg->desc = desc;
	arg->hwirq = 0;
}

static struct msi_domain_ops pch_msi_domain_ops = {
	.msi_init	= msi_domain_ops_init,
	.set_desc	= pci_msi_domain_set_desc,
};

static struct msi_domain_info pch_msi_domain_info = {
	.flags	= MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		  MSI_FLAG_MULTI_PCI_MSI | MSI_FLAG_PCI_MSIX,
	.chip	= &pch_msi_irq_chip,
	.ops	= &pch_msi_domain_ops,
};

static struct irq_chip middle_irq_chip = {
	.name			= "PCH MSI",
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_ack		= irq_chip_ack_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_compose_msi_msg	= pch_msi_compose_msi_msg,
};

static int pch_msi_parent_domain_alloc(struct irq_domain *domain,
					unsigned int virq, int hwirq)
{
	struct irq_fwspec fwspec;
	int ret;

	fwspec.fwnode = domain->parent->fwnode;
	fwspec.param_count = 1;
	fwspec.param[0] = hwirq;
	ret = irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
	if (ret)
		return ret;

	return 0;
}

static int pch_msi_middle_domain_alloc(struct irq_domain *domain,
					unsigned int virq,
					unsigned int nr_irqs, void *args)
{
	struct pch_msi_data *priv = domain->host_data;
	int hwirq, err, i;
	msi_alloc_info_t *info = (msi_alloc_info_t *)args;

	hwirq = pch_msi_allocate_hwirq(priv, nr_irqs);
	if (hwirq < 0)
		return hwirq;
	for (i = 0; i < nr_irqs; i++) {
		err = pch_msi_parent_domain_alloc(domain, virq + i, hwirq + i);
		if (err)
			goto err_hwirq;

		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
						&middle_irq_chip, priv);
	}
	info->hwirq = hwirq;
	irq_set_noprobe(virq);
	return 0;

err_hwirq:
	pch_msi_free_hwirq(priv, hwirq, nr_irqs);
	irq_domain_free_irqs_parent(domain, virq, i - 1);

	return err;
}

static void pch_msi_middle_domain_free(struct irq_domain *domain,
					unsigned int virq,
					unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct pch_msi_data *priv = irq_data_get_irq_chip_data(d);

	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
	pch_msi_free_hwirq(priv, d->hwirq, nr_irqs);
}

static const struct irq_domain_ops pch_msi_middle_domain_ops = {
	.alloc	= pch_msi_middle_domain_alloc,
	.free	= pch_msi_middle_domain_free,
};

struct fwnode_handle *msi_irqdomain_handle(void)
{
	return pch_msi_priv ? pch_msi_priv->domain_handle : NULL;
}
static int pch_msi_init_domains(struct pch_msi_data *priv,
				struct irq_domain *parent,
				bool ext)
{
	struct irq_domain *middle_domain, *msi_domain;

	struct fwnode_handle *middle_handle;
	middle_handle = irq_domain_alloc_fwnode(priv);
	middle_domain = irq_domain_create_linear(middle_handle,
						priv->num_irqs,
						&pch_msi_middle_domain_ops,
						priv);
	if (!middle_domain) {
		pr_err("Failed to create the MSI middle domain\n");
		return -ENOMEM;
	}

	middle_domain->parent = parent;
	irq_domain_update_bus_token(middle_domain, DOMAIN_BUS_NEXUS);

	priv->domain_handle = irq_domain_alloc_fwnode(middle_domain);
	if (ext)
		pch_msi_domain_info.chip->name = "PCH-MSI-EXT";
	msi_domain = pci_msi_create_irq_domain(priv->domain_handle,
						&pch_msi_domain_info,
						middle_domain);
	if (!msi_domain) {
		pr_err("Failed to create PCI MSI domain\n");
		irq_domain_remove(middle_domain);
		return -ENOMEM;
	}

	return 0;
}

int pch_msi_init(struct irq_domain *parent_domain, bool ext, int model)
{
	struct pch_msi_data *priv;
	int ret;

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	mutex_init(&priv->msi_map_lock);
	if (model == PCH_IRQ_ROUTE_EXT_GUEST) {
		priv->irq_first = LOONGSON_GUEST_PCH_IRQ_BASE;
		priv->num_irqs = MSI_IRQ_NR_224;
	} else {
		priv->irq_first = LOONGSON_PCH_IRQ_BASE;
		if (loongson_cpu_has_msi256)
			priv->num_irqs = MSI_IRQ_NR_192;
		else
			priv->num_irqs = MSI_IRQ_NR_64;
	}
	priv->msi_map = bitmap_alloc(priv->num_irqs, GFP_KERNEL);
	if (!priv->msi_map) {
		ret = -ENOMEM;
		goto err_priv;
	}

	pr_debug("Registering %d MSIs, starting at %d\n",
		 priv->num_irqs, priv->irq_first);

	ret = pch_msi_init_domains(priv, parent_domain, ext);
	if (ret)
		goto err_map;
	pch_msi_priv = priv;
	return 0;

err_map:
	kfree(priv->msi_map);
err_priv:
	kfree(priv);
	return ret;
}

