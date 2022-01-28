// SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2022 Intel Corporation. All rights reserved.
//
// Authors: Rander Wang <rander.wang@linux.intel.com>
//	    Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//
#include <sound/sof/header.h>
#include <sound/sof/ipc4/header.h>
#include "sof-priv.h"
#include "sof-audio.h"
#include "ipc4-ops.h"
#include "ops.h"

static const struct sof_ipc4_fw_status {
	int status;
	char *msg;
} ipc4_status[] = {
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

static int sof_ipc4_check_reply_status(struct snd_sof_dev *sdev, u32 status)
{
	int i, ret;

	status &= SOF_IPC4_REPLY_STATUS_MASK;

	if (!status)
		return 0;

	for (i = 0; i < ARRAY_SIZE(ipc4_status); i++) {
		if (ipc4_status[i].status == status) {
			dev_err(sdev->dev, "FW reported error: %s\n", ipc4_status[i].msg);
			goto to_errno;
		}
	}

	if (i == ARRAY_SIZE(ipc4_status))
		dev_err(sdev->dev, "FW reported unknown error, status = %d\n", status);

to_errno:
	switch (status) {
	case 8:
	case 11:
	case 105 ... 109:
	case 114 ... 115:
	case 160 ... 163:
	case 155:
		ret = -ENOENT;
		break;
	case 4:
	case 150:
	case 151:
		ret = -EBUSY;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_VERBOSE_IPC)
static void sof_ipc4_log_header(struct device *dev, u8 *text, struct sof_ipc4_msg *msg,
				bool data_size_valid)
{
	u32 val, type;
	u8 *str2 = NULL;
	u8 *str;

	val = msg->primary & SOF_IPC4_GLB_MSG_TARGET_MASK;
	type = msg->primary & SOF_IPC4_GLB_MSG_TYPE_MASK;
	type >>= SOF_IPC4_GLB_MSG_TYPE_SHIFT;
	if (val == SOF_IPC4_GLB_MSG_TARGET(SOF_IPC4_MODULE_MSG)) {
		/* Module message */
		switch (type) {
		case SOF_IPC4_MOD_INIT_INSTANCE:
			str = "MOD_INIT_INSTANCE";
			break;
		case SOF_IPC4_MOD_CONFIG_GET:
			str = "MOD_CONFIG_GET";
			break;
		case SOF_IPC4_MOD_CONFIG_SET:
			str = "MOD_CONFIG_SET";
			break;
		case SOF_IPC4_MOD_LARGE_CONFIG_GET:
			str = "MOD_LARGE_CONFIG_GET";
			break;
		case SOF_IPC4_MOD_LARGE_CONFIG_SET:
			str = "MOD_LARGE_CONFIG_SET";
			break;
		case SOF_IPC4_MOD_BIND:
			str = "MOD_BIND";
			break;
		case SOF_IPC4_MOD_UNBIND:
			str = "MOD_UNBIND";
			break;
		case SOF_IPC4_MOD_SET_DX:
			str = "MOD_SET_DX";
			break;
		case SOF_IPC4_MOD_SET_D0IX:
			str = "MOD_SET_D0IX";
			break;
		case SOF_IPC4_MOD_ENTER_MODULE_RESTORE:
			str = "MOD_ENTER_MODULE_RESTORE";
			break;
		case SOF_IPC4_MOD_EXIT_MODULE_RESTORE:
			str = "MOD_EXIT_MODULE_RESTORE";
			break;
		case SOF_IPC4_MOD_DELETE_INSTANCE:
			str = "MOD_DELETE_INSTANCE";
			break;
		default:
			str = "Unknown Module message type";
			break;
		}
	} else {
		/* Global FW message */
		switch (type) {
		case SOF_IPC4_GLB_BOOT_CONFIG:
			str = "GLB_BOOT_CONFIG";
			break;
		case SOF_IPC4_GLB_ROM_CONTROL:
			str = "GLB_ROM_CONTROL";
			break;
		case SOF_IPC4_GLB_IPCGATEWAY_CMD:
			str = "GLB_IPCGATEWAY_CMD";
			break;
		case SOF_IPC4_GLB_PERF_MEASUREMENTS_CMD:
			str = "GLB_PERF_MEASUREMENTS_CMD";
			break;
		case SOF_IPC4_GLB_CHAIN_DMA:
			str = "GLB_CHAIN_DMA";
			break;
		case SOF_IPC4_GLB_LOAD_MULTIPLE_MODULES:
			str = "GLB_LOAD_MULTIPLE_MODULES";
			break;
		case SOF_IPC4_GLB_UNLOAD_MULTIPLE_MODULES:
			str = "GLB_UNLOAD_MULTIPLE_MODULES";
			break;
		case SOF_IPC4_GLB_CREATE_PIPELINE:
			str = "GLB_CREATE_PIPELINE";
			break;
		case SOF_IPC4_GLB_DELETE_PIPELINE:
			str = "GLB_DELETE_PIPELINE";
			break;
		case SOF_IPC4_GLB_SET_PIPELINE_STATE:
			str = "GLB_SET_PIPELINE_STATE";
			break;
		case SOF_IPC4_GLB_GET_PIPELINE_STATE:
			str = "GLB_GET_PIPELINE_STATE";
			break;
		case SOF_IPC4_GLB_GET_PIPELINE_CONTEXT_SIZE:
			str = "GLB_GET_PIPELINE_CONTEXT_SIZE";
			break;
		case SOF_IPC4_GLB_SAVE_PIPELINE:
			str = "GLB_SAVE_PIPELINE";
			break;
		case SOF_IPC4_GLB_RESTORE_PIPELINE:
			str = "GLB_RESTORE_PIPELINE";
			break;
		case SOF_IPC4_GLB_LOAD_LIBRARY:
			str = "GLB_LOAD_LIBRARY";
			break;
		case SOF_IPC4_GLB_INTERNAL_MESSAGE:
			str = "GLB_INTERNAL_MESSAGE";
			break;
		case SOF_IPC4_GLB_NOTIFICATION:
			str = "GLB_NOTIFICATION";

			switch (SOF_IPC4_GLB_NOTIFY_TYPE(msg->primary)) {
			case SOF_IPC4_GLB_NOTIFY_PHRASE_DETECTED:
				str2 = "GLB_NOTIFY_PHRASE_DETECTED";
				break;
			case SOF_IPC4_GLB_NOTIFY_RESOURCE_EVENT:
				str2 = "GLB_NOTIFY_RESOURCE_EVENT";
				break;
			case SOF_IPC4_GLB_NOTIFY_LOG_BUFFER_STATUS:
				str2 = "GLB_NOTIFY_LOG_BUFFER_STATUS";
				break;
			case SOF_IPC4_GLB_NOTIFY_TIMESTAMP_CAPTURED:
				str2 = "GLB_NOTIFY_TIMESTAMP_CAPTURED";
				break;
			case SOF_IPC4_GLB_NOTIFY_FW_READY:
				str2 = "GLB_NOTIFY_FW_READY";
				break;
			case SOF_IPC4_GLB_NOTIFY_FW_AUD_CLASS_RESULT:
				str2 = "GLB_NOTIFY_FW_AUD_CLASS_RESULT";
				break;
			case SOF_IPC4_GLB_NOTIFY_EXCEPTION_CAUGHT:
				str2 = "GLB_NOTIFY_EXCEPTION_CAUGHT";
				break;
			case SOF_IPC4_GLB_NOTIFY_MODULE_NOTIFICATION:
				str2 = "GLB_NOTIFY_MODULE_NOTIFICATION";
				break;
			case SOF_IPC4_GLB_NOTIFY_PROBE_DATA_AVAILABLE:
				str2 = "GLB_NOTIFY_PROBE_DATA_AVAILABLE";
				break;
			case SOF_IPC4_GLB_NOTIFY_ASYNC_MSG_SRVC_MESSAGE:
				str2 = "GLB_NOTIFY_ASYNC_MSG_SRVC_MESSAGE";
				break;
			default:
				str2 = "Unknown Global notification";
				break;
			}
			break;
		default:
			str = "Unknown Global message type";
			break;
		}
	}

	if (str2) {
		if (data_size_valid && msg->data_size)
			dev_dbg(dev, "%s: %#x|%#x [data size: %zu]: %s|%s\n",
				text, msg->primary, msg->extension, msg->data_size,
				str, str2);
		else
			dev_dbg(dev, "%s: %#x|%#x: %s|%s\n", text, msg->primary,
				msg->extension, str, str2);
	} else {
		if (data_size_valid && msg->data_size)
			dev_dbg(dev, "%s: %#x|%#x [data size: %zu]: %s\n",
				text, msg->primary, msg->extension, msg->data_size,
				str);
		else
			dev_dbg(dev, "%s: %#x|%#x: %s\n", text, msg->primary,
				msg->extension, str);
	}
}
#else /* CONFIG_SND_SOC_SOF_DEBUG_VERBOSE_IPC */
static void sof_ipc4_log_header(struct device *dev, u8 *text, struct sof_ipc4_msg *msg,
				bool data_size_valid)
{
	if (data_size_valid && msg->data_size)
		dev_dbg(dev, "%s: %#x|%#x [data size: %zu]\n", text,
			msg->primary, msg->extension, msg->data_size);
	else
		dev_dbg(dev, "%s: %#x|%#x\n", text, msg->primary, msg->extension);
}
#endif

static int sof_ipc4_get_reply(struct snd_sof_dev *sdev)
{
	struct snd_sof_ipc_msg *msg = sdev->msg;
	struct sof_ipc4_msg *ipc4_reply;
	int ret;

	/* get the generic reply */
	ipc4_reply = msg->reply_data;

	sof_ipc4_log_header(sdev->dev, "ipc tx reply", ipc4_reply, false);

	ret = sof_ipc4_check_reply_status(sdev, ipc4_reply->primary);
	if (ret)
		return ret;

	/* No other information is expected for non large config get replies */
	if (!msg->reply_size ||
	    (ipc4_reply->primary & SOF_IPC4_GLB_MSG_TARGET_MASK) !=
	    SOF_IPC4_GLB_MSG_TARGET(SOF_IPC4_MODULE_MSG) ||
	    (ipc4_reply->primary & SOF_IPC4_GLB_MSG_TYPE_MASK) !=
	    SOF_IPC4_GLB_MSG_TYPE(SOF_IPC4_MOD_LARGE_CONFIG_GET))
		return 0;

	/* Read the requested payload */
	snd_sof_dsp_mailbox_read(sdev, sdev->dsp_box.offset, ipc4_reply->data_ptr,
				 msg->reply_size);

	return 0;
}

/* wait for IPC message reply */
static int ipc4_wait_tx_done(struct snd_sof_ipc *ipc, void *reply_data)
{
	struct snd_sof_ipc_msg *msg = &ipc->msg;
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
	} else {
		if (msg->reply_error) {
			dev_err(sdev->dev, "ipc error for msg %#x|%#x\n",
				ipc4_msg->primary, ipc4_msg->extension);
			ret =  msg->reply_error;
		} else if (reply_data) {
			struct sof_ipc4_msg *ipc4_reply = msg->reply_data;
			struct sof_ipc4_msg *ipc4_reply_data = reply_data;

			/* Copy the header */
			ipc4_reply_data->header_u64 = ipc4_reply->header_u64;
			if (msg->reply_size && ipc4_reply_data->data_ptr) {
				/* copy the payload returned from DSP */
				memcpy(ipc4_reply_data->data_ptr, ipc4_reply->data_ptr,
				       msg->reply_size);
				ipc4_reply_data->data_size = msg->reply_size;
			}
		}

		if (!msg->reply_error) {
			ret = 0;
			sof_ipc4_log_header(sdev->dev, "ipc tx done ", ipc4_msg, true);
		}

		/* re-enable dumps after successful IPC tx */
		if (sdev->ipc_dump_printed) {
			sdev->dbg_dump_printed = false;
			sdev->ipc_dump_printed = false;
		}
	}

	return ret;
}

static int ipc4_tx_msg_unlocked(struct snd_sof_ipc *ipc,
				void *msg_data, size_t msg_bytes,
				void *reply_data, size_t reply_bytes)
{
	struct sof_ipc4_msg *ipc4_msg = msg_data;
	struct snd_sof_dev *sdev = ipc->sdev;
	int ret;

	if (msg_bytes > ipc->max_payload_size || reply_bytes > ipc->max_payload_size)
		return -EINVAL;

	ret = sof_ipc_send_msg(sdev, msg_data, msg_bytes, reply_bytes);

	if (ret) {
		dev_err_ratelimited(sdev->dev,
				    "%s: ipc message send for %#x|%#x failed: %d\n",
				    __func__, ipc4_msg->primary, ipc4_msg->extension, ret);
		return ret;
	}

	sof_ipc4_log_header(sdev->dev, "ipc tx      ", msg_data, true);

	/* now wait for completion */
	return ipc4_wait_tx_done(ipc, reply_data);
}

static int sof_ipc4_tx_msg(struct snd_sof_dev *sdev, void *msg_data, size_t msg_bytes,
			   void *reply_data, size_t reply_bytes, bool no_pm)
{
	struct snd_sof_ipc *ipc = sdev->ipc;
	int ret;

	if (!msg_data)
		return -EINVAL;

	/* Serialise IPC TX */
	mutex_lock(&ipc->tx_mutex);

	ret = ipc4_tx_msg_unlocked(ipc, msg_data, msg_bytes, reply_data, reply_bytes);

	mutex_unlock(&ipc->tx_mutex);

	return ret;
}

static int sof_ipc4_set_get_data(struct snd_sof_dev *sdev, void *data,
				 size_t payload_bytes, bool set)
{
	size_t payload_limit = sdev->ipc->max_payload_size;
	struct sof_ipc4_msg *ipc4_msg = data;
	struct sof_ipc4_msg tx = {{ 0 }};
	struct sof_ipc4_msg rx = {{ 0 }};
	size_t remaining = payload_bytes;
	size_t offset = 0;
	size_t chunk_size;
	int ret;

	if (!data)
		return -EINVAL;

	if ((ipc4_msg->primary & SOF_IPC4_GLB_MSG_TARGET_MASK) !=
	    SOF_IPC4_GLB_MSG_TARGET(SOF_IPC4_MODULE_MSG))
		return -EINVAL;

	ipc4_msg->primary &= ~SOF_IPC4_GLB_MSG_TYPE_MASK;
	tx.primary = ipc4_msg->primary;
	tx.extension = ipc4_msg->extension;

	if (set)
		tx.primary |= SOF_IPC4_GLB_MSG_TYPE(SOF_IPC4_MOD_LARGE_CONFIG_SET);
	else
		tx.primary |= SOF_IPC4_GLB_MSG_TYPE(SOF_IPC4_MOD_LARGE_CONFIG_GET);

	tx.extension &= ~SOF_IPC4_MOD_EXT_MSG_SIZE_MASK;
	tx.extension |= SOF_IPC4_MOD_EXT_MSG_SIZE(payload_bytes);

	tx.extension |= SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK(1);

	/* Serialise IPC TX */
	mutex_lock(&sdev->ipc->tx_mutex);

	do {
		size_t tx_size, rx_size;

		if (remaining > payload_limit) {
			chunk_size = payload_limit;
		} else {
			chunk_size = remaining;
			if (set)
				tx.extension |= SOF_IPC4_MOD_EXT_MSG_LAST_BLOCK(1);
		}

		if (offset) {
			tx.extension &= ~SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK_MASK;
			tx.extension &= ~SOF_IPC4_MOD_EXT_MSG_SIZE_MASK;
			tx.extension |= SOF_IPC4_MOD_EXT_MSG_SIZE(offset);
		}

		if (set) {
			tx.data_size = chunk_size;
			tx.data_ptr = ipc4_msg->data_ptr + offset;

			tx_size = chunk_size;
			rx_size = 0;
		} else {
			rx.primary = 0;
			rx.extension = 0;
			rx.data_size = chunk_size;
			rx.data_ptr = ipc4_msg->data_ptr + offset;

			tx_size = 0;
			rx_size = chunk_size;
		}

		/* Send the message for the current chunk */
		ret = ipc4_tx_msg_unlocked(sdev->ipc, &tx, tx_size, &rx, rx_size);
		if (ret < 0) {
			dev_err(sdev->dev,
				"%s: large config %s failed at offset %zu: %d\n",
				__func__, set ? "set" : "get", offset, ret);
			goto out;
		}

		if (!set && rx.extension & SOF_IPC4_MOD_EXT_MSG_FIRST_BLOCK_MASK) {
			/* Verify the firmware reported total payload size */
			rx_size = rx.extension & SOF_IPC4_MOD_EXT_MSG_SIZE_MASK;

			if (rx_size > payload_bytes) {
				dev_err(sdev->dev,
					"%s: Receive buffer (%zu) is too small for %zu\n",
					__func__, payload_bytes, rx_size);
				ret = -ENOMEM;
				goto out;
			}

			if (rx_size < chunk_size) {
				chunk_size = rx_size;
				remaining = rx_size;
			} else if (rx_size < payload_bytes) {
				remaining = rx_size;
			}
		}

		offset += chunk_size;
		remaining -= chunk_size;
	} while (remaining);

	/* Adjust the received data size if needed */
	if (!set && payload_bytes != offset)
		ipc4_msg->data_size = offset;

out:
	mutex_unlock(&sdev->ipc->tx_mutex);

	return ret;
}

static int sof_ipc4_init_msg_memory(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_msg *ipc4_msg;
	struct snd_sof_ipc_msg *msg = &sdev->ipc->msg;

	/* TODO: get max_payload_size from firmware */
	sdev->ipc->max_payload_size = SOF_IPC4_MSG_MAX_SIZE;

	/* Allocate memory for the ipc4 container and the maximum payload */
	msg->reply_data = devm_kzalloc(sdev->dev, sdev->ipc->max_payload_size +
				       sizeof(struct sof_ipc4_msg), GFP_KERNEL);
	if (!msg->reply_data)
		return -ENOMEM;

	ipc4_msg = msg->reply_data;
	ipc4_msg->data_ptr = msg->reply_data + sizeof(struct sof_ipc4_msg);

	return 0;
}

static int ipc4_fw_ready(struct snd_sof_dev *sdev, struct sof_ipc4_msg *ipc4_msg)
{
	int inbox_offset, inbox_size, outbox_offset, outbox_size;

	/* no need to re-check version/ABI for subsequent boots */
	if (!sdev->first_boot)
		return 0;

	/* Set up the windows for IPC communication */
	inbox_offset = snd_sof_dsp_get_mailbox_offset(sdev);
	if (inbox_offset < 0) {
		dev_err(sdev->dev, "%s: No mailbox offset\n", __func__);
		return inbox_offset;
	}
	inbox_size = SOF_IPC4_MSG_MAX_SIZE;
	outbox_offset = snd_sof_dsp_get_window_offset(sdev, 1);
	outbox_size = SOF_IPC4_MSG_MAX_SIZE;

	sdev->dsp_box.offset = inbox_offset;
	sdev->dsp_box.size = inbox_size;
	sdev->host_box.offset = outbox_offset;
	sdev->host_box.size = outbox_size;

	dev_dbg(sdev->dev, "mailbox upstream 0x%x - size 0x%x\n",
		inbox_offset, inbox_size);
	dev_dbg(sdev->dev, "mailbox downstream 0x%x - size 0x%x\n",
		outbox_offset, outbox_size);

	sdev->fw_ready.version.abi_version = SOF_ABI_VER(4, 0, 0);

	return sof_ipc4_init_msg_memory(sdev);
}

static void sof_ipc4_rx_msg(struct snd_sof_dev *sdev)
{
	struct sof_ipc4_msg *ipc4_msg = sdev->ipc->msg.rx_data;
	size_t data_size = 0;
	int err;

	if (!ipc4_msg || !SOF_IPC4_GLB_NOTIFY_MSG_TYPE(ipc4_msg->primary))
		return;

	ipc4_msg->data_ptr = NULL;
	ipc4_msg->data_size = 0;

	sof_ipc4_log_header(sdev->dev, "ipc rx      ", ipc4_msg, false);

	switch (SOF_IPC4_GLB_NOTIFY_TYPE(ipc4_msg->primary)) {
	case SOF_IPC4_GLB_NOTIFY_FW_READY:
		/* check for FW boot completion */
		if (sdev->fw_state == SOF_FW_BOOT_IN_PROGRESS) {
			err = ipc4_fw_ready(sdev, ipc4_msg);
			if (err < 0)
				sof_set_fw_state(sdev, SOF_FW_BOOT_READY_FAILED);
			else
				sof_set_fw_state(sdev, SOF_FW_BOOT_READY_OK);

			/* wake up firmware loader */
			wake_up(&sdev->boot_wait);
		}

		break;
	case SOF_IPC4_GLB_NOTIFY_RESOURCE_EVENT:
		data_size = sizeof(struct sof_ipc4_notify_resource_data);
		break;
	default:
		dev_dbg(sdev->dev, "%s: Unhandled DSP message: %#x|%#x\n", __func__,
			ipc4_msg->primary, ipc4_msg->extension);
		break;
	}

	if (data_size) {
		ipc4_msg->data_ptr = kmalloc(data_size, GFP_KERNEL);
		if (!ipc4_msg->data_ptr)
			return;

		ipc4_msg->data_size = data_size;
		snd_sof_ipc_msg_data(sdev, NULL, ipc4_msg->data_ptr, ipc4_msg->data_size);
	}

	sof_ipc4_log_header(sdev->dev, "ipc rx done ", ipc4_msg, true);

	if (data_size) {
		kfree(ipc4_msg->data_ptr);
		ipc4_msg->data_ptr = NULL;
		ipc4_msg->data_size = 0;
	}
}

const struct sof_ipc_ops ipc4_ops = {
	.tx_msg = sof_ipc4_tx_msg,
	.rx_msg = sof_ipc4_rx_msg,
	.set_get_data = sof_ipc4_set_get_data,
	.get_reply = sof_ipc4_get_reply,
};
