#ifndef __SOF_SND_PCM_FORMAT_SIZE_COMPAT_H
#define __SOF_SND_PCM_FORMAT_SIZE_COMPAT_H

#include <sound/pcm.h>

/*
 * Some kernel build variants do not export snd_pcm_format_size() in Module.symvers.
 * Provide a compile-time fallback for SOF call sites without touching C sources.
 */
#ifndef snd_pcm_format_size
#define snd_pcm_format_size(format, samples) \
	((((snd_pcm_format_physical_width(format)) * (samples)) + 7) / 8)
#endif

#endif