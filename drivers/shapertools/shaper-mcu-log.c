/*
 * Copyright 2020 Shaper Tools
 */

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/rpmsg.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/skbuff.h>
#include <linux/list.h>

#include <shapertools/shaper-mcu-log.h>

#define SHAPER_MCU_LOG_MARKER 0x01377310UL

struct shaper_mcu_log_platform_device
{
	unsigned int marker;

	struct platform_device *parent;
	struct device *dev;
	struct cdev cdev;
	dev_t devt;

	struct sk_buff_head data_queue;
	wait_queue_head_t read_queue;

	struct list_head node;
};

struct shaper_mcu_log_rpmsg
{
	unsigned int marker;

	struct shaper_mcu_log_device *log_dev;
	struct rpmsg_device *rpmsg_dev;
	struct list_head node;
};

struct shaper_mcu_log_file
{
	unsigned int marker;

	struct shaper_mcu_log_platform_device *mlpdev;
	bool raw;
};

struct shaper_mcu_log_driver
{
	unsigned int marker;

	struct rpmsg_driver rpmsg_drv;
	dev_t devnum;
	struct class *class;
	struct mutex lock;
	struct list_head rpmsg_devs;
	struct list_head mcu_log_devs;
};

static int shaper_mcu_log_rpmsg_callback(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src);
static int shaper_mcu_log_rpmsg_probe(struct rpmsg_device *rpdev);
static void shaper_mcu_log_rpmsg_remove(struct rpmsg_device *rpdev);

static const char *mcu_log_level_fmt[MCU_LOG_TRACE + 1] =
{
	[MCU_LOG_NONE] = "%u.%04u %s: %s",
	[MCU_LOG_FATAL] = "%u.%04u FATAL - %s: %s",
	[MCU_LOG_ERROR] = "%u.%04u ERROR - %s: %s",
	[MCU_LOG_WARN] = "%u.%04u WARN  - %s: %s",
	[MCU_LOG_INFO] = "%u.%04u INFO  - %s: %s",
	[MCU_LOG_DEBUG] = "%u.%04u DEBUG - %s: %s",
	[MCU_LOG_TRACE] = "%u.%04u TRACE - %s: %s",
};

static struct rpmsg_device_id shaper_mcu_log_rpmsg_id_table[] =
{
	{.name = "shaper-rpmsg-mcu-log" },
	{.name = "shaper-rpmsg-mcu-log-0" },
	{.name = "shaper-rpmsg-mcu-log-1" },
	{.name = "shaper-rpmsg-mcu-log-2" },
	{.name = "shaper-rpmsg-mcu-log-3" },
	{.name = "shaper-rpmsg-mcu-log-4" },
	{.name = "shaper-rpmsg-mcu-log-5" },
	{.name = "shaper-rpmsg-mcu-log-6" },
	{},
};

static struct shaper_mcu_log_driver shaper_mcu_log_driver =
{
	.marker = SHAPER_MCU_LOG_MARKER,
	.rpmsg_drv.drv.name = KBUILD_MODNAME,
	.rpmsg_drv.drv.owner = THIS_MODULE,
	.rpmsg_drv.id_table = shaper_mcu_log_rpmsg_id_table,
	.rpmsg_drv.probe = shaper_mcu_log_rpmsg_probe,
	.rpmsg_drv.callback = shaper_mcu_log_rpmsg_callback,
	.rpmsg_drv.remove = shaper_mcu_log_rpmsg_remove,
};

static int shaper_mcu_log_open(struct inode *inode, struct file *file)
{
	struct shaper_mcu_log_platform_device *mlpdev = container_of(inode->i_cdev, struct shaper_mcu_log_platform_device, cdev);
	struct shaper_mcu_log_file *mcu_log_file;

	/* Sanity check */
	if (mlpdev->marker != SHAPER_MCU_LOG_MARKER) {
		pr_err("%s - invalid shaper_mcu_log_platform_device\n", __func__);
		return -EINVAL;
	}

	/* Allocate the file */
	mcu_log_file = kzalloc(sizeof(struct shaper_mcu_log_file), GFP_KERNEL);
	if (!mcu_log_file) {
		dev_err(mlpdev->dev, "Could not allocate file\n");
		return -ENOMEM;
	}

	/* Initialize */
	mcu_log_file->marker = SHAPER_MCU_LOG_MARKER;
	mcu_log_file->mlpdev = mlpdev;

	/* Stash everything */
	file->private_data = mcu_log_file;

	dev_info(mlpdev->dev, "opened mcu log with file %p\n", mcu_log_file);

	return 0;
}

