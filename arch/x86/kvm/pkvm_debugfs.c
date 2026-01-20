// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "pkvm-host: " fmt

#include <linux/debugfs.h>
#include <linux/kvm_host.h>

void pkvm_init_debugfs(void)
{
	struct dentry *debugfs_dir = debugfs_create_dir("pkvm", NULL);

	if (IS_ERR_OR_NULL(debugfs_dir))
		pr_err("failed to add debugfs root\n");
}
