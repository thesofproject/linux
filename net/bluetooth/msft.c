// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Google Corporation
 */

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/l2cap.h>
#include <net/bluetooth/mgmt.h>

#include "mgmt_util.h"
#include "msft.h"

#define MSFT_RSSI_THRESHOLD_VALUE_MIN		-127
#define MSFT_RSSI_THRESHOLD_VALUE_MAX		20
#define MSFT_RSSI_LOW_TIMEOUT_MAX		0x3C

#define MSFT_OP_READ_SUPPORTED_FEATURES		0x00
struct msft_cp_read_supported_features {
	__u8   sub_opcode;
} __packed;

struct msft_rp_read_supported_features {
	__u8   status;
	__u8   sub_opcode;
	__le64 features;
	__u8   evt_prefix_len;
	__u8   evt_prefix[];
} __packed;

#define MSFT_OP_LE_MONITOR_ADVERTISEMENT	0x03
#define MSFT_MONITOR_ADVERTISEMENT_TYPE_PATTERN	0x01
struct msft_le_monitor_advertisement_pattern {
	__u8 length;
	__u8 data_type;
	__u8 start_byte;
	__u8 pattern[];
};

struct msft_le_monitor_advertisement_pattern_data {
	__u8 count;
	__u8 data[];
};

struct msft_cp_le_monitor_advertisement {
	__u8 sub_opcode;
	__s8 rssi_high;
	__s8 rssi_low;
	__u8 rssi_low_interval;
	__u8 rssi_sampling_period;
	__u8 cond_type;
	__u8 data[];
} __packed;

struct msft_rp_le_monitor_advertisement {
	__u8 status;
	__u8 sub_opcode;
	__u8 handle;
} __packed;

#define MSFT_OP_LE_CANCEL_MONITOR_ADVERTISEMENT	0x04
struct msft_cp_le_cancel_monitor_advertisement {
	__u8 sub_opcode;
	__u8 handle;
} __packed;

struct msft_rp_le_cancel_monitor_advertisement {
	__u8 status;
	__u8 sub_opcode;
} __packed;

#define MSFT_OP_LE_SET_ADVERTISEMENT_FILTER_ENABLE	0x05
struct msft_cp_le_set_advertisement_filter_enable {
	__u8 sub_opcode;
	__u8 enable;
} __packed;

struct msft_rp_le_set_advertisement_filter_enable {
	__u8 status;
	__u8 sub_opcode;
} __packed;

/* Microsoft Bluetooth HCI Vendor-Specific Extension for AVDTP hardware offload
 * (feature bit 10 in HCI_VS_MSFT_Read_Supported_Features). Command and event
 * structures are wire-protocol HCI packets mapped onto skb->data with __le16
 * fields at unaligned byte offsets, so __packed is required without extra
 * struct alignment padding. Capability payloads use standard Bluetooth SIG
 * AVDTP v1.3 (Section 8.21.5) Media Codec Service Capability elements and
 * A2DP v1.3 (Sections 4.3.2 & 4.5.2) SBC / MPEG-2,4 AAC Codec Information
 * Elements.
 */
#define AVDTP_SVC_CAT_MEDIA_CODEC		0x07
#define AVDTP_CAP_HDR_LEN			2
#define AVDTP_MEDIA_CODEC_HDR_LEN		2
#define AVDTP_MEDIA_TYPE_AUDIO			0x00

#define A2DP_CODEC_SBC				0x00
#define A2DP_CODEC_MPEG24			0x02
#define A2DP_CODEC_VENDOR			0xff

#define AVDTP_SBC_CIE_LEN			4
#define AVDTP_SBC_LOSC				(AVDTP_MEDIA_CODEC_HDR_LEN + \
						 AVDTP_SBC_CIE_LEN)
#define AVDTP_SBC_CAP_LEN			(AVDTP_CAP_HDR_LEN + \
						 AVDTP_SBC_LOSC)

#define AVDTP_AAC_CIE_LEN			6
#define AVDTP_AAC_LOSC				(AVDTP_MEDIA_CODEC_HDR_LEN + \
						 AVDTP_AAC_CIE_LEN)
#define AVDTP_AAC_CAP_LEN			(AVDTP_CAP_HDR_LEN + \
						 AVDTP_AAC_LOSC)
#define AVDTP_AAC_VBR_BIT			0x80
#define AVDTP_AAC_BITRATE_HI_MASK		0x7f
#define AVDTP_AAC_MIN_BITRATE			128000
#define AVDTP_AAC_MAX_BITRATE			192000

/* Intel Bluetooth Assigned Company ID and vendor A2DP offload codec IDs */
#define BT_COMPID_INTEL				0x0002
#define BTINTEL_A2DP_VID_SBC			0x0000
#define BTINTEL_A2DP_VID_AAC			0x0002

#define MSFT_OP_AVDTP_CAPABILITIES_CONF		0x07
struct msft_cp_avdtp_capabilities_conf {
	__u8 sub_opcode;
	__u8 ext_codec_count;
	__u8 audio_if_param_count;
} __packed;

struct msft_rp_avdtp_capabilities_conf {
	__u8 status;
	__u8 sub_opcode;
	__u8 int_codec_count;
	__u8 data[];
} __packed;

#define MSFT_OP_AVDTP_OPEN			0x08
struct msft_cp_avdtp_open {
	__u8   sub_opcode;
	__le16 acl_handle;
	__le16 l2cap_dcid;
	__le16 l2cap_mtu;
	__u8   data[];
} __packed;

struct msft_rp_avdtp_open {
	__u8   status;
	__u8   sub_opcode;
	__le16 avdtp_handle;
	__u8   audio_if_param_count;
} __packed;

#define MSFT_OP_AVDTP_START			0x09
#define MSFT_OP_AVDTP_SUSPEND			0x0a
#define MSFT_OP_AVDTP_CLOSE			0x0b
struct msft_cp_avdtp_handle {
	__u8   sub_opcode;
	__le16 avdtp_handle;
} __packed;

struct msft_rp_avdtp_handle {
	__u8 status;
	__u8 sub_opcode;
} __packed;

#define MSFT_EV_LE_MONITOR_DEVICE	0x02
struct msft_ev_le_monitor_device {
	__u8     addr_type;
	bdaddr_t bdaddr;
	__u8     monitor_handle;
	__u8     monitor_state;
} __packed;

struct msft_monitor_advertisement_handle_data {
	__u8  msft_handle;
	__u16 mgmt_handle;
	__s8 rssi_high;
	__s8 rssi_low;
	__u8 rssi_low_interval;
	__u8 rssi_sampling_period;
	__u8 cond_type;
	struct list_head list;
};

enum monitor_addr_filter_state {
	AF_STATE_IDLE,
	AF_STATE_ADDING,
	AF_STATE_ADDED,
	AF_STATE_REMOVING,
};

#define MSFT_MONITOR_ADVERTISEMENT_TYPE_ADDR	0x04
struct msft_monitor_addr_filter_data {
	__u8     msft_handle;
	__u8     pattern_handle; /* address filters pertain to */
	__u16    mgmt_handle;
	int      state;
	__s8     rssi_high;
	__s8     rssi_low;
	__u8     rssi_low_interval;
	__u8     rssi_sampling_period;
	__u8     addr_type;
	bdaddr_t bdaddr;
	struct list_head list;
};

