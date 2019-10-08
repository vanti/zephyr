/*
 * Copyright (c) 2019 Brett Witherspoon
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <sys/__assert.h>
#include <device.h>
#include <errno.h>
#include <drivers/gpio.h>

#include <driverlib/gpio.h>
#include <driverlib/interrupt.h>
#include <driverlib/ioc.h>
#include <driverlib/prcm.h>

#include "gpio_utils.h"

/* bits 16-18 in iocfg registers correspond to interrupt settings */
#define IOCFG_INT_MASK    0x00070000

/* the rest are for general (non-interrupt) config */
#define IOCFG_GEN_MASK    (~IOCFG_INT_MASK)

struct gpio_cc13xx_cc26xx_data {
	/* gpio_driver_data needs to be first */
	struct gpio_driver_data common;
	sys_slist_t callbacks;
	u32_t pin_callback_enables;
};

static struct gpio_cc13xx_cc26xx_data gpio_cc13xx_cc26xx_data_0;

static int gpio_cc13xx_cc26xx_port_set_bits_raw(struct device *port,
	u32_t mask);
static int gpio_cc13xx_cc26xx_port_clear_bits_raw(struct device *port,
	u32_t mask);

static int gpio_cc13xx_cc26xx_config(struct device *port, int access_op,
				     u32_t pin, int flags)
{
	u32_t config = 0;

	if (access_op != GPIO_ACCESS_BY_PIN) {
		return -ENOTSUP;
	}

	if (((flags & GPIO_INPUT) != 0) && ((flags & GPIO_OUTPUT) != 0)) {
		return -ENOTSUP;
	}

	if (flags == GPIO_DISCONNECTED) {
		IOCPortConfigureSet(pin, IOC_PORT_GPIO, 0);
		GPIO_setOutputEnableDio(pin, GPIO_OUTPUT_DISABLE);
		return 0;
	}

	__ASSERT_NO_MSG(pin < NUM_IO_MAX);

	config = IOC_CURRENT_2MA | IOC_STRENGTH_AUTO | IOC_SLEW_DISABLE |
		 IOC_NO_WAKE_UP;

	config |= (flags & GPIO_INT_DEBOUNCE) ? IOC_HYST_ENABLE :
							IOC_HYST_DISABLE;

	if ((flags & GPIO_INPUT) != 0) {
		config |= IOC_INPUT_ENABLE;
	} else {
		config |= IOC_INPUT_DISABLE;
	}

	switch (flags & GPIO_PUD_MASK) {
	case GPIO_PUD_NORMAL:
		config |= IOC_NO_IOPULL;
		break;
	case GPIO_PUD_PULL_UP:
		config |= IOC_IOPULL_UP;
		break;
	case GPIO_PUD_PULL_DOWN:
		config |= IOC_IOPULL_DOWN;
		break;
	default:
		return -EINVAL;
	}

	config |= IOCPortConfigureGet(pin) & IOCFG_INT_MASK;
	IOCPortConfigureSet(pin, IOC_PORT_GPIO, config);

	if ((flags & GPIO_OUTPUT) != 0) {
		if ((flags & GPIO_OUTPUT_INIT_HIGH) != 0) {
			gpio_cc13xx_cc26xx_port_set_bits_raw(port, BIT(pin));
		} else if ((flags & GPIO_OUTPUT_INIT_LOW) != 0) {
			gpio_cc13xx_cc26xx_port_clear_bits_raw(port, BIT(pin));
		}
		GPIO_setOutputEnableDio(pin, GPIO_OUTPUT_ENABLE);
	} else {
		GPIO_setOutputEnableDio(pin, GPIO_OUTPUT_DISABLE);
	}

	return 0;
}

