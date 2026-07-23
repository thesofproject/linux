/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_SOF_CLIENT_H
#define __SOC_SOF_CLIENT_H

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/list.h>
#include <sound/sof.h>

struct sof_ipc_fw_version;
struct sof_ipc_cmd_hdr;
struct snd_sof_dev;
struct dentry;

struct sof_ipc4_fw_module;
struct sof_client_ops;

/**
 * struct sof_client_dev - SOF client device
 * @auxdev:	auxiliary device
 * @ops:	client event callbacks, set via sof_client_register_ops()
 * @ops_node:	entry in the SOF core client ops list
 * @data:	device specific data
 */
struct sof_client_dev {
	struct auxiliary_device auxdev;
	const struct sof_client_ops *ops;
	struct list_head ops_node;
	void *data;
};

#define auxiliary_dev_to_sof_client_dev(auxiliary_dev) \
	container_of(auxiliary_dev, struct sof_client_dev, auxdev)

#define dev_to_sof_client_dev(dev) \
	container_of(to_auxiliary_dev(dev), struct sof_client_dev, auxdev)

int sof_client_ipc_tx_message(struct sof_client_dev *cdev, void *ipc_msg,
			      void *reply_data, size_t reply_bytes);
static inline int sof_client_ipc_tx_message_no_reply(struct sof_client_dev *cdev, void *ipc_msg)
{
	return sof_client_ipc_tx_message(cdev, ipc_msg, NULL, 0);
}
int sof_client_ipc_set_get_data(struct sof_client_dev *cdev, void *ipc_msg,
				bool set);

struct sof_ipc4_fw_module *sof_client_ipc4_find_module(struct sof_client_dev *c, const guid_t *u);
struct snd_sof_widget *sof_client_ipc4_find_swidget_by_id(struct sof_client_dev *cdev,
							  u32 module_id, int instance_id);

struct dentry *sof_client_get_debugfs_root(struct sof_client_dev *cdev);
struct device *sof_client_get_dma_dev(struct sof_client_dev *cdev);
const struct sof_ipc_fw_version *sof_client_get_fw_version(struct sof_client_dev *cdev);
size_t sof_client_get_ipc_max_payload_size(struct sof_client_dev *cdev);
enum sof_ipc_type sof_client_get_ipc_type(struct sof_client_dev *cdev);

/* DSP/firmware boot request */
int sof_client_boot_dsp(struct sof_client_dev *cdev);

/* module refcount management of SOF core */
int sof_client_core_module_get(struct sof_client_dev *cdev);
void sof_client_core_module_put(struct sof_client_dev *cdev);

/* IPC notification */
typedef void (*sof_client_event_callback)(struct sof_client_dev *cdev, void *msg_buf);

/* DSP state notification */
typedef void (*sof_client_fw_state_callback)(struct sof_client_dev *cdev,
					     enum sof_fw_state state);

/**
 * struct sof_client_ops - SOF client event callbacks
 * @ipc_rx_handler:	Called for every DSP-initiated IPC notification. The
 *			client is responsible for filtering the message types it
 *			is interested in.
 * @fw_state_handler:	Called on every DSP firmware state change.
 *
 * A client that wants to receive notifications registers a static instance of
 * this structure with sof_client_register_ops() from its probe and drops it
 * with sof_client_unregister_ops() from its remove. All callbacks are optional
 * and are invoked for every client that registered ops.
 */
struct sof_client_ops {
	sof_client_event_callback ipc_rx_handler;
	sof_client_fw_state_callback fw_state_handler;
};

int sof_client_register_ops(struct sof_client_dev *cdev,
			    const struct sof_client_ops *ops);
void sof_client_unregister_ops(struct sof_client_dev *cdev);

enum sof_fw_state sof_client_get_fw_state(struct sof_client_dev *cdev);
int sof_client_ipc_rx_message(struct sof_client_dev *cdev, void *ipc_msg, void *msg_buf);

void sof_client_mailbox_read(struct sof_client_dev *cdev, u32 offset,
			     void *message, size_t bytes);
void sof_client_mailbox_write(struct sof_client_dev *cdev, u32 offset,
			      void *message, size_t bytes);

ssize_t sof_client_ipc4_find_debug_slot_offset_by_type(struct sof_client_dev *cdev,
						       u32 type);

#endif /* __SOC_SOF_CLIENT_H */
