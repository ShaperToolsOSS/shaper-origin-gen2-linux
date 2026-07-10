// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Shaper Tools Inc, Stephen Street <stephen@shapertools.com>
 * Copyright (c) 2017 Pengutronix, Oleksij Rempel <kernel@pengutronix.de>
 * Copyright 2020 NXP, Peng Fan <peng.fan@nxp.com>
 */

#include "shaper-rproc.h"

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/slab.h>

#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/elf.h>
#include <linux/types.h>

#include <linux/mfd/syscon.h>

#include <uapi/shapertools/mcu-ioctl.h>

#define IMX8MM_SRC_SCR 0x0C
#define IMX8MM_ENABLE_M4 BIT(3)
#define IMX8MM_SW_M4P_RST BIT(2)
#define IMX8MM_SW_M4C_RST BIT(1)
#define IMX8MM_SW_M4C_NON_SCLR_RST BIT(0)

#define IMX8MM_M4_RST_MASK (IMX8MM_ENABLE_M4 | IMX8MM_SW_M4P_RST | IMX8MM_SW_M4C_RST | IMX8MM_SW_M4C_NON_SCLR_RST)
//#define IMX8MM_M4_START (IMX8MM_ENABLE_M4 | IMX8MM_SW_M4P_RST | IMX8MM_SW_M4C_RST)
#define IMX8MM_M4_START (IMX8MM_ENABLE_M4 | IMX8MM_SW_M4P_RST | IMX8MM_SW_M4C_RST)
#define IMX8MM_M4_STOP (IMX8MM_ENABLE_M4 | IMX8MM_SW_M4C_RST | IMX8MM_SW_M4C_NON_SCLR_RST)

#define ATT_OWN	BIT(31)
#define ATT_CORE_MASK 0xffff
#define ATT_CORE(I)	BIT((I))

#define REMOTE_IS_READY BIT(0)
#define REMOTE_READY_WAIT_MAX_RETRIES 5000

#define IMAGE_HEADER_PTR_OFFSET 0x0000001c
#define IMAGE_HEADER_MAGIC 0xBADA5559
#define IMAGE_LOAD_ADDR 0x00000000

struct shaper_rproc_image_header
{
	u32 image_magic;
	char image_name[64];
} __packed;

struct shaper_rproc_att
{
	u32 da; /* device address (From Cortex M4 view)*/
	u32 sa; /* system bus address */
	u32 size; /* size of reg range */
	int flags;
};

static const struct shaper_rproc_att shaper_rproc_att_imx8mm[] =
{
	/* dev addr , sys addr  , size	    , flags */
	/* TCML - alias */
	{0x00000000, 0x007e0000, 0x00020000, 0},
	/* OCRAM_S */
	{0x00180000, 0x00180000, 0x00008000, 0},
	/* OCRAM */
	{0x00900000, 0x00900000, 0x00020000, 0},
	/* OCRAM */
	{0x00920000, 0x00920000, 0x00020000, 0},
	/* QSPI Code - alias */
	{0x08000000, 0x08000000, 0x08000000, 0},
	/* DDR (Code) - alias */
	{0x10000000, 0x40000000, 0x0FFE0000, 0},
	/* DDR (Data) */
	{0x40000000, 0x40000000, 0x80000000, 0},

	/* TCML */
	{0x1ffe0000, 0x007e0000, 0x00020000, ATT_OWN},
	/* DDR Code */
	{0x1efe0000, 0x4efe0000, 0x00100000, ATT_OWN},
	/* TCMU */
	{0x20000000, 0x00800000, 0x00020000, ATT_OWN | ATT_CORE(0)},
	/* OCRAM_S */
	{0x20180000, 0x00180000, 0x00008000, ATT_OWN},
	/* OCRAM */
	{0x20200000, 0x00900000, 0x00020000, ATT_OWN},
	/* OCRAM */
	{0x20220000, 0x00920000, 0x00020000, ATT_OWN},
	/* DDR Data*/
	{0x4f0e0000, 0x4f0e0000, 0x00200000, ATT_OWN | ATT_CORE(1)},
};

extern irqreturn_t rproc_vq_interrupt(struct rproc *rproc, int notifyid);
extern u64 rproc_elf_get_boot_addr(struct rproc *rproc, const struct firmware *fw);
extern int rproc_elf_sanity_check(struct rproc *rproc, const struct firmware *fw);
extern int rproc_elf_load_segments(struct rproc *rproc, const struct firmware *fw);

static int shaper_m4_enable_clks(struct shaper_rproc *p)
{
    if (p->num_clks <= 0)
        return 0;
    return clk_bulk_prepare_enable(p->num_clks, p->clks);
}

static void shaper_m4_disable_clks(struct shaper_rproc *p)
{
    if (p->num_clks > 0)
        clk_bulk_disable_unprepare(p->num_clks, p->clks);
}

