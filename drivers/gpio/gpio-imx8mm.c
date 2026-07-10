// SPDX-License-Identifier: GPL-2.0+
/*
 * IMX8MM RDC GPIO Support (c) 2021 Shaper Tools <stephen@shapertools.com>
 * MXC GPIO support. (c) 2008 Daniel Mack <daniel@caiaq.de>
 * Copyright 2008 Juergen Beisert, kernel@pengutronix.de
 *
 * Based on code from Freescale Semiconductor,
 * Authors: Daniel Mack, Juergen Beisert.
 * Copyright (C) 2004-2010 Freescale Semiconductor, Inc. All Rights Reserved.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>
#include <linux/gpio/driver.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/bug.h>
#include <linux/hwspinlock.h>

struct imx8mm_gpio_port {
	struct list_head node;
	void __iomem *base;
	struct clk *clk;
	int irq_low;
	int irq_high;
	uint32_t assigned_irqs;
	struct irq_domain *domain;
	struct gpio_chip gc;
	struct irq_chip_generic *ic;
	struct device *dev;
	bool gpio_ranges;

	struct hwspinlock *hw_lock;
	spinlock_t sw_lock;
};

#define GPIO_DR(port)       (port->base + 0x00)
#define GPIO_GDIR(port)     (port->base + 0x04)
#define GPIO_PSR(port)      (port->base + 0x08)
#define GPIO_ICR1(port)     (port->base + 0x0c)
#define GPIO_ICR2(port)     (port->base + 0x10)
#define GPIO_IMR(port)      (port->base + 0x14)
#define GPIO_ISR(port)      (port->base + 0x18)
#define GPIO_EDGE_SEL(port)	(port->base + 0x1c)

#define GPIO_INT_LOW_LEVEL  (0x00UL)
#define GPIO_INT_HIGH_LEVEL (0x01UL)
#define GPIO_INT_RISE_EDGE  (0x02UL)
#define GPIO_INT_FALL_EDGE  (0x03UL)
#define GPIO_INT_BOTH_EDGES (0x01UL)

static LIST_HEAD(imx8mm_gpio_ports);

static inline void imx8mm_gpio_block_lock(struct imx8mm_gpio_port *port, unsigned long *flags)
{
	int status;

	if (port->hw_lock) {
		status = hwspin_lock_timeout_irqsave(port->hw_lock, 500, flags);
		if (status < 0)
			panic("could not aquire hardware spin lock in a reasonable timeframe\n");
		return;
	}

	spin_lock_irqsave(&port->sw_lock, *flags);
}

static inline void imx8mm_gpio_block_unlock(struct imx8mm_gpio_port *port, unsigned long *flags)
{
	if (port->hw_lock) {
		hwspin_unlock(port->hw_lock);
		return;
	}

	spin_unlock_irqrestore(&port->sw_lock, *flags);
}

static void imx8mm_gpio_irq_ack(struct irq_data *d)
{
	struct irq_chip_generic *ic = irq_data_get_irq_chip_data(d);
	struct imx8mm_gpio_port *port = ic->private;
	unsigned long flags;

	imx8mm_gpio_block_lock(port, &flags);
	writel(d->mask, GPIO_ISR(port));
	imx8mm_gpio_block_unlock(port, &flags);
}

static void imx8mm_gpio_irq_mask(struct irq_data *d)
{
	struct irq_chip_generic *ic = irq_data_get_irq_chip_data(d);
	struct imx8mm_gpio_port *port = ic->private;
	unsigned long flags;
	uint32_t val;

	imx8mm_gpio_block_lock(port, &flags);
	val = readl(GPIO_IMR(port)) & ~(d->mask);
	writel(val, GPIO_IMR(port));
	imx8mm_gpio_block_unlock(port, &flags);
}

static void imx8mm_gpio_irq_unmask(struct irq_data *d)
{
	struct irq_chip_generic *ic = irq_data_get_irq_chip_data(d);
	struct imx8mm_gpio_port *port = ic->private;
	unsigned long flags;
	uint32_t val;

	imx8mm_gpio_block_lock(port, &flags);
	val = readl(GPIO_IMR(port)) | d->mask;
	writel(val, GPIO_IMR(port));
	imx8mm_gpio_block_unlock(port, &flags);
}

static void imx8mm_gpio_parent_enable(int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	if (chip->irq_enable)
		chip->irq_enable(&desc->irq_data);
	else
		chip->irq_unmask(&desc->irq_data);
}

static void imx8mm_gpio_parent_disable(int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irq_chip *chip = irq_desc_get_chip(desc);

	if (chip->irq_disable)
		chip->irq_disable(&desc->irq_data);
	else
		chip->irq_mask(&desc->irq_data);
}

static int imx8mm_gpio_irq_request_resource(struct irq_data *d)
{
	struct irq_chip_generic *ic = irq_data_get_irq_chip_data(d);
	struct imx8mm_gpio_port *port = ic->private;
	int status;
	unsigned long flags;
	bool low;

	status = gpiochip_lock_as_irq(&port->gc, d->hwirq);
	if (status < 0)	{
		dev_err(port->gc.parent, "unable to lock HW IRQ %lu for IRQ\n",	d->hwirq);
		return status;
	}

	status = irq_chip_pm_get(d);
	if (status < 0) {
		dev_err(port->gc.parent, "failed to power up irq: %d\n", status);
		return status;
	}


	/* Assign the interrupt */
	spin_lock_irqsave(&port->sw_lock, flags);
	port->assigned_irqs |= d->mask;
	low = d->hwirq < 16;
	spin_unlock_irqrestore(&port->sw_lock, flags);

	/* Enable the root interrupt */
	imx8mm_gpio_parent_enable(low ? port->irq_low : port->irq_high);

	return 0;
}

