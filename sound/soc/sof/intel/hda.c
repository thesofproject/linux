// SPDX-License-Identifier: (GPL-2.0 OR BSD-3-Clause)
//
// This file is provided under a dual BSD/GPLv2 license.  When using or
// redistributing this file, you may do so under either license.
//
// Copyright(c) 2018 Intel Corporation. All rights reserved.
//
// Authors: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//	    Ranjani Sridharan <ranjani.sridharan@linux.intel.com>
//	    Rander Wang <rander.wang@intel.com>
//          Keyon Jie <yang.jie@linux.intel.com>
//

/*
 * Hardware interface for generic Intel audio DSP HDA IP
 */

#include <sound/hdaudio_ext.h>
#include <sound/hda_register.h>

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/soundwire/sdw_intel.h>
#include <sound/intel-nhlt.h>
#include <sound/sof.h>
#include <sound/sof/xtensa.h>
#include "../ops.h"
#include "hda.h"

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
#include <sound/soc-acpi-intel-match.h>
#endif

/* platform specific devices */
#include "shim.h"

#define IS_CFL(pci) ((pci)->vendor == 0x8086 && (pci)->device == 0xa348)
#define IS_CNL(pci) ((pci)->vendor == 0x8086 && (pci)->device == 0x9dc8)

#define EXCEPT_MAX_HDR_SIZE	0x400

#if IS_ENABLED(CONFIG_SOUNDWIRE_INTEL)

static int sdw_params_stream(struct device *dev,
			     struct sdw_intel_stream_params_data *params_data)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_soc_dai *d = params_data->dai;
	struct sof_ipc_dai_config config;
	struct sof_ipc_reply reply;
	int link_id = params_data->link_id;
	int alh_stream_id = params_data->alh_stream_id;
	int ret;
	u32 size = sizeof(config);

	memset(&config, 0, size);
	config.hdr.size = size;
	config.hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG;
	config.type = SOF_DAI_INTEL_ALH;
	config.dai_index = (link_id << 8) | (d->id);
	config.alh.stream_id = alh_stream_id;

	/* send message to DSP */
	ret = sof_ipc_tx_message(sdev->ipc,
				 config.hdr.cmd, &config, size, &reply,
				 sizeof(reply));
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: failed to set DAI hw_params for link %d dai->id %d ALH %d\n",
			link_id, d->id, alh_stream_id);
	}

	return ret;
}

static int sdw_free_stream(struct device *dev,
			   struct sdw_intel_stream_free_data *free_data)
{
	struct snd_sof_dev *sdev = dev_get_drvdata(dev);
	struct snd_soc_dai *d = free_data->dai;
	struct sof_ipc_dai_config config;
	struct sof_ipc_reply reply;
	int link_id = free_data->link_id;
	int ret;
	u32 size = sizeof(config);

	memset(&config, 0, size);
	config.hdr.size = size;
	config.hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG;
	config.type = SOF_DAI_INTEL_ALH;
	config.dai_index = (link_id << 8) | d->id;
	config.alh.stream_id = 0xFFFF; /* invalid value on purpose */

	/* send message to DSP */
	ret = sof_ipc_tx_message(sdev->ipc,
				 config.hdr.cmd, &config, size, &reply,
				 sizeof(reply));
	if (ret < 0) {
		dev_err(sdev->dev,
			"error: failed to free stream for link %d dai->id %d\n",
			link_id, d->id);
	}

	return ret;
}

static const struct sdw_intel_ops sdw_callback = {
	.params_stream = sdw_params_stream,
	.free_stream = sdw_free_stream,
};

static void hda_sdw_int_enable(struct snd_sof_dev *sdev, bool enable)
{
	if (enable) {
		snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
					HDA_DSP_REG_ADSPIC2,
					HDA_DSP_ADSPIC2_SNDW,
					HDA_DSP_ADSPIC2_SNDW);
	} else {
		snd_sof_dsp_update_bits(sdev, HDA_DSP_BAR,
					HDA_DSP_REG_ADSPIC2,
					HDA_DSP_ADSPIC2_SNDW,
					0);
	}
}

