/* SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause) */
/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * Copyright(c) 2019 Intel Corporation. All rights reserved.
 *
 * Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
 */

#ifndef __SOUND_SOC_SOF_AUDIO_H
#define __SOUND_SOC_SOF_AUDIO_H

#include <sound/sof/stream.h> /* needs to be included before control.h */
#include <sound/sof/control.h>
#include <sound/sof/dai.h>
#include <sound/sof/topology.h>

#define DRV_NAME	"sof-audio-component"

struct snd_soc_tplg_ops;
struct snd_soc_component;

struct snd_sof_audio_ops {

	/* host configure DSP HW parameters */
	int (*ipc_pcm_params)(struct snd_soc_component *scomp,
			      struct snd_pcm_substream *substream,
			      const struct sof_ipc_pcm_params_reply *reply); /* mandatory */

	/* connect pcm substream to a host stream */
	int (*pcm_open)(struct snd_soc_component *scomp,
			struct snd_pcm_substream *substream); /* optional */

	/* disconnect pcm substream to a host stream */
	int (*pcm_close)(struct snd_soc_component *scomp,
			 struct snd_pcm_substream *substream); /* optional */

	/* host stream hw params */
	int (*pcm_hw_params)(struct snd_soc_component *scomp,
			     struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct sof_ipc_stream_params *ipc_params); /* optional */

	/* host stream hw_free */
	int (*pcm_hw_free)(struct snd_soc_component *scomp,
			   struct snd_pcm_substream *substream); /* optional */

	/* host stream trigger */
	int (*pcm_trigger)(struct snd_soc_component *scomp,
			   struct snd_pcm_substream *substream,
			   int cmd); /* optional */

	/* host stream pointer */
	snd_pcm_uframes_t (*pcm_pointer)(struct snd_soc_component *scomp,
					 struct snd_pcm_substream *substream); /* optional */

	/* platform-specific machine driver check */
	int (*machine_driver_select)(struct snd_sof_dev *sdev,
				     struct sof_audio_dev *sof_audio); /* optional */

	/* DAI ops */
	struct snd_soc_dai_driver *drv;
	int num_drv;

	/* ALSA HW info flags, will be stored in snd_pcm_runtime.hw.info */
	u32 hw_info;
};

/* PCM stream, mapped to FW component */
struct snd_sof_pcm_stream {
	u32 comp_id;
	struct snd_dma_buffer page_table;
	struct sof_ipc_stream_posn posn;
	struct snd_pcm_substream *substream;
	struct work_struct period_elapsed_work;
	bool d0i3_compatible; /* DSP can be in D0I3 when this pcm is opened */
};

/* ALSA SOF PCM device */
struct snd_sof_pcm {
	struct snd_soc_component *scomp;
	struct snd_soc_tplg_pcm pcm;
	struct snd_sof_pcm_stream stream[2];
	struct list_head list;	/* list in sof_audio_dev pcm list */
	struct snd_pcm_hw_params params[2];
	bool prepared[2]; /* PCM_PARAMS set successfully */
};

struct snd_sof_led_control {
	unsigned int use_led;
	unsigned int direction;
	unsigned int led_value;
};

/* ALSA SOF Kcontrol device */
struct snd_sof_control {
	struct snd_soc_component *scomp;
	int comp_id;
	int min_volume_step; /* min volume step for volume_table */
	int max_volume_step; /* max volume step for volume_table */
	int num_channels;
	u32 readback_offset; /* offset to mmaped data if used */
	struct sof_ipc_ctrl_data *control_data;
	u32 size;	/* cdata size */
	enum sof_ipc_ctrl_cmd cmd;
	u32 *volume_table; /* volume table computed from tlv data*/

	struct list_head list;	/* list in sof_audio_dev control list */

	struct snd_sof_led_control led_ctl;
};

/* ASoC SOF DAPM widget */
struct snd_sof_widget {
	struct snd_soc_component *scomp;
	int comp_id;
	int pipeline_id;
	int complete;
	int id;

	struct snd_soc_dapm_widget *widget;
	struct list_head list;	/* list in sof_audio_dev widget list */

	void *private;		/* core does not touch this */
};

/* ASoC SOF DAPM route */
struct snd_sof_route {
	struct snd_soc_component *scomp;

	struct snd_soc_dapm_route *route;
	struct list_head list;	/* list in sof_audio_dev route list */

	void *private;
};

/* ASoC DAI device */
struct snd_sof_dai {
	struct snd_soc_component *scomp;
	const char *name;
	const char *cpu_dai_name;

	struct sof_ipc_comp_dai comp_dai;
	struct sof_ipc_dai_config *dai_config;
	struct list_head list;	/* list in sof_audio_dev dai list */
};

