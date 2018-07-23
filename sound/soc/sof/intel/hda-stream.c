// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2017 Intel Corporation. All rights reserved.
 *
 * Authors: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *	    Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 *	    Jeeja KP <jeeja.kp@intel.com>
 *	    Rander Wang <rander.wang@intel.com>
 *          Keyon Jie <yang.jie@linux.intel.com>
 */

/*
 * Hardware interface for generic Intel audio DSP HDA IP
 */

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
#include <sound/hda_register.h>
#include <sound/sof.h>
#include <sound/pcm_params.h>
#include <linux/pm_runtime.h>

#include "../sof-priv.h"
#include "../ops.h"
#include "hda.h"

/*
 * set up Buffer Descriptor List (BDL) for host memory transfer
 * BDL describes the location of the individual buffers and is little endian.
 */
int hda_dsp_stream_setup_bdl(struct snd_sof_dev *sdev,
			     struct snd_dma_buffer *dmab,
			     struct sof_intel_hda_stream *stream,
			     struct sof_intel_dsp_bdl *bdl, int size,
			     struct snd_pcm_hw_params *params)
{
	int offset = 0;
	int chunk = PAGE_SIZE, entry_size;
	dma_addr_t addr;

	if (stream->substream && params) {
		chunk = params_period_bytes(params);
		dev_dbg(sdev->dev, "period_bytes:0x%x\n", chunk);
	}

	while (size > 0) {
		if (stream->frags >= HDA_DSP_MAX_BDL_ENTRIES) {
			dev_err(sdev->dev, "error: stream frags exceeded\n");
			return -EINVAL;
		}

		addr = snd_sgbuf_get_addr(dmab, offset);

		/* program BDL addr */
		bdl->addr_l = lower_32_bits(addr);
		bdl->addr_h = upper_32_bits(addr);

		entry_size = size > chunk ? chunk : size;

		/* program BDL size */
		bdl->size = snd_sgbuf_get_chunk_size(dmab, offset, entry_size);

		/* program the IOC to enable interrupt
		 * when the whole fragment is processed
		 */
		size -= entry_size;
		if (size)
			bdl->ioc = 0;
		else
			bdl->ioc = 1;

		stream->frags++;
		offset += bdl->size;

		dev_vdbg(sdev->dev, "bdl, frags:%d, entry size:0x%x;\n",
			 stream->frags, entry_size);

		bdl++;
	}

	return offset;
}

int hda_dsp_stream_spib_config(struct snd_sof_dev *sdev,
			       struct sof_intel_hda_stream *stream,
			       int enable, u32 size)
{
	u32 mask = 0;

	if (!sdev->bar[HDA_DSP_SPIB_BAR]) {
		dev_err(sdev->dev, "error: address of spib capability is NULL\n");
		return -EINVAL;
	}

	mask |= (1 << stream->index);

	/* enable/disable SPIB for the stream */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_SPIB_BAR,
				SOF_HDA_ADSP_REG_CL_SPBFIFO_SPBFCCTL, mask,
				enable << stream->index);

	/* set the SPIB value */
	hda_dsp_write(sdev, stream->spib_addr, size);

	return 0;
}


/* get next unused stream */
struct sof_intel_hda_stream *
hda_dsp_stream_get(struct snd_sof_dev *sdev, int direction)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s, *stream = NULL;

	/* get an unused playback stream */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (s->direction == direction && !s->open) {
			s->open = true;
			stream = s;
			break;
		}
	}

	/* stream found ? */
	if (!stream)
		dev_err(sdev->dev, "error: no free %s streams\n",
			direction ==SNDRV_PCM_STREAM_PLAYBACK ? "playback" : "capture" );

	return stream;
}

/* get next unused playback stream */
struct sof_intel_hda_stream *
hda_dsp_stream_get_pstream(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s, *stream = NULL;

	/* get an unused playback stream */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (s->direction == SNDRV_PCM_STREAM_PLAYBACK
			&& !s->open) {
			s->open = true;
			stream = s;
			break;
		}
	}

	/* stream found ? */
	if (!stream)
		dev_err(sdev->dev, "error: no free playback streams\n");

	return stream;
}