static int shaper_rproc_mem_alloc(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	void *va;

	dev_dbg(&rproc->dev, "%s - map memory %s: %pad+%zx\n", __func__, mem->name, &mem->dma, mem->len);
	va = ioremap_wc(mem->dma, mem->len);
	if (IS_ERR_OR_NULL(va)) {
		dev_err(&rproc->dev, "Unable to map memory region: %pad+%zx\n", &mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;
	return 0;
}

static int shaper_rproc_mem_release(struct rproc *rproc, struct rproc_mem_entry *mem)
{
	dev_dbg(&rproc->dev, "%s - release memory %s: %pad+%zx\n", __func__, mem->name, &mem->dma, mem->len);
	iounmap(mem->va);
	return 0;
}

static int shaper_rproc_register_memory(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	struct device_node *device_node = shaper_rproc->dev->of_node;
	struct of_phandle_iterator phandle_iterator;
	struct rproc_mem_entry *rproc_memory;
	struct reserved_mem *reserved_memory;

	/* Register associated reserved memory regions */
	of_phandle_iterator_init(&phandle_iterator, device_node, "memory-region", 0, 0);
	while (of_phandle_iterator_next(&phandle_iterator) == 0) {

		/* Look it up */
		reserved_memory = of_reserved_mem_lookup(phandle_iterator.node);
		if (!reserved_memory) {
			dev_err(shaper_rproc->dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}

		/* Initialize and register the rproc memory regions */
		rproc_memory = rproc_mem_entry_init(shaper_rproc->dev, 0, (dma_addr_t)reserved_memory->base, reserved_memory->size, reserved_memory->base, shaper_rproc_mem_alloc, shaper_rproc_mem_release, phandle_iterator.node->name);
		if (!rproc_memory)
			return -ENOMEM;
		/* rproc_coredump_add_segment(rproc, reserved_memory->base, reserved_memory->size); */
		rproc_add_carveout(rproc, rproc_memory);
		dev_info(shaper_rproc->dev, "registered rproc memory %s, %pad@%zu %p", rproc_memory->name, &rproc_memory->dma, rproc_memory->len, shaper_rproc_mem_release);
	}

	return 0;
}

static int shaper_rproc_of_parse_resource_table(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	struct device_node *device_node = shaper_rproc->dev->of_node;
	struct resource_table *resource_table;
	int resource_elements;
	int status;

	/*Parse device tree to get resource table */
	resource_elements = of_property_count_u32_elems(device_node, "rsrc-table");
	if (resource_elements < 0) {
		dev_err(&rproc->dev, "no dtb rsrc-table\n");
		return -EINVAL;
	}

	/* Get the space for the table */
	resource_table = kzalloc(resource_elements * sizeof(uint32_t), GFP_KERNEL);
	if (!resource_table)
		return -ENOMEM;

	/* load it up */
	status = of_property_read_u32_array(device_node, "rsrc-table", (uint32_t*)resource_table, resource_elements);
	if (status < 0) {
		dev_err(&rproc->dev, "failed to read rsrc-table: %d\n", status);
		kfree(resource_table);
		return status;
	}

	/* Handed rproc */
	rproc->table_ptr = resource_table;
	rproc->table_sz = resource_elements * sizeof(u32);

	/* All good */
	return 0;
}

static bool shaper_rproc_is_running(struct rproc *rproc)
{
	int status;
	uint32_t value;
	struct shaper_rproc *shaper_rproc = rproc->priv;

	/* Read the current reset state */
	status = regmap_read(shaper_rproc->syscon, IMX8MM_SRC_SCR, &value);
	if (status < 0) {
		dev_err(&rproc->dev, "Failed to read src: %d\n", status);
		return false;
	}

	return (value & IMX8MM_M4_STOP) != 0;
}

static bool shaper_rproc_ready(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	int i;

	for (i = 0; i < REMOTE_READY_WAIT_MAX_RETRIES; i++) {
		if (shaper_rproc->mu_flags & REMOTE_IS_READY)
			return 0;
		udelay(100);
	}

	return -ETIMEDOUT;
}

static int shaper_rproc_enable(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	uint32_t val;
	int status;

	if (!shaper_rproc->syscon)
		return -ENOTSUPP;

	status = regmap_read(shaper_rproc->syscon, IMX8MM_SRC_SCR, &val);
	if (status < 0)
		return status;

	if ((val & IMX8MM_M4_STOP) == 0) {
		dev_info(&rproc->dev, "already started\n");
		return 0;
	}

	return regmap_update_bits(shaper_rproc->syscon, IMX8MM_SRC_SCR, IMX8MM_M4_RST_MASK, IMX8MM_M4_START);
}

static int shaper_rproc_disable(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;

	if (!shaper_rproc->syscon)
		return -ENOTSUPP;

	return regmap_update_bits(shaper_rproc->syscon, IMX8MM_SRC_SCR, IMX8MM_M4_RST_MASK, IMX8MM_M4_STOP);
}

static int shaper_rproc_start(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	int status = 0;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	/* Enable it */
	status = shaper_rproc_enable(rproc);
	if (status < 0) {
		dev_err(shaper_rproc->dev, "Failed to enable M4!\n");
		return status;
	}

	/* Wait for the core to initialize */
	status = shaper_rproc_ready(rproc);
	if (status < 0) {
		dev_warn(shaper_rproc->dev, "M4 did not start: %d\n", status);
		goto error_disable;
	}

	dev_info(shaper_rproc->dev, "M4 ready\n");
	return 0;

error_disable:
	shaper_rproc_disable(rproc);

	return status;
}

static int shaper_rproc_stop(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	int status;
	u32 mmsg = 0;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	/* Send a knock for the shutdown */
	if (shaper_rproc->txdb_ch) {
		status = mbox_send_message(shaper_rproc->txdb_ch, (void*)&mmsg);
		if (status < 0) {
			dev_err(&rproc->dev, "txdb send fail: %d\n", status);
			return status;
		}
	}

	/* Disable the remote processor */
	status = shaper_rproc_disable(rproc);
	if (status < 0) {
		dev_err(&rproc->dev, "Failed to stop M4!\n");
		return status;
	}

	/* Reset the state */
	shaper_rproc->mu_flags &= ~REMOTE_IS_READY;

	/* All good */
	return 0;
}

static int shaper_rproc_attach(struct rproc *rproc)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	return 0;
}

static void shaper_rproc_kick(struct rproc *rproc, int vqid)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	uint32_t mmsg;
	int status;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	/* Make sure we have a mailbox channel to use */
	if (!shaper_rproc->tx_ch) {
		dev_err(shaper_rproc->dev, "No initialized mbox tx channel\n");
		return;
	}

	/* Send the queue id in the upper 16 bits */
	mmsg = vqid << 16;

	/* Send it off */
	shaper_rproc->xtr_client.tx_tout = 100;
	status = mbox_send_message(shaper_rproc->tx_ch, (void *)&mmsg);
	if (status < 0)
		dev_err(shaper_rproc->dev, "%s: failed sending vqid %d: %d)\n", __func__, vqid, status);
}

static int shaper_rproc_da_to_sys(struct shaper_rproc *shaper_rproc, uint64_t da, int len, uint64_t *sys)
{
	int i;

	/* parse address translation table */
	for (i = 0; i < ARRAY_SIZE(shaper_rproc_att_imx8mm); i++)
		if (da >= shaper_rproc_att_imx8mm[i].da && da + len <= shaper_rproc_att_imx8mm[i].da + shaper_rproc_att_imx8mm[i].size) {
			*sys = shaper_rproc_att_imx8mm[i].sa + (da - shaper_rproc_att_imx8mm[i].da);
			dev_dbg(shaper_rproc->dev, "%s - translated da = 0x%llx sys = 0x%llx\n", __func__, da, *sys);
			return 0;
		}

	dev_warn(shaper_rproc->dev, "device address to system address translation failed: da = 0x%llx len = 0x%x\n", da, len);
	return -ENOENT;
}

static void *shaper_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	void *va = 0;
	uint64_t sys;
	int i;

	/* Ignore zero length requests */
	if (len == 0) {
		return 0;
	}

	/* On device side we have many aliases, so we need to convert device address (M4) to system bus address first. */
	if (shaper_rproc_da_to_sys(shaper_rproc, da, len, &sys) < 0) {
		return 0;
	}

	/* Now search the local memory mapping table */
	for (i = 0; i < IMX8MM_RPROC_MEM_MAX; i++) {
		if (sys >= shaper_rproc->mem[i].sys_addr && sys + len <= shaper_rproc->mem[i].sys_addr + shaper_rproc->mem[i].size) {
			unsigned int offset = sys - shaper_rproc->mem[i].sys_addr;
			va = (__force void*)(shaper_rproc->mem[i].cpu_addr + offset);
			break;
		}
	}

	dev_dbg(&rproc->dev, "%s - da = 0x%llx len = 0x%zx va = 0x%p\n", __func__, da, len, va);

	/* Should be good, but might be zero */
	return va;
}

