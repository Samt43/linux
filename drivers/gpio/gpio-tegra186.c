/*
 * Copyright (c) 2016 NVIDIA Corporation
 *
 * Author: Thierry Reding <treding@nvidia.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#include <linux/gpio/driver.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#include <dt-bindings/gpio/tegra186-gpio.h>

#define TEGRA186_GPIO_ENABLE_CONFIG 0x00
#define  TEGRA186_GPIO_ENABLE_CONFIG_ENABLE BIT(0)
#define  TEGRA186_GPIO_ENABLE_CONFIG_OUT BIT(1)
#define  TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_NONE (0x0 << 2)
#define  TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_LEVEL (0x1 << 2)
#define  TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_SINGLE_EDGE (0x2 << 2)
#define  TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_DOUBLE_EDGE (0x3 << 2)
#define  TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_MASK (0x3 << 2)
#define  TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_LEVEL BIT(4)
#define  TEGRA186_GPIO_ENABLE_CONFIG_INTERRUPT BIT(6)

#define TEGRA186_GPIO_DEBOUNCE_CONTROL 0x04
#define  TEGRA186_GPIO_DEBOUNCE_CONTROL_THRESHOLD(x) ((x) & 0xff)

#define TEGRA186_GPIO_INPUT 0x08
#define  TEGRA186_GPIO_INPUT_HIGH BIT(0)

#define TEGRA186_GPIO_OUTPUT_CONTROL 0x0c
#define  TEGRA186_GPIO_OUTPUT_CONTROL_FLOATED BIT(0)

#define TEGRA186_GPIO_OUTPUT_VALUE 0x10
#define  TEGRA186_GPIO_OUTPUT_VALUE_HIGH BIT(0)

#define TEGRA186_GPIO_INTERRUPT_CLEAR 0x14

#define TEGRA186_GPIO_INTERRUPT_STATUS(x) (0x100 + (x) * 4)

struct tegra_gpio_port {
	unsigned int offset;
	unsigned int pins;
};

struct tegra_gpio_soc {
	const struct tegra_gpio_port *ports;
	unsigned int num_ports;
};

struct tegra_gpio {
	struct gpio_chip gpio;
	struct irq_chip intc;
	unsigned int num_irq;
	unsigned int *irq;

	const struct tegra_gpio_soc *soc;

	void __iomem *base;

	struct irq_domain *domain;
};

static inline struct tegra_gpio *to_tegra_gpio(struct gpio_chip *chip)
{
	return container_of(chip, struct tegra_gpio, gpio);
}

static const struct tegra_gpio_port *
tegra186_gpio_get_port(struct tegra_gpio *gpio, unsigned int *pin)
{
	unsigned int start = 0, i;

	for (i = 0; i < gpio->soc->num_ports; i++) {
		const struct tegra_gpio_port *port = &gpio->soc->ports[i];

		if (*pin >= start && *pin < start + port->pins) {
			*pin -= start;
			return port;
		}

		start += port->pins;
	}

	return NULL;
}

static void __iomem *tegra186_gpio_get_base(struct tegra_gpio *gpio,
					    unsigned int pin)
{
	const struct tegra_gpio_port *port;

	port = tegra186_gpio_get_port(gpio, &pin);
	if (!port)
		return NULL;

	return gpio->base + port->offset + pin * 0x20;
}

static int tegra186_gpio_get_direction(struct gpio_chip *chip,
				       unsigned int offset)
{
	struct tegra_gpio *gpio = to_tegra_gpio(chip);
	void __iomem *base;
	u32 value;

	base = tegra186_gpio_get_base(gpio, offset);
	if (WARN_ON(base == NULL))
		return -ENODEV;

	value = readl(base + TEGRA186_GPIO_ENABLE_CONFIG);
	if (value & TEGRA186_GPIO_ENABLE_CONFIG_OUT)
		return GPIOF_DIR_OUT;

	return GPIOF_DIR_IN;
}

static int tegra186_gpio_direction_input(struct gpio_chip *chip,
					 unsigned int offset)
{
	struct tegra_gpio *gpio = to_tegra_gpio(chip);
	void __iomem *base;
	u32 value;

	base = tegra186_gpio_get_base(gpio, offset);
	if (WARN_ON(base == NULL))
		return -ENODEV;

	value = readl(base + TEGRA186_GPIO_OUTPUT_CONTROL);
	value |= TEGRA186_GPIO_OUTPUT_CONTROL_FLOATED;
	writel(value, base + TEGRA186_GPIO_OUTPUT_CONTROL);

	value = readl(base + TEGRA186_GPIO_ENABLE_CONFIG);
	value |= TEGRA186_GPIO_ENABLE_CONFIG_ENABLE;
	value &= ~TEGRA186_GPIO_ENABLE_CONFIG_OUT;
	writel(value, base + TEGRA186_GPIO_ENABLE_CONFIG);

	return 0;
}

static int tegra186_gpio_direction_output(struct gpio_chip *chip,
					  unsigned int offset, int level)
{
	struct tegra_gpio *gpio = to_tegra_gpio(chip);
	void __iomem *base;
	u32 value;

	/* configure output level first */
	chip->set(chip, offset, level);

	base = tegra186_gpio_get_base(gpio, offset);
	if (WARN_ON(base == NULL))
		return -EINVAL;

	/* set the direction */
	value = readl(base + TEGRA186_GPIO_OUTPUT_CONTROL);
	value &= ~TEGRA186_GPIO_OUTPUT_CONTROL_FLOATED;
	writel(value, base + TEGRA186_GPIO_OUTPUT_CONTROL);

	value = readl(base + TEGRA186_GPIO_ENABLE_CONFIG);
	value |= TEGRA186_GPIO_ENABLE_CONFIG_ENABLE;
	value |= TEGRA186_GPIO_ENABLE_CONFIG_OUT;
	writel(value, base + TEGRA186_GPIO_ENABLE_CONFIG);

	return 0;
}

