/*
 *  Loongson-3A/3B/7A GPIO Support
 *
 *  Copyright (c) 2018 Juxin Gao <gaojuxin@loongson.cn>
 *  Copyright (c) 2019 Weihui Wang <wangweihui@loongson.cn>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#include <linux/acpi.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/platform_device.h>
#include <linux/bitops.h>
#include <asm/types.h>
#include <loongson-pch.h>

#define LOONGSON_GPIO_IN_OFFSET	16
#define GPIO_IO_CONF(x)	                (x->base + x->conf_offset)
#define GPIO_OUT(x)	                (x->base + x->out_offset)
#define GPIO_IN(x)	                (x->base + x->in_offset)
#define GPIO_IO_CONF_BYTE(x, gpio)      (x->base + x->conf_offset + gpio)
#define GPIO_OUT_BYTE(x, gpio)          (x->base + x->out_offset + gpio)
#define GPIO_IN_BYTE(x, gpio)           (x->base + x->in_offset + gpio)
#define LS7A_DC_GPIO_IO_REG(x) \
                (LS7A_DC_CNT_REG_BASE + x->in_offset)
#define LS7A_DC_GPIO_CONF_REG(x) \
                (LS7A_DC_CNT_REG_BASE + x->conf_offset)

struct loongson_gpio_chip {
	struct gpio_chip	chip;
	spinlock_t		lock;
	void __iomem		*base;
	int conf_offset;
	int out_offset;
	int in_offset;
};

/*
 * GPIO primitives.
 */
static int loongson_gpio_request(struct gpio_chip *chip, unsigned pin)
{
	if (pin >= (chip->ngpio + chip->base))
		return -EINVAL;
	else
		return 0;
}

static inline void
__set_direction(struct loongson_gpio_chip *lgpio, unsigned pin, int input)
{
	u32 temp;
	u8  value;
	u64 u;
	unsigned long flags;

	if (!strcmp(lgpio->chip.label,"loongson-gpio")){
		temp = readl(GPIO_IO_CONF(lgpio));
		if (input)
			temp |= 1 << pin;
		else
			temp &= ~(1 << pin);
		writel(temp, GPIO_IO_CONF(lgpio));
		return ;
	}
	if (!strcmp(lgpio->chip.label,"loongson,ls7a-gpio") ||
			!strncmp(lgpio->chip.label, "LOON0002", 8)){
		if (input)
			value = 1;
		else
			value = 0;
		ls7a_writeb(value, (unsigned long)GPIO_IO_CONF_BYTE(lgpio, pin));
		return ;
	}

	ls7a_read(u, (unsigned long)LS7A_DC_GPIO_CONF_REG(lgpio));
	if (input)
		u |= 1UL << pin;
	else
		u &= ~(1UL << pin);
	ls7a_write(u, (unsigned long)LS7A_DC_GPIO_CONF_REG(lgpio));
}

static void __set_level(struct loongson_gpio_chip *lgpio, unsigned pin, int high)
{
	u32 temp;
	u8 value;
	u64 u;
	unsigned long flags;

	/* If GPIO controller is on 3A,then... */
	if (!strcmp(lgpio->chip.label,"loongson-gpio")){
		temp = readl(GPIO_OUT(lgpio));
		if (high)
			temp |= 1 << pin;
		else
			temp &= ~(1 << pin);
		writel(temp, GPIO_OUT(lgpio));
		return;
	}

	if (!strcmp(lgpio->chip.label,"loongson,ls7a-gpio") ||
			!strncmp(lgpio->chip.label,"LOON0002", 8)){
	        if (high)
		        value = 1;
	        else
		        value = 0;
	        ls7a_writeb(value, (unsigned long)GPIO_OUT_BYTE(lgpio, pin));
		return;
	}

	ls7a_read(u, (unsigned long)LS7A_DC_GPIO_IO_REG(lgpio));
	if (high)
		u |= 1UL << pin;
	else
		u &= ~(1UL << pin);
	ls7a_write(u, (unsigned long)LS7A_DC_GPIO_IO_REG(lgpio));
}

static int loongson_gpio_direction_input(struct gpio_chip *chip, unsigned pin)
{
	unsigned long flags;
	struct loongson_gpio_chip *lgpio =
		container_of(chip, struct loongson_gpio_chip, chip);

	spin_lock_irqsave(&lgpio->lock, flags);
	__set_direction(lgpio, pin, 1);
	spin_unlock_irqrestore(&lgpio->lock, flags);

	return 0;
}