static int shaper_mcu_log_release(struct inode *inode, struct file *file)
{
	struct shaper_mcu_log_platform_device *mlpdev = container_of(inode->i_cdev, struct shaper_mcu_log_platform_device, cdev);
	struct shaper_mcu_log_file *mcu_log_file = file->private_data;

	/* Sanity check */
	if (mcu_log_file->marker != SHAPER_MCU_LOG_MARKER) {
		pr_err("%s - invalid shaper_mcu_log_platform_device\n", __func__);
		return -EINVAL;
	}

	dev_info(mlpdev->dev, "releasing mcu log file %p\n", mcu_log_file);

	kfree(mcu_log_file);

	return 0;
}

static ssize_t shaper_mcu_log_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	ssize_t amount;
	char msg_str[MCU_LOG_MESSAGE_SIZE * 2];
	struct sk_buff *skb;
	struct log_msg *log_msg;
	struct shaper_mcu_log_file *mcu_log_file = file->private_data;
	struct shaper_mcu_log_platform_device *mlpdev = mcu_log_file->mlpdev;

	/* Sanity check */
	if (mcu_log_file->marker != SHAPER_MCU_LOG_MARKER) {
		pr_err("%s - invalid shaper_mcu_log_platform_device\n", __func__);
		return -EINVAL;
	}

	/* Something to read? */
	while (skb_queue_empty(&mlpdev->data_queue)) {

		/* Non-blocking? */
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* Wait until buffer or interruption */
		if (wait_event_interruptible(mlpdev->read_queue, !skb_queue_empty(&mlpdev->data_queue)))
			return -ERESTARTSYS;
	}


	/* Oh yea, some maybe some joy */
	skb = skb_dequeue(&mlpdev->data_queue);
	log_msg = (struct log_msg *)skb->data;

	/* Raw output? */
	if (mcu_log_file->raw) {
		amount = min(count, sizeof(struct log_msg));
		if (copy_to_user(buf, skb->data, amount))
			amount = -EFAULT;
	} else {
		amount = scnprintf(msg_str, sizeof(msg_str), mcu_log_level_fmt[log_msg->level], log_msg->sec, log_msg->nsec / 1000000, log_msg->caller, log_msg->msg_str);
		amount = amount + 1 < count ? amount + 1 : count;
		if (copy_to_user(buf, msg_str, amount))
			amount = -EFAULT;
	}

	/* Clean up and return */
	kfree_skb(skb);
	return amount;
}

static __poll_t shaper_mcu_log_poll(struct file *file, struct poll_table_struct *poll_table)
{
	struct shaper_mcu_log_file *mcu_log_file = file->private_data;
	struct shaper_mcu_log_platform_device *mlpdev = mcu_log_file->mlpdev;
	unsigned int mask;

	/* Sanity check the file data */
	if (mcu_log_file->marker != SHAPER_MCU_LOG_MARKER) {
		pr_err("%s - invalid shaper_mcu_log_platform_device\n", __func__);
		return -EINVAL;
	}

	/* Initialize the the wait table */
	poll_wait(file, &mlpdev->read_queue, poll_table);

	/* Setup the current state */
	mask = !skb_queue_empty(&mlpdev->data_queue) ? POLLIN | POLLRDNORM : 0;

	/* All good */
	return mask;
}

static long shaper_mcu_log_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return -ENOTSUPP;
}

static struct file_operations shaper_mcu_log_fops =
{
   .open = shaper_mcu_log_open,
   .release = shaper_mcu_log_release,
   .read = shaper_mcu_log_read,
   .poll = shaper_mcu_log_poll,
   .unlocked_ioctl = shaper_mcu_log_ioctl,
   .owner = THIS_MODULE,
};

