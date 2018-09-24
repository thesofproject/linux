// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Authors: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//	    Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//	    Jeeja KP <jeeja.kp@intel.com>
//	    Rander Wang <rander.wang@intel.com>
//          Keyon Jie <yang.jie@linux.intel.com>
//

/*
 * Hardware interface for HDA DSP code loader
 */

#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/pci.h>
#include <sound/hdaudio_ext.h>
#include <sound/sof.h>
#include <sound/pcm_params.h>
#include <linux/pm_runtime.h>

#include "../sof-priv.h"
#include "../ops.h"
#include "hda.h"

//#include "../../intel/skylake/skl-sst-cldma.h"

#define SKL_MAX_BUFFER_SIZE		(32 * PAGE_SIZE)

/* Stream Reset */
#define HDA_CL_SD_CTL_SRST_SHIFT	0
#define HDA_CL_SD_CTL_SRST(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_SRST_SHIFT)

/* Stream Run */
#define HDA_CL_SD_CTL_RUN_SHIFT		1
#define HDA_CL_SD_CTL_RUN(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_RUN_SHIFT)

/* Interrupt On Completion Enable */
#define HDA_CL_SD_CTL_IOCE_SHIFT	2
#define HDA_CL_SD_CTL_IOCE(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_IOCE_SHIFT)

/* FIFO Error Interrupt Enable */
#define HDA_CL_SD_CTL_FEIE_SHIFT	3
#define HDA_CL_SD_CTL_FEIE(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_FEIE_SHIFT)

/* Descriptor Error Interrupt Enable */
#define HDA_CL_SD_CTL_DEIE_SHIFT	4
#define HDA_CL_SD_CTL_DEIE(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_DEIE_SHIFT)

/* FIFO Limit Change */
#define HDA_CL_SD_CTL_FIFOLC_SHIFT	5
#define HDA_CL_SD_CTL_FIFOLC(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_FIFOLC_SHIFT)

/* Stripe Control */
#define HDA_CL_SD_CTL_STRIPE_SHIFT	16
#define HDA_CL_SD_CTL_STRIPE(x)		(((x) & 0x3) << \
					HDA_CL_SD_CTL_STRIPE_SHIFT)

/* Traffic Priority */
#define HDA_CL_SD_CTL_TP_SHIFT		18
#define HDA_CL_SD_CTL_TP(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_TP_SHIFT)

/* Bidirectional Direction Control */
#define HDA_CL_SD_CTL_DIR_SHIFT		19
#define HDA_CL_SD_CTL_DIR(x)		(((x) & 0x1) << \
					HDA_CL_SD_CTL_DIR_SHIFT)

/* Stream Number */
#define HDA_CL_SD_CTL_STRM_SHIFT	20
#define HDA_CL_SD_CTL_STRM(x)		(((x) & 0xf) << \
					HDA_CL_SD_CTL_STRM_SHIFT)

#define HDA_CL_SD_CTL_INT(x)	\
		(HDA_CL_SD_CTL_IOCE(x) | \
		HDA_CL_SD_CTL_FEIE(x) | \
		HDA_CL_SD_CTL_DEIE(x))

#define HDA_CL_SD_CTL_INT_MASK	\
		(HDA_CL_SD_CTL_IOCE(1) | \
		HDA_CL_SD_CTL_FEIE(1) | \
		HDA_CL_SD_CTL_DEIE(1))

#define DMA_ADDRESS_128_BITS_ALIGNMENT	7
#define BDL_ALIGN(x)			((x) >> DMA_ADDRESS_128_BITS_ALIGNMENT)

/* Buffer Descriptor List Lower Base Address */
#define HDA_CL_SD_BDLPLBA_SHIFT		7
#define HDA_CL_SD_BDLPLBA_MASK		(0x1ffffff << HDA_CL_SD_BDLPLBA_SHIFT)
#define HDA_CL_SD_BDLPLBA(x)		\
	((BDL_ALIGN(lower_32_bits(x)) << HDA_CL_SD_BDLPLBA_SHIFT) & \
	 HDA_CL_SD_BDLPLBA_MASK)