/* get next unused capture stream */
struct sof_intel_hda_stream *
hda_dsp_stream_get_cstream(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s, *stream = NULL;

	/* get an unused capture stream */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (s->direction == SNDRV_PCM_STREAM_CAPTURE
			&& !s->open) {
			s->open = true;
			stream = s;
			break;
		}
	}

	/* stream found ? */
	if (!stream)
		dev_err(sdev->dev, "error: no free capture streams\n");

	return stream;
}

/* free a stream */
int hda_dsp_stream_put(struct snd_sof_dev *sdev, int direction, int tag)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s;

	/* find used stream */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (s->direction == direction
			&& s->open && s->tag == tag) {
			s->open = false;
			return 0;
		}
	}

	dev_dbg(sdev->dev, "tag %d not opened!\n", tag);
	return -ENODEV;
}

/* free playback stream */
int hda_dsp_stream_put_pstream(struct snd_sof_dev *sdev, int tag)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s;

	/* find used playback stream */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (s->direction == SNDRV_PCM_STREAM_PLAYBACK
			&& s->open && s->tag == tag) {
			s->open = false;
			return 0;
		}
	}

	dev_dbg(sdev->dev, "tag %d not opened!\n", tag);
	return -ENODEV;
}

/* free capture stream */
int hda_dsp_stream_put_cstream(struct snd_sof_dev *sdev, int tag)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s;

	/* find used capture stream */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (s->direction == SNDRV_PCM_STREAM_CAPTURE
			&& s->open && s->tag == tag) {
			s->open = false;
			return 0;
		}
	}

	dev_dbg(sdev->dev, "tag %d not opened!\n", tag);
	return -ENODEV;
}

int hda_dsp_stream_trigger(struct snd_sof_dev *sdev,
			   struct sof_intel_hda_stream *stream, int cmd)
{
	/* cmd must be for audio stream */
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_START:
		snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTCTL,
					1 << stream->index,
					1 << stream->index);

		snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
					stream->sd_offset,
					SOF_HDA_SD_CTL_DMA_START |
					SOF_HDA_CL_DMA_SD_INT_MASK,
					SOF_HDA_SD_CTL_DMA_START |
					SOF_HDA_CL_DMA_SD_INT_MASK);

		stream->running = true;
		break;
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_STOP:
		snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
					stream->sd_offset,
					SOF_HDA_SD_CTL_DMA_START |
					SOF_HDA_CL_DMA_SD_INT_MASK, 0x0);

		snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR, stream->sd_offset +
				  SOF_HDA_ADSP_REG_CL_SD_STS,
				  SOF_HDA_CL_DMA_SD_INT_MASK);

		stream->running = false;
		snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTCTL,
					1 << stream->index, 0x0);
		break;
	default:
		dev_err(sdev->dev, "error: unknown command: %d\n", cmd);
		return -EINVAL;
	}

	return 0;
}

/*
 * prepare for common hdac registers settings, for both code loader
 * and normal stream.
 */
