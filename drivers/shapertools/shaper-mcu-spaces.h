#ifndef _SHAPER_MCU_SPACES_H_
#define _SHAPER_MCU_SPACES_H_

#include <linux/ioctl.h>
#include <linux/types.h>

#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/mailbox_client.h>
#include <linux/sysfs.h>
#include <linux/completion.h>
#include <linux/cdev.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>

#include "space-descriptor.h"

#define SHAPER_NUM_SPACES 64UL
#define SHAPER_MCU_SPACE_MARKER 0x01377310UL

#define CMD_MAP 0UL
#define CMD_READ 1UL
#define CMD_WRITE 2UL
#define CMD_EXT 3UL

#define CMD_DONE 0UL
#define CMD_CRASHED 0xffffffffUL

struct space_cmd
{
	union {
		uint32_t value;
		struct {
			unsigned int cmd_type : 2;
			unsigned int cmd_space: 6;
			unsigned int cmd_offset: 12;
			unsigned int cmd_size: 12;
		};
		struct {
			unsigned int ext_type : 2;
			unsigned int ext_data : 30;
		};
	};
} __attribute__((packed));

struct shaper_mcu_space_device;

struct shaper_mcu_space_platform_device
{
	unsigned int marker;

	struct platform_device *parent;

	struct class *class;
	const char *name;
	dev_t devt;

	unsigned int major;
	struct mbox_client xtr_client;
	struct mbox_client txdb_client;
	struct mbox_client rxdb_client;
	struct mbox_chan *tx_chan;
	struct mbox_chan *rx_chan;
	struct mbox_chan *txdb_chan;
	struct mbox_chan *rxdb_chan;
	struct reserved_mem *space_mem;
	void *space_mem_addr;
	struct mutex mbox_lock;

	struct mutex lock;
	struct list_head devices;
	struct list_head events;

	spinlock_t devices_lock;
	spinlock_t events_lock;
	spinlock_t active_lock;

	struct rproc_subdev rproc_subdev;
	struct rproc *remote_proc;

	struct space_descriptor **descs;

	struct shaper_mcu_space_device *active;
};

struct shaper_mcu_space_device
{
	unsigned int marker;

	struct device *this;
	struct cdev cdev;

	struct shaper_mcu_space_platform_device *parent;

	dev_t devt;
	umode_t mode;

	struct mutex lock;
	void *mem;
	struct list_head node;
	struct completion done;

	struct space_descriptor *desc;
};

extern int shaper_mcu_space_capture(struct shaper_mcu_space_device *space_dev, off_t offset, size_t size);
extern int shaper_mcu_space_flush(struct shaper_mcu_space_device *space_dev, off_t offset, size_t size);

extern struct shaper_mcu_space_device *shaper_mcu_get_space_device(struct shaper_mcu_space_platform_device *space_pdev, unsigned int space);
extern void shaper_mcu_put_space_device(struct shaper_mcu_space_device *space_dev);

extern int shaper_mcu_space_register(struct shaper_mcu_space_device *space_dev, const struct space_descriptor *desc, const struct file_operations *fops, struct module *owner);
extern int shaper_mcu_space_unregister(struct shaper_mcu_space_device *space_dev);

extern const struct file_operations space_pdev_fops;
extern const struct file_operations space_dev_fops;

#endif