static void imx8mm_gpio_irq_release_resource(struct irq_data *d)
{
	struct irq_chip_generic *ic = irq_data_get_irq_chip_data(d);
	struct imx8mm_gpio_port *port = ic->private;
	unsigned long flags;
	bool low;
	bool disable;

	/* Unassign the interrupt */
	spin_lock_irqsave(&port->sw_lock, flags);
	port->assigned_irqs &= ~d->mask;
	low = d->hwirq < 16;
	if (low)
		disable = (port->assigned_irqs & 0x0000ffff) == 0;
	else
		disable = (port->assigned_irqs & 0xffff0000) == 0;
	spin_unlock_irqrestore(&port->sw_lock, flags);

	/* Disable root interrupt if this the last enable interrupt */
	if (disable)
		imx8mm_gpio_parent_disable(low ? port->irq_low : port->irq_high);

	gpiochip_unlock_as_irq(&port->gc, d->hwirq);
	irq_chip_pm_put(d);
}


static int imx8mm_gpio_set_irq_type(struct irq_data *d, uint32_t type)
{
	struct irq_chip_generic *ic = irq_data_get_irq_chip_data(d);
	struct imx8mm_gpio_port *port = ic->private;
	unsigned long flags;
	uint32_t edge_select_val;
	uint32_t icr_val;
	uint32_t edge_type = 0;
	uint32_t offset = d->hwirq;
	void __iomem *icr_reg = GPIO_ICR1(port) + ((offset & 0x10) >> 2);
	uint32_t icr_shift = ((offset & 0x0f) << 1);

	dev_info(port->dev, "%s\n", __func__);

	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
		edge_type = GPIO_INT_RISE_EDGE;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		edge_type = GPIO_INT_FALL_EDGE;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		edge_select_val |= (1UL << offset);
		break;
	case IRQ_TYPE_LEVEL_LOW:
		edge_type = GPIO_INT_LOW_LEVEL;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
		edge_type = GPIO_INT_HIGH_LEVEL;
		break;
	default:
		return -EINVAL;
	}

	imx8mm_gpio_block_lock(port, &flags);

	edge_select_val = readl(GPIO_EDGE_SEL(port)) & ~(1UL << offset);
	icr_val = readl(icr_reg) & ~(0x03 << icr_shift);

	writel(edge_select_val, GPIO_EDGE_SEL(port));
	writel(icr_val | (edge_type << icr_shift), icr_reg);
	writel(1UL << offset, GPIO_ISR(port));

	imx8mm_gpio_block_unlock(port, &flags);

	return 0;
}