int hda_dsp_stream_hw_params(struct snd_sof_dev *sdev,
			     struct sof_intel_hda_stream *stream,
			     struct snd_dma_buffer *dmab,
			     struct snd_pcm_hw_params *params)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_dsp_bdl *bdl;
	int ret, timeout = HDA_DSP_STREAM_RESET_TIMEOUT;
	u32 val, mask;

	if (!stream) {
		dev_err(sdev->dev, "error: no stream available\n");
		return -ENODEV;
	}

	/* decouple host and link DMA */
	mask = 0x1 << stream->index;
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				mask, mask);

	if (!dmab) {
		dev_err(sdev->dev, "error: no dma buffer allocated!\n");
		return -ENODEV;
	}

	/* clear stream status */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, stream->sd_offset,
				SOF_HDA_CL_DMA_SD_INT_MASK |
				SOF_HDA_SD_CTL_DMA_START, 0);
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
				stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_STS,
				SOF_HDA_CL_DMA_SD_INT_MASK,
				SOF_HDA_CL_DMA_SD_INT_MASK);

	/* stream reset */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, stream->sd_offset, 0x1,
				0x1);
	udelay(3);
	do {
		val = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR,
				       stream->sd_offset);
		if (val & 0x1)
			break;
	} while (--timeout);
	if (timeout == 0) {
		dev_err(sdev->dev, "error: stream reset failed\n");
		return -ETIMEDOUT;
	}

	timeout = HDA_DSP_STREAM_RESET_TIMEOUT;
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, stream->sd_offset, 0x1,
				0x0);

	/* wait for hardware to report that stream is out of reset */
	udelay(3);
	do {
		val = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR,
				       stream->sd_offset);
		if ((val & 0x1) == 0)
			break;
	} while (--timeout);
	if (timeout == 0) {
		dev_err(sdev->dev, "error: timeout waiting for stream reset\n");
		return -ETIMEDOUT;
	}

	if (stream->posbuf)
		*stream->posbuf = 0;

	/* reset BDL address */
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR,
			  stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPL,
			  0x0);
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR,
			  stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPU,
			  0x0);

	/* clear stream status */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, stream->sd_offset,
				SOF_HDA_CL_DMA_SD_INT_MASK |
				SOF_HDA_SD_CTL_DMA_START, 0);
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
				stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_STS,
				SOF_HDA_CL_DMA_SD_INT_MASK,
				SOF_HDA_CL_DMA_SD_INT_MASK);

	stream->frags = 0;

	bdl = (struct sof_intel_dsp_bdl *)stream->bdl.area;
	ret = hda_dsp_stream_setup_bdl(sdev, dmab, stream, bdl,
				       stream->bufsize, params);
	if (ret < 0) {
		dev_err(sdev->dev, "error: set up of BDL failed\n");
		return ret;
	}

	/* set up stream descriptor for DMA */
	/* program stream tag */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, stream->sd_offset,
				SOF_HDA_CL_SD_CTL_STREAM_TAG_MASK,
				stream->tag <<
				SOF_HDA_CL_SD_CTL_STREAM_TAG_SHIFT);

	/* program cyclic buffer length */
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR,
			  stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_CBL,
			  stream->bufsize);

	/* program stream format */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
				stream->sd_offset +
				SOF_HDA_ADSP_REG_CL_SD_FORMAT,
				0xffff, stream->config);

	/* program last valid index */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
				stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_LVI,
				0xffff, (stream->frags - 1));

	/* program BDL address */
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR,
			  stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPL,
			  (u32)stream->bdl.addr);
	snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR,
			  stream->sd_offset + SOF_HDA_ADSP_REG_CL_SD_BDLPU,
			  upper_32_bits(stream->bdl.addr));

	/* enable position buffer */
	if (!(snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_ADSP_DPLBASE)
				& SOF_HDA_ADSP_DPLBASE_ENABLE))
		snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR, SOF_HDA_ADSP_DPLBASE,
				  (u32)bus->posbuf.addr |
				  SOF_HDA_ADSP_DPLBASE_ENABLE);

	/* set interrupt enable bits */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, stream->sd_offset,
				SOF_HDA_CL_DMA_SD_INT_MASK,
				SOF_HDA_CL_DMA_SD_INT_MASK);

	/* read FIFO size */
	if (stream->direction == SNDRV_PCM_STREAM_PLAYBACK) {
		stream->fifo_size =
			snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR,
					 stream->sd_offset +
					 SOF_HDA_ADSP_REG_CL_SD_FIFOSIZE);
		stream->fifo_size &= 0xffff;
		stream->fifo_size += 1;
	} else {
		stream->fifo_size = 0;
	}

	return ret;
}

irqreturn_t hda_dsp_stream_interrupt(int irq, void *context)
{
	struct snd_sof_dev *sdev = (struct snd_sof_dev *)context;
	struct hdac_bus *bus = sof_to_bus(sdev);
	u32 status;

	if (!pm_runtime_active(sdev->dev))
		return IRQ_NONE;

	spin_lock(&bus->reg_lock);

	status = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTSTS);
	if (status == 0 || status == 0xffffffff) {
		spin_unlock(&bus->reg_lock);
		return IRQ_NONE;
	}

	/* clear rirb int */
	status = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_RIRBSTS);
	if (status & RIRB_INT_MASK) {
		if (status & RIRB_INT_RESPONSE)
			snd_hdac_bus_update_rirb(bus);
		snd_sof_dsp_write(sdev, HDA_DSP_HDA_BAR, SOF_HDA_RIRBSTS,
				  RIRB_INT_MASK);
	}

	spin_unlock(&bus->reg_lock);

	return snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTSTS)
				? IRQ_WAKE_THREAD : IRQ_HANDLED;
}