static int shaper_mcu_log_rpmsg_callback(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src)
{
	struct sk_buff *skb;
	void *skb_data;
	struct shaper_mcu_log_platform_device *mlpdev;

	dev_dbg(&rpdev->dev, "%s invoked\n", __func__);

	/* Add the message to each log device */
	list_for_each_entry(mlpdev, &shaper_mcu_log_driver.mcu_log_devs, node) {

		/* Allocate a buffer */
		skb = alloc_skb(len, GFP_KERNEL);
		if (!skb) {
			dev_err(&rpdev->dev, "could not allocate skb of len: %d\n", len);
			return 0;
		}

		/* Load the data into the skb */
		skb_data = skb_put(skb, len);
		memcpy(skb_data, data, len);

		/* Queue is up */
		skb_queue_tail(&mlpdev->data_queue, skb);

		/* Kick any waiters */
		wake_up_interruptible(&mlpdev->read_queue);
	}

	/* All good */
	return 0;
}

static int shaper_mcu_log_rpmsg_probe(struct rpmsg_device *rpdev)
{
	int status;
	dev_dbg(&rpdev->dev, "%s invoked\n", __func__);

	/* We need to kick the MCU */
	status = rpmsg_send(rpdev->ept, rpdev->id.name, strlen(rpdev->id.name));
	if (status < 0) {
		dev_err(&rpdev->dev, "problem kicking MCU log endpoint: %d\n", status);
		return status;
	}

	/* Log it */
	dev_info(&rpdev->dev, "mcu log rpmsg created with endpoint: 0x%0x\n", rpdev->dst);

	return 0;
}

static void shaper_mcu_log_rpmsg_remove(struct rpmsg_device *rpdev)
{
	dev_dbg(&rpdev->dev, "%s invoked\n", __func__);

	/* Log it */
	dev_info(&rpdev->dev, "mcu log rpmsg removed endpoint: 0x%0x\n", rpdev->dst);
}

static int shaper_mcu_log_probe(struct platform_device *pdev)
{
	struct shaper_mcu_log_platform_device *mlpdev;
	int status;

	dev_dbg(&pdev->dev, "%s invoked\n", __func__);

	/* Get some memory for the driver data */
	mlpdev = kzalloc(sizeof(struct shaper_mcu_log_platform_device), GFP_KERNEL);
	if (!mlpdev) {
		dev_err(&pdev->dev, "%s could not allocate mcu log platform device", __func__);
		return -ENOMEM;
	}

	/* Basic initialization */
	mlpdev->marker = SHAPER_MCU_LOG_MARKER;
	mlpdev->parent = pdev;
	mlpdev->devt = shaper_mcu_log_driver.devnum;
	INIT_LIST_HEAD(&mlpdev->node);
	skb_queue_head_init(&mlpdev->data_queue);
	init_waitqueue_head(&mlpdev->read_queue);

	/* Create the matching device so /dev shakes */
	mlpdev->dev = device_create(shaper_mcu_log_driver.class, &pdev->dev, mlpdev->devt, mlpdev, "mcu/log");
	if (IS_ERR(mlpdev->dev)) {
		status = PTR_ERR(mlpdev->dev);
		dev_err(&pdev->dev, "Could create require device: %d\n", status);
		goto error_free_platform_device;
	}

	/* Initialize and register the char device */
	cdev_init(&mlpdev->cdev, &shaper_mcu_log_fops);
	mlpdev->cdev.owner = THIS_MODULE;
	status = cdev_add(&mlpdev->cdev, mlpdev->devt, 1);
	if (status < 0) {
		dev_err(&pdev->dev, "Could not add the cdev: %d\n", status);
		goto error_device_destroy;
	}

	/* Stash ourselves for later */
	platform_set_drvdata(pdev, mlpdev);

	/* Add ourself to the driver */
	mutex_lock(&shaper_mcu_log_driver.lock);
	list_add(&mlpdev->node, &shaper_mcu_log_driver.mcu_log_devs);
	mutex_unlock(&shaper_mcu_log_driver.lock);

	/* All good */
	return 0;

error_device_destroy:
	device_destroy(shaper_mcu_log_driver.class, mlpdev->devt);

error_free_platform_device:
	kfree(mlpdev);

	/* All bad */
	return status;
}

