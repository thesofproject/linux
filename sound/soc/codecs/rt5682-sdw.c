// SPDX-License-Identifier: GPL-2.0-only
/*
 * rt5682-sdw.c  --  RT5682 ALSA SoC audio component driver
 *
 * Copyright 2019 Realtek Semiconductor Corp.
 * Author: Oder Chiou <oder_chiou@realtek.com>
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/acpi.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <linux/soundwire/sdw.h>
#include <linux/soundwire/sdw_type.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>

#include "rt5682.h"
#include "rt5682-sdw.h"

struct sdw_stream_data {
	struct sdw_stream_runtime *sdw_stream;
};

int rt5682_set_sdw_stream(struct snd_soc_dai *dai, void *sdw_stream,
				int direction)
{
	struct sdw_stream_data *stream;

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;

	stream->sdw_stream = (struct sdw_stream_runtime *)sdw_stream;

	/* Use tx_mask or rx_mask to configure stream tag and set dma_data */
	if (direction == SNDRV_PCM_STREAM_PLAYBACK)
		dai->playback_dma_data = stream;
	else
		dai->capture_dma_data = stream;

	return 0;
}
EXPORT_SYMBOL_GPL(rt5682_set_sdw_stream);

void rt5682_sdw_shutdown(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct sdw_stream_data *stream;

	stream = snd_soc_dai_get_dma_data(dai, substream);
	snd_soc_dai_set_dma_data(dai, substream, NULL);
	kfree(stream);
}
EXPORT_SYMBOL_GPL(rt5682_sdw_shutdown);

int rt5682_sdw_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct rt5682_priv *rt5682 = snd_soc_component_get_drvdata(component);
	struct sdw_stream_config stream_config;
	struct sdw_port_config port_config;
	enum sdw_data_direction direction;
	struct sdw_stream_data *stream;
	int retval, port, num_channels;
	unsigned int val = 0;

	dev_dbg(dai->dev, "%s %s", __func__, dai->name);
	stream = snd_soc_dai_get_dma_data(dai, substream);

	if (!stream)
		return -ENOMEM;

	if (!rt5682->slave)
		return -EINVAL;

	/* SoundWire specific configuration */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		direction = SDW_DATA_DIR_RX;
		port = 1;
	} else {
		direction = SDW_DATA_DIR_TX;
		port = 2;
	}

	stream_config.frame_rate = params_rate(params);
	stream_config.ch_count = params_channels(params);
	stream_config.bps = snd_pcm_format_width(params_format(params));
	stream_config.direction = direction;

	num_channels = params_channels(params);
	port_config.ch_mask = (1 << (num_channels)) - 1;
	port_config.num = port;

	retval = sdw_stream_add_slave(rt5682->slave, &stream_config,
				      &port_config, 1, stream->sdw_stream);
	if (retval) {
		dev_err(dai->dev, "Unable to configure port\n");
		return retval;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		switch (params_rate(params)) {
		case 48000:
			val = RT5682_SDW_REF_1_48K;
			break;
		case 96000:
			val = RT5682_SDW_REF_1_96K;
			break;
		case 192000:
			val = RT5682_SDW_REF_1_192K;
			break;
		case 32000:
			val = RT5682_SDW_REF_1_32K;
			break;
		case 24000:
			val = RT5682_SDW_REF_1_24K;
			break;
		case 16000:
			val = RT5682_SDW_REF_1_16K;
			break;
		case 12000:
			val = RT5682_SDW_REF_1_12K;
			break;
		case 8000:
			val = RT5682_SDW_REF_1_8K;
			break;
		case 44100:
			val = RT5682_SDW_REF_1_44K;
			break;
		case 88200:
			val = RT5682_SDW_REF_1_88K;
			break;
		case 176400:
			val = RT5682_SDW_REF_1_176K;
			break;
		case 22050:
			val = RT5682_SDW_REF_1_22K;
			break;
		case 11025:
			val = RT5682_SDW_REF_1_11K;
			break;
		default:
			return -EINVAL;
		}

		regmap_update_bits(rt5682->regmap, RT5682_SDW_REF_CLK,
			RT5682_SDW_REF_1_MASK, val);

		if (params_rate(params) <= 48000)
			val = RT5682_DAC_OSR_D_8;
		else if (params_rate(params) <= 96000)
			val = RT5682_DAC_OSR_D_4;
		else
			val = RT5682_DAC_OSR_D_2;

		regmap_update_bits(rt5682->regmap, RT5682_ADDA_CLK_1,
			RT5682_DAC_OSR_MASK, val);
	} else {
		switch (params_rate(params)) {
		case 48000:
			val = RT5682_SDW_REF_2_48K;
			break;
		case 96000:
			val = RT5682_SDW_REF_2_96K;
			break;
		case 192000:
			val = RT5682_SDW_REF_2_192K;
			break;
		case 32000:
			val = RT5682_SDW_REF_2_32K;
			break;
		case 24000:
			val = RT5682_SDW_REF_2_24K;
			break;
		case 16000:
			val = RT5682_SDW_REF_2_16K;
			break;
		case 12000:
			val = RT5682_SDW_REF_2_12K;
			break;
		case 8000:
			val = RT5682_SDW_REF_2_8K;
			break;
		case 44100:
			val = RT5682_SDW_REF_2_44K;
			break;
		case 88200:
			val = RT5682_SDW_REF_2_88K;
			break;
		case 176400:
			val = RT5682_SDW_REF_2_176K;
			break;
		case 22050:
			val = RT5682_SDW_REF_2_22K;
			break;
		case 11025:
			val = RT5682_SDW_REF_2_11K;
			break;
		default:
			return -EINVAL;
		}

		regmap_update_bits(rt5682->regmap, RT5682_SDW_REF_CLK,
			RT5682_SDW_REF_2_MASK, val);

		if (params_rate(params) <= 48000)
			val = RT5682_ADC_OSR_D_8;
		else if (params_rate(params) <= 96000)
			val = RT5682_ADC_OSR_D_4;
		else
			val = RT5682_ADC_OSR_D_2;

		regmap_update_bits(rt5682->regmap, RT5682_ADDA_CLK_1,
			RT5682_ADC_OSR_MASK, val);
	}

	return retval;
}
EXPORT_SYMBOL_GPL(rt5682_sdw_hw_params);