static int shaper_rproc_sanity_check(struct rproc *rproc, const struct firmware *fw)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	return rproc_elf_sanity_check(rproc, fw);
}

static int shaper_rproc_parse_descriptor(struct rproc *rproc, const struct firmware *fw)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	Elf32_Ehdr *elf_header;
	Elf32_Shdr *section_headers;
	Elf32_Shdr *section_names_header;
	char *section_names;
	int i;
	int status;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	/* Is this a good elf? */
	status = shaper_rproc_sanity_check(rproc, fw);
	if (status < 0) {
		dev_err(&rproc->dev, "bad elf image: %d\n", status);
		return status;
	}

	/* Look for the descriptors section */
	elf_header = (void *)fw->data;
	section_headers = (Elf32_Shdr *)(fw->data + elf_header->e_shoff);
	section_names_header = &section_headers[elf_header->e_shstrndx];
	section_names = (char *)(fw->data + section_names_header->sh_offset);
	for (i = 0; i < elf_header->e_shnum; ++i)
		if (strcmp(section_names + section_headers[i].sh_name, ".descriptor") == 0 && section_headers[i].sh_size > 0) {

			/* Keep a copy of the descriptor section */
			shaper_rproc->desc_section.data = kmemdup(fw->data + section_headers[i].sh_offset, section_headers[i].sh_size, GFP_KERNEL);
			if (!shaper_rproc->desc_section.data) {
				dev_err(&rproc->dev, "could not duplicate descriptor section of size: %u\n", section_headers[i].sh_size);
				return -ENOMEM;
			}
			shaper_rproc->desc_section.size = section_headers[i].sh_size;

			/* All good */
			return 0;
		}

	/* Did not find a descriptor */
	return -ENOENT;
}

static int shaper_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	int status;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	status = shaper_rproc_register_memory(rproc);
	if (status < 0) {
		dev_err(&rproc->dev, "failed to register memory: %d\n", status);
		return status;
	}

	status = shaper_rproc_of_parse_resource_table(rproc);
	if (status < 0) {
		dev_err(&rproc->dev, "failed to parse resource table: %d\n", status);
		return status;
	}

	status = shaper_rproc_parse_descriptor(rproc, fw);
	if (status < 0)
		dev_warn(&rproc->dev, "%s - did not find descriptor section\n", __func__);

	/* Well ok, but */
	return 0;
}

static struct resource_table *shaper_rproc_find_loaded_rsc_table(struct rproc *rproc, const struct firmware *fw)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	return 0;
}

static u64 shaper_rproc_get_boot_addr(struct rproc *rproc, const struct firmware *fw)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	return rproc_elf_get_boot_addr(rproc, fw);
}

static void shaper_rproc_elf_memcpy(struct rproc *rproc, void *dest, const void *src, size_t count)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	memcpy_toio((void * __iomem)dest, src, count);
}

static void shaper_rproc_elf_memset(struct rproc *rproc, void *s, int c, size_t count)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	memset_io((void * __iomem)s, c, count);
}

static unsigned long shaper_rproc_panic(struct rproc *rproc)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	return 0;
}

