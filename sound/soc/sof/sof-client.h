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
struct snd_sof_pcm_stream;
struct snd_soc_acpi_mach;
struct dentry;

struct sof_ipc4_fw_module;
struct sof_ipc4_fw_data;
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
int sof_client_ipc_msg_data(struct sof_client_dev *cdev,
			    struct snd_sof_pcm_stream *sps, void *p, size_t sz);

struct sof_ipc4_fw_module *sof_client_ipc4_find_module(struct sof_client_dev *c, const guid_t *u);

/*
 * The name is copied out of the topology of the owning client, which can be
 * removed as soon as the lookup returns. Widget names are bound by the
 * topology ABI, a truncated name would only affect diagnostic output.
 */
#define SOF_CLIENT_MODULE_NAME_MAX	SNDRV_CTL_ELEM_ID_NAME_MAXLEN

int sof_client_ipc4_get_module_name(struct sof_client_dev *cdev, u32 module_id,
				    int instance_id, char *name, size_t size);

struct dentry *sof_client_get_debugfs_root(struct sof_client_dev *cdev);
struct device *sof_client_get_dma_dev(struct sof_client_dev *cdev);
const struct sof_ipc_fw_version *sof_client_get_fw_version(struct sof_client_dev *cdev);
size_t sof_client_get_ipc_max_payload_size(struct sof_client_dev *cdev);
enum sof_ipc_type sof_client_get_ipc_type(struct sof_client_dev *cdev);
const char *sof_client_get_topology_name(struct sof_client_dev *cdev);
const char *sof_client_get_topology_prefix(struct sof_client_dev *cdev);
bool sof_client_get_ssp_mclk_id_quirk(struct sof_client_dev *cdev, u16 *mclk_id);
const struct snd_soc_acpi_mach *sof_client_get_machine(struct sof_client_dev *cdev);
const char *sof_client_get_machine_drv_name(struct sof_client_dev *cdev);
bool sof_client_is_function_topology_disabled(struct sof_client_dev *cdev);
bool sof_client_is_suspend_target_s0ix(struct sof_client_dev *cdev);
bool sof_client_is_dsp_in_d0(struct sof_client_dev *cdev);

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

/*
 * enum sof_d0i3_vote - a client's vote on DSP D0i3 low-power entry
 *
 * A client reports whether its currently active streams permit the DSP to
 * enter the low-power D0i3 substate. The core folds the votes of all clients
 * that implement the d0i3_vote callback:
 *   - any SOF_D0I3_INCOMPATIBLE           -> D0i3 denied
 *   - else any SOF_D0I3_COMPATIBLE_ACTIVE -> D0i3 allowed
 *   - else (all SOF_D0I3_NO_ACTIVITY)     -> D0i3 denied, nothing to keep it up
 * Clients without the callback do not participate in the vote.
 */
enum sof_d0i3_vote {
	SOF_D0I3_NO_ACTIVITY = 0,	/* no active stream; does not affect the vote */
	SOF_D0I3_COMPATIBLE_ACTIVE,	/* active streams, all D0i3-compatible */
	SOF_D0I3_INCOMPATIBLE,		/* an active stream requires the DSP in D0i0 */
};

typedef enum sof_d0i3_vote (*sof_client_d0i3_vote_callback)(struct sof_client_dev *cdev);

/* IPC4 module instance name lookup */
typedef int (*sof_client_module_name_callback)(struct sof_client_dev *cdev,
					       u32 module_id, int instance_id,
					       char *name, size_t size);

typedef int (*sof_client_fw_booted_callback)(struct sof_client_dev *cdev);

typedef bool (*sof_client_keep_dsp_in_d0_callback)(struct sof_client_dev *cdev);

typedef bool (*sof_client_period_elapsed_callback)(struct sof_client_dev *cdev,
						  struct snd_pcm_substream *substream);

/**
 * struct sof_client_ops - SOF client event callbacks
 * @ipc_rx_handler:	Called for every DSP-initiated IPC notification. The
 *			client is responsible for filtering the message types it
 *			is interested in.
 * @fw_state_handler:	Called on every DSP firmware state change.
 * @fw_booted:		Called after the DSP firmware has been (re)booted and
 *			before the firmware context is restored. Nothing of the
 *			state the client had in the DSP survived the boot. A
 *			non zero return aborts the boot.
 * @keep_dsp_in_d0:	Returns true if the client left something running in
 *			the DSP on purpose which would not survive powering it
 *			down. The DSP is kept in D0 over the system suspend if
 *			any client asks for it.
 * @d0i3_vote:		Returns the client's D0i3 compatibility vote, see
 *			enum sof_d0i3_vote.
 * @period_elapsed:	Called when the platform completed a period of a DMA
 *			stream. Returns true if the substream belongs to the
 *			client, which ends the search. Unlike the other
 *			callbacks this one is invoked from atomic context and
 *			must not sleep.
 * @get_module_name:	Copies the topology name of a module instance owned by
 *			the client. Returns -ENOENT if the client does not own
 *			the module.
 *
 * A client that wants to receive notifications registers a static instance of
 * this structure with sof_client_register_ops() from its probe and drops it
 * with sof_client_unregister_ops() from its remove. All callbacks are optional
 * and are invoked for every client that registered ops.
 *
 * The callbacks are invoked from an SRCU read side critical section, so they
 * may sleep and may send IPC messages to the DSP, unless noted otherwise.
 */
struct sof_client_ops {
	sof_client_event_callback ipc_rx_handler;
	sof_client_fw_state_callback fw_state_handler;
	sof_client_fw_booted_callback fw_booted;
	sof_client_keep_dsp_in_d0_callback keep_dsp_in_d0;
	sof_client_d0i3_vote_callback d0i3_vote;
	sof_client_period_elapsed_callback period_elapsed;
	sof_client_module_name_callback get_module_name;
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
struct snd_sof_mailbox *sof_client_get_mailbox(struct sof_client_dev *cdev,
					       enum snd_sof_mailbox_type type);

ssize_t sof_client_ipc4_find_debug_slot_offset_by_type(struct sof_client_dev *cdev,
						       u32 type);

bool sof_client_is_dspless(struct sof_client_dev *cdev);
int sof_client_get_num_cores(struct sof_client_dev *cdev);
struct sof_ipc4_fw_data *sof_client_get_ipc4_fw_data(struct sof_client_dev *cdev);
u32 sof_client_get_new_comp_id(struct sof_client_dev *cdev);

/* machine driver registration */
int sof_client_machine_register(struct sof_client_dev *cdev);
void sof_client_machine_unregister(struct sof_client_dev *cdev);

/* audio client pdata initialization */
struct sof_audio_client_pdata;
void sof_audio_client_init_pdata(struct snd_sof_dev *sdev,
				 struct sof_audio_client_pdata *pdata);

/* default audio client registration for vendor ops */
int sof_register_audio_client(struct snd_sof_dev *sdev);
void sof_unregister_audio_client(struct snd_sof_dev *sdev);

#endif /* __SOC_SOF_CLIENT_H */