irqreturn_t hda_dsp_stream_threaded_handler(int irq, void *context)
{
	struct snd_sof_dev *sdev = (struct snd_sof_dev *)context;
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s;
	//struct sof_intel_hda_dev *hdev = sdev->hda;
	u32 status = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTSTS);
	u32 sd_status;

	/* check streams */
	list_for_each_entry(s, &bus->stream_list, list) {
		if (status & (1 << s->index)
			&& !s->open) {
			sd_status =
				snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR,
						 s->sd_offset +
						 SOF_HDA_ADSP_REG_CL_SD_STS) &
						 0xff;

			dev_dbg(sdev->dev, "stream %d status 0x%x\n",
				s->index, sd_status);

			snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR,
						s->sd_offset +
						SOF_HDA_ADSP_REG_CL_SD_STS,
						SOF_HDA_CL_DMA_SD_INT_MASK,
						SOF_HDA_CL_DMA_SD_INT_MASK);

			if (!s->substream ||
			    !s->running ||
			    (sd_status & SOF_HDA_CL_DMA_SD_INT_MASK) == 0)
				continue;

		}
	}


	// TODO: legacy code call snd_pcm_period_elapsed(hstr->substream);
	// we probably dont need this since we get updates via IPC/SRAM/
	// TODO: evaluate.

	return IRQ_HANDLED;
}

