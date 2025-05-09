#include "zephyr/kernel.h"
#include "zephyr/sys/time_units.h"
#include "zephyr/sys/util_macro.h"
#include "zephyr/toolchain.h"
#include <stdbool.h>
#define DT_DRV_COMPAT ti_cc13xx_cc26xx_i2s

#include <zephyr/devicetree.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
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

#define LOG_LEVEL CONFIG_I2S_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2s_cc13xx_cc26xx);

/* TODO: Replace this with DT_ based expression &cpu0->clock-frequency */
#define CPU_CLK_FREQ 48000000
#define I2S_CLOCK_DIVIDER_MAX 1024U
#define I2S_CLOCK_DIVIDER_MIN 2U
#define I2S_NB_CHANNELS_MAX   8U

#define I2S(idx) DT_NODELABEL(i2s##idx)

#define CHECK_IRQ(irqs, mask) ((irqs & (uint32_t)mask) != 0U)

/*
 * Unlike other SoCs, CC13XX has dedicated DMA for I2S operation which operates independently to
 * other DMA channels
 */
struct i2s_cc13xx_cc26xx_config {
	uint32_t base;
	const struct pinctrl_dev_config *pcfg;
};

struct i2s_queue_item {
	void *mem_block;
	size_t size;
};


struct dma_stream {
	struct k_msgq *q;
	void * buffers;
	uint16_t bufsize;
	struct i2s_config cfg;
	uint8_t dma_buf_incr;
};

struct i2s_cc13xx_cc26xx_data {
	enum i2s_state state;
	enum i2s_dir cur_dir;
	bool is_drv_master;
	struct dma_stream tx;
	struct dma_stream rx;
};

struct trx_fifo_item {
	void *next;
	struct i2s_buf buf;
};

K_FIFO_DEFINE(rx_fifo);
K_FIFO_DEFINE(tx_fifo);


static int queue_put(struct k_msgq *queue, void *mem_block, uint16_t buf_size){
	struct i2s_queue_item buf = {
		.mem_block = mem_block,
		.size = buf_size
	};

	int ret = k_msgq_put(queue, &buf, K_NO_WAIT);

	return ret;
}

static int queue_get(struct k_msgq *queue, void **mem_block, uint16_t *size){

	struct i2s_queue_item buf;

	int ret = k_msgq_get(queue, &buf, K_NO_WAIT);
	if (ret == 0) {
		*mem_block = buf.mem_block;
		*size = buf.size;
	}

	return ret;
}

static int i2s_rx_callback(struct device *dev){
	ARG_UNUSED(dev);
	return 0;
}

static int i2s_tx_callback(struct device *dev){
	ARG_UNUSED(dev);
	return 0;
}


static void free_tx_buffer(struct i2s_cc13xx_cc26xx_data *drv_data, void *buf){
	k_mem_slab_free(drv_data->tx.cfg.mem_slab, (void *)buf);
	return;
}

static void free_rx_buffer(struct i2s_cc13xx_cc26xx_data *drv_data, void *buf){
	k_mem_slab_free(drv_data->rx.cfg.mem_slab, buf);
	return;
}

// To be updated in DMA_IN_PTR
static int get_tx_next_buffer(struct i2s_cc13xx_cc26xx_data *drv_data){
	int ret = queue_get(drv_data->tx.q, (void **)(&drv_data->tx.buffers), &drv_data->tx.bufsize);

	return ret;
}

static bool get_mem_step(struct i2s_config *i2s_cfg, enum i2s_dir dir, uint8_t *dma_buf_size){
	uint16_t word_size = (uint16_t)i2s_cfg->word_size;
	uint16_t n_chan = (uint16_t)i2s_cfg->channels;
	uint16_t bytes_per_sample = 2;
	uint16_t block_size = (uint16_t)i2s_cfg->block_size;


	if ((i2s_cfg->word_size >= 8) && (i2s_cfg->word_size <= 16)) {
		bytes_per_sample = 2;
	} else if ((i2s_cfg->word_size > 16) && (i2s_cfg->word_size <= 24)) {
		bytes_per_sample = 3;
	} else {
		return false;
	}


	bytes_per_sample = n_chan * bytes_per_sample * 2;

	uint16_t dma_buf_incr = (((block_size / bytes_per_sample) * 2) - 1);

	if (dma_buf_incr > 255){
		LOG_ERR("Currently only buffer size upto 255 is supported");
		return false;
	}

	*dma_buf_size = (uint8_t)dma_buf_incr;
	return true;
}

