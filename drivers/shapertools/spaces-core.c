// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Shaper Tools Inc, Stephen Street <stephen@shapertools.com>
 */

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/io.h>
#include <linux/firmware.h>
#include <linux/elf.h>
#include <linux/remoteproc.h>

#include "shaper-mcu-spaces.h"
#include "space-descriptor.h"
#include "shaper-rproc.h"

struct shaper_spaces_event
{
	int (*notify)(unsigned int event, void *context);
	void *context;
	unsigned int event_mask;
	struct list_head node;
};

static void shaper_mcu_space_rx_callback(struct mbox_client *client, void *msg)
{
	struct platform_device *pdev = to_platform_device(client->dev);
	struct shaper_mcu_space_platform_device *space_pdev = platform_get_drvdata(pdev);

	dev_dbg(client->dev, "%s invoked %p %u\n", __func__, msg, *(uint32_t *)msg);

	if (!space_pdev) {
		dev_err(client->dev, "%s: space_pdev is NULL! (drvdata not set yet?)\n", __func__);
		return;
	}

	if (space_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(client->dev, "bad marker\n");
		return;
	}

	/* Check for crashed signal */
	if (*((uint32_t *)msg) == CMD_CRASHED) {
		dev_warn(client->dev, "%s: signaled crash\n", __func__);
		rproc_report_crash(space_pdev->remote_proc, RPROC_FATAL_ERROR);
		return;
	}

	/* Make sure there is a waiter */
	if (!space_pdev->active) {
		dev_dbg(client->dev, "%s: no waiter\n", __func__);
		return;
	}

	/* Release the waiter */
	complete(&space_pdev->active->done);
}

static void shaper_mcu_space_rxdb_callback(struct mbox_client *client, void *msg)
{
	struct shaper_spaces_event *entry;
	struct platform_device *pdev = to_platform_device(client->dev);
	struct shaper_mcu_space_platform_device *space_pdev = platform_get_drvdata(pdev);

	dev_dbg(client->dev, "%s invoked space_pdev: %p, msg: %p\n", __func__, space_pdev, msg);

	if (!space_pdev) {
		dev_err(client->dev, "%s: space_pdev is NULL! (drvdata not set yet?)\n", __func__);
		return;
	}

	if (space_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(client->dev, "bad marker\n");
		return;
	}

	list_for_each_entry(entry, &space_pdev->events, node)
		entry->notify(0, entry->context);
}

static inline void shaper_mcu_activate_space(struct shaper_mcu_space_platform_device *space_pdev, struct shaper_mcu_space_device *space_dev)
{
	unsigned long flags;
	spin_lock_irqsave(&space_pdev->active_lock, flags);
	space_pdev->active = space_dev;
	spin_unlock_irqrestore(&space_pdev->active_lock, flags);
}

int shaper_mcu_space_capture(struct shaper_mcu_space_device *space_dev, off_t offset, size_t size)
{
	int status;
	struct space_cmd cmd;

	if (space_dev == 0 || space_dev->marker != SHAPER_MCU_SPACE_MARKER)
		return -EINVAL;

	mutex_lock(&space_dev->parent->lock);

	/* Create the read command */
	cmd.cmd_type = CMD_READ;
	cmd.cmd_space = space_dev->desc->id;
	cmd.cmd_offset = offset;
	cmd.cmd_size = size;

	/* Active the space */
	shaper_mcu_activate_space(space_dev->parent, space_dev);

	/* Send the command */
	status = mbox_send_message(space_dev->parent->tx_chan, &cmd);
	if (status < 0)
		goto error;

	/* Wait for ack */
	status = wait_for_completion_interruptible_timeout(&space_dev->done,  msecs_to_jiffies(500));
	if (status > 0)
		status = 0;
	else if (status == 0)
		status = -ETIMEDOUT;

	/* Release the active space */
	shaper_mcu_activate_space(space_dev->parent, 0);

error:
	mutex_unlock(&space_dev->parent->lock);

	return status;
}
EXPORT_SYMBOL(shaper_mcu_space_capture);