struct msft_data {
	__u64 features;
	__u8  evt_prefix_len;
	__u8  *evt_prefix;
	struct list_head handle_map;
	struct list_head address_filters;
	__u8 resuming;
	__u8 suspending;
	__u8 filter_enabled;
	/* To synchronize add/remove address filter and monitor device event.*/
	struct mutex filter_lock;
};

bool msft_monitor_supported(struct hci_dev *hdev)
{
	return !!(msft_get_features(hdev) & MSFT_FEATURE_MASK_LE_ADV_MONITOR);
}

bool msft_avdtp_supported(struct hci_dev *hdev)
{
	return !!(msft_get_features(hdev) & MSFT_FEATURE_MASK_AVDTP);
}

/* Validate an AVDTP Media Codec Service Capability TLV element */
static bool msft_avdtp_valid_codec_cap(const u8 *pos, size_t rem,
				       size_t *elem_len, u8 *codec_id)
{
	size_t len;
	u8 losc;
	u8 id;

	if (rem < AVDTP_CAP_HDR_LEN + AVDTP_MEDIA_CODEC_HDR_LEN)
		return false;

	if (pos[0] != AVDTP_SVC_CAT_MEDIA_CODEC)
		return false;

	losc = pos[1];
	if (losc < AVDTP_MEDIA_CODEC_HDR_LEN)
		return false;

	len = AVDTP_CAP_HDR_LEN + (size_t)losc;
	if (len > rem)
		return false;

	if (pos[2] != AVDTP_MEDIA_TYPE_AUDIO)
		return false;

	id = pos[3];
	switch (id) {
	case A2DP_CODEC_SBC:
		if (losc != AVDTP_SBC_LOSC)
			return false;
		break;
	case A2DP_CODEC_MPEG24:
		if (losc != AVDTP_AAC_LOSC)
			return false;
		break;
	default:
		return false;
	}

	*elem_len = len;
	*codec_id = id;
	return true;
}

/* This function requires the caller holds hci_req_sync_lock */
static void msft_read_avdtp_capabilities(struct hci_dev *hdev)
{
	struct msft_cp_avdtp_capabilities_conf cp = {
		.sub_opcode = MSFT_OP_AVDTP_CAPABILITIES_CONF,
		.ext_codec_count = 0,
		.audio_if_param_count = 0,
	};
	struct msft_rp_avdtp_capabilities_conf *rp;
	struct sk_buff *skb;
	const u8 *pos;
	size_t rem;
	u8 i;

	skb = __hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(cp), &cp,
			     HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Failed to read MSFT AVDTP capabilities (%ld)",
			   PTR_ERR(skb));
		return;
	}

	if (skb->len < sizeof(*rp)) {
		bt_dev_err(hdev, "MSFT AVDTP capabilities length mismatch");
		goto done;
	}

	rp = (void *)skb->data;
	if (rp->status || rp->sub_opcode != MSFT_OP_AVDTP_CAPABILITIES_CONF)
		goto done;

	pos = rp->data;
	rem = skb->len - sizeof(*rp);

	/* Iterate through each internal AVDTP codec capability block returned by the controller */
	for (i = 0;
	     i < rp->int_codec_count &&
	     rem >= AVDTP_CAP_HDR_LEN + AVDTP_MEDIA_CODEC_HDR_LEN;
	     i++) {
		struct codec_list *c, *entry;
		size_t elem_len;
		u8 codec_id;
		bool exists = false;

		/* Validate AVDTP Service Category, LOSC bounds, media type, and codec ID */
		if (!msft_avdtp_valid_codec_cap(pos, rem, &elem_len, &codec_id))
			break;

		hci_dev_lock(hdev);
		list_for_each_entry(c, &hdev->local_codecs, list) {
			if (c->transport == HCI_TRANSPORT_ACL &&
			    c->id == codec_id) {
				exists = true;
				break;
			}
		}

		/* Register the AVDTP offload codec if not already present in local_codecs */
		if (!exists) {
			entry = kzalloc(sizeof(*entry) +
					sizeof(struct hci_codec_caps) +
					elem_len, GFP_KERNEL);
			if (entry) {
				entry->id = codec_id;
				entry->transport = HCI_TRANSPORT_ACL;
				entry->num_caps = 1;
				entry->len = sizeof(struct hci_codec_caps) +
					     elem_len;
				entry->caps[0].len = elem_len;
				memcpy(entry->caps[0].data, pos, elem_len);
				list_add(&entry->list, &hdev->local_codecs);
				bt_dev_info(hdev,
					    "MSFT AVDTP offload codec 0x%02x registered (%zu bytes)",
					    codec_id, elem_len);
			}
		}
		hci_dev_unlock(hdev);

		pos += elem_len;
		rem -= elem_len;
	}

done:
	kfree_skb(skb);
}

int msft_avdtp_close(struct hci_dev *hdev, __u16 avdtp_handle)
{
	struct msft_cp_avdtp_handle cp;
	struct sk_buff *skb;
	int err = 0;

	if (!avdtp_handle || !msft_avdtp_supported(hdev))
		return 0;

	cp.sub_opcode = MSFT_OP_AVDTP_SUSPEND;
	cp.avdtp_handle = cpu_to_le16(avdtp_handle);
	skb = hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(cp), &cp,
			   HCI_CMD_TIMEOUT);
	if (!IS_ERR(skb))
		kfree_skb(skb);

	cp.sub_opcode = MSFT_OP_AVDTP_CLOSE;
	cp.avdtp_handle = cpu_to_le16(avdtp_handle);
	skb = hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(cp), &cp,
			   HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "MSFT AVDTP close failed (%d)", err);
		return err;
	}

	kfree_skb(skb);
	bt_dev_info(hdev, "MSFT AVDTP offload closed: handle 0x%04x",
		    avdtp_handle);
	return 0;
}

int msft_avdtp_open(struct hci_dev *hdev, struct l2cap_chan *chan,
		    struct bt_codec *codec, size_t caps_len)
{
	static const u8 default_sbc_cap[AVDTP_SBC_CAP_LEN] = {
		AVDTP_SVC_CAT_MEDIA_CODEC, AVDTP_SBC_LOSC,
		AVDTP_MEDIA_TYPE_AUDIO, A2DP_CODEC_SBC,
		0x11, 0x15, 0x02, 0x33
	};
	static const u8 default_aac_cap[AVDTP_AAC_CAP_LEN] = {
		AVDTP_SVC_CAT_MEDIA_CODEC, AVDTP_AAC_LOSC,
		AVDTP_MEDIA_TYPE_AUDIO, A2DP_CODEC_MPEG24,
		0x80, 0x00, 0x84, 0x82, 0xee, 0x00
	};
	struct msft_cp_avdtp_open *open_cp;
	struct msft_rp_avdtp_open *open_rp;
	struct msft_cp_avdtp_handle start_cp;
	struct msft_rp_avdtp_handle *start_rp;
	u8 custom_cap[AVDTP_AAC_CAP_LEN];
	const u8 *cap_ptr = NULL;
	size_t cap_size = 0;
	size_t open_len;
	struct sk_buff *skb;
	u16 acl_handle = 0;
	u16 avdtp_handle;
	u16 dcid, mtu;
	bool is_sbc, is_aac;
	bool found_conn = false;
	int err;