/* Buffer Descriptor List Upper Base Address */
#define HDA_CL_SD_BDLPUBA_SHIFT		0
#define HDA_CL_SD_BDLPUBA_MASK		(0xffffffff << HDA_CL_SD_BDLPUBA_SHIFT)
#define HDA_CL_SD_BDLPUBA(x)		\
		((upper_32_bits(x) << HDA_CL_SD_BDLPUBA_SHIFT) & \
		 HDA_CL_SD_BDLPUBA_MASK)

/* Software Position in Buffer Enable */
#define HDA_CL_SPBFIFO_SPBFCCTL_SPIBE_SHIFT	0
#define HDA_CL_SPBFIFO_SPBFCCTL_SPIBE_MASK	\
			(1 << HDA_CL_SPBFIFO_SPBFCCTL_SPIBE_SHIFT)

#define HDA_CL_SPBFIFO_SPBFCCTL_SPIBE(x)	\
			(((x) << HDA_CL_SPBFIFO_SPBFCCTL_SPIBE_SHIFT) & \
			 HDA_CL_SPBFIFO_SPBFCCTL_SPIBE_MASK)

static int cl_stream_prepare(struct snd_sof_dev *sdev, unsigned int format,
			     unsigned int size, struct snd_dma_buffer *dmab,
			     int direction)
{
	struct hdac_ext_stream *stream = NULL;
	struct hdac_stream *hstream;
	struct pci_dev *pci = sdev->pci;
	int ret;

	if (direction == SNDRV_PCM_STREAM_PLAYBACK) {
		stream = hda_dsp_stream_get_pstream(sdev);
	} else {
		dev_err(sdev->dev, "error: code loading DMA is playback only\n");
		return -EINVAL;
	}

	if (!stream) {
		dev_err(sdev->dev, "error: no stream available\n");
		return -ENODEV;
	}
	hstream = &stream->hstream;

	/* allocate DMA buffer */
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV_SG, &pci->dev, size, dmab);
	if (ret < 0) {
		dev_err(sdev->dev, "error: memory alloc failed: %x\n", ret);
		goto error;
	}

	hstream->format_val = format;
	hstream->bufsize = size;

	ret = hda_dsp_stream_hw_params(sdev, stream, dmab, NULL);
	if (ret < 0) {
		dev_err(sdev->dev, "error: hdac prepare failed: %x\n", ret);
		goto error;
	}

	hda_dsp_stream_spib_config(sdev, stream, HDA_DSP_SPIB_ENABLE, size);

	return hstream->stream_tag;

error:
	hda_dsp_stream_put_pstream(sdev, hstream->stream_tag);
	snd_dma_free_pages(dmab);
	return ret;
}

/*
 * first boot sequence has some extra steps. core 0 waits for power
 * status on core 1, so power up core 1 also momentarily, keep it in
 * reset/stall and then turn it off
 */
