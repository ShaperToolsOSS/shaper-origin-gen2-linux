/*
 * Copyright 2021 Shaper Tools
 */

#define DEBUG 1

#include <linux/kernel.h>
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

struct shaper_mcu_capture_device
{
	struct rpmsg_device *rpdev;
	struct cdev cdev;
	struct device *dev;

	struct mutex lock;
	struct list_head capture_files;

	struct list_head driver_node;
};

struct shaper_mcu_capture_file
{
	struct mutex lock;
	struct sk_buff_head data_queue;
	wait_queue_head_t read_queue;

	struct shaper_mcu_capture_device *mcu_capture_dev;
	struct list_head node;
};

struct shaper_mcu_capture_driver
{
	struct rpmsg_driver rpmsg_drv;
	dev_t devnum;
	struct class *class;
	struct mutex lock;
	struct list_head devices;
};

static int shaper_mcu_capture_callback(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src);
static int shaper_mcu_capture_probe(struct rpmsg_device *rpdev);
static void shaper_mcu_capture_remove(struct rpmsg_device *rpdev);

static struct rpmsg_device_id shaper_mcu_capture_id_table[] =
{
	{.name = "shaper-rpmsg-mcu-capture" },
	{},
};

static struct shaper_mcu_capture_driver mcu_capture_driver =
{
	.rpmsg_drv.drv.name = KBUILD_MODNAME,
	.rpmsg_drv.drv.owner = THIS_MODULE,
	.rpmsg_drv.id_table = shaper_mcu_capture_id_table,
	.rpmsg_drv.probe = shaper_mcu_capture_probe,
	.rpmsg_drv.callback = shaper_mcu_capture_callback,
	.rpmsg_drv.remove = shaper_mcu_capture_remove,
};

static int shaper_mcu_capture_open(struct inode *inode, struct file *file)
{
	struct shaper_mcu_capture_device *mcu_capture_dev = container_of(inode->i_cdev, struct shaper_mcu_capture_device, cdev);
	struct shaper_mcu_capture_file *mcu_capture_file;

	/* Allocate the file */
	mcu_capture_file = kzalloc(sizeof(struct shaper_mcu_capture_file), GFP_KERNEL);
	if (!mcu_capture_file) {
		dev_err(mcu_capture_dev->dev, "Could not allocate file\n");
		return -ENOMEM;
	}

	/* Initialize */
	mcu_capture_file->mcu_capture_dev = mcu_capture_dev;
	mutex_init(&mcu_capture_file->lock);
	skb_queue_head_init(&mcu_capture_file->data_queue);
	init_waitqueue_head(&mcu_capture_file->read_queue);
	INIT_LIST_HEAD(&mcu_capture_file->node);

	/* Add ourself to the device so we get buffers */
	mutex_lock(&mcu_capture_dev->lock);
	list_add(&mcu_capture_file->node, &mcu_capture_dev->capture_files);
	mutex_unlock(&mcu_capture_dev->lock);

	/* Stash everything */
	file->private_data = mcu_capture_file;

	dev_dbg(mcu_capture_dev->dev, "opened mcu capture with file %p\n", mcu_capture_file);

	return 0;
}

static int shaper_mcu_capture_release(struct inode *inode, struct file *file)
{
	struct shaper_mcu_capture_file *mcu_capture_file = file->private_data;

	/* Remove ourselfs from the capture device */
	mutex_lock(&mcu_capture_file->mcu_capture_dev->lock);
	list_del(&mcu_capture_file->node);
	mutex_unlock(&mcu_capture_file->mcu_capture_dev->lock);

	/* Empty the sk queue, no locking required because we are not getting buffer any longer */
	skb_queue_purge(&mcu_capture_file->data_queue);

	/* Done here */
	kfree(mcu_capture_file);

	return 0;
}

