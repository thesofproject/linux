
#ifndef __INCLUDE_SOUND_SOF_IPC4_TRACE_H__
#define __INCLUDE_SOUND_SOF_IPC4_TRACE_H__

#define SOF_IPC4_BASE_FW	0

#define IPC4_DBOX_CAVS_25_SIZE 0x10000

#define INVALID_SLOT_RESOURCE_ID    0xffffffff
#define MEMORY_WINDOW_SLOTS_COUNT   15
#define MEMORY_WINDOW_SLOT_SIZE     0x1000
#define SLOT_DEBUG_LOG	0x474f4c00
#define SLOT_DEBUG_LOG_MASK	GENMASK(31, 8)
#define MAX_ALLOWED_LIBRARIES   16

/* ipc4 mtrace */
enum sof_mtrace_level
{
	L_CRITICAL = BIT(0),
	L_ERROR = BIT(1),
	L_WARNING = BIT(2),
	L_INFO = BIT(3),
	L_VERBOSE = BIT(4),
	L_DEFAULT = L_CRITICAL | L_ERROR |L_INFO
};

enum sof_mtrace_source
{
	S_INFRA = BIT(5),
	S_HAL = BIT(6),
	S_MODULE  = BIT(7),
	S_AUDIO = BIT(8),
	S_DEFAULT = S_INFRA | S_HAL | S_MODULE |S_AUDIO
};

struct sof_log_setting
{
    uint32_t aging_timer_period;
    uint32_t fifo_full_timer_period;
    uint32_t enable;
    uint32_t logs_priorities_mask[MAX_ALLOWED_LIBRARIES];
};

int sof_ipc4_init_mtrace(struct snd_sof_dev *sdev);
void sof_ipc4_enable_mtrace(struct snd_sof_dev *sdev, u32 module_id);
void sof_ipc4_disable_mtrace(struct snd_sof_dev *sdev, u32 module_id);
int sof_ipc4_mtrace_update_pos(struct snd_sof_dev *sdev);
#endif