static int cl_dsp_init(struct snd_sof_dev *sdev, const void *fwdata,
		       u32 fwsize)
{
	int tag, ret, i;
	u32 hipcie;
	const struct sof_intel_dsp_desc *chip = sdev->hda->desc;

	/* prepare DMA for code loader stream */
	tag = cl_stream_prepare(sdev, 0x40, fwsize, &sdev->dmab,
				SNDRV_PCM_STREAM_PLAYBACK);

	if (tag <= 0) {
		dev_err(sdev->dev, "error: dma prepare for fw loading err: %x\n",
			tag);
		return tag;
	}

	memcpy(sdev->dmab.area, fwdata, fwsize);

	/* step 1: power up corex */
	ret = hda_dsp_core_power_up(sdev, chip->cores_mask);
	if (ret < 0) {
		dev_err(sdev->dev, "error: dsp core 0/1 power up failed\n");
		goto err;
	}

	/* step 2: purge FW request */
	snd_sof_dsp_write(sdev, HDA_DSP_BAR, chip->ipc_req,
			  chip->ipc_req_mask | (HDA_DSP_IPC_PURGE_FW |
			  ((tag - 1) << 9)));

	/* step 3: unset core 0 reset state & unstall/run core 0 */
	ret = hda_dsp_core_run(sdev, HDA_DSP_CORE_MASK(0));
	if (ret < 0) {
		dev_err(sdev->dev, "error: dsp core start failed %d\n", ret);
		ret = -EIO;
		goto err;
	}

	/* step 4: wait for IPC DONE bit from ROM */
	for (i = HDA_DSP_INIT_TIMEOUT; i > 0; i--) {
		hipcie = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
					  chip->ipc_ack);

		if (hipcie & chip->ipc_ack_mask) {
			snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
						chip->ipc_ack,
						chip->ipc_ack_mask,
						chip->ipc_ack_mask);
			goto step5;
		}
		mdelay(1);
	}

	dev_err(sdev->dev, "error: waiting for HIPCIE done, reg: 0x%x\n",
		hipcie);
	goto err;

step5:
	/* step 5: power down corex */
	ret = hda_dsp_core_power_down(sdev,
				      chip->cores_mask & ~(HDA_DSP_CORE_MASK(0)));
	if (ret < 0) {
		dev_err(sdev->dev, "error: dsp core x power down failed\n");
		goto err;
	}

	/* step 6: enable interrupt */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, HDA_DSP_REG_ADSPIC,
				HDA_DSP_ADSPIC_IPC, HDA_DSP_ADSPIC_IPC);

	/* enable IPC DONE interrupt */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, chip->ipc_ctl,
				HDA_DSP_REG_HIPCCTL_DONE,
				HDA_DSP_REG_HIPCCTL_DONE);

	/* enable IPC BUSY interrupt */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, chip->ipc_ctl,
				HDA_DSP_REG_HIPCCTL_BUSY,
				HDA_DSP_REG_HIPCCTL_BUSY);

	/* step 7: wait for ROM init */
	ret = snd_sof_dsp_register_poll(sdev, HDA_DSP_BAR,
					HDA_DSP_SRAM_REG_ROM_STATUS,
					HDA_DSP_ROM_STS_MASK, HDA_DSP_ROM_INIT,
					HDA_DSP_INIT_TIMEOUT);
	if (ret >= 0)
		goto out;

	ret = -EIO;

err:
	hda_dsp_dump(sdev, SOF_DBG_REGS | SOF_DBG_PCI | SOF_DBG_MBOX);
	//sdev->dsp_ops.cleanup(sdev->dev, &sdev->dmab, tag);
	hda_dsp_core_reset_power_down(sdev, HDA_DSP_CORE_MASK(0) |
				      HDA_DSP_CORE_MASK(1));
	return ret;

out:
	return tag;
}

static int cl_trigger(struct snd_sof_dev *sdev,
		      struct hdac_ext_stream *stream, int cmd)
{
	struct hdac_stream *hstream = &stream->hstream;
	int sd_offset = SOF_STREAM_SD_OFFSET(hstream);

	/* code loader is special case that reuses stream ops */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		wait_event_timeout(sdev->waitq, !sdev->code_loading,
				   HDA_DSP_CL_TRIGGER_TIMEOUT);

		snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTCTL,
					1 << hstream->index,
					1 << hstream->index);

		snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
					sd_offset,
					SOF_HDA_SD_CTL_DMA_START |
					SOF_HDA_CL_DMA_SD_INT_MASK,
					SOF_HDA_SD_CTL_DMA_START |
					SOF_HDA_CL_DMA_SD_INT_MASK);

		hstream->running = true;
		return 0;
	default:
		return hda_dsp_stream_trigger(sdev, stream, cmd);
	}
}

static int cl_cleanup(struct snd_sof_dev *sdev, struct snd_dma_buffer *dmab,
		      struct hdac_ext_stream *stream)
{
	struct hdac_stream *hstream = &stream->hstream;
	int sd_offset = SOF_STREAM_SD_OFFSET(hstream);
	int ret;