	/* Verify the controller supports MSFT AVDTP offload */
	if (!msft_avdtp_supported(hdev))
		return -EOPNOTSUPP;

	/* Close any active offload stream before reconfiguring or disabling */
	if (chan->avdtp_handle) {
		msft_avdtp_close(hdev, chan->avdtp_handle);
		chan->avdtp_handle = 0;
	}

	if (!codec->data_path)
		return 0;

	/* Match standard A2DP codec IDs or Intel vendor-specific ACL codec IDs */
	is_sbc = (codec->id == A2DP_CODEC_SBC) ||
		 (codec->id == A2DP_CODEC_VENDOR &&
		  codec->cid == BT_COMPID_INTEL &&
		  codec->vid == BTINTEL_A2DP_VID_SBC);
	is_aac = (codec->id == A2DP_CODEC_MPEG24) ||
		 (codec->id == A2DP_CODEC_VENDOR &&
		  codec->cid == BT_COMPID_INTEL &&
		  codec->vid == BTINTEL_A2DP_VID_AAC);

	if (!is_sbc && !is_aac) {
		bt_dev_err(hdev,
			   "Unsupported MSFT AVDTP codec id 0x%02x (cid 0x%04x vid 0x%04x)",
			   codec->id, codec->cid, codec->vid);
		return -EINVAL;
	}

	/* Check if user-space supplied codec-specific capability parameters */
	if (codec->num_caps > 0) {
		const struct bt_codec_caps *caps;
		size_t parsed_len;
		u8 parsed_id;

		if (caps_len < sizeof(*caps))
			return -EINVAL;

		caps = (const void *)&codec[1];
		if (caps_len < sizeof(*caps) + caps->len)
			return -EINVAL;

		/* Use a full AVDTP Media Codec Service Capability TLV directly if valid */
		if (msft_avdtp_valid_codec_cap(caps->data, caps->len,
					       &parsed_len, &parsed_id) &&
		    parsed_len == caps->len) {
			if ((is_sbc && parsed_id != A2DP_CODEC_SBC) ||
			    (is_aac && parsed_id != A2DP_CODEC_MPEG24))
				return -EINVAL;
			cap_ptr = caps->data;
			cap_size = caps->len;
		/* Wrap a raw 4-byte SBC CIE into an 8-byte AVDTP Service Capability TLV */
		} else if (is_sbc && caps->len == AVDTP_SBC_CIE_LEN) {
			custom_cap[0] = AVDTP_SVC_CAT_MEDIA_CODEC;
			custom_cap[1] = AVDTP_SBC_LOSC;
			custom_cap[2] = AVDTP_MEDIA_TYPE_AUDIO;
			custom_cap[3] = A2DP_CODEC_SBC;
			memcpy(&custom_cap[4], caps->data, AVDTP_SBC_CIE_LEN);
			cap_ptr = custom_cap;
			cap_size = AVDTP_SBC_CAP_LEN;
		/* Wrap a raw 6-byte AAC CIE into a 10-byte AVDTP Service Capability TLV */
		} else if (is_aac && caps->len == AVDTP_AAC_CIE_LEN) {
			u32 br;

			custom_cap[0] = AVDTP_SVC_CAT_MEDIA_CODEC;
			custom_cap[1] = AVDTP_AAC_LOSC;
			custom_cap[2] = AVDTP_MEDIA_TYPE_AUDIO;
			custom_cap[3] = A2DP_CODEC_MPEG24;
			memcpy(&custom_cap[4], caps->data, AVDTP_AAC_CIE_LEN);
			br = ((u32)(custom_cap[7] &
				    AVDTP_AAC_BITRATE_HI_MASK) << 16) |
			     ((u32)custom_cap[8] << 8) | custom_cap[9];
			/* Reject nonzero bitrates below 128 kbps; cap 0 or >192 kbps */
			if (br && br < AVDTP_AAC_MIN_BITRATE)
				return -EINVAL;
			if (!br || br > AVDTP_AAC_MAX_BITRATE) {
				custom_cap[7] = (custom_cap[7] &
						 AVDTP_AAC_VBR_BIT) |
						((AVDTP_AAC_MAX_BITRATE >> 16) &
						 AVDTP_AAC_BITRATE_HI_MASK);
				custom_cap[8] = (AVDTP_AAC_MAX_BITRATE >> 8) &
						0xff;
				custom_cap[9] = AVDTP_AAC_MAX_BITRATE & 0xff;
			}
			cap_ptr = custom_cap;
			cap_size = AVDTP_AAC_CAP_LEN;
		} else {
			return -EINVAL;
		}
	} else {
		/* Use default SBC or AAC capability when no capability was supplied */
		if (is_sbc) {
			cap_ptr = default_sbc_cap;
			cap_size = sizeof(default_sbc_cap);
		} else {
			cap_ptr = default_aac_cap;
			cap_size = sizeof(default_aac_cap);
		}
	}

	/* Require a connected BR/EDR ACL L2CAP channel with an assigned DCID */
	l2cap_chan_lock(chan);
	if (chan->state == BT_CONNECTED && chan->conn && chan->conn->hcon &&
	    chan->conn->hcon->type == ACL_LINK &&
	    !HCI_CONN_HANDLE_UNSET(chan->conn->hcon->handle) && chan->dcid) {
		acl_handle = chan->conn->hcon->handle;
		dcid = chan->dcid;
		mtu = chan->omtu ? chan->omtu : L2CAP_DEFAULT_MTU;
		found_conn = true;
	}
	l2cap_chan_unlock(chan);

	if (!found_conn) {
		bt_dev_err(hdev,
			   "MSFT AVDTP offload requires a connected BR/EDR ACL L2CAP channel");
		return -ENOTCONN;
	}

	open_len = sizeof(*open_cp) + cap_size + 1;
	open_cp = kzalloc(open_len, GFP_KERNEL);
	if (!open_cp)
		return -ENOMEM;

	open_cp->sub_opcode = MSFT_OP_AVDTP_OPEN;
	open_cp->acl_handle = cpu_to_le16(acl_handle);
	open_cp->l2cap_dcid = cpu_to_le16(dcid);
	open_cp->l2cap_mtu = cpu_to_le16(mtu);
	memcpy(open_cp->data, cap_ptr, cap_size);
	open_cp->data[cap_size] = 0x00; /* Audio_interface_parameter_count */

