// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2021 Intel Corporation. All rights reserved.
//
// Author: Rander Wang <rander.wang@linux.intel.com>
//
#include <sound/sof/header.h>
#include <sound/sof/ipc4/header.h>
#include "sof-priv.h"
#include "sof-audio.h"
#include "ipc4.h"
#include "ops.h"

static const struct sof_ipc4_fw_status ipc4_status[] = {
	{0, "The operation was successful"},
	{1, "Invalid parameter specified"},
	{2, "Unknown message type specified"},
	{3, "Not enough space in the IPC reply buffer to complete the request"},
	{4, "The system or resource is busy"},
	{5, "Replaced ADSP IPC PENDING (unused)"},
	{6, "Unknown error while processing the request"},
	{7, "Unsupported operation requested"},
	{8, "Reserved (ADSP_STAGE_UNINITIALIZED removed)"},
	{9, "Specified resource not found"},
	{10, "A resource's ID requested to be created is already assigned"},
	{11, "Reserved (ADSP_IPC_OUT_OF_MIPS removed)"},
	{12, "Required resource is in invalid state"},
	{13, "Requested power transition failed to complete"},
	{14, "Manifest of the library being loaded is invalid"},
	{15, "Requested service or data is unavailable on the target platform"},
	{42, "Library target address is out of storage memory range"},
	{43, "Reserved"},
	{44, "Image verification by CSE failed"},
	{100, "General module management error"},
	{101, "Module loading failed"},
	{102, "Integrity check of the loaded module content failed"},
	{103, "Attempt to unload code of the module in use"},
	{104, "Other failure of module instance initialization request"},
	{105, "Reserved (ADSP_IPC_OUT_OF_MIPS removed)"},
	{106, "Reserved (ADSP_IPC_CONFIG_GET_ERROR removed)"},
	{107, "Reserved (ADSP_IPC_CONFIG_SET_ERROR removed)"},
	{108, "Reserved (ADSP_IPC_LARGE_CONFIG_GET_ERROR removed)"},
	{109, "Reserved (ADSP_IPC_LARGE_CONFIG_SET_ERROR removed)"},
	{110, "Invalid (out of range) module ID provided"},
	{111, "Invalid module instance ID provided"},
	{112, "Invalid queue (pin) ID provided"},
	{113, "Invalid destination queue (pin) ID provided"},
	{114, "Reserved (ADSP_IPC_BIND_UNBIND_DST_SINK_UNSUPPORTED removed)"},
	{115, "Reserved (ADSP_IPC_UNLOAD_INST_EXISTS removed)"},
	{116, "Invalid target code ID provided"},
	{117, "Injection DMA buffer is too small for probing the input pin"},
	{118, "Extraction DMA buffer is too small for probing the output pin"},
	{120, "Invalid ID of configuration item provided in TLV list"},
	{121, "Invalid length of configuration item provided in TLV list"},
	{122, "Invalid structure of configuration item provided"},
	{140, "Initialization of DMA Gateway failed"},
	{141, "Invalid ID of gateway provided"},
	{142, "Setting state of DMA Gateway failed"},
	{143, "DMA_CONTROL message targeting gateway not allocated yet"},
	{150, "Attempt to configure SCLK while I2S port is running"},
	{151, "Attempt to configure MCLK while I2S port is running"},
	{152, "Attempt to stop SCLK that is not running"},
	{153, "Attempt to stop MCLK that is not running"},
	{160, "Reserved (ADSP_IPC_PIPELINE_NOT_INITIALIZED removed)"},
	{161, "Reserved (ADSP_IPC_PIPELINE_NOT_EXIST removed)"},
	{162, "Reserved (ADSP_IPC_PIPELINE_SAVE_FAILED removed)"},
	{163, "Reserved (ADSP_IPC_PIPELINE_RESTORE_FAILED removed)"},
	{165, "Reserved (ADSP_IPC_PIPELINE_ALREADY_EXISTS removed)"},
};

void sof_ipc4_process_reply(struct snd_sof_dev *sdev, u32 msg)
{
	u32 status = msg & SOF_IPC4_REPLY_STATUS_MASK;
	int i;

	sdev->ipc->msg.reply_error = status;
	if (status) {
		for (i = 0; i < ARRAY_SIZE(ipc4_status); i++) {
			if (ipc4_status[i].status == status) {
				dev_err(sdev->dev, "FW reported error: %s\n",
					ipc4_status[i].msg);
				break;
			}
		}

		if (i == ARRAY_SIZE(ipc4_status))
			dev_err(sdev->dev,
				"FW reported unknown error, status = %d\n", status);
	}

	snd_sof_ipc_reply(sdev, msg);
}
EXPORT_SYMBOL(sof_ipc4_process_reply);