int shaper_mcu_space_flush(struct shaper_mcu_space_device *space_dev, off_t offset, size_t size)
{
	int status;
	struct space_cmd cmd;

	if (space_dev == 0 || space_dev->marker != SHAPER_MCU_SPACE_MARKER)
		return -EINVAL;

	mutex_lock(&space_dev->parent->lock);

	/* Create the read command */
	cmd.cmd_type = CMD_WRITE;
	cmd.cmd_space = space_dev->desc->id;
	cmd.cmd_offset = offset;
	cmd.cmd_size = size;

	/* Active the space */
	shaper_mcu_activate_space(space_dev->parent, space_dev);

	/* Send the command */
	status = mbox_send_message(space_dev->parent->tx_chan, &cmd);
	if (status < 0)
		goto error;

	status = wait_for_completion_interruptible_timeout(&space_dev->done,  msecs_to_jiffies(500));
	if (status > 0)
		status = 0;
	else if (status == 0)
		status = -ETIMEDOUT;

	/* Release the active space */
	shaper_mcu_activate_space(space_dev->parent, 0);

error:
	mutex_unlock(&space_dev->parent->lock);

	return status;
}
EXPORT_SYMBOL(shaper_mcu_space_flush);

struct shaper_mcu_space_device *shaper_mcu_get_space_device(struct shaper_mcu_space_platform_device *space_pdev, unsigned int space)
{
	struct shaper_mcu_space_device *space_dev;

	mutex_lock(&space_pdev->lock);
	list_for_each_entry(space_dev, &space_pdev->devices, node)
		if (space_dev->desc->id == space) {
			kobject_get(&space_dev->this->kobj);
			mutex_unlock(&space_dev->parent->lock);
			return space_dev;
		}
	mutex_unlock(&space_pdev->lock);
	return 0;
}
EXPORT_SYMBOL(shaper_mcu_get_space_device);

void shaper_mcu_put_space_device(struct shaper_mcu_space_device *space_dev)
{
	kobject_put(&space_dev->this->kobj);
}
EXPORT_SYMBOL(shaper_mcu_put_space_device);

static int shaper_platform_dev_node_match(struct device *dev, const void *data)
{
	return dev->of_node == data;
}

static ssize_t desc_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int i;
	const char *type_name;
	int amount = 0;
	struct shaper_mcu_space_device *space_dev = dev_get_drvdata(dev);

	if (space_dev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(dev, "%s - bad marker\n", __func__);
		return 0;
	}

	/* Dump the descriptor */
	amount = scnprintf(buf, PAGE_SIZE - amount, "%u name: %s size: %lu\n", space_dev->desc->id, space_dev->desc->name, space_dev->desc->size);
	for (i = 0; i < space_dev->desc->num_fields && amount < PAGE_SIZE - 1; ++i) {

		/* Range check the type */
		type_name = "unknown";
		if (space_dev->desc->fields[i].type <= MCU_DESC_FIELD_TYPE_LAST)
			type_name = field_descriptor_type_str[space_dev->desc->fields[i].type];

		/* Display array type with size */
		if (space_dev->desc->fields[i].type >= MCU_DESC_FIELD_TYPE_FLOAT_ARRAY && space_dev->desc->fields[i].type <= MCU_DESC_FIELD_TYPE_UINT64_ARRAY)
			amount += scnprintf(buf + amount, PAGE_SIZE - amount, "\t0x%08lx: size: %4lu %s %s[]\n", space_dev->desc->fields[i].offset, space_dev->desc->fields[i].size, type_name, space_dev->desc->fields[i].name);
		else
			amount += scnprintf(buf + amount, PAGE_SIZE - amount, "\t0x%08lx: size: %4lu %s %s\n", space_dev->desc->fields[i].offset, space_dev->desc->fields[i].size, type_name, space_dev->desc->fields[i].name);
	}

	/* Success */
	return amount;
}
static DEVICE_ATTR_RO(desc);