static int shaper_rproc_prepare(struct rproc *rproc)
{
	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);
	return 0;
}

static int shaper_rproc_unprepare(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	void *image_addr;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	/* Clean up descriptor */
	kfree(shaper_rproc->desc_section.data);
	shaper_rproc->desc_section.data = 0;
	shaper_rproc->desc_section.size = 0;

	/* Ensure any old image header is erased */
	image_addr = shaper_rproc_da_to_va(rproc, IMAGE_LOAD_ADDR, PAGE_SIZE);
	if (!image_addr) {
		dev_err(&rproc->dev, "could not translate image load address\n");
		return -ENOMEM;
	}
	shaper_rproc_elf_memset(rproc, image_addr, 0, PAGE_SIZE);

	return 0;
}

static int shaper_rproc_load(struct rproc *rproc, const struct firmware *fw)
{
	const struct elf32_hdr *ehdr;
	const struct elf32_phdr *phdr;
	int i;
	void *va;

	struct device *dev = &rproc->dev;
	int ret = 0;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	ehdr = (struct elf32_hdr *)fw->data;
	phdr = (struct elf32_phdr *)(fw->data + ehdr->e_phoff);

	/* Loop through the available ELF segments */
	for (i = 0; i < ehdr->e_phnum; i++, ++phdr) {

		/* Only handle segments marked as load */
		if (phdr->p_type != PT_LOAD)
			continue;

		dev_dbg(dev, "phdr: type %d da 0x%x memsz 0x%x filesz 0x%x\n", phdr->p_type, phdr->p_paddr, phdr->p_memsz, phdr->p_filesz);

		/* Validate that the section size */
		if (phdr->p_filesz > phdr->p_memsz) {
			dev_err(dev, "bad phdr filesz 0x%x memsz 0x%x\n", phdr->p_filesz, phdr->p_memsz);
			ret = -EINVAL;
			break;
		}

		/* Validate the section size against the actual firmware size */
		if (phdr->p_offset + phdr->p_filesz > fw->size) {
			dev_err(dev, "truncated fw: need 0x%x avail 0x%zx\n",	phdr->p_offset + phdr->p_filesz, fw->size);
			ret = -EINVAL;
			break;
		}

		/* Grab the kernel address for this device address */
		va = shaper_rproc_da_to_va(rproc, phdr->p_paddr, phdr->p_memsz);
		if (!va) {
			dev_err(dev, "bad phdr da 0x%x size 0x%x\n", phdr->p_paddr, phdr->p_memsz);
			ret = -EINVAL;
			break;
		}

		/* Put the segment where the remote processor expects it */
		if (phdr->p_filesz)
			shaper_rproc_elf_memcpy(rproc, va, fw->data + phdr->p_offset, phdr->p_filesz);

		/* Clear unused memory, not required do careful */
		if (phdr->p_memsz > phdr->p_filesz)
			shaper_rproc_elf_memset(rproc, va + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);

		/* Register as a core dump segment */
		rproc_coredump_add_segment(rproc, phdr->p_paddr, phdr->p_memsz);
	}

	return ret;
}

static int shaper_rproc_add_coredump_seqments(struct rproc *rproc, const struct firmware *fw)
{
	const struct elf32_hdr *ehdr;
	const struct elf32_phdr *phdr;
	int i;

	struct device *dev = &rproc->dev;
	int status = 0;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	if (list_empty(&rproc->dump_segments)) {

		ehdr = (struct elf32_hdr *)fw->data;
		phdr = (struct elf32_phdr *)(fw->data + ehdr->e_phoff);

		/* Loop through the available ELF segments */
		for (i = 0; i < ehdr->e_phnum; i++, ++phdr) {

			/* Only handle segments marked as load */
			if (phdr->p_type != PT_LOAD)
				continue;

			dev_dbg(dev, "phdr: type %d da 0x%x memsz 0x%x filesz 0x%x\n", phdr->p_type, phdr->p_paddr, phdr->p_memsz, phdr->p_filesz);

			/* Validate that the section size */
			if (phdr->p_filesz > phdr->p_memsz) {
				dev_err(dev, "bad phdr filesz 0x%x memsz 0x%x\n", phdr->p_filesz, phdr->p_memsz);
				status = -EINVAL;
				break;
			}

			/* Validate the section size against the actual firmware size */
			if (phdr->p_offset + phdr->p_filesz > fw->size) {
				dev_err(dev, "truncated fw: need 0x%x avail 0x%zx\n",	phdr->p_offset + phdr->p_filesz, fw->size);
				status = -EINVAL;
				break;
			}

			/* Register as a core dump segment */
			rproc_coredump_add_segment(rproc, phdr->p_paddr, phdr->p_memsz);
		}
	}

	return status;
}

static struct rproc_ops shaper_rproc_ops =
{
	.prepare = shaper_rproc_prepare,
	.unprepare = shaper_rproc_unprepare,
	.start = shaper_rproc_start,
	.stop = shaper_rproc_stop,
	.attach = shaper_rproc_attach,
	.kick = shaper_rproc_kick,
	.parse_fw = shaper_rproc_parse_fw,
	.find_loaded_rsc_table = shaper_rproc_find_loaded_rsc_table,
	.load = shaper_rproc_load,
	.sanity_check = shaper_rproc_sanity_check,
	.get_boot_addr = shaper_rproc_get_boot_addr,
	.panic = shaper_rproc_panic,
};