int rt5682_sdw_hw_free(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_component *component = dai->component;
	struct rt5682_priv *rt5682 = snd_soc_component_get_drvdata(component);
	struct sdw_stream_data *stream =
		snd_soc_dai_get_dma_data(dai, substream);

	if (!rt5682->slave)
		return -EINVAL;

	sdw_stream_remove_slave(rt5682->slave, stream->sdw_stream);
	return 0;
}
EXPORT_SYMBOL_GPL(rt5682_sdw_hw_free);

int rt5682_sdw_read(void *context, unsigned int reg, unsigned int *val)
{
	struct device *dev = context;
	struct rt5682_priv *rt5682 = dev_get_drvdata(dev);
	unsigned int data_l, data_h;

	regmap_write(rt5682->sdw_regmap, RT5682_SDW_CMD, 0);
	regmap_write(rt5682->sdw_regmap, RT5682_SDW_ADDR_H, (reg >> 8) & 0xff);
	regmap_write(rt5682->sdw_regmap, RT5682_SDW_ADDR_L, (reg & 0xff));
	regmap_read(rt5682->sdw_regmap, RT5682_SDW_DATA_H, &data_h);
	regmap_read(rt5682->sdw_regmap, RT5682_SDW_DATA_L, &data_l);

	*val = (data_h << 8) | data_l;

	dev_vdbg(dev, "[%s] %04x => %04x\n", __func__, reg, *val);

	return 0;
}
EXPORT_SYMBOL_GPL(rt5682_sdw_read);

int rt5682_sdw_write(void *context, unsigned int reg, unsigned int val)
{
	struct device *dev = context;
	struct rt5682_priv *rt5682 = dev_get_drvdata(dev);

	regmap_write(rt5682->sdw_regmap, RT5682_SDW_CMD, 1);
	regmap_write(rt5682->sdw_regmap, RT5682_SDW_ADDR_H, (reg >> 8) & 0xff);
	regmap_write(rt5682->sdw_regmap, RT5682_SDW_ADDR_L, (reg & 0xff));
	regmap_write(rt5682->sdw_regmap, RT5682_SDW_DATA_H, (val >> 8) & 0xff);
	regmap_write(rt5682->sdw_regmap, RT5682_SDW_DATA_L, (val & 0xff));

	dev_vdbg(dev, "[%s] %04x <= %04x\n", __func__, reg, val);

	return 0;
}
EXPORT_SYMBOL_GPL(rt5682_sdw_write);

static bool rt5682_sdw_readable_register(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0x00e0:
	case 0x00f0:
	case 0x3000:
	case 0x3001:
	case 0x3004:
	case 0x3005:
	case 0x3008:
		return true;
	default:
		return false;
	}
}

