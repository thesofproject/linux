// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//
#define DEBUG

#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/virtual_bus.h>
#include <sound/soc.h>
#include "compress.h"
#include "probe.h"
#include "sof-client.h"

#define SOF_PROBES_SUSPEND_DELAY_MS 3000
/* only extraction supported for now */
#define SOF_PROBES_NUM_DAI_LINKS 1

/**
 * strsplit_u32 - Split string into sequence of u32 tokens
 * @buf:	String to split into tokens.
 * @delim:	String containing delimiter characters.
 * @tkns:	Returned u32 sequence pointer.
 * @num_tkns:	Returned number of tokens obtained.
 */
static int
strsplit_u32(char **buf, const char *delim, u32 **tkns, size_t *num_tkns)
{
	char *s;
	u32 *data, *tmp;
	size_t count = 0;
	size_t cap = 32;
	int ret = 0;

	*tkns = NULL;
	*num_tkns = 0;
	data = kcalloc(cap, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	while ((s = strsep(buf, delim)) != NULL) {
		ret = kstrtouint(s, 0, data + count);
		if (ret)
			goto exit;
		if (++count >= cap) {
			cap *= 2;
			tmp = krealloc(data, cap * sizeof(*data), GFP_KERNEL);
			if (!tmp) {
				ret = -ENOMEM;
				goto exit;
			}
			data = tmp;
		}
	}

	if (!count)
		goto exit;
	*tkns = kmemdup(data, count * sizeof(*data), GFP_KERNEL);
	if (!(*tkns)) {
		ret = -ENOMEM;
		goto exit;
	}
	*num_tkns = count;

exit:
	kfree(data);
	return ret;
}

static int tokenize_input(const char __user *from, size_t count,
			  loff_t *ppos, u32 **tkns, size_t *num_tkns)
{
	char *buf;
	int ret;

	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, ppos, from, count);
	if (ret != count) {
		ret = ret >= 0 ? -EIO : ret;
		goto exit;
	}

	buf[count] = '\0';
	ret = strsplit_u32((char **)&buf, ",", tkns, num_tkns);
exit:
	kfree(buf);
	return ret;
}

static ssize_t probe_points_read(struct file *file, char __user *to,
				 size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_probes_data *probes_data = cdev->data;
	struct device *dev = &cdev->vdev.dev;
	struct sof_probe_point_desc *desc;
	size_t num_desc;
	int remaining;
	char *buf;
	int i, ret, err;

	if (probes_data->extractor_stream_tag == SOF_PROBE_INVALID_NODE_ID) {
		dev_warn(dev, "no extractor stream running\n");
		return -ENOENT;
	}

	buf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err_ratelimited(dev,
				    "error: debugfs read failed to resume %d\n",
				    ret);
		pm_runtime_put_noidle(dev);
		goto exit;
	}

	ret = sof_probe_points_info(cdev, &desc, &num_desc);
	if (ret < 0)
		goto exit;

	pm_runtime_mark_last_busy(dev);
	err = pm_runtime_put_autosuspend(dev);
	if (err < 0)
		dev_err_ratelimited(dev,
				    "error: debugfs read failed to idle %d\n",
				    err);

	for (i = 0; i < num_desc; i++) {
		remaining = PAGE_SIZE - strlen(buf);
		if (remaining > 0) {
			ret = snprintf(buf + strlen(buf), remaining,
				       "Id: %#010x  Purpose: %d  Node id: %#x\n",
				       desc[i].buffer_id, desc[i].purpose,
				       desc[i].stream_tag);
			if (ret < 0)
				goto free_desc;
		} else {
			break;
		}
	}

	ret = simple_read_from_buffer(to, count, ppos, buf, strlen(buf));
free_desc:
	kfree(desc);
exit:
	kfree(buf);
	return ret;
}

