/*
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#define pr_fmt(fmt) "tegra-pmc: " fmt

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/system-power.h>

#define PMC_CNTRL 0x000
#define  PMC_CNTRL_MAIN_RST BIT(4)

#define PMC_RST_STATUS 0x070

#define WAKE_AOWAKE_CTRL 0x4f4
#define  WAKE_AOWAKE_CTRL_INTR_POLARITY BIT(0)

#define SCRATCH_SCRATCH0 0x2000
#define  SCRATCH_SCRATCH0_MODE_RECOVERY BIT(31)
#define  SCRATCH_SCRATCH0_MODE_BOOTLOADER BIT(30)
#define  SCRATCH_SCRATCH0_MODE_RCM BIT(1)
#define  SCRATCH_SCRATCH0_MODE_MASK (SCRATCH_SCRATCH0_MODE_RECOVERY | \
				     SCRATCH_SCRATCH0_MODE_BOOTLOADER | \
				     SCRATCH_SCRATCH0_MODE_RCM)

struct tegra_pmc {
	struct system_power_chip chip;
	struct device *dev;
	void __iomem *regs;
	void __iomem *wake;
	void __iomem *aotag;
	void __iomem *scratch;

	void (*system_restart)(enum reboot_mode mode, const char *cmd);
	struct notifier_block restart;
};

static inline struct tegra_pmc *to_pmc(struct system_power_chip *chip)
{
	return container_of(chip, struct tegra_pmc, chip);
}

static int tegra186_pmc_restart_prepare(struct system_power_chip *chip,
					enum reboot_mode mode, char *cmd)
{
	struct tegra_pmc *pmc = to_pmc(chip);
	u32 value;

	value = readl(pmc->scratch + SCRATCH_SCRATCH0);
	value &= ~SCRATCH_SCRATCH0_MODE_MASK;

	if (cmd) {
		if (strcmp(cmd, "recovery") == 0)
			value |= SCRATCH_SCRATCH0_MODE_RECOVERY;

		if (strcmp(cmd, "bootloader") == 0)
			value |= SCRATCH_SCRATCH0_MODE_BOOTLOADER;

		if (strcmp(cmd, "forced-recovery") == 0)
			value |= SCRATCH_SCRATCH0_MODE_RCM;
	}

	writel(value, pmc->scratch + SCRATCH_SCRATCH0);

	return 0;
}

static int tegra186_pmc_restart(struct system_power_chip *chip,
				enum reboot_mode mode, char *cmd)
{
	struct tegra_pmc *pmc = to_pmc(chip);
	u32 value;

	/* reset everything but SCRATCH0_SCRATCH0 and PMC_RST_STATUS */
	value = readl(pmc->regs + PMC_CNTRL);
	value |= PMC_CNTRL_MAIN_RST;
	writel(value, pmc->regs + PMC_CNTRL);

	return 0;
}

static void tegra186_pmc_setup(struct tegra_pmc *pmc)
{
	struct device_node *np = pmc->dev->of_node;
	bool invert;
	u32 value;

	invert = of_property_read_bool(np, "nvidia,invert-interrupt");

	value = readl(pmc->wake + WAKE_AOWAKE_CTRL);

	if (invert)
		value |= WAKE_AOWAKE_CTRL_INTR_POLARITY;
	else
		value &= ~WAKE_AOWAKE_CTRL_INTR_POLARITY;

	writel(value, pmc->wake + WAKE_AOWAKE_CTRL);
}

static int tegra186_pmc_probe(struct platform_device *pdev)
{
	struct tegra_pmc *pmc;
	struct resource *res;
	int err;

	pmc = devm_kzalloc(&pdev->dev, sizeof(*pmc), GFP_KERNEL);
	if (!pmc)
		return -ENOMEM;

	pmc->dev = &pdev->dev;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pmc");
	pmc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pmc->regs))
		return PTR_ERR(pmc->regs);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wake");
	pmc->wake = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pmc->wake))
		return PTR_ERR(pmc->wake);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "aotag");
	pmc->aotag = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pmc->aotag))
		return PTR_ERR(pmc->aotag);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scratch");
	pmc->scratch = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pmc->scratch))
		return PTR_ERR(pmc->scratch);

	pmc->chip.level = SYSTEM_POWER_LEVEL_SOC;
	pmc->chip.dev = &pdev->dev;
	pmc->chip.restart_prepare = tegra186_pmc_restart_prepare;
	pmc->chip.restart = tegra186_pmc_restart;

	err = system_power_chip_add(&pmc->chip);
	if (err < 0) {
		dev_err(&pdev->dev,
			"failed to register system power chip: %d\n",
			err);
		return err;
	}

	tegra186_pmc_setup(pmc);

	platform_set_drvdata(pdev, pmc);

	return 0;
}

static int tegra186_pmc_remove(struct platform_device *pdev)
{
	struct tegra_pmc *pmc = platform_get_drvdata(pdev);

	return system_power_chip_remove(&pmc->chip);
}

static const struct of_device_id tegra186_pmc_of_match[] = {
	{ .compatible = "nvidia,tegra186-pmc" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, tegra186_pmc_of_match);

static struct platform_driver tegra186_pmc_driver = {
	.driver = {
		.name = "tegra186-pmc",
		.of_match_table = tegra186_pmc_of_match,
	},
	.probe = tegra186_pmc_probe,
	.remove = tegra186_pmc_remove,
};
builtin_platform_driver(tegra186_pmc_driver);