static ssize_t shaper_mcu_capture_read(struct file *file, char __user *buf, size_t count, loff_t *offset)
{
	ssize_t amount;
	struct sk_buff *skb;
	struct shaper_mcu_capture_file *mcu_capture_file = file->private_data;

	/* Hold the device lock */
	if (mutex_lock_interruptible(&mcu_capture_file->lock))
		return -ERESTARTSYS;

	/* Something to read? */
	while (skb_queue_empty(&mcu_capture_file->data_queue)) {

		/* Nope, unlock for exit or waiting */
		mutex_unlock(&mcu_capture_file->lock);

		/* Non-blocking? */
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;

		/* Wait until buffer or interruption */
		if (wait_event_interruptible(mcu_capture_file->read_queue, !skb_queue_empty(&mcu_capture_file->data_queue)))
			return -ERESTARTSYS;

		/* Lock and check again */
		if (mutex_lock_interruptible(&mcu_capture_file->lock))
			return -ERESTARTSYS;
	}


	/* Oh yea, some maybe some joy */
	skb = skb_dequeue(&mcu_capture_file->data_queue);

	/* Release the lock */
	mutex_unlock(&mcu_capture_file->lock);

	/* Return the skb data */
	if (count < skb->len)
		amount = count;
	else
		amount = skb->len;

	/* Move the data around */
	if (copy_to_user(buf, skb->data, amount))
		amount = -EFAULT;

	/* Clean up and return */
	kfree_skb(skb);
	return amount;
}

static __poll_t shaper_mcu_capture_poll(struct file *file, struct poll_table_struct *poll_table)
{
	struct shaper_mcu_capture_file *mcu_capture_file = file->private_data;
	unsigned int mask;

	/* Lock up the device */
	mutex_lock(&mcu_capture_file->lock);

	/* Initialize the the wait table */
	poll_wait(file, &mcu_capture_file->read_queue, poll_table);

	/* Setup the current state */
	mask = !skb_queue_empty(&mcu_capture_file->data_queue) ? POLLIN | POLLRDNORM : 0;

	/* All good */
	mutex_unlock(&mcu_capture_file->lock);
	return mask;
}

static long shaper_mcu_capture_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return -ENOTSUPP;
}

static struct file_operations mcu_capture_fops =
{
   .open = shaper_mcu_capture_open,
   .release = shaper_mcu_capture_release,
   .read = shaper_mcu_capture_read,
   .poll = shaper_mcu_capture_poll,
   .unlocked_ioctl = shaper_mcu_capture_ioctl,
   .owner = THIS_MODULE,
};

static int shaper_mcu_capture_callback(struct rpmsg_device *rpdev, void *data, int len, void *priv, u32 src)
{
	struct sk_buff *skb;
	void *skb_data;
	struct shaper_mcu_capture_device *mcu_capture_dev = dev_get_drvdata(&rpdev->dev);
	struct shaper_mcu_capture_file *mcu_capture_file = 0;

	/* First lock the device list */
	mutex_lock(&mcu_capture_dev->lock);

	/* Loop through all open files posting the capture data to the file queue */
	list_for_each_entry(mcu_capture_file, &mcu_capture_dev->capture_files, node) {

		/* Allocate a buffer */
		skb = alloc_skb(len, GFP_KERNEL);
		if (!skb) {
			dev_err(&rpdev->dev, "could not allocate skb of len: %d\n", len);
			goto error_no_mem;
		}

		/* Load the data into the skb */
		skb_data = skb_put(skb, len);
		memcpy(skb_data, data, len);

		/* Queue it up */
		mutex_lock(&mcu_capture_file->lock);
		skb_queue_tail(&mcu_capture_file->data_queue, skb);
		mutex_unlock(&mcu_capture_file->lock);

		/* Kick any waiters */
		wake_up_interruptible(&mcu_capture_file->read_queue);
	}

error_no_mem:
	mutex_unlock(&mcu_capture_dev->lock);

	/* All good */
	return 0;
}