static ssize_t probe_points_write(struct file *file, const char __user *from,
				  size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_probes_data *probes_data = cdev->data;
	struct device *dev = &cdev->vdev.dev;
	struct sof_probe_point_desc *desc;
	size_t num_tkns, bytes;
	u32 *tkns;
	int ret, err;

	if (probes_data->extractor_stream_tag == SOF_PROBE_INVALID_NODE_ID) {
		dev_warn(dev, "no extractor stream running\n");
		return -ENOENT;
	}

	ret = tokenize_input(from, count, ppos, &tkns, &num_tkns);
	if (ret < 0)
		return ret;
	bytes = sizeof(*tkns) * num_tkns;
	if (!num_tkns || (bytes % sizeof(*desc))) {
		ret = -EINVAL;
		goto exit;
	}

	desc = (struct sof_probe_point_desc *)tkns;

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err_ratelimited(dev,
				    "error: debugfs write failed to resume %d\n",
				    ret);
		pm_runtime_put_noidle(dev);
		goto exit;
	}

	ret = sof_probe_points_add(cdev, desc, bytes / sizeof(*desc));
	if (!ret)
		ret = count;

	pm_runtime_mark_last_busy(dev);
	err = pm_runtime_put_autosuspend(dev);
	if (err < 0)
		dev_err_ratelimited(dev,
				    "error: debugfs write failed to idle %d\n",
				    err);
exit:
	kfree(tkns);
	return ret;
}

static const struct file_operations probe_points_fops = {
	.open = simple_open,
	.read = probe_points_read,
	.write = probe_points_write,
	.llseek = default_llseek,
};

static ssize_t
probe_points_remove_write(struct file *file, const char __user *from,
			  size_t count, loff_t *ppos)
{
	struct sof_client_dev *cdev = file->private_data;
	struct sof_probes_data *probes_data = cdev->data;
	struct device *dev = &cdev->vdev.dev;
	size_t num_tkns;
	u32 *tkns;
	int ret, err;

	if (probes_data->extractor_stream_tag == SOF_PROBE_INVALID_NODE_ID) {
		dev_warn(dev, "no extractor stream running\n");
		return -ENOENT;
	}

	ret = tokenize_input(from, count, ppos, &tkns, &num_tkns);
	if (ret < 0)
		return ret;
	if (!num_tkns) {
		ret = -EINVAL;
		goto exit;
	}

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err_ratelimited(dev,
				    "error: debugfs write failed to resume %d\n",
				    ret);
		pm_runtime_put_noidle(dev);
		goto exit;
	}

	ret = sof_probe_points_remove(cdev, tkns, num_tkns);
	if (!ret)
		ret = count;

	pm_runtime_mark_last_busy(dev);
	err = pm_runtime_put_autosuspend(dev);
	if (err < 0)
		dev_err_ratelimited(dev,
				    "error: debugfs write failed to idle %d\n",
				    err);
exit:
	kfree(tkns);
	return ret;
}

static const struct file_operations probe_points_remove_fops = {
	.open = simple_open,
	.write = probe_points_remove_write,
	.llseek = default_llseek,
};

struct snd_soc_dai_driver sof_probes_dai_drv[] = {
{
	.name = "Probe Extraction CPU DAI",
	.compress_new = snd_soc_new_compress,
	.cops = &sof_probe_compr_ops,
	.capture = {
		.stream_name = "Probe Extraction",
		.channels_min = 1,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_48000,
		.rate_min = 48000,
		.rate_max = 48000,
	},
},
};

static const struct snd_soc_component_driver sof_probes_component = {
	.name = "sof-probes-component",
	.compr_ops = &sof_probe_compressed_ops,
	.module_get_upon_open = 1,
};

SND_SOC_DAILINK_DEF(dummy, DAILINK_COMP_ARRAY(COMP_DUMMY()));

static struct snd_soc_card sof_probes_card = {
	.name = "probes", /* the sof- prefix is added by the core */
};