static int hda_sdw_acpi_scan(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hdev;
	acpi_handle handle;
	int ret;

	handle = ACPI_HANDLE(sdev->dev);

	/* save ACPI info for the probe step */
	hdev = sdev->pdata->hw_pdata;

	ret = sdw_intel_acpi_scan(handle, &hdev->info);
	if (ret < 0) {
		dev_err(sdev->dev, "%s failed\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int hda_sdw_probe(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hdev;
	struct sdw_intel_res res;
	acpi_handle handle;
	void *sdw;

	handle = ACPI_HANDLE(sdev->dev);

	hdev = sdev->pdata->hw_pdata;

	memset(&res, 0, sizeof(res));

	res.mmio_base = sdev->bar[HDA_DSP_BAR];
	res.irq = sdev->ipc_irq;
	res.handle = hdev->info.handle;
	res.parent = sdev->dev;
	res.ops = &sdw_callback;
	res.dev = sdev->dev;

	/*
	 * ops and arg fields are not populated for now,
	 * they will be needed when the DAI callbacks are
	 * provided
	 */

	/* we could filter links here if needed, e.g for quirks */
	res.count = hdev->info.count;
	res.link_mask = hdev->info.link_mask;

	sdw = sdw_intel_probe(&res);
	if (!sdw) {
		dev_err(sdev->dev, "error: SoundWire probe failed\n");
		return -EINVAL;
	}

	/* save context */
	hdev->sdw = sdw;

	return 0;
}

int hda_sdw_startup(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hdev;
	int ret;

	hdev = sdev->pdata->hw_pdata;

	ret = sdw_intel_startup(hdev->sdw);
	if (ret < 0)
		return ret;
	hda_sdw_int_enable(sdev, true);

	return ret;
}

static int hda_sdw_exit(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hdev;

	hdev = sdev->pdata->hw_pdata;

	hda_sdw_int_enable(sdev, false);

	if (hdev->sdw)
		sdw_intel_exit(hdev->sdw);
	hdev->sdw = NULL;

	return 0;
}
#endif

/*
 * Debug
 */

struct hda_dsp_msg_code {
	u32 code;
	const char *msg;
};

static bool hda_use_msi = IS_ENABLED(CONFIG_PCI);
#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG)
module_param_named(use_msi, hda_use_msi, bool, 0444);
MODULE_PARM_DESC(use_msi, "SOF HDA use PCI MSI mode");
#endif

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
static int hda_dmic_num = -1;
module_param_named(dmic_num, hda_dmic_num, int, 0444);
MODULE_PARM_DESC(dmic_num, "SOF HDA DMIC number");
#endif

static const struct hda_dsp_msg_code hda_dsp_rom_msg[] = {
	{HDA_DSP_ROM_FW_MANIFEST_LOADED, "status: manifest loaded"},
	{HDA_DSP_ROM_FW_FW_LOADED, "status: fw loaded"},
	{HDA_DSP_ROM_FW_ENTERED, "status: fw entered"},
	{HDA_DSP_ROM_CSE_ERROR, "error: cse error"},
	{HDA_DSP_ROM_CSE_WRONG_RESPONSE, "error: cse wrong response"},
	{HDA_DSP_ROM_IMR_TO_SMALL, "error: IMR too small"},
	{HDA_DSP_ROM_BASE_FW_NOT_FOUND, "error: base fw not found"},
	{HDA_DSP_ROM_CSE_VALIDATION_FAILED, "error: signature verification failed"},
	{HDA_DSP_ROM_IPC_FATAL_ERROR, "error: ipc fatal error"},
	{HDA_DSP_ROM_L2_CACHE_ERROR, "error: L2 cache error"},
	{HDA_DSP_ROM_LOAD_OFFSET_TO_SMALL, "error: load offset too small"},
	{HDA_DSP_ROM_API_PTR_INVALID, "error: API ptr invalid"},
	{HDA_DSP_ROM_BASEFW_INCOMPAT, "error: base fw incompatible"},
	{HDA_DSP_ROM_UNHANDLED_INTERRUPT, "error: unhandled interrupt"},
	{HDA_DSP_ROM_MEMORY_HOLE_ECC, "error: ECC memory hole"},
	{HDA_DSP_ROM_KERNEL_EXCEPTION, "error: kernel exception"},
	{HDA_DSP_ROM_USER_EXCEPTION, "error: user exception"},
	{HDA_DSP_ROM_UNEXPECTED_RESET, "error: unexpected reset"},
	{HDA_DSP_ROM_NULL_FW_ENTRY,	"error: null FW entry point"},
};

static void hda_dsp_get_status_skl(struct snd_sof_dev *sdev)
{
	u32 status;
	int i;

	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_ADSP_FW_STATUS_SKL);

	for (i = 0; i < ARRAY_SIZE(hda_dsp_rom_msg); i++) {
		if (status == hda_dsp_rom_msg[i].code) {
			dev_err(sdev->dev, "%s - code %8.8x\n",
				hda_dsp_rom_msg[i].msg, status);
			return;
		}
	}

	/* not for us, must be generic sof message */
	dev_dbg(sdev->dev, "unknown ROM status value %8.8x\n", status);
}

static void hda_dsp_get_status(struct snd_sof_dev *sdev)
{
	u32 status;
	int i;

	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_DSP_SRAM_REG_ROM_STATUS);

	for (i = 0; i < ARRAY_SIZE(hda_dsp_rom_msg); i++) {
		if (status == hda_dsp_rom_msg[i].code) {
			dev_err(sdev->dev, "%s - code %8.8x\n",
				hda_dsp_rom_msg[i].msg, status);
			return;
		}
	}

	/* not for us, must be generic sof message */
	dev_dbg(sdev->dev, "unknown ROM status value %8.8x\n", status);
}

