#define DT_DRV_COMPAT ti_cc13xx_cc26xx_i2s

// #include "zephyr/devicetree.h"
#include <stdint.h>
#include <string.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/pinctrl.h>

#include <zephyr/logging/log.h>
#include <zephyr/irq.h>

#include <inc/hw_i2s.h>
// #inlude <inc/hw_power.h>
#include <driverlib/i2s.h>
#include <driverlib/prcm.h>
#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>
// #include <soc.h>

LOG_MODULE_REGISTER(i2s_ti_cc13xx_cc26xx);

/*
 * Unlike other SoCs, CC13XX has dedicated DMA for I2S operation which operates independently to
 * other DMA channels
 */
struct i2s_cc13xx_cc26xx_config {
	uint32_t base;
	const struct pinctrl_dev_config *pcfg;
};

struct i2s_cc13xx_cc26xx_data {
	uint32_t d1;
	uint32_t d2;
};

static int i2s_cc13xx_cc26xx_init(const struct device *dev){

	const struct i2s_cc13xx_cc26xx_config *config  = dev->config;
	const struct i2s_cc13xx_cc26xx_data *data = dev->data;
	const struct base = config->base;
	int err = 0;

	/* Enable I2C power domain */
	PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

	/* Enable I2C peripheral clock */
	PRCMPeripheralRunEnable(PRCM_PERIPH_I2S);
	/* Enable in sleep mode until proper power management is added */
	PRCMPeripheralSleepEnable(PRCM_PERIPH_I2S);
	PRCMPeripheralDeepSleepEnable(PRCM_PERIPH_I2S);

	/* Load PRCM settings */
	PRCMLoadSet();
	while (!PRCMLoadGet()) {
		continue;
	}

	/* I2C should not be accessed until power domain is on. */
	while (PRCMPowerDomainsAllOn(PRCM_DOMAIN_PERIPH) !=
	       PRCM_DOMAIN_POWER_ON) {
		continue;
	}

	err = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
	if (err < 0) {
		LOG_ERR("Failed to configure pinctrl state");
		return err;
	}
	int ret = 0;

	return ret;
}

static int i2s_cc13xx_cc26xx_configure(const struct device *dev, enum i2s_dir dir,
			       const struct i2s_config *i2s_cfg){
	int ret = 0;

	return ret;
}



static int i2s_cc13xx_cc26xx_read(const struct device *dev, void **mem_block,
			  size_t *size){

	int ret = 0;


	return ret;
}

static int i2s_cc13xx_cc26xx_write(const struct device *dev, void *mem_block,
			   size_t size){
	int ret = 0;

	return ret;
}
static int i2s_cc13xx_cc26xx_trigger(const struct device *dev, enum i2s_dir dir,
			     enum i2s_trigger_cmd cmd){
	int ret = 0;

	return ret;
}

static DEVICE_API(i2s, i2s_cc13xx_cc26xx_driver_api) = {
	.configure = i2s_cc13xx_cc26xx_configure,
	.read = i2s_cc13xx_cc26xx_read,
	.write = i2s_cc13xx_cc26xx_write,
	.trigger = i2s_cc13xx_cc26xx_trigger,
};


static const struct i2s_cc13xx_cc26xx_config i2s_cc13xx_cc26xx_config = {
	.base = DT_INST_REG_ADDR(0),
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
};

static const struct i2s_cc13xx_cc26xx_data i2s_cc13xx_cc26xx_data = {
	.d1 = 0,
	.d2 = 1,
};


DEVICE_DT_INST_DEFINE(0,
		i2s_cc13xx_cc26xx_init,
		NULL,
		&i2s_cc13xx_cc26xx_data,
		&i2s_cc13xx_cc26xx_config,
		POST_KERNEL, CONFIG_I2S_INIT_PRIORITY,
		&i2s_cc13xx_cc26xx_driver_api);