	ret = hda_dsp_stream_spib_config(sdev, stream, HDA_DSP_SPIB_DISABLE, 0);

	/* TODO: spin lock ?*/
	hstream->opened = 0;
	hstream->running = 0;
	hstream->substream = NULL;

	/* reset BDL address */
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPL, 0);
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPU, 0);

	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR, sd_offset, 0);
	snd_dma_free_pages(dmab);
	dmab->area = NULL;
	hstream->bufsize = 0;
	hstream->format_val = 0;

	return ret;
}

static int cl_copy_fw(struct snd_sof_dev *sdev, int tag)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct hdac_ext_stream *stream = NULL;
	struct hdac_stream *s;
	int ret, status;

	/* get stream with tag */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (s->direction == SNDRV_PCM_STREAM_PLAYBACK
			&& s->stream_tag == tag) {
			stream = stream_to_hdac_ext_stream(s);
			break;
		}
	}

	if (!stream) {
		dev_err(sdev->dev,
			"error: could not get stream with stream tag%d\n",
			tag);
		return -ENODEV;
	}

	ret = cl_trigger(sdev, stream, SNDRV_PCM_TRIGGER_START);
	if (ret < 0) {
		dev_err(sdev->dev, "error: DMA trigger start failed\n");
		return ret;
	}

	status = snd_sof_dsp_register_poll(sdev, HDA_DSP_BAR,
					   HDA_DSP_SRAM_REG_ROM_STATUS,
					   HDA_DSP_ROM_STS_MASK,
					   HDA_DSP_ROM_FW_ENTERED,
					   HDA_DSP_BASEFW_TIMEOUT);

	ret = cl_trigger(sdev, stream, SNDRV_PCM_TRIGGER_STOP);
	if (ret < 0) {
		dev_err(sdev->dev, "error: DMA trigger stop failed\n");
		return ret;
	}

	ret = cl_cleanup(sdev, &sdev->dmab, stream);
	if (ret < 0) {
		dev_err(sdev->dev, "error: Code loader DSP cleanup failed\n");
		return ret;
	}

	return status;
}

int hda_dsp_cl_load_fw(struct snd_sof_dev *sdev, bool first_boot)
{
	struct snd_sof_pdata *plat_data = dev_get_platdata(sdev->dev);
	const char *fw_filename;

	/* set code loading condition to true */
	sdev->code_loading = 1;

	switch (plat_data->type) {
	case SOF_DEVICE_SPI:
		fw_filename = plat_data->sof_machine->sof_fw_filename;
		break;
	default:
		fw_filename = plat_data->machine->sof_fw_filename;
	}

	return request_firmware(&plat_data->fw, fw_filename, sdev->dev);
}

int hda_dsp_cl_boot_firmware(struct snd_sof_dev *sdev)
{
	struct snd_sof_pdata *plat_data = dev_get_platdata(sdev->dev);
	struct firmware stripped_firmware;
	int ret, tag;

	stripped_firmware.data = plat_data->fw->data;
	stripped_firmware.size = plat_data->fw->size;

	tag = cl_dsp_init(sdev, stripped_firmware.data,
			  stripped_firmware.size);

	/* retry enabling core and ROM load. seemed to help */
	if (tag < 0) {
		tag = cl_dsp_init(sdev, stripped_firmware.data,
				  stripped_firmware.size);
		if (tag <= 0) {
			dev_err(sdev->dev, "Error code=0x%x: FW status=0x%x\n",
				snd_sof_dsp_read(sdev, HDA_DSP_BAR,
						 HDA_DSP_SRAM_REG_ROM_ERROR),
				snd_sof_dsp_read(sdev, HDA_DSP_BAR,
						 HDA_DSP_SRAM_REG_ROM_STATUS));
			dev_err(sdev->dev, "Core En/ROM load fail:%d\n",
				tag);
			ret = tag;
			goto irq_err;
		}
	}

	/* init for booting wait */
	init_waitqueue_head(&sdev->boot_wait);
	sdev->boot_complete = false;

	/* at this point DSP ROM has been initialized and should be ready for
	 * code loading and firmware boot
	 */
	ret = cl_copy_fw(sdev, tag);
	if (ret < 0) {
		dev_err(sdev->dev, "error: load fw failed err: %d\n", ret);
		goto irq_err;
	}

	dev_dbg(sdev->dev, "Firmware download successful, booting...\n");

	return ret;

irq_err:
	hda_dsp_dump(sdev, SOF_DBG_REGS | SOF_DBG_PCI | SOF_DBG_MBOX);

	/* disable DSP */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				SOF_HDA_PPCTL_GPROCEN, 0);
	dev_err(sdev->dev, "error: load fw failed err: %d\n", ret);
	return ret;
}