static int tegra186_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct tegra_gpio *gpio = to_tegra_gpio(chip);
	void __iomem *base;
	u32 value;

	base = tegra186_gpio_get_base(gpio, offset);
	if (WARN_ON(base == NULL))
		return -ENODEV;

	value = readl(base + TEGRA186_GPIO_ENABLE_CONFIG);
	if (value & TEGRA186_GPIO_ENABLE_CONFIG_OUT)
		value = readl(base + TEGRA186_GPIO_OUTPUT_VALUE);
	else
		value = readl(base + TEGRA186_GPIO_INPUT);

	return value & BIT(0);
}

static void tegra186_gpio_set(struct gpio_chip *chip, unsigned int offset,
			      int level)
{
	struct tegra_gpio *gpio = to_tegra_gpio(chip);
	void __iomem *base;
	u32 value;

	base = tegra186_gpio_get_base(gpio, offset);
	if (WARN_ON(base == NULL))
		return;

	value = readl(base + TEGRA186_GPIO_OUTPUT_VALUE);
	if (level == 0)
		value &= ~TEGRA186_GPIO_OUTPUT_VALUE_HIGH;
	else
		value |= TEGRA186_GPIO_OUTPUT_VALUE_HIGH;

	writel(value, base + TEGRA186_GPIO_OUTPUT_VALUE);
}

static int tegra186_gpio_to_irq(struct gpio_chip *chip, unsigned int offset)
{
	struct tegra_gpio *gpio = to_tegra_gpio(chip);

	return irq_find_mapping(gpio->domain, offset);
}

static int tegra186_gpio_of_xlate(struct gpio_chip *chip,
				  const struct of_phandle_args *spec,
				  u32 *flags)
{
	struct tegra_gpio *gpio = to_tegra_gpio(chip);
	unsigned int port, pin, i, offset = 0;

	if (WARN_ON(chip->of_gpio_n_cells < 2))
		return -EINVAL;

	if (WARN_ON(spec->args_count < chip->of_gpio_n_cells))
		return -EINVAL;

	port = spec->args[0] / 8;
	pin = spec->args[0] % 8;

	if (port >= gpio->soc->num_ports) {
		dev_err(chip->parent, "invalid port number: %u\n", port);
		return -EINVAL;
	}

	for (i = 0; i < port; i++)
		offset += gpio->soc->ports[i].pins;

	if (flags)
		*flags = spec->args[1];

	return offset + pin;
}

