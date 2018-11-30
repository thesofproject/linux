#ifndef HW_SPI_H
#define HW_SPI_H

#include <linux/wait.h>
#include <linux/workqueue.h>

extern const struct snd_sof_dsp_ops snd_sof_spi_ops;

struct snd_sof_spi {
	struct sof_platform_priv *sof_plt;
	struct work_struct wr_work;
	struct workqueue_struct *wr_wq;
	struct wait_queue_head wq;
	u32 msg_hdr;
	size_t wr_size;
	u8 *ipc_buf;
	bool fw_loading;
	bool wake;
};

#endif
