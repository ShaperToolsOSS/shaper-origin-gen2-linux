/*#define DEBUG 1*/

#include <linux/poll.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/ioctl.h>

#include "shaper-mcu-spaces.h"

struct shaper_mcu_space_file
{
	uint32_t marker;
	struct shaper_mcu_space_device *space_dev;
	struct mutex lock;
};

static ssize_t shaper_mcu_space_dev_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	int status;
	int amount = count;
	struct shaper_mcu_space_file *space_file = file->private_data;

	/* Make sure we got the correct pointer */
	if (space_file->marker != SHAPER_MCU_SPACE_MARKER) {
		printk(KERN_ERR "%s - bad marker\n", __func__);
		return -EINVAL;
	}

	dev_dbg(space_file->space_dev->this, "%s - mem: %p, file: %p, buf: %p, count: %zu, ppos: %lld\n", __func__, space_file->space_dev->mem, file, buf, count, *ppos);

	/* Lock the device */
	status = mutex_lock_interruptible(&space_file->lock);
	if (status < 0)
		return status;

	/* Capture the space */
	status = shaper_mcu_space_capture(space_file->space_dev, *ppos, amount);
	if (status < 0) {
		amount = status;
		goto error;
	}

	/* Copy space to user */
	if (copy_to_user(buf, space_file->space_dev->mem + *ppos, amount))
		amount = -EFAULT;

error:
	/* All done */
	mutex_unlock(&space_file->lock);

	return amount;
}

static ssize_t shaper_mcu_space_dev_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	int status;
	int amount = count;
	struct shaper_mcu_space_file *space_file = file->private_data;

	/* Make sure we got the correct pointer */
	if (space_file->marker != SHAPER_MCU_SPACE_MARKER) {
		printk(KERN_ERR "%s - bad marker\n", __func__);
		return -EINVAL;
	}

	dev_dbg(space_file->space_dev->this, "%s - mem: %p, file: %p, buf: %p, count: %zu, ppos: %lld\n", __func__, space_file->space_dev->mem, file, buf, count, *ppos);

	/* Lock the device */
	status = mutex_lock_interruptible(&space_file->lock);
	if (status < 0)
		return status;

	/* Copy space from user */
	if (copy_from_user(space_file->space_dev->mem + *ppos, buf, amount)) {
		amount = -EFAULT;
		goto error;
	}

	/* Capture the space */
	status = shaper_mcu_space_flush(space_file->space_dev, *ppos, amount);
	if (status < 0)
		amount = status;

error:
	/* All done */
	mutex_unlock(&space_file->lock);

	return amount;
}

