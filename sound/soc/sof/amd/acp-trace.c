// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Advanced Micro Devices, Inc. All rights reserved.
//
// Authors: Vishnuvardhanrao Ravuapati <vishnuvardhanrao.ravulapati@amd.com>
//	    V Sujith Kumar Reddy <Vsujithkumar.Reddy@amd.com>

/*This file support Host TRACE Logger driver callback for SOF FW */

#include "../sof-client-dma-trace.h"
#include "../sof-client.h"
#include "acp.h"

#define ACP_LOGGER_STREAM	8

static int acp_sof_trace_release(struct sof_client_dev *cdev)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct acp_dev_data *adata = sdev->pdata->hw_pdata;
	struct acp_dsp_stream *stream;
	int ret;

	stream = adata->dtrace_stream;
	ret = acp_dsp_stream_put(sdev, stream);
	if (ret < 0) {
		dev_err(sdev->dev, "Failed to release trace stream\n");
		return ret;
	}

	adata->dtrace_stream = NULL;

	return 0;
}

static int acp_sof_trace_init(struct sof_client_dev *cdev, struct snd_dma_buffer *dmab,
			      struct sof_ipc_dma_trace_params_ext *dtrace_params)
{
	struct snd_sof_dev *sdev = sof_client_dev_to_sof_dev(cdev);
	struct acp_dsp_stream *stream;
	struct acp_dev_data *adata;
	int ret;

	adata = sdev->pdata->hw_pdata;
	stream = acp_dsp_stream_get(sdev, ACP_LOGGER_STREAM);
	if (!stream)
		return -ENODEV;

	stream->dmab = dmab;
	stream->num_pages = dtrace_params->buffer.pages;

	ret = acp_dsp_stream_config(sdev, stream);
	if (ret < 0) {
		acp_dsp_stream_put(sdev, stream);
		adata->dtrace_stream = NULL;
		return ret;
	}

	adata->dtrace_stream = stream;
	dtrace_params->stream_tag = stream->stream_tag;
	dtrace_params->buffer.phy_addr = stream->reg_offset;

	return 0;
}

static const struct sof_dma_trace_host_ops acp_sof_trace_ops = {
	.init = acp_sof_trace_init,
	.release = acp_sof_trace_release,
};

int acp_sof_trace_register(struct snd_sof_dev *sdev)
{
	return sof_client_dev_register(sdev, "host-assisted-dma-trace", 0,
				       &acp_sof_trace_ops, sizeof(acp_sof_trace_ops));
}

void acp_sof_trace_unregister(struct snd_sof_dev *sdev)
{
	sof_client_dev_unregister(sdev, "host-assisted-dma-trace", 0);
}

MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