static void tegra186_irq_ack(struct irq_data *data)
{
	struct tegra_gpio *gpio = irq_data_get_irq_chip_data(data);
	void __iomem *base;

	base = tegra186_gpio_get_base(gpio, data->hwirq);
	if (WARN_ON(base == NULL))
		return;

	writel(1, base + TEGRA186_GPIO_INTERRUPT_CLEAR);
}

static void tegra186_irq_mask(struct irq_data *data)
{
	struct tegra_gpio *gpio = irq_data_get_irq_chip_data(data);
	void __iomem *base;
	u32 value;

	base = tegra186_gpio_get_base(gpio, data->hwirq);
	if (WARN_ON(base == NULL))
		return;

	value = readl(base + TEGRA186_GPIO_ENABLE_CONFIG);
	value &= ~TEGRA186_GPIO_ENABLE_CONFIG_INTERRUPT;
	writel(value, base + TEGRA186_GPIO_ENABLE_CONFIG);
}

static void tegra186_irq_unmask(struct irq_data *data)
{
	struct tegra_gpio *gpio = irq_data_get_irq_chip_data(data);
	void __iomem *base;
	u32 value;

	base = tegra186_gpio_get_base(gpio, data->hwirq);
	if (WARN_ON(base == NULL))
		return;

	value = readl(base + TEGRA186_GPIO_ENABLE_CONFIG);
	value |= TEGRA186_GPIO_ENABLE_CONFIG_INTERRUPT;
	writel(value, base + TEGRA186_GPIO_ENABLE_CONFIG);
}

static int tegra186_irq_set_type(struct irq_data *data, unsigned int flow)
{
	struct tegra_gpio *gpio = irq_data_get_irq_chip_data(data);
	void __iomem *base;
	u32 value;

	base = tegra186_gpio_get_base(gpio, data->hwirq);
	if (WARN_ON(base == NULL))
		return -ENODEV;

	value = readl(base + TEGRA186_GPIO_ENABLE_CONFIG);
	value &= ~TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_MASK;
	value &= ~TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_LEVEL;

	switch (flow & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_NONE:
		break;

	case IRQ_TYPE_EDGE_RISING:
		value |= TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_SINGLE_EDGE;
		value |= TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_LEVEL;
		break;

	case IRQ_TYPE_EDGE_FALLING:
		value |= TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_SINGLE_EDGE;
		break;

	case IRQ_TYPE_EDGE_BOTH:
		value |= TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_DOUBLE_EDGE;
		break;

	case IRQ_TYPE_LEVEL_HIGH:
		value |= TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_LEVEL;
		value |= TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_LEVEL;
		break;

	case IRQ_TYPE_LEVEL_LOW:
		value |= TEGRA186_GPIO_ENABLE_CONFIG_TRIGGER_TYPE_LEVEL;
		break;

	default:
		return -EINVAL;
	}

	writel(value, base + TEGRA186_GPIO_ENABLE_CONFIG);

	if ((flow & IRQ_TYPE_EDGE_BOTH) == 0)
		irq_set_handler_locked(data, handle_level_irq);
	else
		irq_set_handler_locked(data, handle_edge_irq);

	return 0;
}

static void tegra186_gpio_irq(struct irq_desc *desc)
{
	struct tegra_gpio *gpio = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	unsigned int i, offset = 0;

	chained_irq_enter(chip, desc);

	for (i = 0; i < gpio->soc->num_ports; i++) {
		const struct tegra_gpio_port *port = &gpio->soc->ports[i];
		void __iomem *base = gpio->base + port->offset;
		unsigned int pin, irq;
		unsigned long value;

		value = readl(base + TEGRA186_GPIO_INTERRUPT_STATUS(1));

		for_each_set_bit(pin, &value, port->pins) {
			irq = irq_find_mapping(gpio->domain, offset + pin);
			if (WARN_ON(irq == 0))
				continue;

			generic_handle_irq(irq);
		}

		offset += port->pins;
	}

	chained_irq_exit(chip, desc);
}

static int tegra186_gpio_irq_domain_xlate(struct irq_domain *domain,
					  struct device_node *np,
					  const u32 *spec, unsigned int size,
					  unsigned long *hwirq,
					  unsigned int *type)
{
	struct tegra_gpio *gpio = domain->host_data;
	unsigned int port, pin, i, offset = 0;

	if (size < 2)
		return -EINVAL;

	port = spec[0] / 8;
	pin = spec[0] % 8;

	if (port >= gpio->soc->num_ports) {
		dev_err(gpio->gpio.parent, "invalid port number: %u\n", port);
		return -EINVAL;
	}

	for (i = 0; i < port; i++)
		offset += gpio->soc->ports[i].pins;

	*type = spec[1] & IRQ_TYPE_SENSE_MASK;
	*hwirq = offset + pin;

	return 0;
}