const struct regmap_config rt5682_sdw_regmap = {
	.name = "sdw",
	.reg_bits = 32,
	.val_bits = 8,
	.max_register = RT5682_I2C_MODE,
	.readable_reg = rt5682_sdw_readable_register,
	.cache_type = REGCACHE_NONE,
	.use_single_read = true,
	.use_single_write = true,
};

static int rt5682_update_status(struct sdw_slave *slave,
					enum sdw_slave_status status)
{
	struct rt5682_priv *rt5682 = dev_get_drvdata(&slave->dev);

	/* Update the status */
	rt5682->status = status;

	if (status == SDW_SLAVE_UNATTACHED)
		rt5682->hw_init = false;

	/*
	 * Perform initialization only if slave status is present and
	 * hw_init flag is false
	 */
	if (rt5682->hw_init || rt5682->status != SDW_SLAVE_ATTACHED)
		return 0;

	/* perform I/O transfers required for Slave initialization */
	return rt5682_io_init(&slave->dev, slave);
}

static int rt5682_read_prop(struct sdw_slave *slave)
{
	struct sdw_slave_prop *prop = &slave->prop;
	int nval, i, num_of_ports = 1;
	u32 bit;
	unsigned long addr;
	struct sdw_dpn_prop *dpn;

	prop->paging_support = false;

	/* first we need to allocate memory for set bits in port lists */
	prop->source_ports = 0x4;	/* BITMAP: 00000100 */
	prop->sink_ports = 0x2;		/* BITMAP: 00000010 */

	nval = hweight32(prop->source_ports);
	num_of_ports += nval;
	prop->src_dpn_prop = devm_kcalloc(&slave->dev, nval,
					  sizeof(*prop->src_dpn_prop),
					  GFP_KERNEL);
	if (!prop->src_dpn_prop)
		return -ENOMEM;

	i = 0;
	dpn = prop->src_dpn_prop;
	addr = prop->source_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
		dpn[i].type = SDW_DPN_FULL;
		dpn[i].simple_ch_prep_sm = true;
		dpn[i].ch_prep_timeout = 10;
		i++;
	}

	/* do this again for sink now */
	nval = hweight32(prop->sink_ports);
	num_of_ports += nval;
	prop->sink_dpn_prop = devm_kcalloc(&slave->dev, nval,
					   sizeof(*prop->sink_dpn_prop),
					   GFP_KERNEL);
	if (!prop->sink_dpn_prop)
		return -ENOMEM;

	i = 0;
	dpn = prop->sink_dpn_prop;
	addr = prop->sink_ports;
	for_each_set_bit(bit, &addr, 32) {
		dpn[i].num = bit;
		dpn[i].type = SDW_DPN_FULL;
		dpn[i].simple_ch_prep_sm = true;
		dpn[i].ch_prep_timeout = 10;
		i++;
	}

	/* Allocate port_ready based on num_of_ports */
	slave->port_ready = devm_kcalloc(&slave->dev, num_of_ports,
					 sizeof(*slave->port_ready),
					 GFP_KERNEL);
	if (!slave->port_ready)
		return -ENOMEM;

	/* Initialize completion */
	for (i = 0; i < num_of_ports; i++)
		init_completion(&slave->port_ready[i]);

	/* set the timeout values */
	prop->clk_stop_timeout = 20;

	return 0;
}

/* Bus clock frequency */
#define RT5682_CLK_FREQ_9600000HZ 9600000
#define RT5682_CLK_FREQ_12000000HZ 12000000
#define RT5682_CLK_FREQ_6000000HZ 6000000
#define RT5682_CLK_FREQ_4800000HZ 4800000
#define RT5682_CLK_FREQ_2400000HZ 2400000
#define RT5682_CLK_FREQ_12288000HZ 12288000

int rt5682_clock_config(struct device *dev)
{
	struct rt5682_priv *rt5682 = dev_get_drvdata(dev);
	unsigned int clk_freq, value;

	clk_freq = (rt5682->params.curr_dr_freq >> 1);

	switch (clk_freq) {
	case RT5682_CLK_FREQ_12000000HZ:
		value = 0x0;
		break;
	case RT5682_CLK_FREQ_6000000HZ:
		value = 0x1;
		break;
	case RT5682_CLK_FREQ_9600000HZ:
		value = 0x2;
		break;
	case RT5682_CLK_FREQ_4800000HZ:
		value = 0x3;
		break;
	case RT5682_CLK_FREQ_2400000HZ:
		value = 0x4;
		break;
	case RT5682_CLK_FREQ_12288000HZ:
		value = 0x5;
		break;
	default:
		return -EINVAL;
	}

	regmap_write(rt5682->sdw_regmap, 0xe0, value);
	regmap_write(rt5682->sdw_regmap, 0xf0, value);

	dev_dbg(dev, "%s complete, clk_freq=%d\n", __func__, clk_freq);

	return 0;
}