	/* Send MSFT AVDTP Open command to allocate an offloaded stream handle on the controller */
	skb = hci_cmd_sync(hdev, hdev->msft_opcode, open_len, open_cp,
			   HCI_CMD_TIMEOUT);
	kfree(open_cp);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "MSFT AVDTP Open failed (%d)", err);
		return err;
	}

	if (skb->len < sizeof(*open_rp)) {
		kfree_skb(skb);
		return -EIO;
	}

	open_rp = (void *)skb->data;
	if (open_rp->status || open_rp->sub_opcode != MSFT_OP_AVDTP_OPEN) {
		err = -bt_to_errno(open_rp->status);
		kfree_skb(skb);
		return err ? err : -EIO;
	}

	avdtp_handle = __le16_to_cpu(open_rp->avdtp_handle);
	kfree_skb(skb);

	start_cp.sub_opcode = MSFT_OP_AVDTP_START;
	start_cp.avdtp_handle = cpu_to_le16(avdtp_handle);

	/* Send MSFT AVDTP Start command to start SSP TDM audio and hardware encoding */
	skb = hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(start_cp), &start_cp,
			   HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		bt_dev_err(hdev, "MSFT AVDTP Start failed (%d)", err);
		msft_avdtp_close(hdev, avdtp_handle);
		return err;
	}

	if (skb->len < sizeof(*start_rp)) {
		kfree_skb(skb);
		msft_avdtp_close(hdev, avdtp_handle);
		return -EIO;
	}

	start_rp = (void *)skb->data;
	if (start_rp->status || start_rp->sub_opcode != MSFT_OP_AVDTP_START) {
		err = -bt_to_errno(start_rp->status);
		kfree_skb(skb);
		msft_avdtp_close(hdev, avdtp_handle);
		return err ? err : -EIO;
	}

	kfree_skb(skb);
	chan->avdtp_handle = avdtp_handle;

	bt_dev_info(hdev,
		    "MSFT AVDTP offload started: acl 0x%04x dcid 0x%04x mtu %u codec 0x%02x handle 0x%04x",
		    acl_handle, dcid, mtu, codec->id, avdtp_handle);
	return 0;
}

static bool read_supported_features(struct hci_dev *hdev,
				    struct msft_data *msft)
{
	struct msft_cp_read_supported_features cp;
	struct msft_rp_read_supported_features *rp;
	struct sk_buff *skb;

	cp.sub_opcode = MSFT_OP_READ_SUPPORTED_FEATURES;

	skb = __hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(cp), &cp,
			     HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Failed to read MSFT supported features (%ld)",
			   PTR_ERR(skb));
		return false;
	}

	if (skb->len < sizeof(*rp)) {
		bt_dev_err(hdev, "MSFT supported features length mismatch");
		goto failed;
	}

	rp = (struct msft_rp_read_supported_features *)skb->data;

	if (rp->sub_opcode != MSFT_OP_READ_SUPPORTED_FEATURES)
		goto failed;

	if (skb->len < sizeof(*rp) + rp->evt_prefix_len) {
		bt_dev_err(hdev, "MSFT event prefix length mismatch");
		goto failed;
	}

	if (rp->evt_prefix_len > 0) {
		msft->evt_prefix = kmemdup(rp->evt_prefix, rp->evt_prefix_len,
					   GFP_KERNEL);
		if (!msft->evt_prefix)
			goto failed;
	}

	msft->evt_prefix_len = rp->evt_prefix_len;
	msft->features = __le64_to_cpu(rp->features);

	if (msft->features & MSFT_FEATURE_MASK_CURVE_VALIDITY)
		hdev->msft_curve_validity = true;

	kfree_skb(skb);
	return true;

failed:
	kfree_skb(skb);
	return false;
}

/* is_mgmt = true matches the handle exposed to userspace via mgmt.
 * is_mgmt = false matches the handle used by the msft controller.
 * This function requires the caller holds hdev->lock
 */
static struct msft_monitor_advertisement_handle_data *msft_find_handle_data
				(struct hci_dev *hdev, u16 handle, bool is_mgmt)
{
	struct msft_monitor_advertisement_handle_data *entry;
	struct msft_data *msft = hdev->msft_data;

	list_for_each_entry(entry, &msft->handle_map, list) {
		if (is_mgmt && entry->mgmt_handle == handle)
			return entry;
		if (!is_mgmt && entry->msft_handle == handle)
			return entry;
	}

	return NULL;
}

/* This function requires the caller holds msft->filter_lock */
static struct msft_monitor_addr_filter_data *msft_find_address_data
			(struct hci_dev *hdev, u8 addr_type, bdaddr_t *addr,
			 u8 pattern_handle)
{
	struct msft_monitor_addr_filter_data *entry;
	struct msft_data *msft = hdev->msft_data;

	list_for_each_entry(entry, &msft->address_filters, list) {
		if (entry->pattern_handle == pattern_handle &&
		    addr_type == entry->addr_type &&
		    !bacmp(addr, &entry->bdaddr))
			return entry;
	}

	return NULL;
}

/* This function requires the caller holds hdev->lock */
static int msft_monitor_device_del(struct hci_dev *hdev, __u16 mgmt_handle,
				   bdaddr_t *bdaddr, __u8 addr_type,
				   bool notify)
{
	struct monitored_device *dev, *tmp;
	int count = 0;

	list_for_each_entry_safe(dev, tmp, &hdev->monitored_devices, list) {
		/* mgmt_handle == 0 indicates remove all devices, whereas,
		 * bdaddr == NULL indicates remove all devices matching the
		 * mgmt_handle.
		 */
		if ((!mgmt_handle || dev->handle == mgmt_handle) &&
		    (!bdaddr || (!bacmp(bdaddr, &dev->bdaddr) &&
				 addr_type == dev->addr_type))) {
			if (notify && dev->notified) {
				mgmt_adv_monitor_device_lost(hdev, dev->handle,
							     &dev->bdaddr,
							     dev->addr_type);
			}

			list_del(&dev->list);
			kfree(dev);
			count++;
		}
	}

	return count;
}

static int msft_le_monitor_advertisement_cb(struct hci_dev *hdev, u16 opcode,
					    struct adv_monitor *monitor,
					    struct sk_buff *skb)
{
	struct msft_rp_le_monitor_advertisement *rp;
	struct msft_monitor_advertisement_handle_data *handle_data;
	struct msft_data *msft = hdev->msft_data;
	int status = 0;

	hci_dev_lock(hdev);

	rp = (struct msft_rp_le_monitor_advertisement *)skb->data;
	if (skb->len < sizeof(*rp)) {
		status = HCI_ERROR_UNSPECIFIED;
		goto unlock;
	}

	status = rp->status;
	if (status)
		goto unlock;

	handle_data = kmalloc_obj(*handle_data);
	if (!handle_data) {
		status = HCI_ERROR_UNSPECIFIED;
		goto unlock;
	}

	handle_data->mgmt_handle = monitor->handle;
	handle_data->msft_handle = rp->handle;
	handle_data->cond_type   = MSFT_MONITOR_ADVERTISEMENT_TYPE_PATTERN;
	INIT_LIST_HEAD(&handle_data->list);
	list_add(&handle_data->list, &msft->handle_map);

	monitor->state = ADV_MONITOR_STATE_OFFLOADED;

unlock:
	if (status && msft->resuming)
		hci_free_adv_monitor(hdev, monitor);

	hci_dev_unlock(hdev);

	return status;
}