static const struct irq_domain_ops tegra186_gpio_irq_domain_ops = {
	.xlate = tegra186_gpio_irq_domain_xlate,
};

static struct lock_class_key tegra186_gpio_lock_class;

static int tegra186_gpio_probe(struct platform_device *pdev)
{
	struct tegra_gpio *gpio;
	struct resource *res;
	unsigned int i, irq;
	int err;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->soc = of_device_get_match_data(&pdev->dev);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "gpio");
	gpio->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(gpio->base))
		return PTR_ERR(gpio->base);

	err = of_irq_count(pdev->dev.of_node);
	if (err < 0)
		return err;

	gpio->num_irq = err;

	gpio->irq = devm_kcalloc(&pdev->dev, gpio->num_irq, sizeof(*gpio->irq),
				 GFP_KERNEL);
	if (!gpio->irq)
		return -ENOMEM;

	for (i = 0; i < gpio->num_irq; i++) {
		err = platform_get_irq(pdev, i);
		if (err < 0)
			return err;

		gpio->irq[i] = err;
	}

	gpio->gpio.label = "tegra186-gpio";
	gpio->gpio.parent = &pdev->dev;

	gpio->gpio.get_direction = tegra186_gpio_get_direction;
	gpio->gpio.direction_input = tegra186_gpio_direction_input;
	gpio->gpio.direction_output = tegra186_gpio_direction_output;
	gpio->gpio.get = tegra186_gpio_get,
	gpio->gpio.set = tegra186_gpio_set;
	gpio->gpio.to_irq = tegra186_gpio_to_irq;

	gpio->gpio.base = -1;

	for (i = 0; i < gpio->soc->num_ports; i++)
		gpio->gpio.ngpio += gpio->soc->ports[i].pins;

	gpio->gpio.of_node = pdev->dev.of_node;
	gpio->gpio.of_gpio_n_cells = 2;
	gpio->gpio.of_xlate = tegra186_gpio_of_xlate;

	gpio->intc.name = pdev->dev.of_node->name;
	gpio->intc.irq_ack = tegra186_irq_ack;
	gpio->intc.irq_mask = tegra186_irq_mask;
	gpio->intc.irq_unmask = tegra186_irq_unmask;
	gpio->intc.irq_set_type = tegra186_irq_set_type;

	platform_set_drvdata(pdev, gpio);

	err = devm_gpiochip_add_data(&pdev->dev, &gpio->gpio, gpio);
	if (err < 0)
		return err;

	gpio->domain = irq_domain_add_linear(pdev->dev.of_node,
					     gpio->gpio.ngpio,
					     &tegra186_gpio_irq_domain_ops,
					     gpio);
	if (!gpio->domain)
		return -ENODEV;

	for (i = 0; i < gpio->gpio.ngpio; i++) {
		irq = irq_create_mapping(gpio->domain, i);
		if (irq == 0) {
			dev_err(&pdev->dev,
				"failed to create IRQ mapping for GPIO#%u\n",
				i);
			continue;
		}

		irq_set_lockdep_class(irq, &tegra186_gpio_lock_class);
		irq_set_chip_data(irq, gpio);
		irq_set_chip_and_handler(irq, &gpio->intc, handle_simple_irq);
	}

	for (i = 0; i < gpio->num_irq; i++)
		irq_set_chained_handler_and_data(gpio->irq[i],
						 tegra186_gpio_irq,
						 gpio);

	return 0;
}

static int tegra186_gpio_remove(struct platform_device *pdev)
{
	struct tegra_gpio *gpio = platform_get_drvdata(pdev);
	unsigned int i, irq;

	for (i = 0; i < gpio->num_irq; i++)
		irq_set_chained_handler_and_data(gpio->irq[i], NULL, NULL);

	for (i = 0; i < gpio->gpio.ngpio; i++) {
		irq = irq_find_mapping(gpio->domain, i);
		irq_dispose_mapping(irq);
	}

	irq_domain_remove(gpio->domain);

	return 0;
}