static struct attribute *space_dev_attrs[] = {
	&dev_attr_desc.attr,
	0,
};

static const struct attribute_group space_dev_group = {
	.attrs = space_dev_attrs,
};

static const struct attribute_group *space_dev_groups[] = {
	&space_dev_group,
	0,
};

int shaper_mcu_space_register(struct shaper_mcu_space_device *space_dev, const struct space_descriptor *desc, const struct file_operations *fops, struct module *owner)
{
	int status;
	unsigned int id = 0;
	struct device_node *platform_node;
	struct platform_device *pdev;
	struct shaper_mcu_space_platform_device *space_pdev;

	/* Try to find the device tree node */
	platform_node = of_find_node_by_name(NULL, "shaper-m4-spaces");
	if (!platform_node) {
		printk(KERN_ERR "could not find device node: 'shaper-m4-spaces'\n");
		return -ENOENT;
	}

	/* Find the device on the bus */
	pdev = to_platform_device(bus_find_device(&platform_bus_type, NULL, platform_node, shaper_platform_dev_node_match));
	if (!pdev) {
		printk(KERN_ERR "could not find match 'shaper-m4-spaces' platform bus device\n");
		status = -ENODEV;
		goto error_release_platform_node;
	}

	/* Extract the driver data */
	space_pdev = platform_get_drvdata(pdev);
	if (!space_pdev) {
		printk(KERN_ERR "%s: space_pdev is NULL! (drvdata not set yet?)\n", __func__);
		status = -ENODEV;
		goto error_release_platform_node;
	}

	if (space_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(&pdev->dev, "bad space pdev marker\n");
		status = -EINVAL;
		goto error_release_platform_node;
	}

	/* Initialize the cdev */
	cdev_init(&space_dev->cdev, fops);
	space_dev->cdev.owner = owner;

	/* Was a descriptor provided? */
	if (desc) {

		/* Extract the space number */
		id = desc->id;

		/* Duplicate the descriptor */
		space_dev->desc = kmemdup(desc, sizeof(struct space_descriptor) + sizeof(struct field_descriptor) * desc->num_fields, GFP_KERNEL);
		if (!space_dev->desc) {
			dev_err(&pdev->dev, "%s - could not duplicate the space descriptor\n", __func__);
			goto error_delete_cdev;
		}
	} else {
		id = 0;
		space_dev->desc = 0;
	}

	/* Other simple initialization */
	space_dev->marker = SHAPER_MCU_SPACE_MARKER;
	space_dev->parent = space_pdev;
	mutex_init(&space_dev->lock);
	INIT_LIST_HEAD(&space_dev->node);
	init_completion(&space_dev->done);
	space_dev->devt = MKDEV(MAJOR(space_pdev->devt), id);
	space_dev->mem = space_pdev->space_mem_addr + (id * PAGE_SIZE);

	/* Add the cdev */
	status = cdev_add(&space_dev->cdev, space_dev->devt, 1);
	if (status < 0) {
		dev_err(&pdev->dev, "%s - failed to add char device at %d:%d\n", __func__, MAJOR(space_dev->devt), MINOR(space_dev->devt));
		goto error_delete_cdev;
	}

	/* Create the matching device */
	space_dev->this = device_create_with_groups(space_pdev->class, &pdev->dev, space_dev->devt, space_dev, space_dev_groups, "%u", id);
	if (IS_ERR(space_dev->this)) {
		dev_err(&pdev->dev, "%s - unable to create device %d:%d\n", __func__, MAJOR(space_dev->devt), MINOR(space_dev->devt));
		status = PTR_ERR(space_dev->this);
		goto error_delete_cdev;
	}

	/* Stash the pointer for later use */
	dev_set_drvdata(space_dev->this, space_dev);

	/* Add it to the list of space devices */
	mutex_lock(&space_pdev->lock);
	list_add_tail(&space_dev->node, &space_pdev->devices);
	mutex_unlock(&space_pdev->lock);

	/* Release the platform node */
	of_node_put(platform_node);

	dev_dbg(&pdev->dev, "created space device %u with address %p\n", space_dev->desc->id, space_dev->mem);

	/* Device should be alive */
	return 0;

error_delete_cdev:
	cdev_del(&space_dev->cdev);

error_release_platform_node:
	of_node_put(platform_node);

	return status;
}
EXPORT_SYMBOL(shaper_mcu_space_register);