int hda_dsp_stream_init(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *stream;
	struct pci_dev *pci = sdev->pci;
	int i, num_playback, num_capture, num_total, ret;
	u32 gcap;

	gcap = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_GCAP);
	dev_dbg(sdev->dev, "hda global caps = 0x%x\n", gcap);

	/* get stream count from GCAP */
	num_capture = (gcap >> 8) & 0x0f;
	num_playback = (gcap >> 12) & 0x0f;
	num_total = num_playback + num_capture;

	dev_dbg(sdev->dev, "detected %d playback and %d capture streams\n",
		num_playback, num_capture);

	if (num_playback >= SOF_HDA_PLAYBACK_STREAMS) {
		dev_err(sdev->dev, "error: too many playback streams %d\n",
			num_playback);
		return -EINVAL;
	}

	if (num_capture >= SOF_HDA_CAPTURE_STREAMS) {
		dev_err(sdev->dev, "error: too many capture streams %d\n",
			num_playback);
		return -EINVAL;
	}

	/* mem alloc for the position buffer */
	/* TODO: check postion buffer update */
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev, 8 * num_total,
				  &bus->posbuf);
	if (ret < 0) {
		dev_err(sdev->dev, "error: posbuffer dma alloc failed\n");
		return -ENOMEM;
	}


	/* mem alloc for the CORB/RIRB ringbuffers */
	ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev,
				  PAGE_SIZE, &bus->rb);
	if (ret < 0) {
		dev_err(sdev->dev, "error: RB alloc failed\n");
		return -ENOMEM;
	}

	/* create capture streams */
	for (i = 0; i < num_capture; i++) {

		stream = kzalloc(sizeof(*stream), GFP_KERNEL);
		if (!stream)
			return -ENOMEM;

		stream->pphc_addr = sdev->bar[HDA_DSP_PP_BAR] +
			SOF_HDA_PPHC_BASE + SOF_HDA_PPHC_INTERVAL * i;

		stream->pplc_addr = sdev->bar[HDA_DSP_PP_BAR] +
			SOF_HDA_PPLC_BASE + SOF_HDA_PPLC_MULTI * num_total +
			SOF_HDA_PPLC_INTERVAL * i;

		/* do we support SPIB */
		if (sdev->bar[HDA_DSP_SPIB_BAR]) {
			stream->spib_addr = sdev->bar[HDA_DSP_SPIB_BAR] +
				SOF_HDA_SPIB_BASE + SOF_HDA_SPIB_INTERVAL * i +
				SOF_HDA_SPIB_SPIB;

			stream->fifo_addr = sdev->bar[HDA_DSP_SPIB_BAR] +
				SOF_HDA_SPIB_BASE + SOF_HDA_SPIB_INTERVAL * i +
				SOF_HDA_SPIB_MAXFIFO;
		}

		/* do we support DRSM */
		if (sdev->bar[HDA_DSP_DRSM_BAR])
			stream->drsm_addr = sdev->bar[HDA_DSP_DRSM_BAR] +
				SOF_HDA_DRSM_BASE + SOF_HDA_DRSM_INTERVAL * i;

		stream->sd_offset = 0x20 * i + SOF_HDA_ADSP_LOADER_BASE;
		stream->sd_addr = sdev->bar[HDA_DSP_HDA_BAR] +
					stream->sd_offset;

		stream->tag = i + 1;
		stream->open = false;
		stream->running = false;
		stream->direction = SNDRV_PCM_STREAM_CAPTURE;
		stream->index = i;

		/* memory alloc for stream BDL */
		ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev,
					  HDA_DSP_BDL_SIZE, &stream->bdl);
		if (ret < 0) {
			dev_err(sdev->dev, "error: stream bdl dma alloc failed\n");
			kfree(stream);
			return -ENOMEM;
		}
		stream->posbuf = (__le32 *)(bus->posbuf.area +
			(stream->index) * 8);

		list_add_tail(&stream->list, &bus->stream_list);
	}

	/* create playback streams */
	for (i = num_capture; i < num_total; i++) {

		stream = kzalloc(sizeof(*stream), GFP_KERNEL);
		if (!stream)
			return -ENOMEM;

		/* we always have DSP support */
		stream->pphc_addr = sdev->bar[HDA_DSP_PP_BAR] +
			SOF_HDA_PPHC_BASE + SOF_HDA_PPHC_INTERVAL * i;

		stream->pplc_addr = sdev->bar[HDA_DSP_PP_BAR] +
			SOF_HDA_PPLC_BASE + SOF_HDA_PPLC_MULTI * num_total +
			SOF_HDA_PPLC_INTERVAL * i;

		/* do we support SPIB */
		if (sdev->bar[HDA_DSP_SPIB_BAR]) {
			stream->spib_addr = sdev->bar[HDA_DSP_SPIB_BAR] +
				SOF_HDA_SPIB_BASE + SOF_HDA_SPIB_INTERVAL * i +
				SOF_HDA_SPIB_SPIB;

			stream->fifo_addr = sdev->bar[HDA_DSP_SPIB_BAR] +
				SOF_HDA_SPIB_BASE + SOF_HDA_SPIB_INTERVAL * i +
				SOF_HDA_SPIB_MAXFIFO;
		}

		/* do we support DRSM */
		if (sdev->bar[HDA_DSP_DRSM_BAR])
			stream->drsm_addr = sdev->bar[HDA_DSP_DRSM_BAR] +
				SOF_HDA_DRSM_BASE + SOF_HDA_DRSM_INTERVAL * i;

		stream->sd_offset = 0x20 * i + SOF_HDA_ADSP_LOADER_BASE;
		stream->sd_addr = sdev->bar[HDA_DSP_HDA_BAR] +
					stream->sd_offset;
		stream->tag = i - num_capture + 1;
		stream->open = false;
		stream->running = false;
		stream->direction = SNDRV_PCM_STREAM_PLAYBACK;
		stream->index = i;

		/* mem alloc for stream BDL */
		ret = snd_dma_alloc_pages(SNDRV_DMA_TYPE_DEV, &pci->dev,
					  HDA_DSP_BDL_SIZE, &stream->bdl);
		if (ret < 0) {
			dev_err(sdev->dev, "error: stream bdl dma alloc failed\n");
			kfree(stream);
			return -ENOMEM;
		}

		stream->posbuf = (__le32 *)(bus->posbuf.area +
			(stream->index) * 8);

		list_add_tail(&stream->list, &bus->stream_list);
	}

	return 0;
}

void hda_dsp_stream_free(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct sof_intel_hda_stream *s, *_s;


	/* free position buffer */
	if (bus->posbuf.area)
		snd_dma_free_pages(&bus->posbuf);

	list_for_each_entry_safe(s, _s, &bus->stream_list, list) {
		/* TODO: decouple */

		/* free bdl buffer */
		if (s->bdl.area)
			snd_dma_free_pages(&s->bdl);
		list_del(&s->list);
		kfree(s);
	}
}

