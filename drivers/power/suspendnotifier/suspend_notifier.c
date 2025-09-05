// SPDX-License-Identifier: GPL-2.0

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/suspend.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define SUSPEND_EVENT 1
#define RESUME_EVENT 2

struct pm_event {
	u64 event_code;
	u64 token;      // Unique token for an event
};

struct suspend_notifier_device {
	struct mutex pm_event_lock;
	struct pm_event current_pm_event;
	u64 token_counter;
	int file_open;

	// Wait queue for the userspace process to block on
	struct wait_queue_head read_wq;
	int event_ready;
	// Wait queue for this notifier to block on ack from userspace
	struct wait_queue_head ack_wq;
	int event_acknowledged;

	// From notifier_block to get the data
	struct notifier_block suspend_notifier;
};

static bool check_ack_or_file_closed(struct suspend_notifier_device *drv_data)
{
	bool cond;

	mutex_lock(&drv_data->pm_event_lock);
	cond = !drv_data->file_open || drv_data->event_acknowledged;
	mutex_unlock(&drv_data->pm_event_lock);

	return cond;
}

// Callback for PM's suspend notification
static int suspend_notifier_cb(struct notifier_block *nb, unsigned long action, void *data)
{
	struct suspend_notifier_device *drv_data =
		container_of(nb, struct suspend_notifier_device, suspend_notifier);
	long timeout;
	int ret = NOTIFY_OK;

	mutex_lock(&drv_data->pm_event_lock);

	switch (action) {
	case PM_SUSPEND_PREPARE:
		drv_data->current_pm_event.event_code = SUSPEND_EVENT;
		drv_data->event_ready = 1;
		break;
	case PM_POST_SUSPEND:
		drv_data->current_pm_event.event_code = RESUME_EVENT;
		drv_data->event_ready = 1;
		break;
	default:
		mutex_unlock(&drv_data->pm_event_lock);
		return NOTIFY_OK;
	}
	drv_data->current_pm_event.token = drv_data->token_counter++;
	drv_data->event_acknowledged = 0;
	wake_up_interruptible(&drv_data->read_wq);
	mutex_unlock(&drv_data->pm_event_lock);

	// If suspend is imminent and there is client wait suspend preparation for
	// at most 500ms. Waiting stops if file is closed or writer sends ack.
	if (action == PM_SUSPEND_PREPARE) {
		timeout = wait_event_interruptible_timeout(drv_data->ack_wq,
			check_ack_or_file_closed(drv_data),
			msecs_to_jiffies(500));

		if (timeout == 0)
			pr_err("suspend_notifier: Timed out waiting for userspace acknowledgment!\n");
		if (timeout == -ERESTARTSYS) {
			pr_warn("suspend_notifier: Caught signal while waiting for userspace acknowledgment.\n");
			ret = notifier_from_errno(-EINTR);
		}
		mutex_lock(&drv_data->pm_event_lock);
		drv_data->current_pm_event.token = 0;
		mutex_unlock(&drv_data->pm_event_lock);
	}

	return ret;
}

static bool check_event_ready(struct suspend_notifier_device *drv_data)
{
	bool cond;

	mutex_lock(&drv_data->pm_event_lock);
	cond = !!drv_data->event_ready;
	mutex_unlock(&drv_data->pm_event_lock);

	return cond;
}

static struct miscdevice misc_device;

// File operations
static ssize_t device_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct suspend_notifier_device *drv_data = file->private_data;
	int ret;

	if (count < sizeof(drv_data->current_pm_event))
		return -EINVAL;

	// Block until the notifier callback signals an event.
	ret = wait_event_interruptible(drv_data->read_wq, check_event_ready(drv_data));
	if (ret == -ERESTARTSYS)
		return -EINTR;

	// Atomically consume the event.
	mutex_lock(&drv_data->pm_event_lock);
	if (copy_to_user(buf, &drv_data->current_pm_event, sizeof(drv_data->current_pm_event)))
		ret = -EFAULT;
	else
		ret = sizeof(drv_data->current_pm_event);
	drv_data->event_ready = 0; // Reset for next event
	mutex_unlock(&drv_data->pm_event_lock);

	return ret;
}

static ssize_t device_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct suspend_notifier_device *drv_data = file->private_data;
	u64 received_token;

	if (count != sizeof(received_token))
		return -EINVAL;
	if (copy_from_user(&received_token, buf, sizeof(received_token)))
		return -EFAULT;

	// Check if the received token matches the current event's token
	mutex_lock(&drv_data->pm_event_lock);
	if (drv_data->current_pm_event.token != 0 &&
			drv_data->current_pm_event.token == received_token) {
		drv_data->event_acknowledged = 1;
		wake_up_interruptible(&drv_data->ack_wq);
	}
	mutex_unlock(&drv_data->pm_event_lock);

	return sizeof(received_token);
}

static int device_open(struct inode *inode, struct file *file)
{
	struct suspend_notifier_device *drv_data = dev_get_drvdata(misc_device.this_device);
	int status = 0;

	if (!drv_data)
		return -ENODEV;
	file->private_data = drv_data;

	mutex_lock(&drv_data->pm_event_lock);
	if (drv_data->file_open)  {
		status = -EBUSY;
		goto done;
	}
	drv_data->file_open = 1;
done:
	mutex_unlock(&drv_data->pm_event_lock);
	return status;
}

static int device_release(struct inode *inode, struct file *file)
{
	struct suspend_notifier_device *drv_data = file->private_data;

	mutex_lock(&drv_data->pm_event_lock);
	drv_data->file_open = 0;
	// Wake up the callback thread in case it was waiting for a response
	wake_up_interruptible(&drv_data->ack_wq);
	mutex_unlock(&drv_data->pm_event_lock);

	return 0;
}

static const struct file_operations fops = {
	.owner = THIS_MODULE,
	.read  = device_read,
	.write = device_write,
	.open = device_open,
	.release = device_release,
};

static struct miscdevice misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = "suspend_notifier",
	.fops  = &fops,
};

static int __init suspend_notifier_init(void)
{
	int ret;
	struct suspend_notifier_device *drv_data;

	ret = misc_register(&misc_device);
	if (ret) {
		pr_err("suspend_notifier: Failed to register misc device\n");
		return ret;
	}

	drv_data = devm_kzalloc(misc_device.this_device, sizeof(*drv_data), GFP_KERNEL);
	if (!drv_data) {
		misc_deregister(&misc_device);
		return -ENOMEM;
	}

	mutex_init(&drv_data->pm_event_lock);
	init_waitqueue_head(&drv_data->read_wq);
	init_waitqueue_head(&drv_data->ack_wq);
	drv_data->token_counter = 1;
	drv_data->suspend_notifier.notifier_call = suspend_notifier_cb;
	dev_set_drvdata(misc_device.this_device, drv_data);
	ret = register_pm_notifier(&drv_data->suspend_notifier);
	if (ret) {
		pr_err("suspend_notifier: Failed to register PM notifier\n");
		misc_deregister(&misc_device);
		return ret;
	}
	return 0;
}
module_init(suspend_notifier_init);

static void __exit suspend_notifier_exit(void)
{
	struct suspend_notifier_device *drv_data = dev_get_drvdata(misc_device.this_device);

	unregister_pm_notifier(&drv_data->suspend_notifier);
	misc_deregister(&misc_device);
}
module_exit(suspend_notifier_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yanye Li");
MODULE_DESCRIPTION("suspend/resume notifier module.");
