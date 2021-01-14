/*
 *	Support irq for Loongson chipsets.
 *
 *	lvjianmin <lvjianmin@loongson.cn>
 *
 */

#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/stddef.h>
#include <linux/acpi.h>
#include <loongson.h>
#include <irq.h>
#include <linux/module.h>
#include <linux/syscore_ops.h>
#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>
#include <loongson.h>
#include <loongson-pch.h>
#include <linux/irqdomain.h>
#include <linux/irqchip.h>

#define	for_each_pch_pic(idx)		\
	for ((idx) = 0; (idx) < nr_pch_pics; (idx)++)
#define	for_each_pin(idx, pin)		\
	for ((pin) = 0; (pin) < pch_pics[(idx)].nr_registers; (pin)++)
#define	for_each_pch_pic_pin(idx, pin)	\
	for_each_pch_pic((idx))		\
		for_each_pin((idx), (pin))
extern const struct plat_smp_ops *mp_ops;
extern u64 acpi_liointc_addr;
extern void (*do_cascade)(void);
extern void do_pch(void);
enum pch_irq_route_model_id pch_irq_route_model = PCH_IRQ_ROUTE_END;
/* The one past the highest gsi number used */
u32 gsi_top;
int nr_pch_pics;
static struct pch_pic {
	/*
	 * # of IRQ routing registers
	 */
	int nr_registers;

	/* pch pic config */
	struct pch_pic_config config;
	/* pch pic gsi routing info */
	struct pch_pic_gsi  gsi_config;
} pch_pics[MAX_PCH_PICS];

static int __init pch_pic_get_redir_entries(int pch_pic)
{
	if (cpu_guestmode)
		return 32;
	return 64;
}
struct pch_pic_gsi *pch_pic_gsi_routing(int pch_pic_idx)
{
	return &pch_pics[pch_pic_idx].gsi_config;
}

int pch_pic_id(int pch_pic_idx)
{
	return pch_pics[pch_pic_idx].config.pch_pic_id;
}

unsigned long pch_pic_addr(int pch_pic_idx)
{
	return pch_pics[pch_pic_idx].config.pch_pic_addr;
}

static __init int bad_pch_pic(unsigned long address)
{
	if (nr_pch_pics >= MAX_PCH_PICS) {
		pr_warn("WARNING: Max # of I/O PCH_PICs (%d) exceeded (found %d), skipping\n",
			MAX_PCH_PICS, nr_pch_pics);
		return 1;
	}
	if (!address) {
		pr_warn("WARNING: Bogus (zero) I/O PCH_PIC address found in table, skipping!\n");
		return 1;
	}
	return 0;
}
#define pch_pic_ver(pch_pic_idx)	pch_pics[pch_pic_idx].config.pch_pic_ver
void __init register_pch_pic(int id, u64 address, u32 gsi_base)
{
	int idx = 0;
	int entries;
	struct pch_pic_gsi *gsi_cfg;

	if (bad_pch_pic(address))
		return;

	idx = nr_pch_pics;
	if (cpu_guestmode)
		pch_pics[idx].config.pch_pic_addr =
				LS7A_VZ_PCH_REG_BASE | address;
	else
		pch_pics[idx].config.pch_pic_addr = address;

	pch_pics[idx].config.pch_pic_id = id;
	pch_pics[idx].config.pch_pic_ver = 0;

	/*
	 * Build basic GSI lookup table to facilitate lookups
	 * and to prevent reprogramming of PCH_PIC pins (PCI GSIs).
	 */
	entries = pch_pic_get_redir_entries(idx);
	gsi_cfg = pch_pic_gsi_routing(idx);
	gsi_cfg->gsi_base = gsi_base;
	gsi_cfg->gsi_end = gsi_base + entries - 1;
	/*
	 * The number of PCH_PIC IRQ registers (== #pins):
	 */
	pch_pics[idx].nr_registers = entries;

	if (gsi_cfg->gsi_end >= gsi_top)
		gsi_top = gsi_cfg->gsi_end + 1;

	pr_info("PCH_PIC[%d]: pch_pic_id %d, version %d, address 0x%lx, GSI %d-%d\n",
		idx, pch_pic_id(idx),
		pch_pic_ver(idx), pch_pic_addr(idx),
		gsi_cfg->gsi_base, gsi_cfg->gsi_end);

	nr_pch_pics++;
}

int find_pch_pic(u32 gsi)
{
	int i;

	if (nr_pch_pics == 0)
		return -1;

	/* Find the PCH_PIC that manages this GSI. */
	for_each_pch_pic(i) {
		struct pch_pic_gsi *gsi_cfg = pch_pic_gsi_routing(i);
		if (gsi >= gsi_cfg->gsi_base && gsi <= gsi_cfg->gsi_end)
			return i;
	}

	printk(KERN_ERR "ERROR: Unable to locate PCH_PIC for GSI %d\n", gsi);
	return -1;
}

void handle_virq(unsigned int irq, unsigned int cpu)
{
	struct irq_data *irqd;
	struct cpumask affinity;

	/* handled by local core */
	if (ipi_irq2pos[irq] == -1) {
		generic_handle_irq(irq);
		return;
	}

	irqd = irq_get_irq_data(irq);
	cpumask_and(&affinity, irqd->common->affinity, cpu_active_mask);
	if (cpumask_empty(&affinity)) {
		generic_handle_irq(irq);
		return;
	}

	irq_cpu[irq] = cpumask_next(irq_cpu[irq], &affinity);
	if (irq_cpu[irq] >= nr_cpu_ids)
		irq_cpu[irq] = cpumask_first(&affinity);

	if (irq_cpu[irq] == cpu) {
		generic_handle_irq(irq);
		return;
	}

	/* balanced by other cores */
	mp_ops->send_ipi_single(irq_cpu[irq], (0x1 << (ipi_irq2pos[irq])) << IPI_IRQ_OFFSET);
}