static void hda_dsp_get_registers(struct snd_sof_dev *sdev,
				  struct sof_ipc_dsp_oops_xtensa *xoops,
				  struct sof_ipc_panic_info *panic_info,
				  u32 *stack, size_t stack_words)
{
	u32 offset = sdev->dsp_oops_offset;

	/* first read registers */
	sof_mailbox_read(sdev, offset, xoops, sizeof(*xoops));

	/* note: variable AR register array is not read */

	/* then get panic info */
	if (xoops->arch_hdr.totalsize > EXCEPT_MAX_HDR_SIZE) {
		dev_err(sdev->dev, "invalid header size 0x%x. FW oops is bogus\n",
			xoops->arch_hdr.totalsize);
		return;
	}
	offset += xoops->arch_hdr.totalsize;
	sof_block_read(sdev, sdev->mmio_bar, offset,
		       panic_info, sizeof(*panic_info));

	/* then get the stack */
	offset += sizeof(*panic_info);
	sof_block_read(sdev, sdev->mmio_bar, offset, stack,
		       stack_words * sizeof(u32));
}

void hda_dsp_dump_skl(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	struct sof_ipc_panic_info panic_info;
	u32 stack[HDA_DSP_STACK_DUMP_SIZE];
	u32 status, panic;

	/* try APL specific status message types first */
	hda_dsp_get_status_skl(sdev);

	/* now try generic SOF status messages */
	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_ADSP_ERROR_CODE_SKL);

	/*TODO: Check: there is no define in spec, but it is used in the code*/
	panic = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				 HDA_ADSP_ERROR_CODE_SKL + 0x4);

	if (sdev->boot_complete) {
		hda_dsp_get_registers(sdev, &xoops, &panic_info, stack,
				      HDA_DSP_STACK_DUMP_SIZE);
		snd_sof_get_status(sdev, status, panic, &xoops, &panic_info,
				   stack, HDA_DSP_STACK_DUMP_SIZE);
	} else {
		dev_err(sdev->dev, "error: status = 0x%8.8x panic = 0x%8.8x\n",
			status, panic);
		hda_dsp_get_status_skl(sdev);
	}
}