int shaper_mcu_space_unregister(struct shaper_mcu_space_device *space_dev)
{
	dev_dbg(space_dev->this, "destroying space: %d\n", MINOR(space_dev->devt));

	/* Remove the device from the internel list to prevent further opening */
	mutex_lock(&space_dev->parent->lock);
	list_del(&space_dev->node);
	mutex_unlock(&space_dev->parent->lock);

	/* Remove the device */
	device_destroy(space_dev->parent->class, space_dev->devt);

	/* Free the descriptor */
	kfree(space_dev->desc);

	/* And the matching dev */
	cdev_del(&space_dev->cdev);

	/* Yep always joy */
	return 0;
}
EXPORT_SYMBOL(shaper_mcu_space_unregister);

static struct shaper_mcu_space_device *shaper_mcu_space_create_device(const struct space_descriptor *desc)
{
	int status;

	/* Allocate the new space device */
	struct shaper_mcu_space_device *space_dev = kzalloc(sizeof(struct shaper_mcu_space_device), GFP_KERNEL);
	if (!space_dev) {
		printk(KERN_ERR "could allocate space device for %d\n", desc->id);
		return 0;
	}

	/* Now register it */
	status = shaper_mcu_space_register(space_dev, desc, desc->id == 0 ? &space_pdev_fops : &space_dev_fops, THIS_MODULE);
	if (status < 0) {
		printk(KERN_ERR "could register space device for %d\n", desc->id);
		kfree(space_dev);
		return 0;
	}

	/* Should be good */
	return space_dev;
}

static void shaper_mcu_space_destroy_device(struct shaper_mcu_space_device *space_dev)
{
	/* Clean up, first unregister and the free */
	shaper_mcu_space_unregister(space_dev);
	kfree(space_dev);
}

static int shaper_rproc_subdev_prepare(struct rproc_subdev *subdev)
{
	struct shaper_mcu_space_platform_device *spaces_pdev = container_of(subdev, struct shaper_mcu_space_platform_device, rproc_subdev);
	const struct firmware *desc_fw;

	if (spaces_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(&spaces_pdev->parent->dev, "%s - bad marker\n", __func__);
		return -EINVAL;
	}

	dev_dbg(&spaces_pdev->parent->dev, "%s invoked\n", __func__);

	/* Ask rproc for the descriptor firmware */
	desc_fw = shaper_rproc_descriptor(spaces_pdev->remote_proc);
	if (!IS_ERR(desc_fw) && desc_fw->data) {

		/* Lets try parse is */
		dev_dbg(&spaces_pdev->parent->dev, "%s - parsing descriptor firmware\n", __func__);
		spaces_pdev->descs = space_descriptor_parse(desc_fw->data);
		if (!spaces_pdev->descs)
			dev_warn(&spaces_pdev->parent->dev, "%s - could not parse descriptors section\n", __func__);
	} else
		dev_warn(&spaces_pdev->parent->dev, "%s - no descriptor firmware available: %ld\n", __func__, PTR_ERR(desc_fw));

	/* Alway good */
	return 0;
}

