// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2024 Intel Corporation.
 */

#include "xe_pxp_multi_session.h"

#include <uapi/drm/xe_drm.h>
#include <uapi/drm/xe_drm_prelim.h>

#include "abi/gsc_command_header_abi.h"
#include "xe_assert.h"
#include "xe_device.h"
#include "xe_force_wake.h"
#include "xe_macros.h"
#include "xe_mmio.h"
#include "xe_pm.h"
#include "xe_pxp.h"
#include "xe_pxp_submit.h"
#include "xe_pxp_types.h"
#include "regs/xe_pxp_regs.h"

/**
 * struct xe_pxp_client
 *
 * A single drm_client can use multiple PXP sessions but all share the same
 * execution resources. So, use a linked list to map from a given DRM client to
 * its associated resource set.
 */
struct xe_pxp_client {
	/** @link: anchor for linked list. */
	struct list_head link;
	/** @exec: session execution resource for the given client. */
	struct xe_pxp_gsc_client_resources res;
	/** @drmfile: drm_file handle for the given client. */
	struct drm_file *drmfile;
};

static struct xe_pxp_client *find_client(struct xe_pxp *pxp, struct drm_file *drmfile)
{
	struct xe_pxp_client *client;

	if (!drmfile)
		return NULL;

	lockdep_assert_held(&pxp->multi_session.mutex);

	list_for_each_entry(client, &pxp->multi_session.client_list, link)
		if (client->drmfile == drmfile)
			return client;

	return NULL;
}

static struct xe_pxp_client *xe_pxp_alloc_client_resources(struct xe_pxp *pxp,
							   struct drm_file *drmfile)
{
	struct xe_pxp_client *client;
	int ret;

	xe_assert(pxp->xe, drmfile);

	lockdep_assert_held(&pxp->multi_session.mutex);

	client = find_client(pxp, drmfile);
	if (client)
		return client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return ERR_PTR(-ENOMEM);

	ret = xe_pxp_allocate_client_resources(pxp, &client->res);
	if (ret) {
		kfree(client);
		return ERR_PTR(ret);
	}

	INIT_LIST_HEAD(&client->link);
	client->drmfile = drmfile;
	list_add_tail(&client->link, &pxp->multi_session.client_list);

	return client;
}

static int pxp_terminate_session(struct xe_pxp *pxp,
				 struct xe_pxp_gsc_client_resources *gsc_res, u32 id)
{
	int ret;

	lockdep_assert_held(&pxp->multi_session.mutex);

	/* terminate the hw session */
	ret = xe_pxp_submit_session_termination(pxp, BIT(id));
	if (ret)
		goto out;

	ret = xe_pxp_wait_for_session_state(pxp, id, false);
	if (ret)
		goto out;

	/* now we can tell the GSC to clean up its own state */
	ret = xe_pxp_submit_session_invalidation(gsc_res, id);

out:
	if (ret)
		drm_err(&pxp->xe->drm, "failed to kill PXP session %u, ret=%d\n", id, ret);

	return ret;
}

static u32 __pxp_tag(struct xe_pxp *pxp, int idx, int mode, u8 instance)
{
	u32 pxp_tag = 0;

	switch (mode) {
	case PRELIM_DRM_XE_PXP_MODE_LM:
		break;
	case PRELIM_DRM_XE_PXP_MODE_HM:
		pxp_tag |= PRELIM_DRM_XE_PXP_TAG_SESSION_HM;
		break;
	case PRELIM_DRM_XE_PXP_MODE_SM:
		pxp_tag |= PRELIM_DRM_XE_PXP_TAG_SESSION_HM;
		pxp_tag |= PRELIM_DRM_XE_PXP_TAG_SESSION_SM;
		break;
	default:
		drm_err(&pxp->xe->drm, "unexpected PXP protection mode %d\n", mode);
	}

	pxp_tag |= PRELIM_DRM_XE_PXP_TAG_SESSION_ENABLED;
	pxp_tag |= FIELD_PREP(PRELIM_DRM_XE_PXP_TAG_INSTANCE_ID_MASK, instance);
	pxp_tag |= FIELD_PREP(PRELIM_DRM_XE_PXP_TAG_SESSION_ID_MASK, idx);

	return pxp_tag;
}