static void imx8mm_gpio_irq_handler(struct irq_desc *desc)
{
	u32 irq_stat;
	struct imx8mm_gpio_port *port = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned long flags;

	chained_irq_enter(chip, desc);

	imx8mm_gpio_block_lock(port, &flags);
	irq_stat = readl(GPIO_ISR(port)) & readl(GPIO_IMR(port)) & port->assigned_irqs;
	imx8mm_gpio_block_unlock(port, &flags);

	while (irq_stat != 0) {
		int irqoffset = fls(irq_stat) - 1;
		generic_handle_irq(irq_find_mapping(port->domain, irqoffset));
		irq_stat &= ~(1 << irqoffset);
	}

	chained_irq_exit(chip, desc);
}

static int imx8mm_gpio_set_wake_irq(struct irq_data *d, u32 enable)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct imx8mm_gpio_port *port = gc->private;
	u32 gpio_idx = d->hwirq;
	int ret;

	if (enable) {
		if (port->irq_high && (gpio_idx >= 16))
			ret = enable_irq_wake(port->irq_high);
		else
			ret = enable_irq_wake(port->irq_low);
	} else {
		if (port->irq_high && (gpio_idx >= 16))
			ret = disable_irq_wake(port->irq_high);
		else
			ret = disable_irq_wake(port->irq_low);
	}

	return ret;
}

static int imx8mm_gpio_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(gc);

	return irq_find_mapping(port->domain, offset);
}

static int imx8mm_gpio_request(struct gpio_chip *chip, unsigned offset)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(chip);
	int ret;

	if (port->gpio_ranges) {
		ret = gpiochip_generic_request(chip, offset);
		if (ret)
			return ret;
	}

	ret = pm_runtime_get_sync(chip->parent);
	return ret < 0 ? ret : 0;
}

static void imx8mm_gpio_free(struct gpio_chip *chip, unsigned offset)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(chip);

	if (port->gpio_ranges)
		gpiochip_generic_free(chip, offset);
	pm_runtime_put(chip->parent);
}

static int imx8mm_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(gc);
	unsigned long flags;
	uint32_t value;

	imx8mm_gpio_block_lock(port, &flags);

	value = readl(GPIO_GDIR(port));

	imx8mm_gpio_block_unlock(port, &flags);

	/* GPIOF_DIR_OUT == 0, GPIOF_DIR_IN == 1 */
	return (value & (1UL << offset)) == 0;
}

static int imx8mm_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(gc);
	unsigned long flags;
	uint32_t value;

	imx8mm_gpio_block_lock(port, &flags);

	value = readl(GPIO_GDIR(port)) & ~(1UL << offset);
	writel(value, GPIO_GDIR(port));

	imx8mm_gpio_block_unlock(port, &flags);

	return 0;
}

static int imx8mm_gpio_direction_output(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(gc);
	unsigned long flags;

	imx8mm_gpio_block_lock(port, &flags);

	value = readl(GPIO_GDIR(port)) | (1UL << offset);
	writel(value, GPIO_GDIR(port));

	imx8mm_gpio_block_unlock(port, &flags);

	return 0;
}

static void imx8mm_gpio_set_value(struct gpio_chip *gc, unsigned int offset, int v)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(gc);
	unsigned long flags;
	uint32_t value;

	imx8mm_gpio_block_lock(port, &flags);

	value = readl(GPIO_DR(port));
	if (v)
		value |= (1UL << offset);
	else
		value &= ~(1UL << offset);
	writel(value, GPIO_DR(port));

	imx8mm_gpio_block_unlock(port, &flags);
}

static int imx8mm_gpio_get_value(struct gpio_chip *gc, unsigned int offset)
{
	struct imx8mm_gpio_port *port = gpiochip_get_data(gc);
	unsigned long flags;
	uint32_t value;

	imx8mm_gpio_block_lock(port, &flags);

	value = readl(GPIO_DR(port));

	imx8mm_gpio_block_unlock(port, &flags);

	return (value & (1UL << offset)) != 0;
}