/* SOF audio device */
struct sof_audio_dev {
	const char *platform;
	const char *drv_name;

	/* machine */
	struct platform_device *pdev_mach;
	const struct snd_soc_acpi_mach *machine;

	const char *tplg_filename_prefix;
	const char *tplg_filename;

	/*
	 * ASoC components. plat_drv fields are set dynamically so
	 * can't use const
	 */
	struct snd_soc_component_driver plat_drv;

	/* topology */
	struct list_head pcm_list;
	struct list_head kcontrol_list;
	struct list_head widget_list;
	struct list_head dai_list;
	struct list_head route_list;
	struct snd_soc_component *component;

	/* Platform-specific audio ops */
	const struct snd_sof_audio_ops *audio_ops;

	void *private;
};

struct snd_sof_widget *snd_sof_find_swidget(struct snd_soc_component *scomp,
					    const char *name);
struct snd_sof_widget *
snd_sof_find_swidget_sname(struct snd_soc_component *scomp,
			   const char *pcm_name, int dir);
struct snd_sof_dai *snd_sof_find_dai(struct snd_soc_component *scomp,
				     const char *name);

static inline
struct snd_sof_pcm *snd_sof_find_spcm_dai(struct sof_audio_dev *sof_audio,
					  struct snd_soc_pcm_runtime *rtd)
{
	struct snd_sof_pcm *spcm = NULL;

	list_for_each_entry(spcm, &sof_audio->pcm_list, list) {
		if (le32_to_cpu(spcm->pcm.dai_id) == rtd->dai_link->id)
			return spcm;
	}

	return NULL;
}

struct snd_sof_pcm *snd_sof_find_spcm_name(struct snd_soc_component *scomp,
					   const char *name);
struct snd_sof_pcm *snd_sof_find_spcm_comp(struct snd_soc_component *scomp,
					   unsigned int comp_id,
					   int *direction);
struct snd_sof_pcm *snd_sof_find_spcm_pcm_id(struct snd_soc_component *scomp,
					     unsigned int pcm_id);
void snd_sof_pcm_period_elapsed(struct snd_pcm_substream *substream);

/*
 * Mixer IPC
 */
int snd_sof_ipc_set_get_comp_data(struct snd_soc_component *scomp,
				  struct snd_sof_control *scontrol, u32 ipc_cmd,
				  enum sof_ipc_ctrl_type ctrl_type,
				  enum sof_ipc_ctrl_cmd ctrl_cmd,
				  bool send);

/*
 * Topology
 */
int snd_sof_load_topology(struct snd_soc_component *scomp, const char *file);
int snd_sof_complete_pipeline(struct snd_soc_component *scomp,
			      struct snd_sof_widget *swidget);
int sof_load_pipeline_ipc(struct snd_soc_component *scomp,
			  struct sof_ipc_pipe_new *pipeline,
			  struct sof_ipc_comp_reply *r);

void snd_sof_new_platform_drv(struct sof_audio_dev *sof_audio,
			      struct snd_sof_pdata *plat_data);

/*
 * Stream IPC
 */
int snd_sof_ipc_stream_posn(struct snd_soc_component *scomp,
			    struct snd_sof_pcm *spcm, int direction,
			    struct sof_ipc_stream_posn *posn);

/*
 * Kcontrols.
 */

int snd_sof_volume_get(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol);
int snd_sof_volume_put(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol);
int snd_sof_switch_get(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol);
int snd_sof_switch_put(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol);
int snd_sof_enum_get(struct snd_kcontrol *kcontrol,
		     struct snd_ctl_elem_value *ucontrol);
int snd_sof_enum_put(struct snd_kcontrol *kcontrol,
		     struct snd_ctl_elem_value *ucontrol);
int snd_sof_bytes_get(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol);
int snd_sof_bytes_put(struct snd_kcontrol *kcontrol,
		      struct snd_ctl_elem_value *ucontrol);
int snd_sof_bytes_ext_put(struct snd_kcontrol *kcontrol,
			  const unsigned int __user *binary_data,
			  unsigned int size);
int snd_sof_bytes_ext_get(struct snd_kcontrol *kcontrol,
			  unsigned int __user *binary_data,
			  unsigned int size);

int intel_ipc_pcm_params(struct snd_soc_component *scomp,
			 struct snd_pcm_substream *substream,
			 const struct sof_ipc_pcm_params_reply *reply);

int intel_pcm_open(struct snd_soc_component *scomp,
		   struct snd_pcm_substream *substream);
int intel_pcm_close(struct snd_soc_component *scomp,
		    struct snd_pcm_substream *substream);

#endif
