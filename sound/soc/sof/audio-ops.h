/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 */

#ifndef __SOUND_SOC_SOF_AUDIO_OPS_H
#define __SOUND_SOC_SOF_AUDIO_OPS_H

/* host PCM ops */
static inline int
snd_sof_pcm_platform_open(struct sof_audio_dev *sof_audio,
			  struct snd_pcm_substream *substream)
{
	if (sof_audio->audio_ops->pcm_open)
		return sof_audio->audio_ops->pcm_open(sof_audio->component,
						      substream);

	return 0;
}

/* disconnect pcm substream to a host stream */
static inline int
snd_sof_pcm_platform_close(struct sof_audio_dev *sof_audio,
			   struct snd_pcm_substream *substream)
{
	if (sof_audio->audio_ops->pcm_close)
		return sof_audio->audio_ops->pcm_close(sof_audio->component,
						       substream);

	return 0;
}

/* host stream hw params */
static inline int
snd_sof_pcm_platform_hw_params(struct sof_audio_dev *sof_audio,
			       struct snd_pcm_substream *substream,
			       struct snd_pcm_hw_params *params,
			       struct sof_ipc_stream_params *ipc_params)
{
	if (sof_audio->audio_ops->pcm_hw_params)
		return sof_audio->audio_ops->pcm_hw_params(sof_audio->component,
							   substream, params,
							   ipc_params);

	return 0;
}

/* host stream hw free */
static inline int
snd_sof_pcm_platform_hw_free(struct sof_audio_dev *sof_audio,
			     struct snd_pcm_substream *substream)
{
	if (sof_audio->audio_ops->pcm_hw_free)
		return sof_audio->audio_ops->pcm_hw_free(sof_audio->component,
							 substream);

	return 0;
}

/* host stream trigger */
static inline int
snd_sof_pcm_platform_trigger(struct sof_audio_dev *sof_audio,
			     struct snd_pcm_substream *substream, int cmd)
{
	if (sof_audio->audio_ops->pcm_trigger)
		return sof_audio->audio_ops->pcm_trigger(sof_audio->component,
							 substream, cmd);

	return 0;
}

/* host configure DSP HW parameters */
static inline int
snd_sof_ipc_pcm_params(struct sof_audio_dev *sof_audio,
		       struct snd_pcm_substream *substream,
		       const struct sof_ipc_pcm_params_reply *reply)
{
	return sof_audio->audio_ops->ipc_pcm_params(sof_audio->component,
						    substream, reply);
}

/* host stream pointer */
static inline snd_pcm_uframes_t
snd_sof_pcm_platform_pointer(struct sof_audio_dev *sof_audio,
			     struct snd_pcm_substream *substream)
{
	if (sof_audio->audio_ops->pcm_pointer)
		return sof_audio->audio_ops->pcm_pointer(sof_audio->component,
							 substream);

	return 0;
}
#endif