static u32 pxp_tag_fill(struct xe_pxp *pxp, int idx, int mode)
{
	u8 instance = ++pxp->multi_session.sessions[idx].instance;

	if (!instance)
		instance = ++pxp->multi_session.sessions[idx].instance;

	return __pxp_tag(pxp, idx, mode, instance);
}

void xe_pxp_multi_session_set_arb_session_tag(struct xe_pxp *pxp, bool active)
{
	u32 tag = 0;

	if (active)
		tag = __pxp_tag(pxp, DRM_XE_PXP_HWDRM_DEFAULT_SESSION,
				PRELIM_DRM_XE_PXP_MODE_HM, (u8)pxp->key_instance);

	pxp->multi_session.sessions[DRM_XE_PXP_HWDRM_DEFAULT_SESSION].tag = tag;
}

static void __pxp_reserve_session(struct xe_pxp *pxp, struct xe_pxp_client *client,
				  u32 id, u32 tag)
{
	pxp->multi_session.reserved_sessions |= BIT(id);
	pxp->multi_session.sessions[id].tag = tag;
	pxp->multi_session.sessions[id].owner = client ? client->drmfile : NULL;
}

static void __pxp_release_session(struct xe_pxp *pxp, u32 id)
{
	pxp->multi_session.sessions[id].owner = NULL;
	pxp->multi_session.sessions[id].tag = 0;
	pxp->multi_session.reserved_sessions &= ~BIT(id);
}

static bool pxp_session_is_reserved(struct xe_pxp *pxp, u32 id)
{
	return pxp->multi_session.reserved_sessions & BIT(id);
}

static bool pxp_session_is_owned(struct xe_pxp *pxp, struct xe_pxp_client *client, u32 id)
{
	return pxp->multi_session.sessions[id].owner == client->drmfile;
}

static int pxp_reserve_session(struct xe_pxp *pxp, struct xe_pxp_client *client,
			       u32 type, u32 mode, u32 *pxp_tag)
{
	int ret;
	int idx = 0;

	lockdep_assert_held(&pxp->multi_session.mutex);

	if (XE_IOCTL_DBG(pxp->xe, type != DRM_XE_PXP_TYPE_HWDRM))
		return -EINVAL;

	if (XE_IOCTL_DBG(pxp->xe,
			 mode < PRELIM_DRM_XE_PXP_MODE_LM || mode > PRELIM_DRM_XE_PXP_MODE_SM))
		return -EINVAL;

	idx = find_first_zero_bit(&pxp->multi_session.reserved_sessions,
				  INTEL_PXP_MAX_HWDRM_SESSIONS);
	if (idx >= INTEL_PXP_MAX_HWDRM_SESSIONS)
		return PRELIM_DRM_XE_PXP_OP_STATUS_SESSION_NOT_AVAILABLE;

	ret = xe_pxp_wait_for_session_state(pxp, idx, false);
	if (ret) {
		/* force termination of old reservation */
		ret = pxp_terminate_session(pxp, &client->res, idx);
		if (ret) {
			/* mark the broken session as reserved so we stop using it */
			__pxp_reserve_session(pxp, NULL, idx, 0);
			return PRELIM_DRM_XE_PXP_OP_STATUS_RETRY_REQUIRED;
		}
	}

	*pxp_tag = pxp_tag_fill(pxp, idx, mode);
	__pxp_reserve_session(pxp, client, idx, *pxp_tag);

	return ret;
}