static long shaper_mcu_space_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct space_descriptor *space_desc;
	int status = 0;
	struct shaper_mcu_space_device *space_dev;
	unsigned long *value_ptr = (unsigned long *)arg;
	struct shaper_mcu_space_file *space_file = file->private_data;
	struct mcu_ioctl_desc_info *desc_info = (struct mcu_ioctl_desc_info *)arg;
	struct mcu_ioctl_space_desc *desc = (struct mcu_ioctl_space_desc *)arg;
	int i = 0;
	unsigned long num_spaces = 1;

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != _MCU_IOCTL_TYPE)
		return -ENOTTY;

	/* Check direction */
	if ((_IOC_DIR(cmd) & _IOC_READ) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	/* Lock the file */
	mutex_lock(&space_file->lock);

	/* Dispatch */
	switch (_IOC_NR(cmd)) {

	case _MCU_IOCTL_CMD_GET_NUM_DESC:

		mutex_lock(&space_file->space_dev->parent->lock);
		num_spaces = 0;
		list_for_each_entry(space_dev, &space_file->space_dev->parent->devices, node)
			++num_spaces;
		mutex_unlock(&space_file->space_dev->parent->lock);

		if (put_user(num_spaces, value_ptr) != 0)
			status = -EFAULT;

		break;

	case _MCU_IOCTL_CMD_GET_DESC_LIST:

		mutex_lock(&space_file->space_dev->parent->lock);

		list_for_each_entry(space_dev, &space_file->space_dev->parent->devices, node) {

			space_desc = space_dev->desc;

			if (copy_to_user(desc_info[i].desc_name, space_desc->name, strlen(space_desc->name) + 1) != 0) {
				status = -EFAULT;
				break;
			}

			if (put_user(space_desc->id, &desc_info[i].desc_id) != 0) {
				status = -EFAULT;
				break;
			}

			if (put_user(sizeof(struct mcu_ioctl_space_desc) + (sizeof(struct mcu_ioctl_field_desc) * space_desc->num_fields), &desc_info[i].desc_size) != 0) {
				status = -EFAULT;
				break;
			}

			/* Next entry */
			++i;
		}

		mutex_unlock(&space_file->space_dev->parent->lock);

		break;

	case _MCU_IOCTL_CMD_GET_DESC:

		mutex_lock(&space_file->space_dev->parent->lock);

		list_for_each_entry(space_dev, &space_file->space_dev->parent->devices, node)
			if (space_dev->desc && space_dev->desc->id == desc->id) {

				space_desc = space_dev->desc;

				if (copy_to_user(desc->name, space_desc->name, MCU_DESC_MAX_NAME_SIZE) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(space_desc->id, &desc->id) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(space_desc->size, &desc->size) != 0) {
					status = -EFAULT;
					break;
				}

				if (put_user(space_desc->num_fields, &desc->num_fields) != 0) {
					status = -EFAULT;
					break;
				}

				for (i = 0; i < space_desc->num_fields; ++i) {

					if (copy_to_user(desc->fields[i].name, space_desc->fields[i].name, MCU_DESC_MAX_NAME_SIZE) != 0) {
						status = -EFAULT;
						break;
					}

					if (put_user(space_desc->fields[i].type, &desc->fields[i].type) != 0) {
						status = -EFAULT;
						break;
					}

					if (put_user(space_desc->fields[i].size, &desc->fields[i].size) != 0) {
						status = -EFAULT;
						break;
					}

					if (put_user(space_desc->fields[i].offset, &desc->fields[i].offset) != 0) {
						status = -EFAULT;
						break;
					}
				}

				break;
			}

		mutex_unlock(&space_file->space_dev->parent->lock);

		if (!space_desc)
			status = -ENOENT;

		break;

	default:

		/* Opp unknown ioctl */
		dev_warn(space_file->space_dev->this, "invalid cmd: %u\n", _IOC_NR(cmd));
		status = -ENOTTY;
		break;
	}

	/* Unlock the file */
	mutex_unlock(&space_file->lock);

	/* May all good, who knows, not me */
	return status;
}

static int shaper_mcu_space_dev_open(struct inode *inode, struct file *file)
{
	struct shaper_mcu_space_file *space_file;
	struct shaper_mcu_space_device *space_dev = container_of(inode->i_cdev, struct shaper_mcu_space_device, cdev);

	/* Allocate the file data */
	space_file = kzalloc(sizeof(struct shaper_mcu_space_file), GFP_KERNEL);
	if (!space_file)
		return -ENOMEM;

	/* Basic initialization */
	space_file->space_dev = space_dev;
	space_file->marker = SHAPER_MCU_SPACE_MARKER;
	mutex_init(&space_file->lock);

	/* Hold on the underlaying device */
	kobject_get(&space_dev->this->kobj);
	kobject_get(&space_file->space_dev->parent->parent->dev.kobj);

	/* Stash it in the file struct */
	file->private_data = space_file;

	/* All good */
	return 0;
}

static int shaper_mcu_space_dev_release(struct inode *inode, struct file *file)
{
	struct shaper_mcu_space_file *space_file = file->private_data;

	/* Make sure we got the correct pointer */
	if (space_file->marker != SHAPER_MCU_SPACE_MARKER) {
		printk(KERN_ERR "%s - bad marker\n", __func__);
		return -EINVAL;
	}

	kobject_put(&space_file->space_dev->parent->parent->dev.kobj);
	kobject_put(&space_file->space_dev->this->kobj);

	kfree(space_file);

	return 0;
}

const struct file_operations space_dev_fops =
{
		.owner	= THIS_MODULE,
		.open	= shaper_mcu_space_dev_open,
		.release = shaper_mcu_space_dev_release,
		.read	= shaper_mcu_space_dev_read,
		.write	= shaper_mcu_space_dev_write,
		.unlocked_ioctl = shaper_mcu_space_dev_ioctl,
		.llseek		= noop_llseek,
};
