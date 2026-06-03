// SPDX-License-Identifier: GPL-2.0-only
//
// Copyright(c) 2026 Intel Corporation
//
// Authors: Peter Ujfalusi <peter.ujfalusi@linux.intel.com>
//

#include <linux/auxiliary_bus.h>
#include <linux/module.h>
#include <sound/soc.h>

#include "sof-client.h"
#include "sof-client-audio.h"

static int sof_audio_client_probe(struct auxiliary_device *auxdev,
				  const struct auxiliary_device_id *id)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct sof_audio_client_pdata *pdata = dev_get_platdata(&auxdev->dev);
	struct device *dma_dev = sof_client_get_dma_dev(cdev);
	int ret;

	ret = snd_soc_register_component(dma_dev, &pdata->plat_drv,
					 pdata->drv, pdata->num_drv);
	if (ret < 0)
		return ret;

	ret = sof_client_machine_register(cdev);
	if (ret < 0) {
		snd_soc_unregister_component(dma_dev);
		return ret;
	}

	return 0;
}

static void sof_audio_client_remove(struct auxiliary_device *auxdev)
{
	struct sof_client_dev *cdev = auxiliary_dev_to_sof_client_dev(auxdev);
	struct device *dma_dev = sof_client_get_dma_dev(cdev);

	sof_client_machine_unregister(cdev);
	snd_soc_unregister_component(dma_dev);
}

static const struct auxiliary_device_id sof_audio_client_id_table[] = {
	{ .name = "snd_sof.audio", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, sof_audio_client_id_table);

static struct auxiliary_driver sof_audio_client_drv = {
	.probe = sof_audio_client_probe,
	.remove = sof_audio_client_remove,
	.id_table = sof_audio_client_id_table,
};

module_auxiliary_driver(sof_audio_client_drv);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("SOF Audio Client Driver");
MODULE_IMPORT_NS("SND_SOC_SOF_CLIENT");