static int cl_skl_cldma_setup_bdle(struct snd_sof_dev *sdev,
				   struct snd_dma_buffer *dmab_data,
				   u32 **bdlp, int size, int with_ioc)
{
	u32 *bdl = *bdlp;
	int frags = 0;

	while (size > 0) {
		phys_addr_t addr = virt_to_phys(dmab_data->area +
						(frags * size));

		bdl[0] = cpu_to_le32(lower_32_bits(addr));
		bdl[1] = cpu_to_le32(upper_32_bits(addr));

		bdl[2] = cpu_to_le32(size);

		size -= size;
		bdl[3] = (size || !with_ioc) ? 0 : cpu_to_le32(0x01);

		bdl += 4;
		frags++;
	}

	return frags;
}

static void cl_skl_cldma_stream_run(struct snd_sof_dev *sdev, bool enable)
{
	unsigned char val;
	int timeout;
	int sd_offset = SOF_HDA_ADSP_LOADER_BASE;
	u32 run = enable ? 0x1 : 0;

	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
				sd_offset + SOF_HDA_ADSP_REG_CL_SD_CTL,
				HDA_CL_SD_CTL_RUN(1), HDA_CL_SD_CTL_RUN(run));

	udelay(3);
	timeout = 300;
	do {
		/* waiting for hardware to report the stream Run bit set */
		val = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				       sd_offset + SOF_HDA_ADSP_REG_CL_SD_CTL)
				       & HDA_CL_SD_CTL_RUN(1);
		if (enable && val)
			break;
		else if (!enable && !val)
			break;
		udelay(3);
	} while (--timeout);

	if (timeout == 0)
		dev_err(sdev->dev, "Failed to set Run bit=%d enable=%d\n",
			val, enable);
}

static void cl_skl_cldma_stream_clear(struct snd_sof_dev *sdev)
{
	int sd_offset = SOF_HDA_ADSP_LOADER_BASE;
	/* make sure Run bit is cleared before setting stream register */
	cl_skl_cldma_stream_run(sdev, 0);

	/* Disable the Interrupt On Completion, FIFO Error Interrupt,
	 * Descriptor Error Interrupt and set the cldma stream number to 0.
	 */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
				sd_offset + SOF_HDA_ADSP_REG_CL_SD_CTL,
				HDA_CL_SD_CTL_INT_MASK, HDA_CL_SD_CTL_INT(0));
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
				sd_offset + SOF_HDA_ADSP_REG_CL_SD_CTL,
				HDA_CL_SD_CTL_STRM(0xf), HDA_CL_SD_CTL_STRM(0));

	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPL,
			  HDA_CL_SD_BDLPLBA(0));
	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPU, 0);

	/* Set the Cyclic Buffer Length to 0. */
	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_CBL, 0);
	/* Set the Last Valid Index. */
	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_LVI, 0);
}

static void cl_skl_cldma_setup_spb(struct snd_sof_dev *sdev,
				   unsigned int size, bool enable)
{
	int sd_offset = SOF_DSP_REG_CL_SPBFIFO;

	if (enable)
		snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
					sd_offset +
					SOF_HDA_ADSP_REG_CL_SPBFIFO_SPBFCCTL,
					HDA_CL_SPBFIFO_SPBFCCTL_SPIBE_MASK,
					HDA_CL_SPBFIFO_SPBFCCTL_SPIBE(1));

	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SPBFIFO_SPIB, size);
}