static int imx8mm_gpio_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct imx8mm_gpio_port *port;
	int irq_base = 0;
	unsigned int hwspinlock_id;
	int status;

	dev_info(&pdev->dev, "initializing gpio-controller with shaper specific RDC friendly driver\n");

	/* Create the port */
	port = devm_kzalloc(&pdev->dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	/* Basic initialization */
	port->dev = &pdev->dev;
	spin_lock_init(&port->sw_lock);

	port->gc.parent = &pdev->dev;
	port->gc.label = dev_name(&pdev->dev);
	port->gc.base = -1;
	port->gc.ngpio = 32;

	port->gpio_ranges = of_property_read_bool(np, "gpio_ranges");
	port->gc.request = imx8mm_gpio_request;
	port->gc.free = imx8mm_gpio_free;
	port->gc.to_irq = imx8mm_gpio_to_irq;
	port->gc.base = (pdev->id < 0) ? of_alias_get_id(np, "gpio") * 32 : pdev->id * 32;

	port->gc.get_direction = imx8mm_gpio_get_direction;
	port->gc.direction_input = imx8mm_gpio_direction_input;
	port->gc.direction_output = imx8mm_gpio_direction_output;

	port->gc.get = imx8mm_gpio_get_value;
	port->gc.set = imx8mm_gpio_set_value;

	/* Get the base address of the gpio peripheral */
	port->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(port->base))
		return PTR_ERR(port->base);

	/* And the optional peripheral clock */
	port->clk = devm_clk_get_optional(&pdev->dev, NULL);
	if (IS_ERR(port->clk))
		return PTR_ERR(port->clk);

	/* Enable the clock */
	status = clk_prepare_enable(port->clk);
	if (status) {
		dev_err(&pdev->dev, "Unable to enable clock.\n");
		return status;
	}

	/* Initialize the power management */
	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	status = pm_runtime_get_sync(&pdev->dev);
	if (status < 0)
		goto error_clk_disable;

	/* Should we use a hardware spin lock? */
	status = of_property_read_u32(np, "hwspinlock", &hwspinlock_id);
	if (status >= 0) {

		/* Acquire the hardware spin lock */
		port->hw_lock = hwspin_lock_request_specific(hwspinlock_id);
		if (!IS_ERR_OR_NULL(port->hw_lock))
			dev_info(&pdev->dev, "using hardware spin lock: %u\n", hwspinlock_id);
		else {
			dev_warn(&pdev->dev, "failed to request hardware spin lock %u: %ld\n", hwspinlock_id, PTR_ERR(port->hw_lock));
			port->hw_lock = 0;
		}
	}

	/* Add the new gpio controller */
	status = devm_gpiochip_add_data(&pdev->dev, &port->gc, port);
	if (status)
		goto error_pm_disable;

	/* Should we also add the gpio peripheral as a interrupt controller */
	if (of_property_read_bool(np, "interrupt-controller")) {

		dev_info(&pdev->dev, "initializing interrupt controller\n");

		/* Get bother GPIO interrupts for the i.MX8mm */
		port->irq_low = platform_get_irq(pdev, 0);
		if (port->irq_low < 0) {
			status = port->irq_low;
			goto error_pm_disable;
		}

		port->irq_high = platform_get_irq(pdev, 1);
		if (port->irq_high < 0) {
			status = port->irq_high;
			goto error_pm_disable;
		}

		/* Need the irq descriptors to get the staring irq number */
		irq_base = devm_irq_alloc_descs(&pdev->dev, -1, 0, 32, numa_node_id());
		if (irq_base < 0) {
			status = irq_base;
			goto error_pm_disable;
		}

		/* And a irq domain to translate the irq numbers to bit offsets */
		port->domain = irq_domain_add_legacy(np, 32, irq_base, 0, &irq_domain_simple_ops, NULL);
		if (!port->domain) {
			status = -ENODEV;
			goto error_pm_disable;
		}

		/* Got everything, allocated a irq chip */
		port->ic = devm_irq_alloc_generic_chip(port->dev, "gpio-imx8mm", 1, irq_base, port->base, handle_level_irq);
		if (!port->ic) {
			status = -ENOMEM;
			goto error_irq_domain_remove;
		}

		/* Configure it */
		port->ic->private = port;
		port->ic->chip_types[0].chip.irq_ack = imx8mm_gpio_irq_ack;
		port->ic->chip_types[0].chip.irq_mask = imx8mm_gpio_irq_mask;
		port->ic->chip_types[0].chip.irq_unmask = imx8mm_gpio_irq_unmask;
		port->ic->chip_types[0].chip.irq_set_type = imx8mm_gpio_set_irq_type;
		port->ic->chip_types[0].chip.irq_set_wake = imx8mm_gpio_set_wake_irq;
		port->ic->chip_types[0].chip.irq_request_resources = imx8mm_gpio_irq_request_resource;
		port->ic->chip_types[0].chip.irq_release_resources = imx8mm_gpio_irq_release_resource,
		port->ic->chip_types[0].chip.flags = IRQCHIP_MASK_ON_SUSPEND;

		/* Add the irq chip to the system */
		status = devm_irq_setup_generic_chip(port->dev, port->ic, IRQ_MSK(32), IRQ_GC_INIT_NESTED_LOCK, IRQ_NOREQUEST, 0);
		if (status < 0)
			goto error_irq_domain_remove;

		/* Bind to our handler to the root gpio irqs */
		irq_set_chained_handler_and_data(port->irq_low, imx8mm_gpio_irq_handler, port);
		irq_set_chained_handler_and_data(port->irq_high, imx8mm_gpio_irq_handler, port);

		/* Now disable them, keep an eye on this as it is racy relative to above */
		imx8mm_gpio_parent_disable(port->irq_low);
		imx8mm_gpio_parent_disable(port->irq_high);
	}

	list_add_tail(&port->node, &imx8mm_gpio_ports);

	platform_set_drvdata(pdev, port);
	pm_runtime_put(&pdev->dev);

	dev_info(&pdev->dev, "ready\n");

	return 0;