static int shaper_rproc_subdev_start(struct rproc_subdev *subdev)
{
	struct space_descriptor **space_desc;
	struct shaper_mcu_space_device *space_dev;
	struct shaper_mcu_space_device *next;
	struct shaper_mcu_space_platform_device *spaces_pdev = container_of(subdev, struct shaper_mcu_space_platform_device, rproc_subdev);
	struct space_descriptor descriptor_space = { .name = "descriptor", .id = 0, .size = 0, .num_fields = 0 };

	if (spaces_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(&spaces_pdev->parent->dev, "%s - bad marker\n", __func__);
		return -EINVAL;
	}

	dev_dbg(&spaces_pdev->parent->dev, "%s invoked\n", __func__);

	/* No devices to add if there is not a descriptor */
	if (!spaces_pdev->descs) {
		dev_warn(&spaces_pdev->parent->dev, "no descriptors found\n");
		return 0;
	}

	/* Add special space 0 */
	space_dev = shaper_mcu_space_create_device(&descriptor_space);
	if (!space_dev) {
		dev_err(&spaces_pdev->parent->dev, "%s - could not create space device for %d\n", __func__, 0);
		return -EIO;
	}

	/* Create the devices from the descriptor */
	space_desc = spaces_pdev->descs;
	while (*space_desc) {

		/* Need some memory for the device */
		space_dev = shaper_mcu_space_create_device(*space_desc);
		if (!space_dev) {
			dev_err(&spaces_pdev->parent->dev, "%s - could not create space device for %d\n", __func__, (*space_desc)->id);
			goto error_remove_devices;
		}

		/* Move to the next descriptor */
		 ++space_desc;
	}

	return 0;

error_remove_devices:

	/* Destroy all devices */
	list_for_each_entry_safe(space_dev, next, &spaces_pdev->devices, node)
		shaper_mcu_space_destroy_device(space_dev);

	return -EIO;
}

static void shaper_rproc_subdev_stop(struct rproc_subdev *subdev, bool crashed)
{
	struct shaper_mcu_space_device *space_dev;
	struct shaper_mcu_space_device *next;
	struct shaper_mcu_space_platform_device *spaces_pdev = container_of(subdev, struct shaper_mcu_space_platform_device, rproc_subdev);

	if (spaces_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(&spaces_pdev->parent->dev, "%s - bad marker\n", __func__);
		return;
	}

	dev_dbg(&spaces_pdev->parent->dev, "%s invoked\n", __func__);

	/* Destroy all devices */
	list_for_each_entry_safe(space_dev, next, &spaces_pdev->devices, node)
		shaper_mcu_space_destroy_device(space_dev);
}

static void shaper_rproc_subdev_unprepare(struct rproc_subdev *subdev)
{
	struct shaper_mcu_space_platform_device *spaces_pdev = container_of(subdev, struct shaper_mcu_space_platform_device, rproc_subdev);

	if (spaces_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(&spaces_pdev->parent->dev, "%s - bad marker\n", __func__);
		return;
	}

	/* Delete the descriptor */
	dev_dbg(&spaces_pdev->parent->dev, "%s invoked\n", __func__);
	if (spaces_pdev->descs)
		space_descriptor_release(spaces_pdev->descs);
	spaces_pdev->descs = 0;
}

static ssize_t descs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int amount = 0;
	struct shaper_mcu_space_platform_device *space_pdev = dev_get_drvdata(dev);
	struct shaper_mcu_space_device *space_dev;

	if (space_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(dev, "%s - bad marker\n", __func__);
		return -EINVAL;
	}

	/* Let keep everything stable */
	if (mutex_lock_interruptible(&space_pdev->lock) < 0)
		return -EINTR;

	/* Create the list of descriptors */
	list_for_each_entry(space_dev, &space_pdev->devices, node) {
		if (space_dev->desc) {
			amount += scnprintf(buf + amount, PAGE_SIZE - amount, "%2u - %s size: %lu\n", space_dev->desc->id, space_dev->desc->name, space_dev->desc->size);
			if (amount >= PAGE_SIZE - 1)
				break;
		}
	}

	/* All good */
	mutex_unlock(&space_pdev->lock);
	return amount;
}
static DEVICE_ATTR_RO(descs);