void hda_dsp_dump(struct snd_sof_dev *sdev, u32 flags)
{
	struct sof_ipc_dsp_oops_xtensa xoops;
	struct sof_ipc_panic_info panic_info;
	u32 stack[HDA_DSP_STACK_DUMP_SIZE];
	u32 status, panic;

	/* try APL specific status message types first */
	hda_dsp_get_status(sdev);

	/* now try generic SOF status messages */
	status = snd_sof_dsp_read(sdev, HDA_DSP_BAR,
				  HDA_DSP_SRAM_REG_FW_STATUS);
	panic = snd_sof_dsp_read(sdev, HDA_DSP_BAR, HDA_DSP_SRAM_REG_FW_TRACEP);

	if (sdev->boot_complete) {
		hda_dsp_get_registers(sdev, &xoops, &panic_info, stack,
				      HDA_DSP_STACK_DUMP_SIZE);
		snd_sof_get_status(sdev, status, panic, &xoops, &panic_info,
				   stack, HDA_DSP_STACK_DUMP_SIZE);
	} else {
		dev_err(sdev->dev, "error: status = 0x%8.8x panic = 0x%8.8x\n",
			status, panic);
		hda_dsp_get_status(sdev);
	}
}

void hda_ipc_irq_dump(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	u32 adspis;
	u32 intsts;
	u32 intctl;
	u32 ppsts;
	u8 rirbsts;

	/* read key IRQ stats and config registers */
	adspis = snd_sof_dsp_read(sdev, HDA_DSP_BAR, HDA_DSP_REG_ADSPIS);
	intsts = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTSTS);
	intctl = snd_sof_dsp_read(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTCTL);
	ppsts = snd_sof_dsp_read(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPSTS);
	rirbsts = snd_hdac_chip_readb(bus, RIRBSTS);

	dev_err(sdev->dev,
		"error: hda irq intsts 0x%8.8x intlctl 0x%8.8x rirb %2.2x\n",
		intsts, intctl, rirbsts);
	dev_err(sdev->dev,
		"error: dsp irq ppsts 0x%8.8x adspis 0x%8.8x\n",
		ppsts, adspis);
}

void hda_ipc_dump(struct snd_sof_dev *sdev)
{
	u32 hipcie;
	u32 hipct;
	u32 hipcctl;

	hda_ipc_irq_dump(sdev);

	/* read IPC status */
	hipcie = snd_sof_dsp_read(sdev, HDA_DSP_BAR, HDA_DSP_REG_HIPCIE);
	hipct = snd_sof_dsp_read(sdev, HDA_DSP_BAR, HDA_DSP_REG_HIPCT);
	hipcctl = snd_sof_dsp_read(sdev, HDA_DSP_BAR, HDA_DSP_REG_HIPCCTL);

	/* dump the IPC regs */
	/* TODO: parse the raw msg */
	dev_err(sdev->dev,
		"error: host status 0x%8.8x dsp status 0x%8.8x mask 0x%8.8x\n",
		hipcie, hipct, hipcctl);
}

static int hda_init(struct snd_sof_dev *sdev)
{
	struct hda_bus *hbus;
	struct hdac_bus *bus;
	struct pci_dev *pci = to_pci_dev(sdev->dev);
	int ret;

	hbus = sof_to_hbus(sdev);
	bus = sof_to_bus(sdev);

	/* HDA bus init */
	sof_hda_bus_init(bus, &pci->dev);

	/* Workaround for a communication error on CFL (bko#199007) and CNL */
	if (IS_CFL(pci) || IS_CNL(pci))
		bus->polling_mode = 1;

	bus->use_posbuf = 1;
	bus->bdl_pos_adj = 0;

	mutex_init(&hbus->prepare_mutex);
	hbus->pci = pci;
	hbus->mixer_assigned = -1;
	hbus->modelname = "sofbus";

	/* initialise hdac bus */
	bus->addr = pci_resource_start(pci, 0);
#if IS_ENABLED(CONFIG_PCI)
	bus->remap_addr = pci_ioremap_bar(pci, 0);
#endif
	if (!bus->remap_addr) {
		dev_err(bus->dev, "error: ioremap error\n");
		return -ENXIO;
	}

	/* HDA base */
	sdev->bar[HDA_DSP_HDA_BAR] = bus->remap_addr;

	/* get controller capabilities */
	ret = hda_dsp_ctrl_get_caps(sdev);
	if (ret < 0)
		dev_err(sdev->dev, "error: get caps error\n");

	return ret;
}

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)