error_irq_domain_remove:
	irq_domain_remove(port->domain);

error_pm_disable:
	pm_runtime_disable(&pdev->dev);

error_clk_disable:
	clk_disable_unprepare(port->clk);
	dev_info(&pdev->dev, "%s failed with errno %d\n", __func__, status);
	return status;
}

static int __maybe_unused imx8mm_gpio_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct imx8mm_gpio_port *port = platform_get_drvdata(pdev);

	clk_disable_unprepare(port->clk);

	return 0;
}

static int __maybe_unused imx8mm_gpio_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct imx8mm_gpio_port *port = platform_get_drvdata(pdev);
	int ret;

	ret = clk_prepare_enable(port->clk);
	if (ret)
		return ret;

	return 0;
}

static int __maybe_unused imx8mm_gpio_noirq_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused imx8mm_gpio_noirq_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops imx8mm_gpio_dev_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(imx8mm_gpio_noirq_suspend, imx8mm_gpio_noirq_resume)
	SET_RUNTIME_PM_OPS(imx8mm_gpio_runtime_suspend, imx8mm_gpio_runtime_resume, NULL)
};

static int imx8mm_gpio_syscore_suspend(void)
{
	struct imx8mm_gpio_port *port;
	int ret;

	/* walk through all ports */
	list_for_each_entry(port, &imx8mm_gpio_ports, node) {
		ret = clk_prepare_enable(port->clk);
		if (ret)
			return ret;
		clk_disable_unprepare(port->clk);
	}

	return 0;
}

static void imx8mm_gpio_syscore_resume(void)
{
	struct imx8mm_gpio_port *port;
	int ret;

	/* walk through all ports */
	list_for_each_entry(port, &imx8mm_gpio_ports, node) {
		ret = clk_prepare_enable(port->clk);
		if (ret) {
			pr_err("mxc: failed to enable gpio clock %d\n", ret);
			return;
		}
		clk_disable_unprepare(port->clk);
	}
}

static const struct of_device_id imx8mm_gpio_dt_ids[] = {
	{ .compatible = "fsl,imx8mm-gpio" },
	{ }
};

static struct syscore_ops imx8mm_gpio_syscore_ops = {
	.suspend = imx8mm_gpio_syscore_suspend,
	.resume = imx8mm_gpio_syscore_resume,
};

static struct platform_driver imx8mm_gpio_driver = {
	.driver		= {
		.name	= "gpio-imx8mm",
		.of_match_table = imx8mm_gpio_dt_ids,
		.suppress_bind_attrs = true,
		.pm = &imx8mm_gpio_dev_pm_ops,
	},
	.probe		= imx8mm_gpio_probe,
};

static int __init gpio_imx8mm_init(void)
{
	register_syscore_ops(&imx8mm_gpio_syscore_ops);

	return platform_driver_register(&imx8mm_gpio_driver);
}
subsys_initcall(gpio_imx8mm_init);

MODULE_DESCRIPTION("iMX8M Mini GPIO driver with RDC shared peripheral support");
MODULE_AUTHOR("Stephen Street <stephen@shapertools.com>");
MODULE_LICENSE("GPL v2");