/* wait for IPC message reply */
static int ipc4_tx_wait_done(struct snd_sof_ipc *ipc, struct snd_sof_ipc_msg *msg,
			     void *reply_data)
{
	struct sof_ipc4_msg *ipc4_msg = msg->msg_data;
	struct snd_sof_dev *sdev = ipc->sdev;
	int ret;

	/* wait for DSP IPC completion */
	ret = wait_event_timeout(msg->waitq, msg->ipc_complete,
				 msecs_to_jiffies(sdev->ipc_timeout));
	if (ret == 0) {
		dev_err(sdev->dev, "ipc timed out for %#x|%#x\n",
			ipc4_msg->primary, ipc4_msg->extension);
		return -ETIMEDOUT;
	} else if (msg->reply_error) {
		dev_err(sdev->dev, "ipc error for msg %#x|%#x\n",
			ipc4_msg->primary, ipc4_msg->extension);
		return -EIO;
	}

	return 0;
}

static int sof_ipc4_tx_message_unlocked(struct snd_sof_ipc *ipc,
					void *msg_data, size_t msg_bytes,
					void *reply_data, size_t reply_bytes)
{
	struct snd_sof_dev *sdev = ipc->sdev;
	struct snd_sof_ipc_msg *msg;
	int ret;

	if (ipc->disable_ipc_tx || sdev->fw_state != SOF_FW_BOOT_COMPLETE)
		return -ENODEV;

	if (msg_bytes > ipc->max_payload_size || reply_bytes > ipc->max_payload_size)
		return -EINVAL;

	/*
	 * The spin-lock is also still needed to protect message objects against
	 * other atomic contexts.
	 */
	spin_lock_irq(&sdev->ipc_lock);

	/* initialise the message */
	msg = &ipc->msg;

	/* attach any data */
	msg->msg_data = msg_data;
	msg->msg_size = msg_bytes;
	msg->reply_size = reply_bytes;
	msg->reply_error = 0;

	sdev->msg = msg;

	ret = snd_sof_dsp_send_msg(sdev, msg);
	/* Next reply that we receive will be related to this message */
	if (!ret)
		msg->ipc_complete = false;

	spin_unlock_irq(&sdev->ipc_lock);

	if (ret) {
		dev_err_ratelimited(sdev->dev, "ipc tx failed with error %d\n", ret);
		return ret;
	}

	/* now wait for completion */
	return ipc4_tx_wait_done(ipc, msg, reply_data);
}

int sof_ipc4_tx_message(struct snd_sof_ipc *ipc,
			void *msg_data, size_t msg_bytes, void *reply_data,
			size_t reply_bytes)
{
	int ret;

	if (!msg_data)
		return -EINVAL;

	/* Serialise IPC TX */
	mutex_lock(&ipc->tx_mutex);

	ret = sof_ipc4_tx_message_unlocked(ipc, msg_data, msg_bytes,
					   reply_data, reply_bytes);

	mutex_unlock(&ipc->tx_mutex);

	return ret;
}
EXPORT_SYMBOL(sof_ipc4_tx_message);

void snd_sof_ipc4_msgs_rx(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_msg *ipc4_msg = sdev->ipc->msg.reply_data;
	int err;

	if (!SOF_IPC4_GLB_NOTIFY_MSG_TYPE(ipc4_msg->primary))
		return;

	switch (SOF_IPC4_GLB_NOTIFY_TYPE(ipc4_msg->primary)) {
	case SOF_IPC4_GLB_NOTIFY_FW_READY:
		/* check for FW boot completion */
		if (sdev->fw_state == SOF_FW_BOOT_IN_PROGRESS) {
			err = sof_ops(sdev)->fw_ready(sdev, ipc4_msg->primary);
			if (err < 0)
				sof_set_fw_state(sdev, SOF_FW_BOOT_READY_FAILED);
			else
				sof_set_fw_state(sdev, SOF_FW_BOOT_READY_OK);

			/* wake up firmware loader */
			wake_up(&sdev->boot_wait);
		}

		break;
	default:
		break;
	}
}
EXPORT_SYMBOL(snd_sof_ipc4_msgs_rx);

int sof_ipc4_init_msg_memory(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_msg *ipc4_msg;
	struct snd_sof_ipc_msg *msg;

	msg = &sdev->ipc->msg;

	/* TODO: get max_payload_size from firmware */
	sdev->ipc->max_payload_size = SOF_IPC4_MSG_MAX_SIZE;

	/* Allocate memory for the ipc4 container and the maximum payload */
	msg->reply_data = devm_kzalloc(sdev->dev, sdev->ipc->max_payload_size +
				       sizeof(struct sof_ipc4_msg), GFP_KERNEL);
	if (!msg->reply_data)
		return -ENOMEM;

	ipc4_msg = msg->reply_data;
	ipc4_msg->data_ptr = msg->reply_data + sizeof(struct sof_ipc4_msg);

	/*
	 * TODO: Currently we are not able to wake DSP up once it is suspended.
	 * Calling pm_runtime_get_sync() here to prevent it from entering
	 * suspend. This should be removed once power management is implemented
	 */
	pm_runtime_get_sync(sdev->dev);
	return 0;
}
EXPORT_SYMBOL(sof_ipc4_init_msg_memory);