static void shaper_mcu_log_remove(struct platform_device *pdev)
{
	struct shaper_mcu_log_platform_device *mlpdev = platform_get_drvdata(pdev);

	dev_dbg(&pdev->dev, "%s invoked\n", __func__);

	/* Sanity check */
	if (mlpdev->marker != SHAPER_MCU_LOG_MARKER) {
		dev_err(&pdev->dev, "%s - bad marker\n", __func__);
		return;
	}

	/* Remove ourselfs from the driver */
	mutex_lock(&shaper_mcu_log_driver.lock);
	list_del(&mlpdev->node);
	mutex_unlock(&shaper_mcu_log_driver.lock);

	/* Clean up */
	cdev_del(&mlpdev->cdev);
	device_destroy(shaper_mcu_log_driver.class, mlpdev->devt);
	kfree(mlpdev);

	/* All good */
	return;
}

static const struct of_device_id shaper_mcu_log_of_match[] =
{
	{
		.compatible = "shapertools,shaper-mcu-log",
	},
	{
	}
};
MODULE_DEVICE_TABLE(of, shaper_mcu_log_of_match);

static struct platform_driver shaper_mcu_log_platform_driver =
{
	.driver =
	{
		.name = "shaper-mcu-log",
		.of_match_table = shaper_mcu_log_of_match,
	},
	.probe = shaper_mcu_log_probe,
	.remove = shaper_mcu_log_remove,
};

static int __init shaper_mcu_log_init(void)
{
	int status;

	pr_debug("%s - invoked\n", __func__);

	/* Basic initialization */
	mutex_init(&shaper_mcu_log_driver.lock);
	INIT_LIST_HEAD(&shaper_mcu_log_driver.rpmsg_devs);
	INIT_LIST_HEAD(&shaper_mcu_log_driver.mcu_log_devs);

	/* We need a class so that the dev nodes appear auto-magically */
	shaper_mcu_log_driver.class = class_create(KBUILD_MODNAME);
	if (IS_ERR(shaper_mcu_log_driver.class)) {
		status = PTR_ERR(shaper_mcu_log_driver.class);
		pr_err("%s could not create class: %d\n", __func__, status);
		return status;
	}

	/* Need a device number */
	status = alloc_chrdev_region(&shaper_mcu_log_driver.devnum, 0, ARRAY_SIZE(shaper_mcu_log_rpmsg_id_table), KBUILD_MODNAME);
	if (status < 0) {
		pr_err("%s could not allocate cdev region: %d\n", __func__, status);
		goto error_destroy_class;
	}

	/* Register the rpmsg driver */
	status = register_rpmsg_driver(&shaper_mcu_log_driver.rpmsg_drv);
	if (status < 0) {
		pr_err("%s Could not register rpmsg driver: %d\n", __func__, status);
		goto error_unregister_chrdev_region;
	}

	/* Register the platform driver */
	status = platform_driver_register(&shaper_mcu_log_platform_driver);
	if (status < 0) {
		pr_err("%s Could not register platform driver: %d\n", __func__, status);
		goto error_unregister_rpmsg_driver;
	}

	/* Done the best we can */
	pr_info("shaper-mcu-log driver loaded\n");
	return 0;

error_unregister_rpmsg_driver:
	unregister_rpmsg_driver(&shaper_mcu_log_driver.rpmsg_drv);

error_unregister_chrdev_region:
	unregister_chrdev_region(shaper_mcu_log_driver.devnum, ARRAY_SIZE(shaper_mcu_log_rpmsg_id_table));

error_destroy_class:
	class_destroy(shaper_mcu_log_driver.class);

	/* Uhmm well this a problem */
	return status;
}
module_init(shaper_mcu_log_init);

static void __exit shaper_mcu_log_fini(void)
{
	pr_debug("%s - invoked\n", __func__);

	platform_driver_unregister(&shaper_mcu_log_platform_driver);
	unregister_rpmsg_driver(&shaper_mcu_log_driver.rpmsg_drv);
	unregister_chrdev_region(shaper_mcu_log_driver.devnum, ARRAY_SIZE(shaper_mcu_log_rpmsg_id_table));
	class_destroy(shaper_mcu_log_driver.class);

	pr_info("shaper-mcu-log driver unloaded\n");
}
module_exit(shaper_mcu_log_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stephen Street <stephen@shapertools.com>");
MODULE_DESCRIPTION("Shaper MCU Log Driver");