static int loongson_gpio_direction_output(struct gpio_chip *chip,
		unsigned pin, int value)
{
	struct loongson_gpio_chip *lgpio =
		container_of(chip, struct loongson_gpio_chip, chip);
	unsigned long flags;

	spin_lock_irqsave(&lgpio->lock, flags);
	__set_level(lgpio, pin, value);
	__set_direction(lgpio, pin, 0);
	spin_unlock_irqrestore(&lgpio->lock, flags);

	return 0;
}

static int loongson_gpio_get(struct gpio_chip *chip, unsigned pin)
{
	struct loongson_gpio_chip *lgpio =
		container_of(chip, struct loongson_gpio_chip, chip);
	u64 val;
	u32 temp;
  	unsigned long flags;
        u8  value;

	/* GPIO controller in 3A is different for 7A */
	if (!strcmp(lgpio->chip.label,"loongson-gpio")){
		temp = readl(GPIO_IN(lgpio));
		return ((temp & (1 << (pin + LOONGSON_GPIO_IN_OFFSET))) != 0);
	}

	if (!strcmp(lgpio->chip.label,"loongson,ls7a-gpio") ||
			!strncmp(lgpio->chip.label, "LOON0002", 8)){
	        value = ls7a_readb((unsigned long)GPIO_OUT_BYTE(lgpio, pin));
                return (value & 1);
        }

	ls7a_read(val, (unsigned long)LS7A_DC_GPIO_IO_REG(lgpio));
	return (val >> pin) & 1;
}

static void loongson_gpio_set(struct gpio_chip *chip, unsigned pin, int value)
{
	struct loongson_gpio_chip *lgpio =
		container_of(chip, struct loongson_gpio_chip, chip);
	unsigned long flags;

	spin_lock_irqsave(&lgpio->lock, flags);
	__set_level(lgpio, pin, value);
	spin_unlock_irqrestore(&lgpio->lock, flags);
}

static int loongson_gpio_init(struct device *dev, struct loongson_gpio_chip *lgpio, struct device_node *np,
			    void __iomem *base)
{
	lgpio->chip.request = loongson_gpio_request;
	lgpio->chip.direction_input = loongson_gpio_direction_input;
	lgpio->chip.get = loongson_gpio_get;
	lgpio->chip.direction_output = loongson_gpio_direction_output;
	lgpio->chip.set = loongson_gpio_set;
	lgpio->chip.can_sleep = 0;
	lgpio->chip.of_node = np;
	lgpio->chip.parent = dev;
	spin_lock_init(&lgpio->lock);
	lgpio->base = (void __iomem *)base;

	gpiochip_add(&lgpio->chip);

	return 0;
}


static void of_loongson_gpio_get_props(struct device_node *np,
				  struct loongson_gpio_chip *lgpio)
{
	const char *name;

	of_property_read_u32(np, "ngpios", (u32 *)&lgpio->chip.ngpio);
	of_property_read_u32(np, "gpio_base", (u32 *)&lgpio->chip.base);
	of_property_read_u32(np, "conf_offset", (u32 *)&lgpio->conf_offset);
	of_property_read_u32(np, "out_offset", (u32 *)&lgpio->out_offset);
	of_property_read_u32(np, "in_offset", (u32 *)&lgpio->in_offset);
	of_property_read_string(np, "compatible", &name);
	lgpio->chip.label = kstrdup(name, GFP_KERNEL);
}

static void acpi_loongson_gpio_get_props(struct platform_device *pdev,
				  struct loongson_gpio_chip *lgpio)
{

	struct device *dev = &pdev->dev;

	device_property_read_u32(dev, "ngpios", (u32 *)&lgpio->chip.ngpio);
	device_property_read_u32(dev, "gpio_base", (u32 *)&lgpio->chip.base);
	device_property_read_u32(dev, "conf_offset", (u32 *)&lgpio->conf_offset);
	device_property_read_u32(dev, "out_offset", (u32 *)&lgpio->out_offset);
	device_property_read_u32(dev, "in_offset", (u32 *)&lgpio->in_offset);
	lgpio->chip.label = kstrdup(pdev->name, GFP_KERNEL);
}