static int pxp_release_session(struct xe_pxp *pxp,
			       struct xe_pxp_client *client,
			       u32 session_id)
{
	int ret;

	lockdep_assert_held(&pxp->multi_session.mutex);

	if (session_id >= INTEL_PXP_MAX_HWDRM_SESSIONS)
		return -EINVAL;

	/* already gone */
	if (!pxp_session_is_reserved(pxp, session_id))
		return 0;

	if (!pxp_session_is_owned(pxp, client, session_id))
		return -EPERM;

	ret = pxp_terminate_session(pxp, &client->res, session_id);
	if (ret)
		return ret;

	__pxp_release_session(pxp, session_id);

	return 0;
}

u32 xe_pxp_release_all_sessions(struct xe_pxp *pxp)
{
	u32 idx;
	u32 mask = 0;

	lockdep_assert_held(&pxp->multi_session.mutex);

	for (idx = 0; idx < INTEL_PXP_MAX_HWDRM_SESSIONS; idx++) {
		if (!pxp_session_is_reserved(pxp, idx))
			continue;

		if (idx != DRM_XE_PXP_HWDRM_DEFAULT_SESSION)
			__pxp_release_session(pxp, idx);

		mask |= BIT(idx);
	}

	return mask;
}

static void pxp_close_client_sessions(struct xe_pxp *pxp, struct xe_pxp_client *client)
{
	int idx;

	for (idx = 0; idx < INTEL_PXP_MAX_HWDRM_SESSIONS; idx++) {
		if (!pxp_session_is_reserved(pxp, idx))
			continue;

		if (!pxp_session_is_owned(pxp, client, idx))
			continue;

		pxp_terminate_session(pxp, &client->res, idx);
		__pxp_release_session(pxp, idx);
	}
}

void xe_pxp_close(struct xe_pxp *pxp, struct drm_file *drmfile)
{
	struct xe_pxp_client *client;

	if (!xe_pxp_is_enabled(pxp))
		return;

	mutex_lock(&pxp->multi_session.mutex);

	client = find_client(pxp, drmfile);

	if (client) {
		pxp_close_client_sessions(pxp, client);

		xe_pxp_destroy_client_resources(pxp, &client->res);
		list_del(&client->link);
		kfree(client);
	}

	mutex_unlock(&pxp->multi_session.mutex);
}

static int pxp_session_op(struct xe_pxp *pxp,
			  struct prelim_drm_xe_pxp_session_op *session_op,
			  struct xe_pxp_client *client)
{
	u32 session_id;
	int ret = 0;

	switch (session_op->action) {
	case PRELIM_DRM_XE_PXP_SESSION_RESERVE:
		/* session id must be empty when reserving */
		if (XE_IOCTL_DBG(pxp->xe, session_op->pxp_tag))
			return -EINVAL;

		ret = pxp_reserve_session(pxp, client,
					  session_op->session_type,
					  session_op->session_mode,
					  &session_op->pxp_tag);
		break;
	case PRELIM_DRM_XE_PXP_SESSION_RELEASE:
		session_id = FIELD_GET(PRELIM_DRM_XE_PXP_TAG_SESSION_ID_MASK,
				       session_op->pxp_tag);

		ret = pxp_release_session(pxp, client, session_id);
		break;
	default:
		ret = -EINVAL;
	}

	return ret;
}

static bool ioctl_buffer_size_valid(struct xe_pxp_client *client, u32 size)
{
	return size > 0 && size <= client->res.inout_size;
}

static int pxp_send_msg(struct xe_pxp *pxp,
			struct prelim_drm_xe_pxp_io_message *io_message,
			struct xe_pxp_client *client)
{
	struct xe_device *xe = pxp->xe;
	void *msg_in = NULL;
	void *msg_out = NULL;
	int ret = 0;

	lockdep_assert_held(&pxp->multi_session.mutex);

	if (XE_IOCTL_DBG(xe, !io_message->msg_in) ||
	    XE_IOCTL_DBG(xe, !io_message->msg_out) ||
	    XE_IOCTL_DBG(xe, !ioctl_buffer_size_valid(client, io_message->msg_out_buf_size)) ||
	    XE_IOCTL_DBG(xe, !ioctl_buffer_size_valid(client, io_message->msg_in_size)))
		return -EINVAL;