static int shaper_mcu_capture_probe(struct rpmsg_device *rpdev)
{
	int status;
	struct shaper_mcu_capture_device *mcu_capture_dev;

	/* Get the memory */
	mcu_capture_dev = kzalloc(sizeof(struct shaper_mcu_capture_device), GFP_KERNEL);
	if (!mcu_capture_dev) {
		dev_err(&rpdev->dev, "Could not allocate mcu capture device");
		return -ENOMEM;
	}

	/* Basic initialization */
	INIT_LIST_HEAD(&mcu_capture_dev->capture_files);
	INIT_LIST_HEAD(&mcu_capture_dev->driver_node);
	mutex_init(&mcu_capture_dev->lock);

	/* Initialize and register the char device */
	cdev_init(&mcu_capture_dev->cdev, &mcu_capture_fops);
	mcu_capture_dev->cdev.owner = THIS_MODULE;
	status = cdev_add(&mcu_capture_dev->cdev, mcu_capture_driver.devnum, 1);
	if (status < 0) {
		dev_err(&rpdev->dev, "Could not add the cdev: %d\n", status);
		goto error_free_device_memory;
	}

	/* Create the match device so /dev shakes */
	mcu_capture_dev->dev = device_create(mcu_capture_driver.class, &rpdev->dev, mcu_capture_driver.devnum, 0, "mcu/capture");
	if (IS_ERR(mcu_capture_dev->dev)) {
		status = PTR_ERR(mcu_capture_dev->dev);
		dev_err(&rpdev->dev, "Could create require device: %d\n", status);
		goto error_cdev_del;
	}

	/* Add the device to driver list */
	mutex_lock(&mcu_capture_driver.lock);
	list_add(&mcu_capture_dev->driver_node, &mcu_capture_driver.devices);
	mutex_unlock(&mcu_capture_driver.lock);

	/* Bind the driver data */
	dev_set_drvdata(&rpdev->dev, mcu_capture_dev);

	/* We need to kick the MCU */
	status = rpmsg_send(rpdev->ept, rpdev->id.name, strlen(rpdev->id.name));
	if (status < 0) {
		dev_err(&rpdev->dev, "problem kicking MCU capture endpoint: %d\n", status);
		goto error_device_destroy;
	}

	/* Log it */
	dev_info(&rpdev->dev, "mcu capture created with endpoint: 0x%0x\n", rpdev->dst);

	/* Lets go */
	return 0;

error_device_destroy:
	device_destroy(mcu_capture_driver.class, mcu_capture_driver.devnum);

error_cdev_del:
	cdev_del(&mcu_capture_dev->cdev);

error_free_device_memory:
	kfree(mcu_capture_dev);

	return status;
}

static void shaper_mcu_capture_remove(struct rpmsg_device *rpdev)
{
	struct shaper_mcu_capture_device *mcu_capture_dev = dev_get_drvdata(&rpdev->dev);

	/* Remove from this driver */
	mutex_lock(&mcu_capture_driver.lock);
	list_del(&mcu_capture_dev->driver_node);
	mutex_unlock(&mcu_capture_driver.lock);

	/* Destroy the underlaying cdev device */
	device_destroy(mcu_capture_driver.class, mcu_capture_driver.devnum);

	/* Destroy the cdev */
	cdev_del(&mcu_capture_dev->cdev);

	/* Release the device */
	kfree(mcu_capture_dev);
}

static int __init shaper_mcu_capture_init(void)
{
	int status;

	/* Simple stuff */
	mutex_init(&mcu_capture_driver.lock);
	INIT_LIST_HEAD(&mcu_capture_driver.devices);

	/* Get some device numbers */
	status = alloc_chrdev_region(&mcu_capture_driver.devnum, 0, sizeof(shaper_mcu_capture_id_table) - 1, KBUILD_MODNAME);
	if (status < 0) {
		pr_err("Can't get device numbers: %d\n", status);
		return status;
	}

	/* Create the class */
	mcu_capture_driver.class = class_create(THIS_MODULE, KBUILD_MODNAME);
	if (IS_ERR(mcu_capture_driver.class)) {
		status = PTR_ERR(mcu_capture_driver.class);
		pr_err("Could not create class: %d\n", status);
		goto error_release_chrdev_region;
	}

	/* Register the rpmsg driver */
	status = register_rpmsg_driver(&mcu_capture_driver.rpmsg_drv);
	if (status < 0) {
		pr_err("Could not register rpmsg driver: %d\n", status);
		goto error_destroy_class;
	}

	pr_info("shaper-mcu-capture driver loaded with major number: %d\n", MAJOR(mcu_capture_driver.devnum));

	return 0;

error_destroy_class:
	class_destroy(mcu_capture_driver.class);

error_release_chrdev_region:
	unregister_chrdev_region(mcu_capture_driver.devnum, sizeof(shaper_mcu_capture_id_table) - 1);

	return status;
}
module_init(shaper_mcu_capture_init);

static void __exit shaper_mcu_capture_fini(void)
{
	/* All devices gone? */
	if (!list_empty(&mcu_capture_driver.devices))
		pr_warn("device list is not empty\n");

	/* Unregister rpmsg driver */
	unregister_rpmsg_driver(&mcu_capture_driver.rpmsg_drv);

	/* Release the class */
	class_destroy(mcu_capture_driver.class);

	/* Release the device numbers */
	unregister_chrdev_region(mcu_capture_driver.devnum, sizeof(shaper_mcu_capture_id_table) - 1);

	pr_info("shaper-mcu-capture driver unloaded\n");
}
module_exit(shaper_mcu_capture_fini);

MODULE_AUTHOR("Shaper Tools, Inc.");
MODULE_DESCRIPTION("Shaper MCU Capture driver");
MODULE_LICENSE("GPL v2");