static int check_nhlt_dmic(struct snd_sof_dev *sdev)
{
	struct nhlt_acpi_table *nhlt;
	int dmic_num;

	nhlt = intel_nhlt_init(sdev->dev);
	if (nhlt) {
		dmic_num = intel_nhlt_get_dmic_geo(sdev->dev, nhlt);
		intel_nhlt_free(nhlt);
		if (dmic_num == 2 || dmic_num == 4)
			return dmic_num;
	}

	return 0;
}

static const char *fixup_tplg_name(struct snd_sof_dev *sdev,
				   const char *sof_tplg_filename,
				   const char *idisp_str,
				   const char *dmic_str)
{
	const char *tplg_filename = NULL;
	char *filename;
	char *split_ext;

	filename = devm_kstrdup(sdev->dev, sof_tplg_filename, GFP_KERNEL);
	if (!filename)
		return NULL;

	/* this assumes a .tplg extension */
	split_ext = strsep(&filename, ".");
	if (split_ext) {
		tplg_filename = devm_kasprintf(sdev->dev, GFP_KERNEL,
					       "%s%s%s.tplg",
					       split_ext, idisp_str, dmic_str);
		if (!tplg_filename)
			return NULL;
	}
	return tplg_filename;
}

#endif

static int hda_init_caps(struct snd_sof_dev *sdev)
{
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct snd_sof_pdata *pdata = sdev->pdata;
	struct snd_soc_acpi_mach *mach;
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	struct hdac_ext_link *hlink;
	struct snd_soc_acpi_mach *hda_mach;
	struct snd_soc_acpi_mach_params *mach_params;
	const char *tplg_filename;
	const char *idisp_str;
	const char *dmic_str;
	int dmic_num;
	int codec_num = 0;
	int i;
#endif
	struct sof_intel_hda_dev *hdev = pdata->hw_pdata;
	u32 link_mask;
	int ret = 0;

	device_disable_async_suspend(bus->dev);

	/* check if dsp is there */
	if (bus->ppcap)
		dev_dbg(sdev->dev, "PP capability, will probe DSP later.\n");

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	/* init i915 and HDMI codecs */
	ret = hda_codec_i915_init(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: init i915 and HDMI codec failed\n");
		return ret;
	}
#endif

	/* Init HDA controller after i915 init */
	ret = hda_dsp_ctrl_init_chip(sdev, true);
	if (ret < 0) {
		dev_err(bus->dev, "error: init chip failed with ret: %d\n",
			ret);
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
		hda_codec_i915_exit(sdev);
#endif
		return ret;
	}

	/* scan SoundWire capabilities exposed by DSDT */
	ret = hda_sdw_acpi_scan(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: SoundWire ACPI scan error\n");
		return ret;
	}

	link_mask = hdev->info.link_mask;
	if (!link_mask)
		goto skip_soundwire;

	/*
	 * probe/allocate SoundWire resources.
	 * The hardware configuration takes place in hda_sdw_startup
	 * after power rails are enabled.
	 * It's entirely possible to have a mix of I2S/DMIC/SoundWire
	 * devices, so we allocate the resources in all cases.
	 */
	ret = hda_sdw_probe(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: SoundWire probe error\n");
		return ret;
	}

	/*
	 * Select SoundWire machine driver if needed using the
	 * alternate tables. This case deals with SoundWire-only
	 * machines, for mixed cases with I2C/I2S the detection relies
	 * on the HID list.
	 */
	if (!pdata->machine) {
		mach = pdata->desc->alt_machines;
		while (mach && mach->link_mask && mach->link_mask != link_mask)
			mach++;
		if (mach && mach->link_mask) {
			dev_dbg(bus->dev,
				"SoundWire machine driver %s topology %s\n",
				mach->drv_name,
				mach->sof_tplg_filename);
			pdata->machine = mach;
			mach->mach_params.platform = dev_name(sdev->dev);
			pdata->fw_filename = mach->sof_fw_filename;
			pdata->tplg_filename = mach->sof_tplg_filename;
		} else {
			dev_info(sdev->dev,
				 "No SoundWire machine driver found\n");
		}
	}

skip_soundwire:

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	if (bus->mlcap)
		snd_hdac_ext_bus_get_ml_capabilities(bus);

	/* codec detection */
	if (!bus->codec_mask) {
		dev_info(bus->dev, "no hda codecs found!\n");
	} else {
		dev_info(bus->dev, "hda codecs found, mask %lx\n",
			 bus->codec_mask);

		for (i = 0; i < HDA_MAX_CODECS; i++) {
			if (bus->codec_mask & (1 << i))
				codec_num++;
		}

		/*
		 * If no machine driver is found, then:
		 *
		 * hda machine driver is used if :
		 * 1. there is one HDMI codec and one external HDAudio codec
		 * 2. only HDMI codec
		 */
		if (!pdata->machine && codec_num <= 2 &&
		    HDA_IDISP_CODEC(bus->codec_mask)) {
			hda_mach = snd_soc_acpi_intel_hda_machines;
			pdata->machine = hda_mach;

			/* topology: use the info from hda_machines */
			pdata->tplg_filename =
				hda_mach->sof_tplg_filename;

			/* firmware: pick the first in machine list */
			mach = pdata->desc->machines;
			pdata->fw_filename = mach->sof_fw_filename;

			dev_info(bus->dev, "using HDA machine driver %s now\n",
				 hda_mach->drv_name);

			if (codec_num == 1)
				idisp_str = "-idisp";
			else
				idisp_str = "";

			/* first check NHLT for DMICs */
			dmic_num = check_nhlt_dmic(sdev);

			/* allow for module parameter override */
			if (hda_dmic_num != -1)
				dmic_num = hda_dmic_num;

			switch (dmic_num) {
			case 2:
				dmic_str = "-2ch";
				break;
			case 4:
				dmic_str = "-4ch";
				break;
			default:
				dmic_num = 0;
				dmic_str = "";
				break;
			}

			tplg_filename = pdata->tplg_filename;
			tplg_filename = fixup_tplg_name(sdev, tplg_filename,
							idisp_str, dmic_str);
			if (!tplg_filename) {
				hda_codec_i915_exit(sdev);
				return ret;
			}
			pdata->tplg_filename = tplg_filename;
		}
	}

	/* used by hda machine driver to create dai links */
	if (pdata->machine) {
		mach_params = (struct snd_soc_acpi_mach_params *)
			&pdata->machine->mach_params;
		mach_params->codec_mask = bus->codec_mask;
		mach_params->platform = dev_name(sdev->dev);
	}

	/* create codec instances */
	hda_codec_probe_bus(sdev);

	hda_codec_i915_put(sdev);

	/*
	 * we are done probing so decrement link counts
	 */
	list_for_each_entry(hlink, &bus->hlink_list, list)
		snd_hdac_ext_bus_link_put(bus, hlink);
#endif
	return 0;
}