/* This function requires the caller holds hci_req_sync_lock */
static void msft_remove_addr_filters_sync(struct hci_dev *hdev, u8 handle)
{
	struct msft_monitor_addr_filter_data *address_filter, *n;
	struct msft_cp_le_cancel_monitor_advertisement cp;
	struct msft_data *msft = hdev->msft_data;
	struct list_head head;
	struct sk_buff *skb;

	INIT_LIST_HEAD(&head);

	/* Cancel all corresponding address monitors */
	mutex_lock(&msft->filter_lock);

	list_for_each_entry_safe(address_filter, n, &msft->address_filters,
				 list) {
		if (address_filter->pattern_handle != handle)
			continue;

		list_del(&address_filter->list);

		/* Keep the address filter and let
		 * msft_add_address_filter_sync() remove and free the address
		 * filter.
		 */
		if (address_filter->state == AF_STATE_ADDING) {
			address_filter->state = AF_STATE_REMOVING;
			continue;
		}

		/* Keep the address filter and let
		 * msft_cancel_address_filter_sync() remove and free the address
		 * filter
		 */
		if (address_filter->state == AF_STATE_REMOVING)
			continue;

		list_add_tail(&address_filter->list, &head);
	}

	mutex_unlock(&msft->filter_lock);

	list_for_each_entry_safe(address_filter, n, &head, list) {
		list_del(&address_filter->list);

		cp.sub_opcode = MSFT_OP_LE_CANCEL_MONITOR_ADVERTISEMENT;
		cp.handle = address_filter->msft_handle;

		skb = __hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(cp), &cp,
				     HCI_CMD_TIMEOUT);
		if (IS_ERR(skb)) {
			kfree(address_filter);
			continue;
		}

		kfree_skb(skb);

		bt_dev_dbg(hdev, "MSFT: Canceled device %pMR address filter",
			   &address_filter->bdaddr);

		kfree(address_filter);
	}
}

static int msft_le_cancel_monitor_advertisement_cb(struct hci_dev *hdev,
						   u16 opcode,
						   struct adv_monitor *monitor,
						   struct sk_buff *skb)
{
	struct msft_rp_le_cancel_monitor_advertisement *rp;
	struct msft_monitor_advertisement_handle_data *handle_data;
	struct msft_data *msft = hdev->msft_data;
	int status = 0;
	u8 msft_handle;

	rp = (struct msft_rp_le_cancel_monitor_advertisement *)skb->data;
	if (skb->len < sizeof(*rp)) {
		status = HCI_ERROR_UNSPECIFIED;
		goto done;
	}

	status = rp->status;
	if (status)
		goto done;

	hci_dev_lock(hdev);

	handle_data = msft_find_handle_data(hdev, monitor->handle, true);

	if (handle_data) {
		if (monitor->state == ADV_MONITOR_STATE_OFFLOADED)
			monitor->state = ADV_MONITOR_STATE_REGISTERED;

		/* Do not free the monitor if it is being removed due to
		 * suspend. It will be re-monitored on resume.
		 */
		if (!msft->suspending) {
			hci_free_adv_monitor(hdev, monitor);

			/* Clear any monitored devices by this Adv Monitor */
			msft_monitor_device_del(hdev, handle_data->mgmt_handle,
						NULL, 0, false);
		}

		msft_handle = handle_data->msft_handle;

		list_del(&handle_data->list);
		kfree(handle_data);

		hci_dev_unlock(hdev);

		msft_remove_addr_filters_sync(hdev, msft_handle);
	} else {
		hci_dev_unlock(hdev);
	}

done:
	return status;
}

/* This function requires the caller holds hci_req_sync_lock */
static int msft_remove_monitor_sync(struct hci_dev *hdev,
				    struct adv_monitor *monitor)
{
	struct msft_cp_le_cancel_monitor_advertisement cp;
	struct msft_monitor_advertisement_handle_data *handle_data;
	struct sk_buff *skb;

	handle_data = msft_find_handle_data(hdev, monitor->handle, true);

	/* If no matched handle, just remove without telling controller */
	if (!handle_data)
		return -ENOENT;

	cp.sub_opcode = MSFT_OP_LE_CANCEL_MONITOR_ADVERTISEMENT;
	cp.handle = handle_data->msft_handle;

	skb = __hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(cp), &cp,
			     HCI_CMD_TIMEOUT);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	return msft_le_cancel_monitor_advertisement_cb(hdev, hdev->msft_opcode,
						       monitor, skb);
}

/* This function requires the caller holds hci_req_sync_lock */
int msft_suspend_sync(struct hci_dev *hdev)
{
	struct msft_data *msft = hdev->msft_data;
	struct adv_monitor *monitor;
	int handle = 0;

	if (!msft || !msft_monitor_supported(hdev))
		return 0;

	msft->suspending = true;

	while (1) {
		monitor = idr_get_next(&hdev->adv_monitors_idr, &handle);
		if (!monitor)
			break;

		msft_remove_monitor_sync(hdev, monitor);

		handle++;
	}

	/* All monitors have been removed */
	msft->suspending = false;

	return 0;
}

static bool msft_monitor_rssi_valid(struct adv_monitor *monitor)
{
	struct adv_rssi_thresholds *r = &monitor->rssi;

	if (r->high_threshold < MSFT_RSSI_THRESHOLD_VALUE_MIN ||
	    r->high_threshold > MSFT_RSSI_THRESHOLD_VALUE_MAX ||
	    r->low_threshold < MSFT_RSSI_THRESHOLD_VALUE_MIN ||
	    r->low_threshold > MSFT_RSSI_THRESHOLD_VALUE_MAX)
		return false;

	/* High_threshold_timeout is not supported,
	 * once high_threshold is reached, events are immediately reported.
	 */
	if (r->high_threshold_timeout != 0)
		return false;

	if (r->low_threshold_timeout > MSFT_RSSI_LOW_TIMEOUT_MAX)
		return false;

	/* Sampling period from 0x00 to 0xFF are all allowed */
	return true;
}

static bool msft_monitor_pattern_valid(struct adv_monitor *monitor)
{
	return msft_monitor_rssi_valid(monitor);
	/* No additional check needed for pattern-based monitor */
}

static int msft_add_monitor_sync(struct hci_dev *hdev,
				 struct adv_monitor *monitor)
{
	struct msft_cp_le_monitor_advertisement *cp;
	struct msft_le_monitor_advertisement_pattern_data *pattern_data;
	struct msft_monitor_advertisement_handle_data *handle_data;
	struct msft_le_monitor_advertisement_pattern *pattern;
	struct adv_pattern *entry;
	size_t total_size = sizeof(*cp) + sizeof(*pattern_data);
	ptrdiff_t offset = 0;
	u8 pattern_count = 0;
	struct sk_buff *skb;
	int err;

	if (!msft_monitor_pattern_valid(monitor))
		return -EINVAL;

	list_for_each_entry(entry, &monitor->patterns, list) {
		pattern_count++;
		total_size += sizeof(*pattern) + entry->length;
	}

	cp = kmalloc(total_size, GFP_KERNEL);
	if (!cp)
		return -ENOMEM;

	cp->sub_opcode = MSFT_OP_LE_MONITOR_ADVERTISEMENT;
	cp->rssi_high = monitor->rssi.high_threshold;
	cp->rssi_low = monitor->rssi.low_threshold;
	cp->rssi_low_interval = (u8)monitor->rssi.low_threshold_timeout;
	cp->rssi_sampling_period = monitor->rssi.sampling_period;

	cp->cond_type = MSFT_MONITOR_ADVERTISEMENT_TYPE_PATTERN;