static long shaper_rproc_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int status = 0;
	struct shaper_rproc *shaper_rproc = file->private_data;
	struct rproc *rproc = shaper_rproc->rproc;
	unsigned long *value_ptr = (unsigned long *)arg;
	struct mcu_ioctl_load mcu_load;
	const char *firmware;

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != _MCU_IOCTL_TYPE)
		return -ENOTTY;

	/* Check direction */
	if ((_IOC_DIR(cmd) & _IOC_READ) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;
	if ((_IOC_DIR(cmd) & _IOC_WRITE) && !access_ok((void __user *)arg, _IOC_SIZE(cmd)))
		return -EFAULT;

	/* Dispatch */
	switch (_IOC_NR(cmd)) {

	case _MCU_IOCTL_CMD_CTL:

		switch (arg) {

			case MCU_CTL_RESET:

				/* Make sure we have a firmware name to load */
				if (rproc->firmware == 0 || strlen(rproc->firmware) == 0) {
					dev_warn(&rproc->dev, "no firmware loaded\n");
					status = -EINVAL;
					break;
				}

				/* First send a shutdown */
				rproc_shutdown(rproc);

				/* Now boot it */
				status = rproc_boot(rproc);

				break;

			case MCU_CTL_STOP:

				/* Send the shutdown */
				rproc_shutdown(rproc);

				break;

			case MCU_CTL_START:

				/* Make sure we have a firmware name to load */
				if (rproc->firmware == 0 || strlen(rproc->firmware) == 0) {
					dev_warn(&rproc->dev, "no firmware loaded\n");
					status = -EINVAL;
					break;
				}

				/* Now boot it */
				status = rproc_boot(rproc);

				break;

			case MCU_CTL_CRASH:

				/* Send the crash detected */
				dev_warn(&rproc->dev, "reporting fatal error\n");
				rproc_report_crash(rproc, RPROC_FATAL_ERROR);

				break;

			default:
				status = -ENOTTY;
				break;
		}

		break;

	case _MCU_IOCTL_CMD_STATE:
		if (put_user(shaper_rproc_is_running(rproc), value_ptr) != 0)
			status = -EFAULT;
				break;

	case _MCU_IOCTL_CMD_LOAD:

		/* Get the load command from user space */
		if (copy_from_user(&mcu_load, (struct mcu_ioctl_load *)arg, sizeof(struct mcu_ioctl_load))) {
			status = -EFAULT;
			break;
		}

		/* Always send the shutdown */
		rproc_shutdown(rproc);

		/* Load the new firmware name */
		mutex_lock(&rproc->lock);
		firmware = rproc->firmware;
		rproc->firmware = kstrdup(mcu_load.path, GFP_KERNEL);
		if (!rproc->firmware) {
			rproc->firmware = firmware;
			status = -ENOMEM;
			mutex_unlock(&rproc->lock);
			break;
		}
		kfree(firmware);
		mutex_unlock(&rproc->lock);

		/* if request, start it up */
		if (mcu_load.flags & MCU_LOAD_LAUNCH)
			status = rproc_boot(rproc);

		break;

	default:

		/* Opp unknown ioctl */
		dev_warn(&rproc->dev, "invalid cmd: %u\n", _IOC_NR(cmd));
		status = -ENOTTY;
		break;
	}

	/* May all good, who knows, not me */
	return status;
}

static int shaper_rproc_open(struct inode *inode, struct file *file)
{
	struct shaper_rproc *shaper_rproc = container_of(inode->i_cdev, struct shaper_rproc, cdev);
	struct rproc *rproc = shaper_rproc->rproc;

	/* Hold on the underlaying device */
	get_device(&rproc->dev);

	/* Stash everything in the file struct */
	file->private_data = shaper_rproc;

	/* All good */
	return 0;
}

static int shaper_rproc_release(struct inode *inode, struct file *file)
{
	struct shaper_rproc *shaper_rproc = container_of(inode->i_cdev, struct shaper_rproc, cdev);
	struct rproc *rproc = shaper_rproc->rproc;

	/* Release our hold on the device */
	put_device(&rproc->dev);

	return 0;
}

const struct file_operations shaper_rproc_fops =
{
		.owner = THIS_MODULE,
		.open= shaper_rproc_open,
		.release = shaper_rproc_release,
		.unlocked_ioctl = shaper_rproc_ioctl,
		.llseek	= noop_llseek,
};

/**
 * shaper_rproc_image_name() - Get the firmware image name from M4 memory
 * @rproc: the remote processor
 * @addr: base address to search for image header
 *
 * Safely reads the firmware name from M4 memory and returns a copy in kernel memory.
 *
 * Return: Allocated string containing firmware name (caller must free with kfree()),
 *         or NULL on error.
 */
char *shaper_rproc_image_name(struct rproc *rproc, uint32_t addr)
{
	uint32_t *image_header_va;
	struct shaper_rproc_image_header *image_header;
	char firmware_name_copy[64]; /* Same size as image_header->image_name */
	char *safe_name;

	/* A pointer to the image header is stashed at the M4 isr vector[7] */
	image_header_va = shaper_rproc_da_to_va(rproc, addr + IMAGE_HEADER_PTR_OFFSET, sizeof(uint32_t));
	if (!image_header_va) {
		dev_err(&rproc->dev, "could not map the image header pointer address vector[7]\n");
		return NULL;
	}

	/* Get a pointer image header */
	image_header = shaper_rproc_da_to_va(rproc, *image_header_va, sizeof(*image_header));
	if (!image_header) {
		dev_err(&rproc->dev, "could not map the image header: 0x%08x\n", *image_header_va);
		return NULL;
	}
	dev_dbg(&rproc->dev, "%s - image_header=0x%p, image_magic=0x%08x\n", __func__, image_header, image_header->image_magic);

	/* Check the image magic number */
	if ((image_header->image_magic & 0xffffff00) != 0xBADA5500)  {
		dev_warn(&rproc->dev, "bad image header: 0x%08x\n", image_header->image_magic);
		return NULL;
	}

	/* Safely copy from I/O memory to kernel memory first */
	strncpy(firmware_name_copy, image_header->image_name, sizeof(firmware_name_copy) - 1);
	firmware_name_copy[sizeof(firmware_name_copy) - 1] = '\0'; /* Ensure null termination */

	dev_dbg(&rproc->dev, "%s - found image name: %s\n", __func__, firmware_name_copy);

	/* Create a safe kernel memory copy that caller can use */
	safe_name = kstrdup(firmware_name_copy, GFP_KERNEL);
	if (!safe_name) {
		dev_err(&rproc->dev, "Failed to allocate memory for firmware name\n");
		return NULL;
	}

	return safe_name;
}
EXPORT_SYMBOL(shaper_rproc_image_name);