static bool update_ptr(struct device *dev){
	const struct i2s_cc13xx_cc26xx_config *config = dev->config;
	struct i2s_cc13xx_cc26xx_data *drv_data = dev->data;

	if (drv_data->cur_dir == I2S_DIR_RX){
		void **rx_mem_block;
		struct dma_stream rx = drv_data->rx;
		int ret = k_mem_slab_alloc(rx.cfg.mem_slab, rx_mem_block, K_NO_WAIT);
		if (ret < 0){
			LOG_ERR("Not enough memory for rx_mem_block");
			return false;
		}

		drv_data->rx.buffers = *rx_mem_block;
		drv_data->rx.bufsize = drv_data->rx.dma_buf_incr; // TODO
		I2SInPointerSet(config->base, drv_data->rx.buffers);

	} else if (drv_data->cur_dir == I2S_DIR_TX){
		LOG_ERR("Not implemented now");
		return false;
	} else {
		return false
	}
}


static bool start_read(struct device *dev){
	struct i2s_cc13xx_cc26xx_data *drv_data = dev->data;
	const struct i2s_cc13xx_cc26xx_config *config = dev->config;

	if (drv_data->tx.dma_buf_incr != 0){
		I2SIntEnable(config->base,
				(uint32_t)I2S_INT_DMA_IN | (uint32_t)I2S_INT_TIMEOUT | (uint32_t)I2S_INT_BUS_ERR |
				(uint32_t)I2S_INT_WCLK_ERR | (uint32_t)I2S_INT_PTR_ERR);

		update_ptr(drv_data);
		update_ptr(drv_data);

		I2SSampleStampInConfigure(config->base, HWREGH(config->base + I2S_O_STMPWCNT));
	}

}


void i2s_cc13xx_cc26xx_isr(struct device *dev){
	const struct i2s_cc13xx_cc26xx_config *config = dev->config;
	struct i2s_cc13xx_cc26xx_data *data = dev->data;
	const uint32_t base = config->base;

	uint32_t irq_status = I2SIntStatus(config->base, false);
	LOG_INF("Recieved callback");


	if (CHECK_IRQ(irq_status, I2S_INT_DMA_IN)){
		(void)i2s_rx_callback(dev);
	}

	if (CHECK_IRQ(irq_status, I2S_INT_DMA_OUT)){
		(void)i2s_tx_callback(dev);
	}

	return;
}



static int get_clock_div(const struct i2s_config *i2s_cfg, uint32_t *bclk_div){
	if (i2s_cfg->frame_clk_freq == 0) {
		LOG_ERR("Frame clock frequency can not be non-positive: %d", i2s_cfg->frame_clk_freq);
		return -1;
	}
	uint32_t frame_clk_freq = i2s_cfg->frame_clk_freq;

	uint16_t n_chan;
	if (i2s_cfg->channels <= 2) {
		n_chan = 2;
	} else {
		LOG_ERR("Unsupported number of channels %d", i2s_cfg->channels);
		n_chan = 0;
		return -1;
	}

	uint32_t expected_bit_rate = i2s_cfg->word_size * n_chan * frame_clk_freq;

	uint32_t n_clk_div = (CPU_CLK_FREQ + (expected_bit_rate / 2)) / (expected_bit_rate);
	if ((n_clk_div >= I2S_CLOCK_DIVIDER_MIN) || (n_clk_div < I2S_CLOCK_DIVIDER_MAX)) {
		*bclk_div = n_clk_div;
		return 0;
	} else {
		LOG_ERR("Can not find suitable clock-divider for given bit-rate: %d %d", expected_bit_rate, n_clk_div);
		return -1;
	}
}

static int i2s_cc13xx_cc26xx_init(const struct device *dev){

	const struct i2s_cc13xx_cc26xx_config *config  = dev->config;
	struct i2s_cc13xx_cc26xx_data *data = dev->data;
	const uint32_t base = config->base;
	int err = 0;
	int ret = 0;

	/* Setting up interrupt for I2S interface */
	IRQ_CONNECT(DT_IRQN(I2S(0)), DT_IRQ(I2S(0), priority), i2s_cc13xx_cc26xx_isr, DEVICE_DT_INST_GET(0), 0);
	irq_enable(DT_INST_IRQN(0));

	/* Enable I2S power domain */
	PRCMPowerDomainOn(PRCM_DOMAIN_PERIPH);

	/* Enable I2S peripheral clock */
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

	data->state = I2S_STATE_READY;

	// I2SIntEnable(config->base, I2S_INT_PTR_ERR | I2S_INT_DMA_IN | I2S_INT_DMA_OUT);
	I2SIntClear(config->base, I2S_INT_ALL);
	I2SIntEnable(config->base, I2S_INT_ALL);

	return ret;
}