static int gpio_cc13xx_cc26xx_write(struct device *port, int access_op,
				    u32_t pin, u32_t value)
{
	switch (access_op) {
	case GPIO_ACCESS_BY_PIN:
		__ASSERT_NO_MSG(pin < NUM_IO_MAX);
		if (value) {
			GPIO_setDio(pin);
		} else {
			GPIO_clearDio(pin);
		}
		break;
	case GPIO_ACCESS_BY_PORT:
		if (value) {
			GPIO_setMultiDio(GPIO_DIO_ALL_MASK);
		} else {
			GPIO_clearMultiDio(GPIO_DIO_ALL_MASK);
		}
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int gpio_cc13xx_cc26xx_read(struct device *port, int access_op,
				   u32_t pin, u32_t *value)
{
	__ASSERT_NO_MSG(value != NULL);

	switch (access_op) {
	case GPIO_ACCESS_BY_PIN:
		__ASSERT_NO_MSG(pin < NUM_IO_MAX);
		*value = GPIO_readDio(pin);
		break;
	case GPIO_ACCESS_BY_PORT:
		*value = GPIO_readMultiDio(GPIO_DIO_ALL_MASK);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int gpio_cc13xx_cc26xx_port_get_raw(struct device *port, u32_t *value)
{
	__ASSERT_NO_MSG(value != NULL);

	*value = GPIO_readMultiDio(GPIO_DIO_ALL_MASK);

	return 0;
}

static int gpio_cc13xx_cc26xx_port_set_masked_raw(struct device *port,
	u32_t mask, u32_t value)
{
	GPIO_setMultiDio(mask & value);
	GPIO_clearMultiDio(mask & ~value);

	return 0;
}

static int gpio_cc13xx_cc26xx_port_set_bits_raw(struct device *port, u32_t mask)
{
	GPIO_setMultiDio(mask);

	return 0;
}

static int gpio_cc13xx_cc26xx_port_clear_bits_raw(struct device *port,
	u32_t mask)
{
	GPIO_clearMultiDio(mask);

	return 0;
}

static int gpio_cc13xx_cc26xx_port_toggle_bits(struct device *port, u32_t mask)
{
	GPIO_toggleMultiDio(mask);

	return 0;
}

static int gpio_cc13xx_cc26xx_pin_interrupt_configure(struct device *port,
		unsigned int pin, enum gpio_int_mode mode,
		enum gpio_int_trig trig)
{
	struct gpio_cc13xx_cc26xx_data *data = port->driver_data;
	u32_t config = 0;

	if (mode != GPIO_INT_MODE_DISABLED) {
		if (mode == GPIO_INT_MODE_EDGE) {
			if (trig == GPIO_INT_TRIG_BOTH) {
				config |= IOC_BOTH_EDGES;
			} else if (trig == GPIO_INT_TRIG_HIGH) {
				config |= IOC_RISING_EDGE;
			} else { /* GPIO_INT_TRIG_LOW */
				config |= IOC_FALLING_EDGE;
			}
		} else {
			return -ENOTSUP;
		}

		config |= IOC_INT_ENABLE;
	} else {
		config |= IOC_INT_DISABLE | IOC_NO_EDGE;
	}

	config |= IOCPortConfigureGet(pin) & IOCFG_GEN_MASK;
	IOCPortConfigureSet(pin, IOC_PORT_GPIO, config);

	WRITE_BIT(data->pin_callback_enables, pin,
		mode != GPIO_INT_MODE_DISABLED);

	return 0;
}

static int gpio_cc13xx_cc26xx_manage_callback(struct device *port,
					      struct gpio_callback *callback,
					      bool set)
{
	struct gpio_cc13xx_cc26xx_data *data = port->driver_data;

	return gpio_manage_callback(&data->callbacks, callback, set);
}

static int gpio_cc13xx_cc26xx_enable_callback(struct device *port,
					      int access_op, u32_t pin)
{
	struct gpio_cc13xx_cc26xx_data *data = port->driver_data;

	switch (access_op) {
	case GPIO_ACCESS_BY_PIN:
		__ASSERT_NO_MSG(pin < NUM_IO_MAX);
		data->pin_callback_enables |= (1 << pin);
		break;
	case GPIO_ACCESS_BY_PORT:
		data->pin_callback_enables = 0xFFFFFFFF;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int gpio_cc13xx_cc26xx_disable_callback(struct device *port,
					       int access_op, u32_t pin)
{
	struct gpio_cc13xx_cc26xx_data *data = port->driver_data;

	switch (access_op) {
	case GPIO_ACCESS_BY_PIN:
		__ASSERT_NO_MSG(pin < NUM_IO_MAX);
		data->pin_callback_enables &= ~(1 << pin);
		break;
	case GPIO_ACCESS_BY_PORT:
		data->pin_callback_enables = 0U;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static u32_t gpio_cc13xx_cc26xx_get_pending_int(struct device *dev)
{
	return GPIO_getEventMultiDio(GPIO_DIO_ALL_MASK);
}

DEVICE_DECLARE(gpio_cc13xx_cc26xx);

static void gpio_cc13xx_cc26xx_isr(void *arg)
{
	struct device *dev = arg;
	struct gpio_cc13xx_cc26xx_data *data = dev->driver_data;

	u32_t status = GPIO_getEventMultiDio(GPIO_DIO_ALL_MASK);
	u32_t enabled = status & data->pin_callback_enables;

	GPIO_clearEventMultiDio(status);

	gpio_fire_callbacks(&data->callbacks, dev, enabled);
}

static int gpio_cc13xx_cc26xx_init(struct device *dev)
{
	struct gpio_cc13xx_cc26xx_data *data = dev->driver_data;

	/* Enable peripheral power domain */
	PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

	/* Enable GPIO peripheral */
	PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);

	/* Load PRCM settings */
	PRCMLoadSet();
	while (!PRCMLoadGet()) {
		continue;
	}

	/* Enable IRQ */
	IRQ_CONNECT(DT_INST_0_TI_CC13XX_CC26XX_GPIO_IRQ_0,
		    DT_INST_0_TI_CC13XX_CC26XX_GPIO_IRQ_0_PRIORITY,
		    gpio_cc13xx_cc26xx_isr, DEVICE_GET(gpio_cc13xx_cc26xx), 0);
	irq_enable(DT_INST_0_TI_CC13XX_CC26XX_GPIO_IRQ_0);

	/* Disable callbacks */
	data->pin_callback_enables = 0;

	/* Peripheral should not be accessed until power domain is on. */
	while (PRCMPowerDomainStatus(PRCM_DOMAIN_PERIPH) !=
	       PRCM_DOMAIN_POWER_ON) {
		continue;
	}

	return 0;
}

static const struct gpio_driver_api gpio_cc13xx_cc26xx_driver_api = {
	.config = gpio_cc13xx_cc26xx_config,
	.write = gpio_cc13xx_cc26xx_write,
	.read = gpio_cc13xx_cc26xx_read,
	.port_get_raw = gpio_cc13xx_cc26xx_port_get_raw,
	.port_set_masked_raw = gpio_cc13xx_cc26xx_port_set_masked_raw,
	.port_set_bits_raw = gpio_cc13xx_cc26xx_port_set_bits_raw,
	.port_clear_bits_raw = gpio_cc13xx_cc26xx_port_clear_bits_raw,
	.port_toggle_bits = gpio_cc13xx_cc26xx_port_toggle_bits,
	.pin_interrupt_configure = gpio_cc13xx_cc26xx_pin_interrupt_configure,
	.manage_callback = gpio_cc13xx_cc26xx_manage_callback,
	.enable_callback = gpio_cc13xx_cc26xx_enable_callback,
	.disable_callback = gpio_cc13xx_cc26xx_disable_callback,
	.get_pending_int = gpio_cc13xx_cc26xx_get_pending_int
};

DEVICE_AND_API_INIT(gpio_cc13xx_cc26xx, DT_INST_0_TI_CC13XX_CC26XX_GPIO_LABEL,
		    gpio_cc13xx_cc26xx_init, &gpio_cc13xx_cc26xx_data_0, NULL,
		    POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,
		    &gpio_cc13xx_cc26xx_driver_api);