void __init of_setup_pch_irqs(struct fwnode_handle *parent_handle, int model)
{
	unsigned int pch_pic, pin;
	struct device_node *of_node;
	struct pch_pic_gsi *gsi_cfg = pch_pic_gsi_routing(0);
	if (!gsi_cfg)
		return;

	of_node = of_find_compatible_node(NULL, NULL, "loongson,ls7a-interrupt-controller");
	if (of_node) {
		pch_pic_init(pch_pic_addr(0), parent_handle,
				model, gsi_cfg->gsi_base, of_node);
	}

	for_each_pch_pic_pin(pch_pic, pin) {
		struct irq_fwspec fwspec;
		fwspec.fwnode = NULL;
		fwspec.param[0] = pin + gsi_cfg->gsi_base;
		fwspec.param_count = 1;
		irq_create_fwspec_mapping(&fwspec);
	}
}
void __init setup_pch_irqs(struct fwnode_handle *parent_handle, int model)
{
	unsigned int pch_pic, pin;
	for_each_pch_pic(pch_pic) {
		struct pch_pic_gsi *gsi_cfg = pch_pic_gsi_routing(pch_pic);
		if (!gsi_cfg)
			continue;
#ifdef CONFIG_LOONGSON_PCH_PIC
		pch_pic_init(pch_pic_addr(pch_pic), parent_handle,
				model, gsi_cfg->gsi_base, NULL);
#endif
	}
	for_each_pch_pic_pin(pch_pic, pin) {
		struct pch_pic_gsi *gsi_cfg = pch_pic_gsi_routing(pch_pic);
		acpi_register_gsi(NULL, pin + gsi_cfg->gsi_base, -1, -1);
	}
}
extern struct system_loongson *esys;
static bool fw_support_fdt(void)
{
	return !!(esys && esys->vers >= 2 && esys->of_dtb_addr);
}

static void pch_pic_lpc_init(void)
{
	struct device_node *np;

	if (fw_support_fdt()) {
		np = of_find_compatible_node(NULL, NULL, "simple-bus");
		if (np) {
			if (of_property_read_bool(np, "enable-lpc-irq")) {
				if (of_property_read_bool(np, "lpc-irq-low")) {
					writel(0, LS7A_LPC_INT_POL);
				} else {
					writel(-1, LS7A_LPC_INT_POL);
				}

			}
		}
	}
}
extern void pci_no_msi(void);
enum pch_irq_route_model_id get_pch_irq_model(void)
{
	if ((loongson_cpu_has_msi128 || loongson_cpu_has_msi256)) {
		if (loongson_cpu_has_extioi) {
			if (cpu_guestmode)
				return PCH_IRQ_ROUTE_EXT_GUEST;
			else
				return PCH_IRQ_ROUTE_EXT;
		} else {
			return PCH_IRQ_ROUTE_HT;
		}
	} else {
		pci_no_msi();
		return PCH_IRQ_ROUTE_LINE;
	}
}

void __init setup_PCH_PIC(void)
{
	int i;
	struct fwnode_handle *pch_parent_handle;

	irq_alloc_descs(-1, MIPS_CPU_IRQ_BASE, 8, 0);
	for (i = MIPS_CPU_IRQ_BASE; i < MIPS_CPU_IRQ_BASE + 8; i++)
		irq_set_noprobe(i);
	do_cascade = do_pch;
	mips_cpu_irq_init();
#ifdef CONFIG_LOONGSON_LIOINTC
	if (!cpu_guestmode)
		liointc_init(acpi_liointc_addr, &liointc_handle);
#endif
	if (cpu_guestmode)
		writel(GUEST_NEW_IRQMODEL, LS7A_GUEST_IRQMODEL_REG);
	pch_irq_route_model = get_pch_irq_model();
	switch (pch_irq_route_model) {
	case PCH_IRQ_ROUTE_EXT:
	case PCH_IRQ_ROUTE_EXT_GUEST:
		pr_info("Support EXT interrupt.\n");
#ifdef CONFIG_LOONGSON_EXTIOI
		extioi_vec_init(&pch_parent_handle, pch_irq_route_model);
#endif
		if (acpi_disabled)
			of_setup_pch_irqs(pch_parent_handle, pch_irq_route_model);
		else
			setup_pch_irqs(pch_parent_handle, pch_irq_route_model);
		break;
	case PCH_IRQ_ROUTE_HT:
		pr_info("Support HT interrupt.\n");
#ifdef CONFIG_LOONGSON_HTVEC
		htvec_init(LOONGSON_HT1_INT_VECTOR_BASE, liointc_handle, &pch_parent_handle);
#endif
		if (acpi_disabled)
			of_setup_pch_irqs(pch_parent_handle, pch_irq_route_model);
		else
			setup_pch_irqs(pch_parent_handle, pch_irq_route_model);
		break;
	case PCH_IRQ_ROUTE_LINE:
		pr_info("Support LINE interrupt.\n");
		if (acpi_disabled)
			of_setup_pch_irqs(pch_parent_handle, pch_irq_route_model);
		else
			setup_pch_irqs(liointc_handle, pch_irq_route_model);
		break;
	default:
		BUG_ON(true);
	}
	if (!cpu_guestmode)
		pch_pic_lpc_init();
}