const struct firmware *shaper_rproc_descriptor(struct rproc *rproc)
{
	struct shaper_rproc *shaper_rproc = rproc->priv;
	char *image_name;
	const struct firmware *fw;
	int status;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	/* Already have a cached descriptor? */
	if (shaper_rproc->desc_section.data)
		return &shaper_rproc->desc_section;

	/* Try to get the image name from the header */
	image_name = shaper_rproc_image_name(rproc, IMAGE_LOAD_ADDR);
	if (image_name == NULL) {
		dev_info(&rproc->dev, "no image name found\n");
		return ERR_PTR(-EINVAL);
	}

	/* Now try to load the fw image */
	status = request_firmware_direct(&fw, image_name, &rproc->dev);
	if (status < 0) {
		dev_info(&rproc->dev, "could not load firmware image '%s': %d\n", image_name, status);
		kfree(image_name); /* Free the allocated string */
		return ERR_PTR(status);
	}
	dev_dbg(&rproc->dev, "%s - loaded elf '%s'\n", __func__, image_name);

	/* Next try to load the descriptor */
	status = shaper_rproc_parse_descriptor(rproc, fw);
	if (status < 0)
		dev_warn(&rproc->dev, "no descriptor found in image '%s'\n", image_name);

	/* Also load the dump segments if they have not been loaded */
	shaper_rproc_add_coredump_seqments(rproc, fw);

	/* All done with the firmware */
	release_firmware(fw);

	/* Free the allocated image name */
	kfree(image_name);

	/* All done */
	return status == 0 ? &shaper_rproc->desc_section : ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL(shaper_rproc_descriptor);

static void shaper_rproc_rxdb_callback(struct mbox_client *client, void *msg)
{
	struct rproc *rproc = dev_get_drvdata(client->dev);
	struct shaper_rproc *shaper_rproc = rproc->priv;
	unsigned long flags;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	spin_lock_irqsave(&shaper_rproc->mu_lock, flags);
	shaper_rproc->mu_flags |= REMOTE_IS_READY;
	spin_unlock_irqrestore(&shaper_rproc->mu_lock, flags);
}

static void shaper_rproc_vq_work(struct work_struct *work)
{
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct shaper_rproc	*shaper_rproc = container_of(delayed_work, struct shaper_rproc, delayed_work);

	dev_dbg(&shaper_rproc->rproc->dev, "%s - invoked\n", __func__);

	/* TODO: take message from rx_callback */
	rproc_vq_interrupt(shaper_rproc->rproc, 0);
	rproc_vq_interrupt(shaper_rproc->rproc, 1);
	rproc_vq_interrupt(shaper_rproc->rproc, 2);
	rproc_vq_interrupt(shaper_rproc->rproc, 3);
}

static void shaper_rproc_rx_callback(struct mbox_client *cl, void *msg)
{
	struct rproc *rproc = dev_get_drvdata(cl->dev);
	struct shaper_rproc *shaper_rproc = rproc->priv;

	dev_dbg(&rproc->dev, "%s - invoked\n", __func__);

	schedule_delayed_work(&shaper_rproc->delayed_work, 0);
}

static int shaper_rproc_probe(struct platform_device *pdev)
{
	int status;
	struct rproc *rproc;
	struct shaper_rproc *shaper_rproc;
	int i;
	int memory_region_count;
	struct device_node *memory_region_node;
	struct resource memory_region_resource;
	uint32_t scr_reg;
	struct device_node *device_node = pdev->dev.of_node;
	const struct regmap_config syscon_regmap_config = { .name = "shaper-m4-rproc" };
	int index = 0;
	char *running_firmware = 0;

	dev_dbg(&pdev->dev, "%s - invoked\n", __func__);

	/* Initialize the new rproc, including ourselves */
	rproc = rproc_alloc(&pdev->dev, "shaper-m4-rproc", &shaper_rproc_ops, 0, sizeof(*shaper_rproc));
	if (!rproc)
		return -ENOMEM;

	/* Basic initialization */
	shaper_rproc = rproc->priv;
	shaper_rproc->rproc = rproc;
	shaper_rproc->dev = &pdev->dev;
	rproc->auto_boot = false;
	rproc->dump_conf = RPROC_COREDUMP_ENABLED;
	spin_lock_init(&shaper_rproc->mu_lock);
	INIT_DELAYED_WORK(&shaper_rproc->delayed_work, shaper_rproc_vq_work);

	/* Set up the elf class for core dump handling */
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_ARM);

	/* Bind the rproc driver data to the pdev */
	platform_set_drvdata(pdev, rproc);

	/* Get the system control register map from the dts */
	shaper_rproc->syscon = syscon_regmap_lookup_by_phandle(device_node, "syscon");
	if (IS_ERR(shaper_rproc->syscon)) {
		dev_err(&pdev->dev, "failed to find syscon: %ld\n", PTR_ERR(shaper_rproc->syscon));
		status = PTR_ERR(shaper_rproc->syscon);
		goto error_free_rproc;
	}

	/* Attach to the remap */
	status = regmap_attach_dev(&pdev->dev, shaper_rproc->syscon, &syscon_regmap_config);
	if (status < 0) {
		dev_err(&pdev->dev, "failed attach syscon regmap: %d\n", status);
		goto error_free_rproc;
	}

	/* Initialize the transmit/receive mbox client */
	shaper_rproc->xtr_client.dev = &pdev->dev;
	shaper_rproc->xtr_client.tx_block = true;
	shaper_rproc->xtr_client.tx_tout = 50;
	shaper_rproc->xtr_client.knows_txdone = false;
	shaper_rproc->xtr_client.rx_callback = shaper_rproc_rx_callback;

	/* Request the transmit channel */
	shaper_rproc->tx_ch = mbox_request_channel_byname(&shaper_rproc->xtr_client, "tx");
	if (IS_ERR(shaper_rproc->tx_ch)) {
		status = PTR_ERR(shaper_rproc->tx_ch);
		dev_err(&pdev->dev, "failed to request mbox tx chan: %d\n", status);
		goto error_free_rproc;
	}

	/* Request the receive channel */
	shaper_rproc->rx_ch = mbox_request_channel_byname(&shaper_rproc->xtr_client, "rx");
	if (IS_ERR(shaper_rproc->rx_ch)) {
		status = PTR_ERR(shaper_rproc->rx_ch);
		dev_err(&pdev->dev, "failed to request mbox rx chan: %d\n", status);
		goto error_free_tx;
	}

	/* TX door bell used to let an already running M4 enable the virtqueue handling */
	shaper_rproc->txdb_client.dev = &pdev->dev;
	shaper_rproc->txdb_client.tx_block = true;
	shaper_rproc->txdb_client.tx_tout = 50;
	shaper_rproc->txdb_client.knows_txdone = false;
	shaper_rproc->txdb_ch = mbox_request_channel_byname(&shaper_rproc->txdb_client, "txdb");
	if (IS_ERR(shaper_rproc->txdb_ch)) {
		status = PTR_ERR(shaper_rproc->txdb_ch);
		dev_err(&pdev->dev, "failed to request the txdb: %d\n", status);
		goto error_free_rx;
	}

	/* RX door bell is used to receive the ready signal from remote after the partition reset of A core */
	shaper_rproc->rxdb_client.dev = &pdev->dev;
	shaper_rproc->rxdb_client.rx_callback = shaper_rproc_rxdb_callback;
	shaper_rproc->rxdb_ch = mbox_request_channel_byname(&shaper_rproc->rxdb_client, "rxdb");
	if (IS_ERR(shaper_rproc->rxdb_ch)) {
		status = PTR_ERR(shaper_rproc->rxdb_ch);
		dev_err(&pdev->dev, "failed to request the rxdb: %d\n", status);
		goto error_free_txdb;
	}

	/* Remap required addresses */
	for (i = 0; i < ARRAY_SIZE(shaper_rproc_att_imx8mm); ++i) {

		/* Skip is we should not map this entry */
		if (!(shaper_rproc_att_imx8mm[i].flags & ATT_OWN))
			continue;

		/* Map this entry */
		shaper_rproc->mem[index].cpu_addr = devm_ioremap(&pdev->dev, shaper_rproc_att_imx8mm[i].sa, shaper_rproc_att_imx8mm[i].size);
		if (IS_ERR_OR_NULL(shaper_rproc->mem[index].cpu_addr)) {
			status = PTR_ERR(shaper_rproc->mem[index].cpu_addr);
			dev_err(&pdev->dev, "devm_ioremap_resource failed: %d\n", status);
			goto error_free_rxdb;
		}
		shaper_rproc->mem[index].sys_addr = shaper_rproc_att_imx8mm[i].sa;
		shaper_rproc->mem[index].size = shaper_rproc_att_imx8mm[i].size;

		dev_info(&pdev->dev, "mapped mem[%d] da: 0x%08x sa: 0x%08x size: %u\n", index, shaper_rproc_att_imx8mm[i].da, shaper_rproc_att_imx8mm[i].sa, shaper_rproc_att_imx8mm[i].size);

		/* Move the the next entry */
		++index;
	}

	/* Find the required memory-region  */
	memory_region_count = of_count_phandle_with_args(device_node, "memory-region", 0);
	if (memory_region_count == 0) {
		dev_err(&pdev->dev, "did not find memory-region");
		status = -EINVAL;
		goto error_free_rxdb;
	}

	/* Remap provided addresses */
	for (i = 0; i < memory_region_count; i++) {

		memory_region_node = of_parse_phandle(device_node, "memory-region", i);
		status = of_address_to_resource(memory_region_node, 0, &memory_region_resource);
		if (status < 0) {
			dev_err(&pdev->dev, "unable to resolve memory region\n");
			of_node_put(memory_region_node);
			goto error_free_rxdb;
		}
		of_node_put(memory_region_node);

		/* Map this entry */
		shaper_rproc->mem[index].cpu_addr = devm_ioremap(&pdev->dev, memory_region_resource.start, resource_size(&memory_region_resource));
		if (IS_ERR_OR_NULL(shaper_rproc->mem[index].cpu_addr)) {
			status = PTR_ERR(shaper_rproc->mem[index].cpu_addr);
			dev_err(&pdev->dev, "devm_ioremap failed: %d\n", status);
			goto error_free_rxdb;
		}
		shaper_rproc->mem[index].sys_addr = memory_region_resource.start;
		shaper_rproc->mem[index].size = resource_size(&memory_region_resource);

		dev_info(&pdev->dev, "mapped mem[%d] memory-region: sa: 0x%08llx size: %llu\n", index, memory_region_resource.start, resource_size(&memory_region_resource));

		/* Move to the next entry */
		++index;
	}

	/* Get the current m4 power state */
	status = regmap_read(shaper_rproc->syscon, IMX8MM_SRC_SCR, &scr_reg);
	if (status < 0) {
		dev_err(&pdev->dev, "Failed to read src: %d\n", status);
		goto error_free_rxdb;
	}

	/* If early boot, then continue with detached setup */
	if ((scr_reg & IMX8MM_M4_RST_MASK) != IMX8MM_M4_STOP) {

		/* Force into the detached state */
		rproc->state = RPROC_DETACHED;

		/* Register the vring memory regions */
		status = shaper_rproc_register_memory(rproc);
		if (status < 0) {
			dev_err(&rproc->dev, "failed to register memory: %d\n", status);
			goto error_free_rxdb;
		}

		/* Parse the resource table */
		status = shaper_rproc_of_parse_resource_table(rproc);
		if (status < 0) {
			dev_err(&rproc->dev, "failed to parse resource table: %d\n", status);
			goto error_free_rxdb;
		}

		/* Try to update the name of the running firmware, need to clear the autonomous flag show sysfs works as expected */
		running_firmware = shaper_rproc_image_name(rproc, 0);
		if (running_firmware) {
			/* Replace the default firmware name with the running one */
			kfree(rproc->firmware);
			rproc->firmware = running_firmware; /* Take ownership of the allocated string */
		}

		dev_info(&pdev->dev, "M4 is RUNNING with firmware %s\n", running_firmware ? running_firmware : "UNKNOWN");

	} else
		dev_info(&pdev->dev, "M4 is IDLE\n");

	/* Get the required m4 clocks */
    shaper_rproc->num_clks = devm_clk_bulk_get_all(&pdev->dev, &shaper_rproc->clks);
    if (shaper_rproc->num_clks < 0)
        return dev_err_probe(&pdev->dev, shaper_rproc->num_clks, "Failed to get clocks\n");

	/* Enable the m4 clocks, it does not matter if it was already enabled */
	status = shaper_m4_enable_clks(shaper_rproc);
	if (status < 0) {
		dev_err(&pdev->dev, "Failed to enable clocks: %d\n", status);
		goto error_free_resource_table;
	}

	/* Allocate a char device region for io */
	status = alloc_chrdev_region(&rproc->dev.devt, 0, 1, "shaper-m4-rproc");
	if (status < 0) {
		dev_err(&pdev->dev, "%s could not allocate cdev region: %d\n", __func__, status);
		goto error_disable_clk;
	}

	/* Initialize the cdev add the cdev */
	cdev_init(&shaper_rproc->cdev, &shaper_rproc_fops);
	status = cdev_add(&shaper_rproc->cdev, rproc->dev.devt, 1);
	if (status < 0) {
		dev_err(&pdev->dev, "%s could not add cdev: %d\n", __func__, status);
		goto error_unregister_chrdev_region;
	}

	/* Add the new rproc to the framework */
	status = devm_rproc_add(&pdev->dev, rproc);
	if (status) {
		dev_err(&pdev->dev, "rproc_add failed: %d\n", status);
		goto error_del_cdev;
	}

	/* Finally all good */
	dev_info(&pdev->dev, "shaper_rproc_probe completed successfully\n");
	return 0;

error_del_cdev:
	cdev_del(&shaper_rproc->cdev);

error_unregister_chrdev_region:
	unregister_chrdev_region(rproc->dev.devt, 1);

error_disable_clk:
	shaper_m4_disable_clks(shaper_rproc);

error_free_resource_table:
	kfree(rproc->table_ptr);

error_free_rxdb:
	mbox_free_channel(shaper_rproc->rxdb_ch);

error_free_txdb:
	mbox_free_channel(shaper_rproc->txdb_ch);

error_free_rx:
	mbox_free_channel(shaper_rproc->rx_ch);

error_free_tx:
	mbox_free_channel(shaper_rproc->tx_ch);

error_free_rproc:
	rproc_free(rproc);
	dev_info(&pdev->dev, "shaper_rproc_probe failed with status %d\n", status);
	return status;
}