static const struct tegra_gpio_port tegra186_main_ports[] = {
	[TEGRA_MAIN_GPIO_PORT_A]  = { 0x2000, 7 },
	[TEGRA_MAIN_GPIO_PORT_B]  = { 0x3000, 7 },
	[TEGRA_MAIN_GPIO_PORT_C]  = { 0x3200, 7 },
	[TEGRA_MAIN_GPIO_PORT_D]  = { 0x3400, 6 },
	[TEGRA_MAIN_GPIO_PORT_E]  = { 0x2200, 8 },
	[TEGRA_MAIN_GPIO_PORT_F]  = { 0x2400, 6 },
	[TEGRA_MAIN_GPIO_PORT_G]  = { 0x4200, 6 },
	[TEGRA_MAIN_GPIO_PORT_H]  = { 0x1000, 7 },
	[TEGRA_MAIN_GPIO_PORT_I]  = { 0x0800, 8 },
	[TEGRA_MAIN_GPIO_PORT_J]  = { 0x5000, 8 },
	[TEGRA_MAIN_GPIO_PORT_K]  = { 0x5200, 1 },
	[TEGRA_MAIN_GPIO_PORT_L]  = { 0x1200, 8 },
	[TEGRA_MAIN_GPIO_PORT_M]  = { 0x5600, 6 },
	[TEGRA_MAIN_GPIO_PORT_N]  = { 0x0000, 7 },
	[TEGRA_MAIN_GPIO_PORT_O]  = { 0x0200, 4 },
	[TEGRA_MAIN_GPIO_PORT_P]  = { 0x4000, 7 },
	[TEGRA_MAIN_GPIO_PORT_Q]  = { 0x0400, 6 },
	[TEGRA_MAIN_GPIO_PORT_R]  = { 0x0a00, 6 },
	[TEGRA_MAIN_GPIO_PORT_T]  = { 0x0600, 4 },
	[TEGRA_MAIN_GPIO_PORT_X]  = { 0x1400, 8 },
	[TEGRA_MAIN_GPIO_PORT_Y]  = { 0x1600, 7 },
	[TEGRA_MAIN_GPIO_PORT_BB] = { 0x2600, 2 },
	[TEGRA_MAIN_GPIO_PORT_CC] = { 0x5400, 4 },
};

static const struct tegra_gpio_soc tegra186_main_soc = {
	.num_ports = ARRAY_SIZE(tegra186_main_ports),
	.ports = tegra186_main_ports,
};

static const struct tegra_gpio_port tegra186_aon_ports[] = {
	[TEGRA_AON_GPIO_PORT_S]  = { 0x0200, 5 },
	[TEGRA_AON_GPIO_PORT_U]  = { 0x0400, 6 },
	[TEGRA_AON_GPIO_PORT_V]  = { 0x0800, 8 },
	[TEGRA_AON_GPIO_PORT_W]  = { 0x0a00, 8 },
	[TEGRA_AON_GPIO_PORT_Z]  = { 0x0e00, 4 },
	[TEGRA_AON_GPIO_PORT_AA] = { 0x0c00, 8 },
	[TEGRA_AON_GPIO_PORT_EE] = { 0x0600, 3 },
	[TEGRA_AON_GPIO_PORT_FF] = { 0x0000, 5 },
};

static const struct tegra_gpio_soc tegra186_aon_soc = {
	.num_ports = ARRAY_SIZE(tegra186_aon_ports),
	.ports = tegra186_aon_ports,
};

static const struct of_device_id tegra186_gpio_of_match[] = {
	{
		.compatible = "nvidia,tegra186-gpio",
		.data = &tegra186_main_soc
	}, {
		.compatible = "nvidia,tegra186-gpio-aon",
		.data = &tegra186_aon_soc
	}, {
		/* sentinel */
	}
};

static struct platform_driver tegra186_gpio_driver = {
	.driver = {
		.name = "tegra186-gpio",
		.of_match_table = tegra186_gpio_of_match,
	},
	.probe = tegra186_gpio_probe,
	.remove = tegra186_gpio_remove,
};
module_platform_driver(tegra186_gpio_driver);

MODULE_DESCRIPTION("NVIDIA Tegra186 GPIO controller driver");
MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_LICENSE("GPL v2");