	pattern_data = (void *)cp->data;
	pattern_data->count = pattern_count;

	list_for_each_entry(entry, &monitor->patterns, list) {
		pattern = (void *)(pattern_data->data + offset);
		/* the length also includes data_type and offset */
		pattern->length = entry->length + 2;
		pattern->data_type = entry->ad_type;
		pattern->start_byte = entry->offset;
		memcpy(pattern->pattern, entry->value, entry->length);
		offset += sizeof(*pattern) + entry->length;
	}

	skb = __hci_cmd_sync(hdev, hdev->msft_opcode, total_size, cp,
			     HCI_CMD_TIMEOUT);

	if (IS_ERR(skb)) {
		err = PTR_ERR(skb);
		goto out_free;
	}

	err = msft_le_monitor_advertisement_cb(hdev, hdev->msft_opcode,
					       monitor, skb);
	if (err)
		goto out_free;

	handle_data = msft_find_handle_data(hdev, monitor->handle, true);
	if (!handle_data) {
		err = -ENODATA;
		goto out_free;
	}

	handle_data->rssi_high	= cp->rssi_high;
	handle_data->rssi_low	= cp->rssi_low;
	handle_data->rssi_low_interval	  = cp->rssi_low_interval;
	handle_data->rssi_sampling_period = cp->rssi_sampling_period;

out_free:
	kfree(cp);
	return err;
}

/* This function requires the caller holds hci_req_sync_lock */
static void reregister_monitor(struct hci_dev *hdev)
{
	struct adv_monitor *monitor;
	struct msft_data *msft = hdev->msft_data;
	int handle = 0;

	if (!msft)
		return;

	msft->resuming = true;

	while (1) {
		monitor = idr_get_next(&hdev->adv_monitors_idr, &handle);
		if (!monitor)
			break;

		msft_add_monitor_sync(hdev, monitor);

		handle++;
	}

	/* All monitors have been reregistered */
	msft->resuming = false;
}

/* This function requires the caller holds hci_req_sync_lock */
int msft_resume_sync(struct hci_dev *hdev)
{
	struct msft_data *msft = hdev->msft_data;

	if (!msft || !msft_monitor_supported(hdev))
		return 0;

	hci_dev_lock(hdev);

	/* Clear already tracked devices on resume. Once the monitors are
	 * reregistered, devices in range will be found again after resume.
	 */
	hdev->advmon_pend_notify = false;
	msft_monitor_device_del(hdev, 0, NULL, 0, true);

	hci_dev_unlock(hdev);

	reregister_monitor(hdev);

	return 0;
}

/* This function requires the caller holds hci_req_sync_lock */
void msft_do_open(struct hci_dev *hdev)
{
	struct msft_data *msft = hdev->msft_data;

	if (hdev->msft_opcode == HCI_OP_NOP)
		return;

	if (!msft) {
		bt_dev_err(hdev, "MSFT extension not registered");
		return;
	}

	bt_dev_dbg(hdev, "Initialize MSFT extension");

	/* Reset existing MSFT data before re-reading */
	kfree(msft->evt_prefix);
	msft->evt_prefix = NULL;
	msft->evt_prefix_len = 0;
	msft->features = 0;

	if (!read_supported_features(hdev, msft)) {
		hci_dev_lock(hdev);
		hdev->msft_data = NULL;
		kfree(msft);
		hci_dev_unlock(hdev);
		return;
	}

	if (msft_monitor_supported(hdev)) {
		msft->resuming = true;
		msft_set_filter_enable(hdev, true);
		/* Monitors get removed on power off, so we need to explicitly
		 * tell the controller to re-monitor.
		 */
		reregister_monitor(hdev);
	}

	if (msft_avdtp_supported(hdev))
		msft_read_avdtp_capabilities(hdev);
}

void msft_do_close(struct hci_dev *hdev)
{
	struct msft_data *msft = hdev->msft_data;
	struct msft_monitor_advertisement_handle_data *handle_data, *tmp;
	struct msft_monitor_addr_filter_data *address_filter, *n;
	struct adv_monitor *monitor;

	if (!msft)
		return;

	bt_dev_dbg(hdev, "Cleanup of MSFT extension");

	hci_dev_lock(hdev);

	/* The controller will silently remove all monitors on power off.
	 * Therefore, remove handle_data mapping and reset monitor state.
	 */
	list_for_each_entry_safe(handle_data, tmp, &msft->handle_map, list) {
		monitor = idr_find(&hdev->adv_monitors_idr,
				   handle_data->mgmt_handle);

		if (monitor && monitor->state == ADV_MONITOR_STATE_OFFLOADED)
			monitor->state = ADV_MONITOR_STATE_REGISTERED;

		list_del(&handle_data->list);
		kfree(handle_data);
	}

	hci_dev_unlock(hdev);

	mutex_lock(&msft->filter_lock);
	list_for_each_entry_safe(address_filter, n, &msft->address_filters,
				 list) {
		list_del(&address_filter->list);
		kfree(address_filter);
	}
	mutex_unlock(&msft->filter_lock);

	hci_dev_lock(hdev);

	/* Clear any devices that are being monitored and notify device lost */
	hdev->advmon_pend_notify = false;
	msft_monitor_device_del(hdev, 0, NULL, 0, true);

	hci_dev_unlock(hdev);
}

static int msft_cancel_address_filter_sync(struct hci_dev *hdev, void *data)
{
	struct msft_monitor_addr_filter_data *address_filter = data;
	struct msft_cp_le_cancel_monitor_advertisement cp;
	struct msft_data *msft = hdev->msft_data;
	struct sk_buff *skb;
	int err = 0;

	if (!msft) {
		bt_dev_err(hdev, "MSFT: msft data is freed");
		return -EINVAL;
	}

	/* The address filter has been removed by hci dev close */
	if (!test_bit(HCI_UP, &hdev->flags))
		return 0;

	mutex_lock(&msft->filter_lock);
	list_del(&address_filter->list);
	mutex_unlock(&msft->filter_lock);

	cp.sub_opcode = MSFT_OP_LE_CANCEL_MONITOR_ADVERTISEMENT;
	cp.handle = address_filter->msft_handle;

	skb = __hci_cmd_sync(hdev, hdev->msft_opcode, sizeof(cp), &cp,
			     HCI_CMD_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "MSFT: Failed to cancel address (%pMR) filter",
			   &address_filter->bdaddr);
		err = PTR_ERR(skb);
		goto done;
	}
	kfree_skb(skb);

	bt_dev_dbg(hdev, "MSFT: Canceled device %pMR address filter",
		   &address_filter->bdaddr);

done:
	kfree(address_filter);

	return err;
}

void msft_register(struct hci_dev *hdev)
{
	struct msft_data *msft = NULL;

	bt_dev_dbg(hdev, "Register MSFT extension");

	msft = kzalloc_obj(*msft);
	if (!msft) {
		bt_dev_err(hdev, "Failed to register MSFT extension");
		return;
	}

	INIT_LIST_HEAD(&msft->handle_map);
	INIT_LIST_HEAD(&msft->address_filters);
	mutex_init(&msft->filter_lock);
	hdev->msft_data = msft;
}