static int i2s_cc13xx_cc26xx_configure(const struct device *dev, enum i2s_dir dir,
			       const struct i2s_config *i2s_cfg){
	struct i2s_cc13xx_cc26xx_data *drv_data = dev->data;
	const struct i2s_cc13xx_cc26xx_config *config = dev->config;

	uint8_t sampling_edge;
	bool wclk_inv;
	bool lsb_first;


	if (drv_data->state != I2S_STATE_READY) {
		LOG_ERR("Cannot configure in state: %d", drv_data->state);
		return -EINVAL;
	}

	if (i2s_cfg->frame_clk_freq == 0){
		return 0;
	}

	__ASSERT_NO_MSG((i2s_cfg->mem_slab != NULL &&
			i2s_cfg->block_size != 0));


	bool dual_phase = true;

	uint8_t ad0_mask = 0;
	uint8_t ad1_mask = 0;

	/* Configuring Channels */
	if (i2s_cfg->channels == 1) {
		ad0_mask = 0x1;
	} else if (i2s_cfg->channels == 2) {
		ad0_mask = 0x3;
	} else {
		return -EINVAL;
	}


	// TODO: Review this
	sampling_edge = (i2s_cfg->format & I2S_FMT_BIT_CLK_INV) ? I2S_NEG_EDGE : I2S_POS_EDGE;
	wclk_inv = (i2s_cfg->format & I2S_FMT_FRAME_CLK_INV) ? true : false;

	// TODO: add support for rest of the data-format
	if (i2s_cfg->format & I2S_FMT_DATA_FORMAT_MASK != I2S_FMT_DATA_FORMAT_I2S){
		LOG_ERR("Currently only I2S data-format is supported, given format: %d", i2s_cfg->format);
		return -EINVAL;
	}

	/* I2S-DMA can transfer either 16-bits or 24-bits word */
	uint8_t mem_24_bits_aligned = 0;
	if ((i2s_cfg->word_size >= 8) && (i2s_cfg->word_size <= 16)) {
		mem_24_bits_aligned = 0;
	} else if ((i2s_cfg->word_size > 16) && (i2s_cfg->word_size <= 24)) {
		mem_24_bits_aligned = 1;
	} else {
		return -EINVAL;
	}

	lsb_first = (i2s_cfg->format & I2S_FMT_DATA_ORDER_LSB) ? true : false;
	if (lsb_first){
		LOG_ERR("Unsupported DATA_ORDER for I2S Format: %d", i2s_cfg->format);
		return -EINVAL;
	}

	uint8_t dma_buf_incr;

	if(!get_mem_step(i2s_cfg, dir, &dma_buf_incr)){
		LOG_ERR("Error finding DMA mem step");
	}

	uint8_t i2s_ad0_dir = 0;
	switch (dir) {
		case I2S_DIR_TX:
			i2s_ad0_dir = I2S_AIFDIRCFG_AD0_OUT;
			drv_data->tx.cfg = *i2s_cfg;
			drv_data->tx.dma_buf_incr = dma_buf_incr;
			break;
		case I2S_DIR_RX:
			i2s_ad0_dir = I2S_AIFDIRCFG_AD0_IN;
			drv_data->rx.cfg = *i2s_cfg;
			drv_data->rx.dma_buf_incr = dma_buf_incr;
			break;
		default:
			LOG_ERR("Unsupported i2s_dir : %d", (uint8_t)dir);
			return -EINVAL;
	}

	/* Either both the clocks should be internal or external */
	if ((i2s_cfg->options & I2S_OPT_BIT_CLK_SLAVE) &&
	    (i2s_cfg->options & I2S_OPT_FRAME_CLK_SLAVE)) {
		drv_data->is_drv_master = false;
	} else if (!(i2s_cfg->options & I2S_OPT_BIT_CLK_SLAVE) &&
		   !(i2s_cfg->options & I2S_OPT_FRAME_CLK_SLAVE)) {
		drv_data->is_drv_master = true;
	} else {
		LOG_ERR("Unsupported operation mode: 0x%02x", i2s_cfg->options);
		return -EINVAL;
	}



	/* Configuring serial format */
	uint16_t tx_delay = 0;
	I2SFormatConfigure(config->base,
				1,
				mem_24_bits_aligned,
				sampling_edge,
				dual_phase,
				i2s_cfg->word_size,
				dma_buf_incr+1
			);

	I2SSampleStampInConfigure(config->base, 0xFFFFU);
	I2SSampleStampOutConfigure(config->base, 0xFFFFU);

	I2SFrameConfigure(config->base,
			  i2s_ad0_dir,
			  ad0_mask,
			  I2S_AIFDIRCFG_AD1_DIS,
			  ad1_mask
			  );

	I2SWclkConfigure(config->base, drv_data->is_drv_master, wclk_inv);
	uint32_t n_bits_per_frame = i2s_cfg->word_size * 2;


	if (drv_data->is_drv_master) {
		uint32_t bclk_div = 0;
		if (get_clock_div(i2s_cfg, &bclk_div) != 0) {
			LOG_ERR("Error in computing clock divider for given bit-rate and frame-rate");
			return -EINVAL;
		}
		PRCMAudioClockInternalSource();
		PRCMAudioClockConfigOverride(sampling_edge,
					PRCM_I2S_WCLK_DUAL_PHASE,
					2,
					bclk_div,
					n_bits_per_frame
					);

		PRCMAudioClockEnable();
		LOG_INF("Driver Started ....");

		// PRCMLoadSet(); // TODO
	} else {
		PRCMAudioClockExternalSource();
	}


	return 0;
}