static void cl_skl_cldma_cleanup_spb(struct snd_sof_dev *sdev)
{
	int sd_offset = SOF_DSP_REG_CL_SPBFIFO;

	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
				sd_offset +
					SOF_HDA_ADSP_REG_CL_SPBFIFO_SPBFCCTL,
				HDA_CL_SPBFIFO_SPBFCCTL_SPIBE_MASK,
				HDA_CL_SPBFIFO_SPBFCCTL_SPIBE(0));

	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SPBFIFO_SPIB, 0);
}

static void cl_skl_cldma_setup_controller(struct snd_sof_dev *sdev,
					  struct snd_dma_buffer *dmab_bdl,
					  unsigned int max_size, u32 count)
{
	int sd_offset = SOF_HDA_ADSP_LOADER_BASE;
	/* Clear the stream first and then set it. */
	cl_skl_cldma_stream_clear(sdev);

	/* setting the stream register */
	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPL,
			  HDA_CL_SD_BDLPLBA(dmab_bdl->addr));
	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPU,
			  HDA_CL_SD_BDLPUBA(dmab_bdl->addr));

	/* Set the Cyclic Buffer Length. */
	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_CBL, max_size);
	/* Set the Last Valid Index. */
	snd_sof_dsp_write(sdev, HDA_DSP_BAR,
			  sd_offset + SOF_HDA_ADSP_REG_CL_SD_LVI, count - 1);

	/* Set the Interrupt On Completion, FIFO Error Interrupt,
	 * Descriptor Error Interrupt and the cldma stream number.
	 */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
				sd_offset + SOF_HDA_ADSP_REG_CL_SD_CTL,
				HDA_CL_SD_CTL_INT_MASK, HDA_CL_SD_CTL_INT(1));
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
				sd_offset + SOF_HDA_ADSP_REG_CL_SD_CTL,
				HDA_CL_SD_CTL_STRM(0xf),
				HDA_CL_SD_CTL_STRM(1));
}

static int cl_stream_prepare_skl(struct snd_sof_dev *sdev)
{
	struct pci_dev *pci = sdev->pci;
	int frags = 0;
	int ret = 0;
	u32 *bdl;
	unsigned int bufsize = SKL_MAX_BUFFER_SIZE;

	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev, bufsize,
				  &sdev->dmab);
	if (ret < 0) {
		dev_err(sdev->dev, "Alloc base fw buffer failed: %x\n", ret);
		return ret;
	}
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev, bufsize,
				  &sdev->dmab_bdl);
	if (ret < 0) {
		dev_err(sdev->dev, "Alloc buffer for blde failed: %x\n", ret);
		snd_dma_free_pages(&sdev->dmab);
		return ret;
	}
	bdl = (u32 *)sdev->dmab_bdl.area;

	frags = cl_skl_cldma_setup_bdle(sdev, &sdev->dmab, &bdl, bufsize, 1);
	cl_skl_cldma_setup_controller(sdev, &sdev->dmab_bdl, bufsize, frags);

	return ret;
}

static void cl_cleanup_skl(struct snd_sof_dev *sdev)
{
	cl_skl_cldma_cleanup_spb(sdev);
	cl_skl_cldma_stream_clear(sdev);
	snd_dma_free_pages(&sdev->dmab);
	snd_dma_free_pages(&sdev->dmab_bdl);
	sdev->dmab.area = NULL;
}