static int sof_probes_client_probe(struct virtbus_device *vdev)
{
	struct sof_client_dev *cdev = virtbus_dev_to_sof_client_dev(vdev);
	struct snd_soc_dai_link_component platform_component[] = {
		{
			.name = dev_name(&vdev->dev),
		}
	};
	struct snd_soc_card *card = &sof_probes_card;
	struct sof_probes_data *probes_client_data;
	struct snd_soc_dai_link_component *cpus;
	struct snd_soc_dai_link *links;
	int ret;

	/*
	 * The virtbus device has a usage count of 0 even before runtime PM
	 * is enabled. So, increment the usage count to let the device
	 * suspend after probe is complete.
	 */
	pm_runtime_get_noresume(&vdev->dev);

	/* register probes component driver and dai */
	ret = devm_snd_soc_register_component(&vdev->dev,
					      &sof_probes_component,
					      sof_probes_dai_drv,
					      ARRAY_SIZE(sof_probes_dai_drv));
	if (ret < 0) {
		dev_err(&vdev->dev,
			"error: failed to register SOF probes DAI driver %d\n",
			ret);
		return ret;
	}

	/* set client data */
	probes_client_data = devm_kzalloc(&vdev->dev,
					  sizeof(*probes_client_data),
					  GFP_KERNEL);
	if (!probes_client_data)
		return -ENOMEM;

	probes_client_data->extractor_stream_tag = SOF_PROBE_INVALID_NODE_ID;
	cdev->data = probes_client_data;

	/* create probes debugfs dir under SOF debugfs root dir */
	probes_client_data->dfs_root =
		debugfs_create_dir("probes",
				   sof_client_get_debugfs_root(cdev));

	/* create read-write probes_points debugfs entry */
	debugfs_create_file("probe_points", 0644, probes_client_data->dfs_root,
			    cdev, &probe_points_fops);

	/* create read-write probe_points_remove debugfs entry */
	debugfs_create_file("probe_points_remove", 0644,
			    probes_client_data->dfs_root,
			    cdev, &probe_points_remove_fops);

	links = devm_kzalloc(&vdev->dev, sizeof(*links) *
			     SOF_PROBES_NUM_DAI_LINKS, GFP_KERNEL);
	cpus = devm_kzalloc(&vdev->dev, sizeof(*cpus) *
			    SOF_PROBES_NUM_DAI_LINKS, GFP_KERNEL);
	if (!links || !cpus)
		return -ENOMEM;

	/* extraction DAI link */
	links[0].name = "Compress Probe Capture";
	links[0].id = 0;
	links[0].cpus = &cpus[0];
	links[0].num_cpus = 1;
	links[0].cpus->dai_name = "Probe Extraction CPU DAI";
	links[0].codecs = dummy;
	links[0].num_codecs = 1;
	links[0].platforms = platform_component;
	links[0].num_platforms = ARRAY_SIZE(platform_component);
	links[0].nonatomic = 1;

	card->num_links = SOF_PROBES_NUM_DAI_LINKS;
	card->dai_link = links;
	card->dev = &vdev->dev;

	/* set idle_bias_off to prevent the core from resuming the card->dev */
	card->dapm.idle_bias_off = true;

	ret = devm_snd_soc_register_card(&vdev->dev, card);
	if (ret < 0) {
		dev_err(&vdev->dev,
			"error: Probes card register failed %d\n",
			ret);
		return ret;
	}

	/*
	 * Override the drvdata for the device set by the core to point to the
	 * client device pointer.
	 */
	dev_set_drvdata(&vdev->dev, cdev);

	/* enable runtime PM */
	pm_runtime_set_autosuspend_delay(&vdev->dev,
					 SOF_PROBES_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(&vdev->dev);
	pm_runtime_set_active(&vdev->dev);
	pm_runtime_enable(&vdev->dev);
	pm_runtime_mark_last_busy(&vdev->dev);
	pm_runtime_put_autosuspend(&vdev->dev);

	/* complete client device registration */
	complete(&cdev->probe_complete);

	return 0;
}

static int sof_probes_client_cleanup(struct virtbus_device *vdev)
{
	struct sof_client_dev *cdev = virtbus_dev_to_sof_client_dev(vdev);
	struct sof_probes_data *probes_client_data = cdev->data;

	pm_runtime_disable(&vdev->dev);
	debugfs_remove_recursive(probes_client_data->dfs_root);

	return 0;
}

static int sof_probes_client_remove(struct virtbus_device *vdev)
{
	return sof_probes_client_cleanup(vdev);
}

static void sof_probes_client_shutdown(struct virtbus_device *vdev)
{
	sof_probes_client_cleanup(vdev);
}

static const struct virtbus_dev_id sof_probes_virtbus_id_table[] = {
	{"sof-probes"},
	{},
};

static struct sof_client_drv sof_probes_test_client_drv = {
	.name = "sof-probes-client-drv",
	.type = SOF_CLIENT_AUDIO,
	.virtbus_drv = {
		.driver = {
			.name = "sof-probes-virtbus-drv",
		},
		.id_table = sof_probes_virtbus_id_table,
		.probe = sof_probes_client_probe,
		.remove = sof_probes_client_remove,
		.shutdown = sof_probes_client_shutdown,
	},
};

module_sof_client_driver(sof_probes_test_client_drv);

MODULE_DESCRIPTION("SOF Probes Client Driver");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_IMPORT_NS(SND_SOC_SOF_CLIENT);
MODULE_ALIAS("virtbus:sof-probes");
