/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/* Copyright(c) 2026 Intel Corporation */

#ifndef __SOF_IPC4_WOV_H
#define __SOF_IPC4_WOV_H

#include "sof-priv.h"
#include "ipc4-priv.h"

void sof_ipc4_wov_phrase_detected(struct snd_sof_dev *sdev,
				  struct sof_ipc4_msg *ipc4_msg);

#endif /* __SOF_IPC4_WOV_H */
