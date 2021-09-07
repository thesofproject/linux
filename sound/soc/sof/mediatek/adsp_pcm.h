/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2021 Mediatek Corporation. All rights reserved.
 *
 * Author: Bo Pan <bo.pan@mediatek.com>
 *		YC Hung <yc.hung@mediatek.com>
 */

#ifndef __SOF_ADSP_PCM_H
#define __SOF_ADSP_PCM_H
#include <linux/module.h>
#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include <linux/genalloc.h>
#include "../ops.h"
#include "../sof-audio.h"

#define SOF_MIN_ADSP_SHIFT (8)
#define SOF_MIN_ADSP_POOL_SIZE BIT(SOF_MIN_ADSP_SHIFT)

struct adsp_mem {
	unsigned long long phy_addr;
	unsigned long long va_addr;
	unsigned long long size;
	unsigned char *vir_addr;
};

struct adsp_mem_pool {
	struct gen_pool *dram_pool;
	struct adsp_mem *dram;
};

/* adsp share memory manage */
int adsp_genpool_create(struct adsp_mem_pool *mem_pool,
			struct adsp_mem *mem_info);

int adsp_genpool_destroy(struct adsp_mem_pool *mem_pool);

int adsp_genpool_alloc(struct adsp_mem_pool *mem_pool,
		       unsigned char **vaddr,
		       dma_addr_t *paddr,
		       unsigned int size);

int adsp_genpool_alloc_align(struct adsp_mem_pool *mem_pool,
			     unsigned char **vaddr,
			     dma_addr_t *paddr,
			     unsigned int size,
			     int align);

int adsp_genpool_free(struct adsp_mem_pool *mem_pool,
		      unsigned char *vaddr,
		      size_t size);

/* SOF adsp pcm */
int adsp_pcm_hw_params(struct snd_sof_dev *sdev,
		       struct snd_pcm_substream *substream,
		       struct snd_pcm_hw_params *params,
		       struct sof_ipc_stream_params *ipc_params);

int adsp_pcm_hw_free(struct snd_sof_dev *sdev,
		     struct snd_pcm_substream *substream);

#endif /* __SOF_ADSP_PCM_H */