static int cl_dsp_init_skl(struct snd_sof_dev *sdev)
{
	const struct sof_intel_dsp_desc *chip = sdev->hda->desc;
	int ret;

	/* check if the core is already enabled, if yes, reset and make it run,
	 * if not, powerdown and enable it again.
	 */
	if (hda_dsp_core_is_enabled(sdev, HDA_DSP_CORE_MASK(0))) {
		/* if enabled, reset it, and run the core. */
		ret = hda_dsp_core_stall_reset(sdev, HDA_DSP_CORE_MASK(0));
		if (ret < 0)
			goto err;

		ret = hda_dsp_core_run(sdev, HDA_DSP_CORE_MASK(0));
		if (ret < 0) {
			dev_err(sdev->dev, "error: dsp core start failed %d\n",
				ret);
			goto err;
		}
	} else {
		/* if not enabled, power down it first and then powerup and run
		 * the core.
		 */
		ret = hda_dsp_core_reset_power_down(sdev, HDA_DSP_CORE_MASK(0));
		if (ret < 0) {
			dev_err(sdev->dev, "dsp core0 disable fail: %d\n", ret);
			goto err;
		}
		ret = hda_dsp_enable_core(sdev, HDA_DSP_CORE_MASK(0));
		if (ret < 0) {
			dev_err(sdev->dev, "dsp core0 enable fail: %d\n", ret);
			goto err;
		}
	}

	/* prepare DMA for code loader stream */
	ret = cl_stream_prepare_skl(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: dma prepare fw loading err: %x\n",
			ret);
		return ret;
	}

	/* enable the interrupt */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, HDA_DSP_REG_ADSPIC,
				HDA_DSP_ADSPIC_IPC, HDA_DSP_ADSPIC_IPC);

	/* enable IPC DONE interrupt */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, chip->ipc_ctl,
				HDA_DSP_REG_HIPCCTL_DONE,
				HDA_DSP_REG_HIPCCTL_DONE);

	/* enable IPC BUSY interrupt */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR, chip->ipc_ctl,
				HDA_DSP_REG_HIPCCTL_BUSY,
				HDA_DSP_REG_HIPCCTL_BUSY);

	/* polling the ROM init status information. */
	ret = snd_sof_dsp_register_poll(sdev, HDA_DSP_BAR,
					HDA_DSP_SRAM_REG_ROM_STATUS_SKL,
					HDA_DSP_ROM_STS_MASK, HDA_DSP_ROM_INIT,
					HDA_DSP_INIT_TIMEOUT);
	if (ret < 0)
		goto err;

	return ret;

err:
	hda_dsp_dump(sdev, SOF_DBG_REGS | SOF_DBG_PCI | SOF_DBG_MBOX);
	cl_cleanup_skl(sdev);
	hda_dsp_core_reset_power_down(sdev, HDA_DSP_CORE_MASK(0));
	return ret;
}

int hda_dsp_cl_boot_firmware_skl(struct snd_sof_dev *sdev)
{
	int ret;

	ret = cl_dsp_init_skl(sdev);

	/* retry enabling core and ROM load. seemed to help */
	if (ret < 0) {
		ret = cl_dsp_init_skl(sdev);
		if (ret < 0) {
			dev_err(sdev->dev, "Error code=0x%x: FW status=0x%x\n",
				snd_sof_dsp_read(sdev, HDA_DSP_BAR,
						 HDA_DSP_SRAM_REG_ROM_ERROR),
				snd_sof_dsp_read(sdev, HDA_DSP_BAR,
						 HDA_DSP_SRAM_REG_ROM_STATUS));
			dev_err(sdev->dev, "Core En/ROM load fail:%d\n", ret);
			goto irq_err;
		}
	}

	dev_dbg(sdev->dev, "ROM init successful\n");

	/* init for booting wait */
	init_waitqueue_head(&sdev->boot_wait);
	sdev->boot_complete = false;

	return ret;

irq_err:
	hda_dsp_dump(sdev, SOF_DBG_REGS | SOF_DBG_PCI | SOF_DBG_MBOX);

	/* power down DSP */
	hda_dsp_core_reset_power_down(sdev, HDA_DSP_CORE_MASK(0));
	cl_cleanup_skl(sdev);

	dev_err(sdev->dev, "error: load fw failed err: %d\n", ret);
	return ret;
}