static const struct sof_intel_dsp_desc
	*get_chip_info(struct snd_sof_pdata *pdata)
{
	const struct sof_dev_desc *desc = pdata->desc;
	const struct sof_intel_dsp_desc *chip_info;

	chip_info = desc->chip_info;

	return chip_info;
}

int hda_dsp_probe(struct snd_sof_dev *sdev)
{
	struct pci_dev *pci = to_pci_dev(sdev->dev);
	struct sof_intel_hda_dev *hdev;
	struct hdac_bus *bus;
	const struct sof_intel_dsp_desc *chip;
	int ret = 0;

	/*
	 * detect DSP by checking class/subclass/prog-id information
	 * class=04 subclass 03 prog-if 00: no DSP, legacy driver is required
	 * class=04 subclass 01 prog-if 00: DSP is present
	 *   (and may be required e.g. for DMIC or SSP support)
	 * class=04 subclass 03 prog-if 80: either of DSP or legacy mode works
	 */
	if (pci->class == 0x040300) {
		dev_err(sdev->dev, "error: the DSP is not enabled on this platform, aborting probe\n");
		return -ENODEV;
	} else if (pci->class != 0x040100 && pci->class != 0x040380) {
		dev_err(sdev->dev, "error: unknown PCI class/subclass/prog-if 0x%06x found, aborting probe\n", pci->class);
		return -ENODEV;
	}
	dev_info(sdev->dev, "DSP detected with PCI class/subclass/prog-if 0x%06x\n", pci->class);

	chip = get_chip_info(sdev->pdata);
	if (!chip) {
		dev_err(sdev->dev, "error: no such device supported, chip id:%x\n",
			pci->device);
		ret = -EIO;
		goto err;
	}

	hdev = devm_kzalloc(sdev->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;
	sdev->pdata->hw_pdata = hdev;
	hdev->desc = chip;

	hdev->dmic_dev = platform_device_register_data(sdev->dev, "dmic-codec",
						       PLATFORM_DEVID_NONE,
						       NULL, 0);
	if (IS_ERR(hdev->dmic_dev)) {
		dev_err(sdev->dev, "error: failed to create DMIC device\n");
		return PTR_ERR(hdev->dmic_dev);
	}

	/*
	 * use position update IPC if either it is forced
	 * or we don't have other choice
	 */
#if IS_ENABLED(CONFIG_SND_SOC_SOF_DEBUG_FORCE_IPC_POSITION)
	hdev->no_ipc_position = 0;
#else
	hdev->no_ipc_position = sof_ops(sdev)->pcm_pointer ? 1 : 0;
#endif

	/* set up HDA base */
	bus = sof_to_bus(sdev);
	ret = hda_init(sdev);
	if (ret < 0)
		goto hdac_bus_unmap;

	/* DSP base */
#if IS_ENABLED(CONFIG_PCI)
	sdev->bar[HDA_DSP_BAR] = pci_ioremap_bar(pci, HDA_DSP_BAR);
#endif
	if (!sdev->bar[HDA_DSP_BAR]) {
		dev_err(sdev->dev, "error: ioremap error\n");
		ret = -ENXIO;
		goto hdac_bus_unmap;
	}

	sdev->mmio_bar = HDA_DSP_BAR;
	sdev->mailbox_bar = HDA_DSP_BAR;

	/* allow 64bit DMA address if supported by H/W */
	if (!dma_set_mask(&pci->dev, DMA_BIT_MASK(64))) {
		dev_dbg(sdev->dev, "DMA mask is 64 bit\n");
		dma_set_coherent_mask(&pci->dev, DMA_BIT_MASK(64));
	} else {
		dev_dbg(sdev->dev, "DMA mask is 32 bit\n");
		dma_set_mask(&pci->dev, DMA_BIT_MASK(32));
		dma_set_coherent_mask(&pci->dev, DMA_BIT_MASK(32));
	}

	/* init streams */
	ret = hda_dsp_stream_init(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to init streams\n");
		/*
		 * not all errors are due to memory issues, but trying
		 * to free everything does not harm
		 */
		goto free_streams;
	}

	/*
	 * register our IRQ
	 * let's try to enable msi firstly
	 * if it fails, use legacy interrupt mode
	 * TODO: support msi multiple vectors
	 */
	if (hda_use_msi && pci_alloc_irq_vectors(pci, 1, 1, PCI_IRQ_MSI) > 0) {
		dev_info(sdev->dev, "use msi interrupt mode\n");
		hdev->irq = pci_irq_vector(pci, 0);
		/* ipc irq number is the same of hda irq */
		sdev->ipc_irq = hdev->irq;
		/* initialised to "false" by kzalloc() */
		sdev->msi_enabled = true;
	}

	if (!sdev->msi_enabled) {
		dev_info(sdev->dev, "use legacy interrupt mode\n");
		/*
		 * in IO-APIC mode, hda->irq and ipc_irq are using the same
		 * irq number of pci->irq
		 */
		hdev->irq = pci->irq;
		sdev->ipc_irq = pci->irq;
	}

	dev_dbg(sdev->dev, "using HDA IRQ %d\n", hdev->irq);
	ret = request_threaded_irq(hdev->irq, hda_dsp_stream_interrupt,
				   hda_dsp_stream_threaded_handler,
				   IRQF_SHARED, "AudioHDA", bus);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register HDA IRQ %d\n",
			hdev->irq);
		goto free_irq_vector;
	}

	dev_dbg(sdev->dev, "using IPC IRQ %d\n", sdev->ipc_irq);
	ret = request_threaded_irq(sdev->ipc_irq, hda_dsp_ipc_irq_handler,
				   sof_ops(sdev)->irq_thread, IRQF_SHARED,
				   "AudioDSP", sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: failed to register IPC IRQ %d\n",
			sdev->ipc_irq);
		goto free_hda_irq;
	}

	pci_set_master(pci);
	synchronize_irq(pci->irq);

	/*
	 * clear TCSEL to clear playback on some HD Audio
	 * codecs. PCI TCSEL is defined in the Intel manuals.
	 */
	snd_sof_pci_update_bits(sdev, PCI_TCSEL, 0x07, 0);

	/* init HDA capabilities */
	ret = hda_init_caps(sdev);
	if (ret < 0)
		goto free_ipc_irq;

	/* enable ppcap interrupt */
	hda_dsp_ctrl_ppcap_enable(sdev, true);
	hda_dsp_ctrl_ppcap_int_enable(sdev, true);

	/* initialize waitq for code loading */
	init_waitqueue_head(&sdev->waitq);

	/* set default mailbox offset for FW ready message */
	sdev->dsp_box.offset = HDA_DSP_MBOX_UPLINK_OFFSET;

