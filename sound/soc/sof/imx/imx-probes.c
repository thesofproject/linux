// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// Copyright 2026 NXP
//
// SOF probes client for i.MX (IPC3). The DSP cannot use a host DMA engine, so
// the host allocates a coherent buffer (header page + ring), gives its bus
// address to the firmware in PROBE_INIT (as stream_tag) and the firmware
// writes the extracted packets into the ring and publishes a byte counter in
// the first word of the header page. A delayed work copies new data into the
// compress buffer and wakes up the reader.

#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <sound/compress_driver.h>
#include <sound/soc.h>
#include "../sof-priv.h"
#include "../sof-client-probes.h"
#include "../sof-client.h"
#include "imx-common.h"

/* must match PROBE_HOST_HDR_SIZE in the firmware */
#define IMX_PROBES_HDR_SIZE	4096
#define IMX_PROBES_POLL_MS	10

struct imx_probes_stream {
	struct delayed_work work;
	struct snd_compr_stream *cstream;
	struct device *dev;
	void *vaddr;
	dma_addr_t paddr;
	size_t ring_size;
	u32 sampling_rate;
	u64 copied;
};

/* only one probes extraction stream exists */
static struct imx_probes_stream imx_stream;

static void imx_probes_work(struct work_struct *work)
{
	struct imx_probes_stream *s = &imx_stream;
	struct snd_compr_runtime *rtd;
	u8 *ring = s->vaddr + IMX_PROBES_HDR_SIZE;
	u32 wr, n, off;

	if (!s->cstream || !s->vaddr)
		return;

	rtd = s->cstream->runtime;
	wr = readl(s->vaddr);
	n = wr - (u32)s->copied;

	if (n > s->ring_size) {
		/* reader too slow, drop the oldest data */
		s->copied = wr - s->ring_size;
		n = s->ring_size;
	}

	n = min_t(u32, n, rtd->buffer_size);
	if (n) {
		u32 src = (u32)s->copied % s->ring_size;
		u32 dst = s->copied % rtd->buffer_size;

		off = src;
		/* copy in chunks that do not wrap either buffer */
		while (n) {
			u32 chunk = min3(n, (u32)(s->ring_size - off),
					 (u32)(rtd->buffer_size - dst));

			memcpy(rtd->dma_area + dst, ring + off, chunk);
			s->copied += chunk;
			off = (off + chunk) % s->ring_size;
			dst = (dst + chunk) % rtd->buffer_size;
			n -= chunk;
		}
		snd_compr_fragment_elapsed(s->cstream);
	}

	schedule_delayed_work(&s->work, msecs_to_jiffies(IMX_PROBES_POLL_MS));
}

static int imx_probes_compr_startup(struct sof_client_dev *cdev,
				    struct snd_compr_stream *cstream,
				    struct snd_soc_dai *dai, u32 *stream_id)
{
	/* real value (buffer address) is set in set_params */
	*stream_id = 0;
	return 0;
}

static void imx_probes_free(struct imx_probes_stream *s)
{
	cancel_delayed_work_sync(&s->work);
	if (s->vaddr)
		dma_free_coherent(s->dev, IMX_PROBES_HDR_SIZE + s->ring_size,
				  s->vaddr, s->paddr);
	s->vaddr = NULL;
	s->cstream = NULL;
}

static int imx_probes_compr_shutdown(struct sof_client_dev *cdev,
				     struct snd_compr_stream *cstream,
				     struct snd_soc_dai *dai)
{
	imx_probes_free(&imx_stream);
	return 0;
}

static int imx_probes_compr_set_params(struct sof_client_dev *cdev,
				       struct snd_compr_stream *cstream,
				       struct snd_compr_params *params,
				       struct snd_soc_dai *dai)
{
	struct sof_probes_priv *priv = cdev->data;
	struct imx_probes_stream *s = &imx_stream;
	struct snd_soc_pcm_stream *pstream = &dai->driver->capture;

	imx_probes_free(s);

	s->dev = sof_client_get_dma_dev(cdev);
	/* DSP can only address 32-bit physical addresses */
	if (dma_set_coherent_mask(s->dev, DMA_BIT_MASK(32)))
		return -ENOMEM;
	s->ring_size = cstream->runtime->dma_bytes;
	s->vaddr = dma_alloc_coherent(s->dev, IMX_PROBES_HDR_SIZE + s->ring_size,
				      &s->paddr, GFP_KERNEL);
	if (!s->vaddr)
		return -ENOMEM;

	s->cstream = cstream;
	s->copied = 0;
	s->sampling_rate = snd_pcm_rate_bit_to_rate(pstream->rates);
	INIT_DELAYED_WORK(&s->work, imx_probes_work);

	/* the firmware takes the buffer address from the stream_tag field */
	priv->extractor_stream_tag = (u32)s->paddr;

	return 0;
}

static int imx_probes_compr_trigger(struct sof_client_dev *cdev,
				    struct snd_compr_stream *cstream,
				    int cmd, struct snd_soc_dai *dai)
{
	struct imx_probes_stream *s = &imx_stream;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		schedule_delayed_work(&s->work, msecs_to_jiffies(IMX_PROBES_POLL_MS));
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		cancel_delayed_work_sync(&s->work);
		break;
	}

	return 0;
}

static int imx_probes_compr_pointer(struct sof_client_dev *cdev,
				    struct snd_compr_stream *cstream,
				    struct snd_compr_tstamp64 *tstamp,
				    struct snd_soc_dai *dai)
{
	tstamp->copied_total = imx_stream.copied;
	tstamp->sampling_rate = imx_stream.sampling_rate;

	return 0;
}

static const struct sof_probes_host_ops imx_probes_ops = {
	.startup = imx_probes_compr_startup,
	.shutdown = imx_probes_compr_shutdown,
	.set_params = imx_probes_compr_set_params,
	.trigger = imx_probes_compr_trigger,
	.pointer = imx_probes_compr_pointer,
};

int imx_probes_register(struct snd_sof_dev *sdev)
{
	return sof_client_dev_register(sdev, "imx-probes", 0, &imx_probes_ops,
				       sizeof(imx_probes_ops));
}

void imx_probes_unregister(struct snd_sof_dev *sdev)
{
	sof_client_dev_unregister(sdev, "imx-probes", 0);
}

MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
