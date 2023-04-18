// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/device.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/gzvm_drv.h>

static struct gzvm_driver gzvm_drv = {
	.drv_version = {
		.major = GZVM_DRV_MAJOR_VERSION,
		.minor = GZVM_DRV_MINOR_VERSION,
		.sub = 0,
	},
};

/**
 * gzvm_err_to_errno() - Convert geniezone return value to standard errno
 *
 * @err: Return value from geniezone function return
 *
 * Return: Standard errno
 */
int gzvm_err_to_errno(unsigned long err)
{
	int gz_err = (int)err;

	switch (gz_err) {
	case 0:
		return 0;
	case ERR_NO_MEMORY:
		return -ENOMEM;
	case ERR_NOT_SUPPORTED:
		fallthrough;
	case ERR_NOT_IMPLEMENTED:
		return -EOPNOTSUPP;
	case ERR_FAULT:
		return -EFAULT;
	default:
		break;
	}

	return -EINVAL;
}

/**
 * gzvm_dev_ioctl_check_extension() - Check if given capability is support
 *				      or not
 *
 * @gzvm: Pointer to struct gzvm
 * @args: Pointer in u64 from userspace
 *
 * Return:
 * * 0			- Supported, no error
 * * -EOPNOTSUPP	- Unsupported
 * * -EFAULT		- Failed to get data from userspace
 */
long gzvm_dev_ioctl_check_extension(struct gzvm *gzvm, unsigned long args)
{
	__u64 cap;
	void __user *argp = (void __user *)args;

	if (copy_from_user(&cap, argp, sizeof(__u64)))
		return -EFAULT;
	return gzvm_arch_check_extension(gzvm, cap, argp);
}

static long gzvm_dev_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long user_args)
{
	switch (cmd) {
	case GZVM_CREATE_VM:
		return gzvm_dev_ioctl_create_vm(&gzvm_drv, user_args);
	case GZVM_CHECK_EXTENSION:
		return gzvm_dev_ioctl_check_extension(NULL, user_args);
	default:
		break;
	}

	return -ENOTTY;
}

static const struct file_operations gzvm_chardev_ops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = gzvm_dev_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice gzvm_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = KBUILD_MODNAME,
	.fops = &gzvm_chardev_ops,
};

static int gzvm_drv_probe(struct platform_device *pdev)
{
	int ret;

	if (gzvm_arch_probe(gzvm_drv.drv_version, &gzvm_drv.hyp_version) != 0) {
		dev_err(&pdev->dev, "Not found available conduit\n");
		return -ENODEV;
	}

	ret = misc_register(&gzvm_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register to misc device\n");
		return ret;
	}

	gzvm_drv.dev = gzvm_dev.this_device;
	dev_dbg(gzvm_dev.this_device,
		"Found GenieZone hypervisor version %u.%u.%llu\n",
		gzvm_drv.hyp_version.major, gzvm_drv.hyp_version.minor,
		gzvm_drv.hyp_version.sub);

	ret = gzvm_drv_irqfd_init();
	if (ret)
		goto err_deregister;

	return 0;

err_deregister:
	misc_deregister(&gzvm_dev);
	return ret;
}

static void gzvm_drv_remove(struct platform_device *pdev)
{
	gzvm_drv_irqfd_exit();
	misc_deregister(&gzvm_dev);
}

static const struct of_device_id gzvm_of_match[] = {
	{ .compatible = "mediatek,geniezone" },
	{/* sentinel */},
};

static struct platform_driver gzvm_driver = {
	.probe = gzvm_drv_probe,
	.remove = gzvm_drv_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.of_match_table = gzvm_of_match,
	},
};

module_platform_driver(gzvm_driver);

MODULE_DEVICE_TABLE(of, gzvm_of_match);
MODULE_AUTHOR("MediaTek");
MODULE_DESCRIPTION("GenieZone interface for VMM");
MODULE_LICENSE("GPL");