#ifdef SOUNDWIRE_POWER_FIRST
	/* need to power-up core before setting-up capabilities */
	ret = hda_dsp_core_power_up(sdev, HDA_DSP_CORE_MASK(0));
	if (ret < 0) {
		dev_err(sdev->dev, "error: could not power-up DSP subsystem\n");
		goto sdw_exit;
	}

	ret = hda_sdw_startup(sdev);
	if (ret < 0) {
		dev_err(sdev->dev, "error: could not startup SoundWire links\n");
		goto core_power_down;
	}
#endif
	return 0;

#ifdef SOUNDWIRE_POWER_FIRST
core_power_down:
	hda_dsp_core_power_down(sdev, HDA_DSP_CORE_MASK(0));
sdw_exit:
	hda_sdw_exit(sdev);
#endif
free_ipc_irq:
	free_irq(sdev->ipc_irq, sdev);
free_hda_irq:
	free_irq(hdev->irq, bus);
free_irq_vector:
	if (sdev->msi_enabled)
		pci_free_irq_vectors(pci);
free_streams:
	hda_dsp_stream_free(sdev);
/* dsp_unmap: not currently used */
	iounmap(sdev->bar[HDA_DSP_BAR]);
hdac_bus_unmap:
	iounmap(bus->remap_addr);
