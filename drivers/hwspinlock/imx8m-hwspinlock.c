// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Shaper Tools 2021
 * Author: Stephen Street <stephen@shapertools.com> for Shaper Tools.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/hwspinlock.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/clk.h>


#include "hwspinlock_internal.h"

#define IMX8M_NUM_LOCKS 64UL
#define IMX8M_A53_DOMAIN_ID 0UL
#define IMX8M_A53_MASTER_INDEX 1UL

#define SEMAPHORE_GATE_LDOM(x) ((x) << 4)
#define SEMAPHORE_GATE_GTFSM(x) ((x) << 0)
#define SEMAPHORE_LOCKED (SEMAPHORE_GATE_LDOM(IMX8M_A53_DOMAIN_ID) | SEMAPHORE_GATE_GTFSM(IMX8M_A53_MASTER_INDEX + 1))

struct imx8mm_hwspinlock {
	struct clk *clk;
	struct hwspinlock_device bank;
};

static int imx8mm_hwspinlock_trylock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	/* Write the lock pattern */
	writeb(IMX8M_A53_MASTER_INDEX + 1, lock_addr);

	/* Did it work? */
	return readb(lock_addr) == SEMAPHORE_LOCKED;
}

static void imx8mm_hwspinlock_unlock(struct hwspinlock *lock)
{
	void __iomem *lock_addr = lock->priv;

	/* Write zero to unlock */
	writeb(0, lock_addr);
}

static void imx8mm_hwspinlock_relax(struct hwspinlock *lock)
{
	ndelay(50);
}

static const struct hwspinlock_ops imx8mm_hwspinlock_ops = {
	.trylock	= imx8mm_hwspinlock_trylock,
	.unlock		= imx8mm_hwspinlock_unlock,
	.relax		= imx8mm_hwspinlock_relax,
};

static int imx8mm_hwspinlock_probe(struct platform_device *pdev)
{
	struct imx8mm_hwspinlock *hw;
	void __iomem *io_base;
	struct resource *res;
	size_t array_size;
	int i;
	int status;

	dev_info(&pdev->dev, "initializing\n");

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	io_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(io_base))
		return PTR_ERR(io_base);

	array_size = IMX8M_NUM_LOCKS * sizeof(struct hwspinlock);
	hw = devm_kzalloc(&pdev->dev, sizeof(*hw) + array_size, GFP_KERNEL);
	if (!hw)
		return -ENOMEM;

	/* Initialize bank 1 */
	for (i = 0; i < IMX8M_NUM_LOCKS; i++)
		hw->bank.lock[i].priv = io_base + i * sizeof(u8);

	platform_set_drvdata(pdev, hw);

	/* Get the clock */
	hw->clk = devm_clk_get(&pdev->dev, 0);
	if (IS_ERR(hw->clk)) {
		if (PTR_ERR(hw->clk) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "could not get the clock: %ld\n", PTR_ERR(hw->clk));
		return PTR_ERR(hw->clk);
	}

	/* Enable the clock */
	status = clk_prepare_enable(hw->clk);
	if (status) {
		dev_err(&pdev->dev, "unable to enable clock: %d\n", status);
		return status;
	}

	pm_runtime_set_active(&pdev->dev);
	pm_runtime_enable(&pdev->dev);
	status = pm_runtime_get_sync(&pdev->dev);
	if (status < 0) {
		dev_err(&pdev->dev, "failed to get pm runtime: %d\n", status);
		clk_disable_unprepare(hw->clk);
		return status;
	}

	/* Register bank SEMA Bank */
	status = hwspin_lock_register(&hw->bank, &pdev->dev, &imx8mm_hwspinlock_ops, 0, IMX8M_NUM_LOCKS);
	if (status < 0) {
		clk_disable_unprepare(hw->clk);
		return status;
	}

	return status;
}

static void imx8mm_hwspinlock_remove(struct platform_device *pdev)
{
	struct imx8mm_hwspinlock *hw = platform_get_drvdata(pdev);
	int ret;

	ret = hwspin_lock_unregister(&hw->bank);
	if (ret)
		dev_err(&pdev->dev, "%s failed: %d\n", __func__, ret);
}

static int __maybe_unused imx8mm_hwspinlock_runtime_suspend(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct imx8mm_hwspinlock *hw = platform_get_drvdata(pdev);

	clk_disable_unprepare(hw->clk);

	return 0;
}

static int __maybe_unused imx8mm_hwspinlock_runtime_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct imx8mm_hwspinlock *hw = platform_get_drvdata(pdev);
	int status;

	status = clk_prepare_enable(hw->clk);
	if (status)
		return status;

	return 0;
}

static int __maybe_unused imx8mm_hwspinlock_noirq_suspend(struct device *dev)
{
	return 0;
}

static int __maybe_unused imx8mm_hwspinlock_noirq_resume(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops imx8mm_hwspinlock_dev_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(imx8mm_hwspinlock_noirq_suspend, imx8mm_hwspinlock_noirq_resume)
	SET_RUNTIME_PM_OPS(imx8mm_hwspinlock_runtime_suspend, imx8mm_hwspinlock_runtime_resume, NULL)
};

static const struct of_device_id imx8mm_hwpinlock_ids[] = {
	{ .compatible = "fsl,imx8m-hwspinlock", },
	{},
};
MODULE_DEVICE_TABLE(of, imx8mm_hwpinlock_ids);

static struct platform_driver imx8mm_hwspinlock_driver = {
	.probe		= imx8mm_hwspinlock_probe,
	.remove		= imx8mm_hwspinlock_remove,
	.driver		= {
		.name	= "imx8m-hwspinlock",
		.of_match_table = imx8mm_hwpinlock_ids,
		.pm = &imx8mm_hwspinlock_dev_pm_ops,
	},

};

static int __init imx8mm_hwspinlock_init(void)
{
	return platform_driver_register(&imx8mm_hwspinlock_driver);
}

/* board init code might need to reserve hwspinlocks for predefined purposes */
postcore_initcall(imx8mm_hwspinlock_init);

static void __exit imx8mm_hwspinlock_exit(void)
{
	platform_driver_unregister(&imx8mm_hwspinlock_driver);
}
module_exit(imx8mm_hwspinlock_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Hardware spinlock driver for the i.MX8MM SEMA1/2 block");
MODULE_AUTHOR("Stephen Street <stephen@shapertools.com");