static int i2s_cc13xx_cc26xx_read(const struct device *dev, void **mem_block,
			  size_t *size){

	struct i2s_cc13xx_cc26xx_data *drv_data = dev->data;
	struct i2s_queue_item i2s_buf = {0};
	int ret;

	ret = k_msgq_get(drv_data->rx.q, &i2s_buf, SYS_TIMEOUT_MS(drv_data->rx.cfg.timeout));

	if (ret == -ENOMSG) {
		return -EIO;
	}

	// LOG_INF("Released RX %p", *buf.mem_block);

	if (ret == 0) {
		*mem_block = i2s_buf.mem_block;
		*size = i2s_buf.size;
	}

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

	struct i2s_cc13xx_cc26xx_data *drv_data = dev->data;

	if (dir == I2S_DIR_TX){

	} else if (dir == I2S_DIR_RX){

	} else {
		LOG_ERR("Both direction is not supported yet");
		return -EIO;
	}

	switch(cmd) {
		case I2S_TRIGGER_START:
			// if ()
			break;

		case I2S_TRIGGER_DROP:
			break;

		case I2S_TRIGGER_DRAIN:
			break;

		case I2S_TRIGGER_PREPARE:
			break;

		case I2S_TRIGGER_STOP:
			break;

		default:
			LOG_ERR("Unknown trigger cmd: %d", cmd);
	}

	return ret;
}

static DEVICE_API(i2s, i2s_cc13xx_cc26xx_driver_api) = {
	.configure = i2s_cc13xx_cc26xx_configure,
	.read = i2s_cc13xx_cc26xx_read,
	.write = i2s_cc13xx_cc26xx_write,
	.trigger = i2s_cc13xx_cc26xx_trigger,
};





PINCTRL_DT_INST_DEFINE(0);
K_MSGQ_DEFINE(tx_0_queue, sizeof(struct i2s_queue_item), CONFIG_I2S_CC13XX_CC26XX_TX_BLOCK_COUNT, 4);
K_MSGQ_DEFINE(rx_0_queue, sizeof(struct i2s_queue_item), CONFIG_I2S_CC13XX_CC26XX_RX_BLOCK_COUNT, 4);

static const struct i2s_cc13xx_cc26xx_config i2s_cc13xx_cc26xx_config = {
	.base = DT_INST_REG_ADDR(0),
	.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(0),
};

static struct i2s_cc13xx_cc26xx_data i2s_cc13xx_cc26xx_data = {
	.state = I2S_STATE_NOT_READY,
	.cur_dir = 0,
	.is_drv_master = false,
	.tx = {0},
	.rx = {0},
};

DEVICE_DT_INST_DEFINE(0,
		i2s_cc13xx_cc26xx_init,
		NULL,
		&i2s_cc13xx_cc26xx_data,
		&i2s_cc13xx_cc26xx_config,
		POST_KERNEL, CONFIG_I2S_INIT_PRIORITY,
		&i2s_cc13xx_cc26xx_driver_api);