void msft_release(struct hci_dev *hdev)
{
	struct msft_data *msft = hdev->msft_data;

	if (!msft)
		return;

	bt_dev_dbg(hdev, "Unregister MSFT extension");

	hdev->msft_data = NULL;

	kfree(msft->evt_prefix);
	mutex_destroy(&msft->filter_lock);
	kfree(msft);
}

/* This function requires the caller holds hdev->lock */
static void msft_device_found(struct hci_dev *hdev, bdaddr_t *bdaddr,
			      __u8 addr_type, __u16 mgmt_handle)
{
	struct monitored_device *dev;

	dev = kmalloc_obj(*dev);
	if (!dev) {
		bt_dev_err(hdev, "MSFT vendor event %u: no memory",
			   MSFT_EV_LE_MONITOR_DEVICE);
		return;
	}

	bacpy(&dev->bdaddr, bdaddr);
	dev->addr_type = addr_type;
	dev->handle = mgmt_handle;
	dev->notified = false;

	INIT_LIST_HEAD(&dev->list);
	list_add(&dev->list, &hdev->monitored_devices);
	hdev->advmon_pend_notify = true;
}

/* This function requires the caller holds hdev->lock */
static void msft_device_lost(struct hci_dev *hdev, bdaddr_t *bdaddr,
			     __u8 addr_type, __u16 mgmt_handle)
{
	if (!msft_monitor_device_del(hdev, mgmt_handle, bdaddr, addr_type,
				     true)) {
		bt_dev_err(hdev, "MSFT vendor event %u: dev %pMR not in list",
			   MSFT_EV_LE_MONITOR_DEVICE, bdaddr);
	}
}

static void *msft_skb_pull(struct hci_dev *hdev, struct sk_buff *skb,
			   u8 ev, size_t len)
{
	void *data;

	data = skb_pull_data(skb, len);
	if (!data)
		bt_dev_err(hdev, "Malformed MSFT vendor event: 0x%02x", ev);

	return data;
}

static int msft_add_address_filter_sync(struct hci_dev *hdev, void *data)
{
	struct msft_monitor_addr_filter_data *address_filter = data;
	struct msft_rp_le_monitor_advertisement *rp;
	struct msft_cp_le_monitor_advertisement *cp;
	struct msft_data *msft = hdev->msft_data;
	struct sk_buff *skb = NULL;
	bool remove = false;
	size_t size;

	if (!msft) {
		bt_dev_err(hdev, "MSFT: msft data is freed");
		return -EINVAL;
	}

	/* The address filter has been removed by hci dev close */
	if (!test_bit(HCI_UP, &hdev->flags))
		return -ENODEV;

	/* We are safe to use the address filter from now on.
	 * msft_monitor_device_evt() wouldn't delete this filter because it's
	 * not been added by now.
	 * And all other functions that requiring hci_req_sync_lock wouldn't
	 * touch this filter before this func completes because it's protected
	 * by hci_req_sync_lock.
	 */

	if (address_filter->state == AF_STATE_REMOVING) {
		mutex_lock(&msft->filter_lock);
		list_del(&address_filter->list);
		mutex_unlock(&msft->filter_lock);
		kfree(address_filter);
		return 0;
	}

	size = sizeof(*cp) +
	       sizeof(address_filter->addr_type) +
	       sizeof(address_filter->bdaddr);
	cp = kzalloc(size, GFP_KERNEL);
	if (!cp) {
		bt_dev_err(hdev, "MSFT: Alloc cmd param err");
		remove = true;
		goto done;
	}

	cp->sub_opcode           = MSFT_OP_LE_MONITOR_ADVERTISEMENT;
	cp->rssi_high		 = address_filter->rssi_high;
	cp->rssi_low		 = address_filter->rssi_low;
	cp->rssi_low_interval    = address_filter->rssi_low_interval;
	cp->rssi_sampling_period = address_filter->rssi_sampling_period;
	cp->cond_type            = MSFT_MONITOR_ADVERTISEMENT_TYPE_ADDR;
	cp->data[0]              = address_filter->addr_type;
	memcpy(&cp->data[1], &address_filter->bdaddr,
	       sizeof(address_filter->bdaddr));

	skb = __hci_cmd_sync(hdev, hdev->msft_opcode, size, cp,
			     HCI_CMD_TIMEOUT);
	kfree(cp);

	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "Failed to enable address %pMR filter",
			   &address_filter->bdaddr);
		skb = NULL;
		remove = true;
		goto done;
	}

	rp = skb_pull_data(skb, sizeof(*rp));
	if (!rp || rp->sub_opcode != MSFT_OP_LE_MONITOR_ADVERTISEMENT ||
	    rp->status)
		remove = true;

done:
	mutex_lock(&msft->filter_lock);

	if (remove) {
		bt_dev_warn(hdev, "MSFT: Remove address (%pMR) filter",
			    &address_filter->bdaddr);
		list_del(&address_filter->list);
		kfree(address_filter);
	} else {
		address_filter->state = AF_STATE_ADDED;
		address_filter->msft_handle = rp->handle;
		bt_dev_dbg(hdev, "MSFT: Address %pMR filter enabled",
			   &address_filter->bdaddr);
	}
	mutex_unlock(&msft->filter_lock);

	kfree_skb(skb);

	return 0;
}

/* This function requires the caller holds msft->filter_lock */
static struct msft_monitor_addr_filter_data *msft_add_address_filter
		(struct hci_dev *hdev, u8 addr_type, bdaddr_t *bdaddr,
		 struct msft_monitor_advertisement_handle_data *handle_data)
{
	struct msft_monitor_addr_filter_data *address_filter = NULL;
	struct msft_data *msft = hdev->msft_data;
	int err;

	address_filter = kzalloc_obj(*address_filter);
	if (!address_filter)
		return NULL;

	address_filter->state             = AF_STATE_ADDING;
	address_filter->msft_handle       = 0xff;
	address_filter->pattern_handle    = handle_data->msft_handle;
	address_filter->mgmt_handle       = handle_data->mgmt_handle;
	address_filter->rssi_high         = handle_data->rssi_high;
	address_filter->rssi_low          = handle_data->rssi_low;
	address_filter->rssi_low_interval = handle_data->rssi_low_interval;
	address_filter->rssi_sampling_period = handle_data->rssi_sampling_period;
	address_filter->addr_type            = addr_type;
	bacpy(&address_filter->bdaddr, bdaddr);

	/* With the above AF_STATE_ADDING, duplicated address filter can be
	 * avoided when receiving monitor device event (found/lost) frequently
	 * for the same device.
	 */
	list_add_tail(&address_filter->list, &msft->address_filters);

	err = hci_cmd_sync_queue(hdev, msft_add_address_filter_sync,
				 address_filter, NULL);
	if (err < 0) {
		bt_dev_err(hdev, "MSFT: Add address %pMR filter err", bdaddr);
		list_del(&address_filter->list);
		kfree(address_filter);
		return NULL;
	}

	bt_dev_dbg(hdev, "MSFT: Add device %pMR address filter",
		   &address_filter->bdaddr);

	return address_filter;
}