static void platform_loongson_gpio_get_props(struct platform_device *pdev,
				  struct loongson_gpio_chip *lgpio)
{
	struct platform_gpio_data *gpio_data =
		(struct platform_gpio_data *)pdev->dev.platform_data;

	lgpio->chip.ngpio = gpio_data->ngpio;
	lgpio->chip.base = gpio_data->gpio_base;
	lgpio->conf_offset = gpio_data->gpio_conf;
	lgpio->out_offset = gpio_data->gpio_out;
	lgpio->in_offset = gpio_data->gpio_in;
	lgpio->chip.label = kstrdup(pdev->name, GFP_KERNEL);
}

static int loongson_gpio_probe(struct platform_device *pdev)
{
	struct resource *iores;
	void __iomem *base;
	int ret = 0;
	struct loongson_gpio_chip *lgpio;
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	bool is_dc_gpio = 0;

	lgpio = kzalloc(sizeof(struct loongson_gpio_chip), GFP_KERNEL);
	if (!lgpio)
		return -ENOMEM;

	if (np){
		of_loongson_gpio_get_props(np,lgpio);

		if (!strcmp(lgpio->chip.label, "loongson,ls7a-dc-gpio")){
			is_dc_gpio = 1;
		}
	} else if (ACPI_COMPANION(&pdev->dev)) {
		acpi_loongson_gpio_get_props(pdev,lgpio);
		if (!strncmp(lgpio->chip.label, "LOON0003", 8)){
			is_dc_gpio = 1;
		}
	} else {
		platform_loongson_gpio_get_props(pdev,lgpio);
	}

	if (is_dc_gpio) {
		base = (void *)TO_UNCAC(LS7A_DC_CNT_REG_BASE);
		if (!base) {
			ret = -ENOMEM;
			goto out;
		}
	} else {
		iores = platform_get_resource(pdev,IORESOURCE_MEM, 0);
		if (!iores) {
			ret = -ENODEV;
			goto out;
		}
		if (!request_mem_region(iores->start, resource_size(iores),
					pdev->name)) {
			ret = -EBUSY;
			goto out;
		}
		base = ioremap(iores->start, resource_size(iores));
		if (!base) {
			ret = -ENOMEM;
			goto out;
		}
	}
	platform_set_drvdata(pdev, lgpio);
	loongson_gpio_init(dev,lgpio, np, base);

	return 0;
out:
	pr_err("%s: %s: missing mandatory property\n", __func__, np->name);
	return ret;
}

static int loongson_gpio_remove(struct platform_device *pdev)
{
	struct loongson_gpio_chip *lgpio = platform_get_drvdata(pdev);
	struct resource		*mem;

	platform_set_drvdata(pdev, NULL);
	gpiochip_remove(&lgpio->chip);
	iounmap(lgpio->base);
	kfree(lgpio);
	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(mem->start, resource_size(mem));
	return 0;
}

static const struct of_device_id loongson_gpio_dt_ids[] = {
	{ .compatible = "loongson,ls7a-gpio"},
	{ .compatible = "loongson,ls7a-dc-gpio"},
	{}
};
MODULE_DEVICE_TABLE(of, loongson_gpio_dt_ids);

static const struct acpi_device_id loongson_gpio_acpi_match[] = {
	{"LOON0002"},
	{"LOON0003"},
	{}
};
MODULE_DEVICE_TABLE(acpi, loongson_gpio_acpi_match);

static struct platform_driver ls_gpio_driver = {
	.driver = {
		.name = "loongson-gpio",
		.owner	= THIS_MODULE,
		.of_match_table = loongson_gpio_dt_ids,
		.acpi_match_table = ACPI_PTR(loongson_gpio_acpi_match),
	},
	.probe = loongson_gpio_probe,
	.remove = loongson_gpio_remove,
};

static int __init loongson_gpio_setup(void)
{
	return platform_driver_register(&ls_gpio_driver);
}
subsys_initcall(loongson_gpio_setup);

static void __exit loongson_gpio_driver(void)
{
	platform_driver_unregister(&ls_gpio_driver);
}
module_exit(loongson_gpio_driver);
MODULE_AUTHOR("Loongson Technology Corporation Limited");
MODULE_DESCRIPTION("LOONGSON GPIO");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:loongson_gpio");
