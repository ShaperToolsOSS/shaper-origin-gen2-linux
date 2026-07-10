#ifndef _SHAPER_RPROC_H_
#define _SHAPER_RPROC_H_

#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/remoteproc.h>
#include <linux/regmap.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/mailbox_client.h>

#define IMX8MM_RPROC_MEM_MAX 16

struct shaper_rproc_mem
{
	void __iomem *cpu_addr;
	phys_addr_t sys_addr;
	size_t size;
};

struct shaper_rproc
{
	struct rproc *rproc;
	struct device *dev;
	struct cdev cdev;
	struct regmap *syscon;
	struct clk_bulk_data *clks;
	int num_clks;
	struct shaper_rproc_mem mem[IMX8MM_RPROC_MEM_MAX];

	struct firmware desc_section;

	struct mbox_client xtr_client;
	struct mbox_client txdb_client;
	struct mbox_client rxdb_client;
	struct mbox_chan *tx_ch;
	struct mbox_chan *rx_ch;
	struct mbox_chan *rxdb_ch;
	struct mbox_chan *txdb_ch;
	uint32_t mub_partition;
	uint32_t mu_flags;
	spinlock_t mu_lock;
	struct delayed_work delayed_work;

};

char *shaper_rproc_image_name(struct rproc *rproc, uint32_t addr);
const struct firmware *shaper_rproc_descriptor(struct rproc *rproc);

#endif
