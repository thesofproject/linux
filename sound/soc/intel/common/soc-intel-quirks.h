/* SPDX-License-Identifier: GPL-2.0 */
/*
 * soc-intel-quirks.h - prototypes for SOF autodetection
 *
 * Copyright (c) 2019, Intel Corporation.
 *
 */

#ifndef _SND_SOC_INTEL_QUIRKS_H
#define _SND_SOC_INTEL_QUIRKS_H

void sof_intel_quirk_apl(bool *is_apl, bool *sof);
void sof_intel_quirk_glk(bool *is_glk, bool *sof);

#endif /* _SND_SOC_INTEL_QUIRKS_H */