	msg_in = kzalloc(io_message->msg_in_size, GFP_KERNEL);
	if (!msg_in)
		return -ENOMEM;

	msg_out = kzalloc(io_message->msg_out_buf_size, GFP_KERNEL);
	if (!msg_out) {
		ret = -ENOMEM;
		goto end;
	}

	if (copy_from_user(msg_in, u64_to_user_ptr(io_message->msg_in), io_message->msg_in_size)) {
		drm_dbg(&xe->drm, "Failed to copy_from_user for PXP message\n");
		ret = -EFAULT;
		goto end;
	}

	ret = xe_pxp_gsccs_send_user_message(&client->res, msg_in,
					     io_message->msg_in_size, msg_out,
					     io_message->msg_out_buf_size,
					     &io_message->msg_out_ret_size);
	if (ret) {
		drm_dbg(&xe->drm, "Failed to send/receive user PXP message\n");
		goto end;
	}

	if (copy_to_user(u64_to_user_ptr(io_message->msg_out), msg_out,
			 io_message->msg_out_ret_size)) {
		drm_dbg(&xe->drm, "Failed copy_to_user for TEE message\n");
		ret = -EFAULT;
		goto end;
	}

end:
	kfree(msg_in);
	kfree(msg_out);
	return ret;
}

static bool pxp_session_is_in_play(struct xe_pxp *pxp, u32 id)
{
	unsigned int fw_ref;
	bool in_play;

	fw_ref = xe_force_wake_get(gt_to_fw(pxp->gt), XE_FW_GT);
	XE_WARN_ON(!xe_force_wake_ref_has_domain(fw_ref, XE_FW_GT));

	in_play = xe_mmio_read32(&pxp->gt->mmio, KCR_SIP) & BIT(id);

	xe_force_wake_put(gt_to_fw(pxp->gt), fw_ref);

	return in_play;
}

static int pxp_query_tag(struct xe_pxp *pxp, struct prelim_drm_xe_pxp_query_tag *query_tag)
{
	int session_id = FIELD_GET(PRELIM_DRM_XE_PXP_TAG_SESSION_ID_MASK, query_tag->pxp_tag);

	if (session_id >= INTEL_PXP_MAX_HWDRM_SESSIONS)
		return -EINVAL;

	if (!pxp_session_is_reserved(pxp, session_id)) {
		query_tag->pxp_tag = 0;
		query_tag->session_is_alive = 0;
	} else {
		query_tag->pxp_tag = pxp->multi_session.sessions[session_id].tag;
		query_tag->session_is_alive = pxp_session_is_in_play(pxp, session_id);
	}

	return 0;
}

static void
pxp_query_host_session_handle(struct prelim_drm_xe_pxp_query_host_session_handle *query_handle,
			      struct xe_pxp_client *client)
{
	query_handle->host_session_handle = client->res.host_session_handle;
}

static bool pxp_op_needs_rpm(u32 op)
{
	return op != PRELIM_DRM_XE_PXP_ACTION_HOST_SESSION_HANDLE_REQ;
}

static bool pxp_op_needs_resources(u32 op)
{
	return op != PRELIM_DRM_XE_PXP_ACTION_QUERY_PXP_TAG;
}

static bool pxp_op_needs_arb(u32 op)
{
	return op != PRELIM_DRM_XE_PXP_ACTION_HOST_SESSION_HANDLE_REQ;
}