/* This function requires the caller holds hdev->lock */
static void msft_monitor_device_evt(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct msft_monitor_addr_filter_data *n, *address_filter = NULL;
	struct msft_ev_le_monitor_device *ev;
	struct msft_monitor_advertisement_handle_data *handle_data;
	struct msft_data *msft = hdev->msft_data;
	u16 mgmt_handle = 0xffff;
	u8 addr_type;

	ev = msft_skb_pull(hdev, skb, MSFT_EV_LE_MONITOR_DEVICE, sizeof(*ev));
	if (!ev)
		return;

	bt_dev_dbg(hdev,
		   "MSFT vendor event 0x%02x: handle 0x%04x state %d addr %pMR",
		   MSFT_EV_LE_MONITOR_DEVICE, ev->monitor_handle,
		   ev->monitor_state, &ev->bdaddr);

	handle_data = msft_find_handle_data(hdev, ev->monitor_handle, false);

	if (!hci_test_quirk(hdev, HCI_QUIRK_USE_MSFT_EXT_ADDRESS_FILTER)) {
		if (!handle_data)
			return;
		mgmt_handle = handle_data->mgmt_handle;
		goto report_state;
	}

	if (handle_data) {
		/* Don't report any device found/lost event from pattern
		 * monitors. Pattern monitor always has its address filters for
		 * tracking devices.
		 */

		address_filter = msft_find_address_data(hdev, ev->addr_type,
							&ev->bdaddr,
							handle_data->msft_handle);
		if (address_filter)
			return;

		if (ev->monitor_state && handle_data->cond_type ==
				MSFT_MONITOR_ADVERTISEMENT_TYPE_PATTERN)
			msft_add_address_filter(hdev, ev->addr_type,
						&ev->bdaddr, handle_data);

		return;
	}

	/* This device event is not from pattern monitor.
	 * Report it if there is a corresponding address_filter for it.
	 */
	list_for_each_entry(n, &msft->address_filters, list) {
		if (n->state == AF_STATE_ADDED &&
		    n->msft_handle == ev->monitor_handle) {
			mgmt_handle = n->mgmt_handle;
			address_filter = n;
			break;
		}
	}

	if (!address_filter) {
		bt_dev_warn(hdev, "MSFT: Unexpected device event %pMR, %u, %u",
			    &ev->bdaddr, ev->monitor_handle, ev->monitor_state);
		return;
	}

report_state:
	switch (ev->addr_type) {
	case ADDR_LE_DEV_PUBLIC:
		addr_type = BDADDR_LE_PUBLIC;
		break;

	case ADDR_LE_DEV_RANDOM:
		addr_type = BDADDR_LE_RANDOM;
		break;

	default:
		bt_dev_err(hdev,
			   "MSFT vendor event 0x%02x: unknown addr type 0x%02x",
			   MSFT_EV_LE_MONITOR_DEVICE, ev->addr_type);
		return;
	}

	if (ev->monitor_state) {
		msft_device_found(hdev, &ev->bdaddr, addr_type, mgmt_handle);
	} else {
		if (address_filter && address_filter->state == AF_STATE_ADDED) {
			address_filter->state = AF_STATE_REMOVING;
			hci_cmd_sync_queue(hdev,
					   msft_cancel_address_filter_sync,
					   address_filter,
					   NULL);
		}
		msft_device_lost(hdev, &ev->bdaddr, addr_type, mgmt_handle);
	}
}

void msft_vendor_evt(struct hci_dev *hdev, void *data, struct sk_buff *skb)
{
	struct msft_data *msft = hdev->msft_data;
	u8 *evt_prefix;
	u8 *evt;

	if (!msft)
		return;

	/* When the extension has defined an event prefix, check that it
	 * matches, and otherwise just return.
	 */
	if (msft->evt_prefix_len > 0) {
		evt_prefix = msft_skb_pull(hdev, skb, 0, msft->evt_prefix_len);
		if (!evt_prefix)
			return;

		if (memcmp(evt_prefix, msft->evt_prefix, msft->evt_prefix_len))
			return;
	}

	/* Every event starts at least with an event code and the rest of
	 * the data is variable and depends on the event code.
	 */
	if (skb->len < 1)
		return;

	evt = msft_skb_pull(hdev, skb, 0, sizeof(*evt));
	if (!evt)
		return;

	hci_dev_lock(hdev);

	switch (*evt) {
	case MSFT_EV_LE_MONITOR_DEVICE:
		mutex_lock(&msft->filter_lock);
		msft_monitor_device_evt(hdev, skb);
		mutex_unlock(&msft->filter_lock);
		break;

	default:
		bt_dev_dbg(hdev, "MSFT vendor event 0x%02x", *evt);
		break;
	}

	hci_dev_unlock(hdev);
}

__u64 msft_get_features(struct hci_dev *hdev)
{
	struct msft_data *msft = hdev->msft_data;

	return msft ? msft->features : 0;
}

static void msft_le_set_advertisement_filter_enable_cb(struct hci_dev *hdev,
						       void *user_data,
						       u8 status)
{
	struct msft_cp_le_set_advertisement_filter_enable *cp = user_data;
	struct msft_data *msft = hdev->msft_data;

	/* Error 0x0C would be returned if the filter enabled status is
	 * already set to whatever we were trying to set.
	 * Although the default state should be disabled, some controller set
	 * the initial value to enabled. Because there is no way to know the
	 * actual initial value before sending this command, here we also treat
	 * error 0x0C as success.
	 */
	if (status != 0x00 && status != 0x0C)
		return;

	hci_dev_lock(hdev);

	msft->filter_enabled = cp->enable;

	if (status == 0x0C)
		bt_dev_warn(hdev, "MSFT filter_enable is already %s",
			    cp->enable ? "on" : "off");

	hci_dev_unlock(hdev);
}

/* This function requires the caller holds hci_req_sync_lock */
int msft_add_monitor_pattern(struct hci_dev *hdev, struct adv_monitor *monitor)
{
	struct msft_data *msft = hdev->msft_data;

	if (!msft)
		return -EOPNOTSUPP;

	if (msft->resuming || msft->suspending)
		return -EBUSY;

	return msft_add_monitor_sync(hdev, monitor);
}

/* This function requires the caller holds hci_req_sync_lock */
int msft_remove_monitor(struct hci_dev *hdev, struct adv_monitor *monitor)
{
	struct msft_data *msft = hdev->msft_data;

	if (!msft)
		return -EOPNOTSUPP;

	if (msft->resuming || msft->suspending)
		return -EBUSY;

	return msft_remove_monitor_sync(hdev, monitor);
}

int msft_set_filter_enable(struct hci_dev *hdev, bool enable)
{
	struct msft_cp_le_set_advertisement_filter_enable cp;
	struct msft_data *msft = hdev->msft_data;
	int err;

	if (!msft)
		return -EOPNOTSUPP;

	cp.sub_opcode = MSFT_OP_LE_SET_ADVERTISEMENT_FILTER_ENABLE;
	cp.enable = enable;
	err = __hci_cmd_sync_status(hdev, hdev->msft_opcode, sizeof(cp), &cp,
				    HCI_CMD_TIMEOUT);

	msft_le_set_advertisement_filter_enable_cb(hdev, &cp, err);

	return 0;
}

bool msft_curve_validity(struct hci_dev *hdev)
{
	return hdev->msft_curve_validity;
}