static void shaper_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	struct shaper_rproc *shaper_rproc = rproc->priv;

	dev_dbg(&pdev->dev, "%s - invoked\n", __func__);

	rproc_del(rproc);

	cdev_del(&shaper_rproc->cdev);
	unregister_chrdev_region(rproc->dev.devt, 1);
	shaper_m4_disable_clks(shaper_rproc);
	mbox_free_channel(shaper_rproc->rxdb_ch);
	mbox_free_channel(shaper_rproc->txdb_ch);
	mbox_free_channel(shaper_rproc->rx_ch);
	mbox_free_channel(shaper_rproc->tx_ch);

	rproc_free(rproc);
}

static const struct of_device_id shaper_rproc_of_match[] =
{
	{
		.compatible = "shapertools,shaper-mcu-rproc",
	},
	{
	},
};
MODULE_DEVICE_TABLE(of, shaper_rproc_of_match);

static struct platform_driver shaper_rproc_driver =
{
	.probe = shaper_rproc_probe,
	.remove = shaper_rproc_remove,
	.driver =
	{
		.name = "shaper-mcu-rproc",
		.of_match_table = shaper_rproc_of_match,
	},
};
module_platform_driver(shaper_rproc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("IMX8MM remote processor control driver");
MODULE_AUTHOR("Stephen Street <stephen@shapertools.com>");