int xe_pxp_ops_ioctl(struct drm_device *dev, void *data, struct drm_file *drmfile)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_pxp *pxp = xe->pxp;
	struct prelim_drm_xe_pxp_ops *pxp_ops = data;
	u32 action = pxp_ops->action;
	struct xe_pxp_client *client;
	int ret = 0;

	if (!xe_pxp_is_enabled(pxp))
		return -ENODEV;

	if (XE_IOCTL_DBG(xe, pxp_ops->extensions))
		return -EINVAL;

	if (XE_IOCTL_DBG(xe, action > PRELIM_DRM_XE_PXP_ACTION_TEE_IO_MESSAGE))
		return -EINVAL;

	if (pxp_op_needs_rpm(action) && !xe_pm_runtime_get_if_in_use(xe)) {
		drm_dbg(&xe->drm, "pxp ioctl blocked due to hw suspend\n");
		pxp_ops->status = PRELIM_DRM_XE_PXP_OP_STATUS_POWER_OFF;
		return 0;
	}

	if (pxp_op_needs_arb(action)) {
		xe_assert(xe, pxp_op_needs_rpm(action));

pxp_start:
		/* This will wait for any pending termination to complete */
		ret = xe_pxp_start(pxp, DRM_XE_PXP_TYPE_HWDRM);
		if (ret)
			goto out_pm;
	}

	mutex_lock(&pxp->multi_session.mutex);

	/*
	 * check if a new termination was issued between the above check and
	 * grabbing the mutex
	 */
	if (pxp_op_needs_arb(action) && !completion_done(&pxp->termination)) {
		mutex_unlock(&pxp->multi_session.mutex);
		goto pxp_start;
	}

	if (pxp_op_needs_resources(action)) {
		client = xe_pxp_alloc_client_resources(pxp, drmfile);
		if (IS_ERR(client)) {
			ret = PTR_ERR(client);
			goto out_unlock;
		}
	}

	switch (pxp_ops->action) {
	case PRELIM_DRM_XE_PXP_ACTION_HOST_SESSION_HANDLE_REQ:
		pxp_query_host_session_handle(&pxp_ops->query_handle, client);
		break;
	case PRELIM_DRM_XE_PXP_ACTION_SESSION_OP:
		ret = pxp_session_op(pxp, &pxp_ops->session_op, client);
		break;
	case PRELIM_DRM_XE_PXP_ACTION_QUERY_PXP_TAG:
		ret = pxp_query_tag(pxp, &pxp_ops->query_tag);
		break;
	case PRELIM_DRM_XE_PXP_ACTION_TEE_IO_MESSAGE:
		ret = pxp_send_msg(pxp, &pxp_ops->io_message, client);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	/*
	 * ops can return a non-negative status code that contains extra info
	 * (see PRELIM_DRM_XE_PXP_OP_STATUS_* defines in the UAPI for possible
	 * values). We need to return this code to the caller.
	 */
	if (ret >= 0) {
		pxp_ops->status = ret;
		ret = 0;
	}

out_unlock:
	mutex_unlock(&pxp->multi_session.mutex);
out_pm:
	if (pxp_op_needs_rpm(action))
		xe_pm_runtime_put(xe);

	return ret;
}

void xe_pxp_multi_session_init(struct xe_pxp *pxp)
{
	INIT_LIST_HEAD(&pxp->multi_session.client_list);
	mutex_init(&pxp->multi_session.mutex);

	/* The default session is perma-reserved by the kernel */
	__pxp_reserve_session(pxp, NULL, DRM_XE_PXP_HWDRM_DEFAULT_SESSION, 0);
}

void xe_pxp_invalidate_sessions(struct xe_pxp *pxp, u32 mask)
{
	int i;

	lockdep_assert_held(&pxp->multi_session.mutex);

	for (i = 0; i < INTEL_PXP_MAX_HWDRM_SESSIONS; i++) {
		struct xe_pxp_client *client;
		struct xe_pxp_gsc_client_resources *gsc_res;
		int ret;

		if (!(mask & BIT(i)))
			continue;

		client = find_client(pxp, pxp->multi_session.sessions[i].owner);

		if (client)
			gsc_res = &client->res;
		else
			gsc_res = &pxp->gsc_res;

		ret = xe_pxp_submit_session_invalidation(gsc_res, i);
		if (ret)
			drm_err(&pxp->xe->drm, "failed to invalidate PXP session %d\n", i);
	}
}
