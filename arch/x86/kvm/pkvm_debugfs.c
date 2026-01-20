// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "pkvm-host: " fmt

#include <linux/debugfs.h>
#include <linux/kvm_host.h>
#include <asm/kvm_pkvm.h>

static void enable_vmexit_trace_func(void *data)
{
	u64 val;

	if (!data)
		return;

	val = *(u64 *)data;
	pkvm_hypercall(enable_vmexit_trace, val);
}

static int enable_vmexit_trace(void *data, u64 val)
{
	int cpu;

	for_each_possible_cpu(cpu)
		smp_call_function_single(cpu, enable_vmexit_trace_func, &val, true);

	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(enable_vmexit_trace_fops, NULL, enable_vmexit_trace, "%llu\n");

struct debugfs_item {
	const char *name;
	const umode_t mode;
	const struct file_operations *fops;
	struct dentry *dentry;
};

struct debugfs_item debugfs_files[] = {
	{ "enable_vmexit_trace", 0222, &enable_vmexit_trace_fops},
	{ NULL }
};

void pkvm_init_debugfs(void)
{
	struct dentry *debugfs_dir = debugfs_create_dir("pkvm", NULL);
	struct debugfs_item *p;

	if (IS_ERR_OR_NULL(debugfs_dir)) {
		pr_err("failed to add debugfs root\n");
		return;
	}

	for (p = debugfs_files; p->name; ++p) {
		p->dentry = debugfs_create_file(p->name, p->mode,
						debugfs_dir,
						NULL, p->fops);
		if (IS_ERR_OR_NULL(p->dentry)) {
			pr_err("failed to add debugfs %s\n", p->name);
			goto out_dir;
		}
	}

	return;

out_dir:
	for (p = debugfs_files; p->dentry; ++p) {
		debugfs_remove(p->dentry);
		p->dentry = NULL;
	}
	debugfs_remove(debugfs_dir);
}