err:
	return ret;
}

int hda_dsp_remove(struct snd_sof_dev *sdev)
{
	struct sof_intel_hda_dev *hda = sdev->pdata->hw_pdata;
	struct hdac_bus *bus = sof_to_bus(sdev);
	struct pci_dev *pci = to_pci_dev(sdev->dev);
	const struct sof_intel_dsp_desc *chip = hda->desc;

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	/* codec removal, invoke bus_device_remove */
	snd_hdac_ext_bus_device_remove(bus);
#endif

	hda_sdw_exit(sdev);

	if (!IS_ERR_OR_NULL(hda->dmic_dev))
		platform_device_unregister(hda->dmic_dev);

	/* disable DSP IRQ */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				SOF_HDA_PPCTL_PIE, 0);

	/* disable CIE and GIE interrupts */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_HDA_BAR, SOF_HDA_INTCTL,
				SOF_HDA_INT_CTRL_EN | SOF_HDA_INT_GLOBAL_EN, 0);

	/* disable cores */
	if (chip)
		hda_dsp_core_reset_power_down(sdev, chip->cores_mask);

	/* disable DSP */
	snd_sof_dsp_update_bits(sdev, HDA_DSP_PP_BAR, SOF_HDA_REG_PP_PPCTL,
				SOF_HDA_PPCTL_GPROCEN, 0);

	free_irq(sdev->ipc_irq, sdev);
	free_irq(hda->irq, bus);
	if (sdev->msi_enabled)
		pci_free_irq_vectors(pci);

	hda_dsp_stream_free(sdev);
#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	snd_hdac_link_free_all(bus);
#endif

	iounmap(sdev->bar[HDA_DSP_BAR]);
	iounmap(bus->remap_addr);

#if IS_ENABLED(CONFIG_SND_SOC_SOF_HDA)
	snd_hdac_ext_bus_exit(bus);
#endif
	hda_codec_i915_exit(sdev);

	return 0;
}

MODULE_LICENSE("Dual BSD/GPL");