static char *shaper_mcu_space_devnode(const struct device *dev, umode_t *mode)
{
	struct shaper_mcu_space_device *space_dev = dev_get_drvdata(dev);
	char name[64];

	if (space_dev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(space_dev->this, "%s - bad marker\n", __func__);
		return 0;
	}

	if (mode && space_dev->mode)
		*mode = space_dev->mode;

	/* Build up the test node name */
	scnprintf(name, sizeof(name), "mcu/%u", space_dev->desc != 0 ? space_dev->desc->id : 0);

	/* Return the new name */
	return kstrdup(name, GFP_KERNEL);
}

static int shaper_mcu_space_probe(struct platform_device *pdev)
{
	int status = 0;
	struct shaper_mcu_space_platform_device *space_pdev;
	struct device_node *dev_node;
	phandle rproc_phandle;

	dev_info(&pdev->dev, "%s invoked\n", __func__);

	/* Get some memory for the driver data */
	space_pdev = kzalloc(sizeof(struct shaper_mcu_space_platform_device), GFP_KERNEL);
	if (!space_pdev) {
		dev_err(&pdev->dev, "%s could not allocate mcu test platform device", __func__);
		return -ENOMEM;
	}

	/* Simple initialization */
	space_pdev->marker = 0x01377310;
	space_pdev->parent = pdev;
	INIT_LIST_HEAD(&space_pdev->devices);
	INIT_LIST_HEAD(&space_pdev->events);
	mutex_init(&space_pdev->lock);
	mutex_init(&space_pdev->mbox_lock);
	spin_lock_init(&space_pdev->devices_lock);
	spin_lock_init(&space_pdev->events_lock);
	spin_lock_init(&space_pdev->active_lock);
	space_pdev->descs = 0;

	/* Stash ourselves for later */
	dev_set_drvdata(&pdev->dev, space_pdev);

	/* Initialize the mailbox tx/rx client */
	space_pdev->xtr_client.dev = &pdev->dev;
	space_pdev->xtr_client.tx_block = true;
	space_pdev->xtr_client.tx_tout = 50;
	space_pdev->xtr_client.knows_txdone = true;
	space_pdev->xtr_client.rx_callback = shaper_mcu_space_rx_callback;

	/* Request the tx channel */
	space_pdev->tx_chan = mbox_request_channel_byname(&space_pdev->xtr_client, "tx");
	if (IS_ERR(space_pdev->tx_chan)) {
		status = PTR_ERR(space_pdev->tx_chan);
		dev_err(&pdev->dev, "failed to request mailbox tx chan: %d\n", status);
		goto error_free_space_pdev;
	}

	/* Request the rx channel */
	space_pdev->rx_chan = mbox_request_channel_byname(&space_pdev->xtr_client, "rx");
	if (IS_ERR(space_pdev->rx_chan)) {
		status = PTR_ERR(space_pdev->rx_chan);
		dev_err(&pdev->dev, "failed to request mailbox rx chan: %d\n", status);
		goto error_free_mailbox_tx_chan;
	}

	/* Initialize txdb client and request the matching channel */
	space_pdev->txdb_client.dev = &pdev->dev;
	space_pdev->txdb_client.tx_block = false;
	space_pdev->txdb_client.tx_tout = 50;
	space_pdev->txdb_client.knows_txdone = true;
	space_pdev->txdb_chan = mbox_request_channel_byname(&space_pdev->txdb_client, "txdb");
	if (IS_ERR(space_pdev->txdb_chan)) {
		status = PTR_ERR(space_pdev->txdb_chan);
		dev_err(&pdev->dev, "failed to request mailbox txdb chan: %d\n", status);
		goto error_free_mailbox_rx_chan;
	}

	/* Initialize rxdb client and request the matching channel */
	space_pdev->rxdb_client.dev = &pdev->dev;
	space_pdev->rxdb_client.rx_callback = shaper_mcu_space_rxdb_callback;
	space_pdev->rxdb_chan = mbox_request_channel_byname(&space_pdev->rxdb_client, "rxdb");
	if (IS_ERR(space_pdev->rxdb_chan)) {
		status = PTR_ERR(space_pdev->rxdb_chan);
		dev_err(&pdev->dev, "failed to request mailbox rxdb chan: %d\n", status);
		goto error_free_mailbox_txdb_chan;
	}

	/* Extract the space memory handle */
	dev_node = of_parse_phandle(pdev->dev.of_node, "space-memory", 0);
	if (!dev_node) {
		status = -EINVAL;
		dev_err(&pdev->dev, "could not find required space-memory\n");
		goto error_free_mailbox_rxdb_chan;
	}

	/* Look up the reserved memory region */
	space_pdev->space_mem = of_reserved_mem_lookup(dev_node);
	if (!space_pdev->space_mem) {
		status = -EINVAL;
		dev_err(&pdev->dev, "unable to acquire space-memory memory-region\n");
		goto error_release_dts_node;
	}

	/* Remap the space memory */
	space_pdev->space_mem_addr = devm_memremap(&pdev->dev, space_pdev->space_mem->base, space_pdev->space_mem->size, MEMREMAP_WC);
	if (!space_pdev->space_mem_addr) {
		status = -ENOMEM;
		goto error_release_dts_node;
	}

	/* We need a class so that the dev nodes appear auto-magically */
	space_pdev->class = class_create("spaces");
	if (IS_ERR(space_pdev->class)) {
		status = PTR_ERR(space_pdev->class);
		dev_err(&pdev->dev, "%s could not create class: %d\n", __func__, status);
		goto error_release_dts_node;
	}

	/* Bind the class device node handler */
	space_pdev->class->devnode = shaper_mcu_space_devnode;

	/* Need the device numbers */
	status = alloc_chrdev_region(&space_pdev->devt, 0, SHAPER_NUM_SPACES, "spaces");
	if (status < 0) {
		dev_err(&pdev->dev, "%s could not allocate cdev region: %d\n", __func__, status);
		goto error_destroy_class;
	}

	/* Try to find the rproc driver */
	if (of_property_read_u32(pdev->dev.of_node, "rproc", &rproc_phandle)) {
		dev_err(&pdev->dev, "could not get rproc phandle\n");
		status = -ENODEV;
		goto error_unregister_chrdev_region;
	}

	/* Look it up DO NEED REF COUNT MGT?*/
	space_pdev->remote_proc = rproc_get_by_phandle(rproc_phandle);
	if (!space_pdev->remote_proc) {
		dev_err(&pdev->dev, "could not get rproc handle\n");
		status = -EPROBE_DEFER;
		goto error_unregister_chrdev_region;
	}

	/* Initialize and hook a rproc subdev */
	INIT_LIST_HEAD(&space_pdev->rproc_subdev.node);
	space_pdev->rproc_subdev.prepare = shaper_rproc_subdev_prepare;
	space_pdev->rproc_subdev.start = shaper_rproc_subdev_start;
	space_pdev->rproc_subdev.stop = shaper_rproc_subdev_stop;
	space_pdev->rproc_subdev.unprepare = shaper_rproc_subdev_unprepare;
	rproc_add_subdev(space_pdev->remote_proc, &space_pdev->rproc_subdev);

	/* Create desc sysfs entry */
	status = device_create_file(&pdev->dev, &dev_attr_descs);
	if (status != 0) {
		dev_err(&pdev->dev, "problem creating desc attribute: %d\n", status);
		goto error_put_rproc;
	}

	/* If early boot, kick the rproc */
	if (space_pdev->remote_proc->state == RPROC_DETACHED) {
		status = rproc_boot(space_pdev->remote_proc);
		if (status < 0) {
			dev_err(&pdev->dev, "failed to boot detached rproc: %d\n", status);
			goto error_put_rproc;
		}
	}

	/* Release the dts node */
	of_node_put(dev_node);

	/* Record success */
	dev_info(&pdev->dev, "ready using dev numbers %u.0-%lu, daddr: 0x%08llx@%llu, vaddr: %p\n", space_pdev->major, SHAPER_NUM_SPACES - 1, space_pdev->space_mem->base, space_pdev->space_mem->size, space_pdev->space_mem_addr);

	/* All good */
	return 0;

error_put_rproc:
	rproc_put(space_pdev->remote_proc);

error_unregister_chrdev_region:
	unregister_chrdev_region(space_pdev->devt, SHAPER_NUM_SPACES);

error_destroy_class:
	class_destroy(space_pdev->class);

error_release_dts_node:
	of_node_put(dev_node);

error_free_mailbox_rxdb_chan:
	mbox_free_channel(space_pdev->rxdb_chan);

error_free_mailbox_txdb_chan:
	mbox_free_channel(space_pdev->txdb_chan);

error_free_mailbox_rx_chan:
	mbox_free_channel(space_pdev->rx_chan);

error_free_mailbox_tx_chan:
	mbox_free_channel(space_pdev->tx_chan);

error_free_space_pdev:
	kfree(space_pdev);

	return status;
}