static int rt5682_bus_config(struct sdw_slave *slave,
					struct sdw_bus_params *params)
{
	struct rt5682_priv *rt5682 = dev_get_drvdata(&slave->dev);
	int ret;

	memcpy(&rt5682->params, params, sizeof(*params));

	ret = rt5682_clock_config(&slave->dev);
	if (ret < 0)
		dev_err(&slave->dev, "Invalid clk config");

	return ret;
}

static int rt5682_interrupt_callback(struct sdw_slave *slave,
					struct sdw_slave_intr_status *status)
{
	struct rt5682_priv *rt5682 = dev_get_drvdata(&slave->dev);

	dev_dbg(&slave->dev,
		"%s control_port_stat=%x", __func__, status->control_port);

	if (status->control_port & 0x4) {
		mod_delayed_work(system_power_efficient_wq,
			&rt5682->jack_detect_work, msecs_to_jiffies(250));
	}

	return 0;
}

static struct sdw_slave_ops rt5682_slave_ops = {
	.read_prop = rt5682_read_prop,
	.interrupt_callback = rt5682_interrupt_callback,
	.update_status = rt5682_update_status,
	.bus_config = rt5682_bus_config,
};

static int rt5682_sdw_probe(struct sdw_slave *slave,
			   const struct sdw_device_id *id)
{
	struct regmap *regmap;

	/* Assign ops */
	slave->ops = &rt5682_slave_ops;

	/* Regmap Initialization */
	regmap = devm_regmap_init_sdw(slave, &rt5682_sdw_regmap);
	if (!regmap)
		return -EINVAL;

	rt5682_sdw_init(&slave->dev, regmap, slave);

	return 0;
}

static int rt5682_sdw_remove(struct sdw_slave *slave)
{
	struct rt5682_priv *rt5682 = dev_get_drvdata(&slave->dev);

	if (rt5682 && rt5682->hw_init)
		cancel_delayed_work(&rt5682->jack_detect_work);

	return 0;
}

static const struct sdw_device_id rt5682_id[] = {
	SDW_SLAVE_ENTRY(0x025d, 0x5682, 0),
	{},
};
MODULE_DEVICE_TABLE(sdw, rt5682_id);

static int rt5682_dev_suspend(struct device *dev)
{
	struct rt5682_priv *rt5682 = dev_get_drvdata(dev);

	if (!rt5682->hw_init)
		return 0;

	regcache_cache_only(rt5682->regmap, true);
	regcache_mark_dirty(rt5682->regmap);

	return 0;
}

#define rt5682_PROBE_TIMEOUT 2000

static int rt5682_dev_resume(struct device *dev)
{
	struct sdw_slave *slave = to_sdw_slave_device(dev);
	struct rt5682_priv *rt5682 = dev_get_drvdata(dev);
	unsigned long time;

	if (!rt5682->hw_init)
		return 0;

	if (!slave->unattach_request)
		goto regmap_sync;

	time = wait_for_completion_timeout(&slave->initialization_complete,
				msecs_to_jiffies(rt5682_PROBE_TIMEOUT));
	if (!time) {
		dev_err(&slave->dev, "Initialization not complete, timed out\n");
		return -ETIMEDOUT;
	}

regmap_sync:
	slave->unattach_request = 0;
	regcache_cache_only(rt5682->regmap, false);
	regcache_sync(rt5682->regmap);

	return 0;
}

static const struct dev_pm_ops rt5682_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(rt5682_dev_suspend, rt5682_dev_resume)
	SET_RUNTIME_PM_OPS(rt5682_dev_suspend, rt5682_dev_resume, NULL)
};

static struct sdw_driver rt5682_sdw_driver = {
	.driver = {
		.name = "rt5682",
		.owner = THIS_MODULE,
		.pm = &rt5682_pm,
	},
	.probe = rt5682_sdw_probe,
	.remove = rt5682_sdw_remove,
	.ops = &rt5682_slave_ops,
	.id_table = rt5682_id,
};
module_sdw_driver(rt5682_sdw_driver);

MODULE_DESCRIPTION("ASoC RT5682 driver SDW");
MODULE_AUTHOR("Oder Chiou <oder_chiou@realtek.com>");
MODULE_LICENSE("GPL v2");