static void shaper_mcu_space_remove(struct platform_device *pdev)
{
	struct shaper_mcu_space_device *next;
	struct shaper_mcu_space_device *cur;
	struct shaper_mcu_space_platform_device *spaces_pdev = dev_get_drvdata(&pdev->dev);

	dev_info(&pdev->dev, "%s invoked\n", __func__);

	/* Make sure our pointers do not stink */
	if (spaces_pdev->marker != SHAPER_MCU_SPACE_MARKER) {
		dev_err(&pdev->dev, "bad marker\n");
	}

	/* Get rid of all registers space devices */
	list_for_each_entry_safe(cur, next, &spaces_pdev->devices, node) {
		shaper_mcu_space_unregister(cur);
		kfree(cur);
	}

	/* Clean up attributes */
	device_remove_file(&pdev->dev, &dev_attr_descs);

	/* Clear the remote proc */
	rproc_remove_subdev(spaces_pdev->remote_proc, &spaces_pdev->rproc_subdev);
	rproc_put(spaces_pdev->remote_proc);

	/* Do not need the class now */
	class_destroy(spaces_pdev->class);

	/* Nor the cdev device numbers */
	unregister_chrdev_region(spaces_pdev->devt, SHAPER_NUM_SPACES);

	/* Clear the mailbox interface */
	mbox_free_channel(spaces_pdev->rxdb_chan);
	mbox_free_channel(spaces_pdev->txdb_chan);
	mbox_free_channel(spaces_pdev->rx_chan);
	mbox_free_channel(spaces_pdev->tx_chan);

	/* At last */
	kfree(spaces_pdev);
}

static const struct of_device_id shaper_mcu_space_of_match[] =
{
	{
		.compatible = "shapertools,shaper-mcu-space",
	},
	{
	}
};
MODULE_DEVICE_TABLE(of, shaper_mcu_space_of_match);

static struct platform_driver shaper_mcu_space_driver =
{
	.driver =
	{
		.name = "shaper-mcu-spaces",
		.of_match_table = shaper_mcu_space_of_match,
	},
	.probe = shaper_mcu_space_probe,
	.remove = shaper_mcu_space_remove,
};
module_platform_driver(shaper_mcu_space_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Stephen Street <stephen@shapertools.com>");
MODULE_DESCRIPTION("Shaper MCU Spaces Driver");
