// SPDX-License-Identifier: GPL-2.0+
// Copyright 2018 Shaper Tools Inc. All Rights Reserved.
// Copyright 2004-2007 Freescale Semiconductor, Inc. All Rights Reserved.
// Copyright (C) 2008 Juergen Beisert

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <linux/platform_data/dma-imx.h>
#include <linux/platform_data/spi-imx.h>

#define DRIVER_NAME "spi_imx_shaper"

#define	CS_ACTIVE	1	/* normally nCS, active low */
#define	CS_INACTIVE	0
#define CS_DELAY	100

#define MXC_CSPIRXDATA		0x00
#define MXC_CSPITXDATA		0x04
#define MXC_CSPICTRL		0x08
#define MXC_CSPIINT		0x0c
#define MXC_RESET		0x1c

/* generic defines to abstract from the different register layouts */
#define MXC_INT_RR	(1 << 0) /* Receive data ready interrupt */
#define MXC_INT_TE	(1 << 1) /* Transmit FIFO empty interrupt */
#define MXC_INT_RDR	BIT(4) /* Receive date threshold interrupt */

/* The maximum  bytes that a sdma BD can transfer.*/
#define MAX_SDMA_BD_BYTES  (1 << 15)
#define MXQL_ECSPI_CTRL_MAX_BURST	512

#define MX6Q_ECSPI_CTRL		0x08
#define MX6Q_ECSPI_CTRL_ENABLE		(1 <<  0)
#define MX6Q_ECSPI_CTRL_XCH		(1 <<  2)
#define MX6Q_ECSPI_CTRL_SMC		(1 << 3)
#define MX6Q_ECSPI_CTRL_MODE_MASK	(0xf << 4)
#define MX6Q_ECSPI_CTRL_DRCTL(drctl)	((drctl) << 16)
#define MX6Q_ECSPI_CTRL_POSTDIV_OFFSET	8
#define MX6Q_ECSPI_CTRL_PREDIV_OFFSET	12
#define MX6Q_ECSPI_CTRL_CS(cs)		((cs) << 18)
#define MX6Q_ECSPI_CTRL_BL_OFFSET	20
#define MX6Q_ECSPI_CTRL_BL_MASK		(0xfff << 20)

#define MX6Q_ECSPI_CONFIG	0x0c
#define MX6Q_ECSPI_CONFIG_SCLKPHA(cs)	(1 << ((cs) +  0))
#define MX6Q_ECSPI_CONFIG_SCLKPOL(cs)	(1 << ((cs) +  4))
#define MX6Q_ECSPI_CONFIG_SBBCTRL(cs)	(1 << ((cs) +  8))
#define MX6Q_ECSPI_CONFIG_SSBPOL(cs)	(1 << ((cs) + 12))
#define MX6Q_ECSPI_CONFIG_DATACTL(cs)	(1 << ((cs) + 16))
#define MX6Q_ECSPI_CONFIG_SCLKCTL(cs)	(1 << ((cs) + 20))

#define MX6Q_ECSPI_INT		0x10
#define MX6Q_ECSPI_INT_TEEN		(1 <<  0)
#define MX6Q_ECSPI_INT_RREN		(1 <<  3)
#define MX6Q_ECSPI_INT_RDREN		(1 <<  4)

#define MX6Q_ECSPI_DMA      0x14
#define MX6Q_ECSPI_DMA_TX_WML(wml)	((wml) & 0x3f)
#define MX6Q_ECSPI_DMA_RX_WML(wml)	(((wml) & 0x3f) << 16)
#define MX6Q_ECSPI_DMA_RXT_WML(wml)	(((wml) & 0x3f) << 24)

#define MX6Q_ECSPI_DMA_TEDEN		(1 << 7)
#define MX6Q_ECSPI_DMA_RXDEN		(1 << 23)
#define MX6Q_ECSPI_DMA_RXTDEN		(1 << 31)

#define MX6Q_ECSPI_STAT		0x18
#define MX6Q_ECSPI_STAT_RR		(1 <<  3)

#define MX6Q_ECSPI_TESTREG	0x20
#define MX6Q_ECSPI_TESTREG_LBC	BIT(31)

#define MX6Q_ECSPI_PERIOD 0x1c
#define MX6Q_ECSPI_PERIOD_CSDCTL(waitstates) ((waitstates & 0x3f) << 16)
#define MX6Q_ECSPI_PERIOD_CSRC_SCLK (0 << 15)
#define MX6Q_ECSPI_PERIOD_CSRC_LFRC (1 << 15)
#define MX6Q_ECSPI_PERIOD_SAMPLEPERIOD(waitstates) (waitstates & 0x7fff)

#define MX6Q_ECSPI_PERIOD_CSRC_S

enum spi_imx_devtype {
	IMX6Q_ECSPI,	/* ECSPI on i.mx6q */
};

struct spi_imx_data;

struct spi_imx_devtype_data {
	void (*intctrl)(struct spi_imx_data *, int);
	int (*config)(struct spi_device *);
	void (*trigger)(struct spi_imx_data *);
	int (*rx_available)(struct spi_imx_data *);
	void (*reset)(struct spi_imx_data *);
	void (*setup_wml)(struct spi_imx_data *);
	void (*disable)(struct spi_imx_data *);
	bool has_dmamode;
	unsigned int fifo_size;
	bool dynamic_burst;
	enum spi_imx_devtype devtype;
};

struct spi_imx_data {
	struct spi_master *master;
	struct device *dev;

	struct completion xfer_done;
	void __iomem *base;
	unsigned long base_phys;

	struct clk *clk_per;
	struct clk *clk_ipg;
	unsigned long spi_clk;
	unsigned int spi_bus_clk;

	unsigned int speed_hz;
	unsigned int bits_per_word;
	unsigned int spi_drctl;

	unsigned int count, remainder;
	void (*tx)(struct spi_imx_data *);
	void (*rx)(struct spi_imx_data *);
	void *rx_buf;
	const void *tx_buf;
	unsigned int txfifo; /* number of words pushed in tx FIFO */
	unsigned int dynamic_burst;

	/* DMA */
	bool usedma;
	u32 wml;
	struct completion dma_rx_completion;
	struct completion dma_tx_completion;

	/* Slave hooks */
	int (*slave_ready)(struct spi_device *spi);

	const struct spi_imx_devtype_data *devtype_data;
};

#define MXC_SPI_BUF_RX(type)						\
static void spi_imx_buf_rx_##type(struct spi_imx_data *spi_imx)		\
{									\
	unsigned int val = readl(spi_imx->base + MXC_CSPIRXDATA);	\
									\
	if (spi_imx->rx_buf) {						\
		*(type *)spi_imx->rx_buf = val;				\
		spi_imx->rx_buf += sizeof(type);			\
	}								\
									\
	spi_imx->remainder -= sizeof(type);				\
}

#define MXC_SPI_BUF_TX(type)						\
static void spi_imx_buf_tx_##type(struct spi_imx_data *spi_imx)		\
{									\
	type val = 0;							\
									\
	if (spi_imx->tx_buf) {						\
		val = *(type *)spi_imx->tx_buf;				\
		spi_imx->tx_buf += sizeof(type);			\
	}								\
									\
	spi_imx->count -= sizeof(type);					\
									\
	writel(val, spi_imx->base + MXC_CSPITXDATA);			\
}

MXC_SPI_BUF_RX(u8)
MXC_SPI_BUF_TX(u8)
MXC_SPI_BUF_RX(u16)
MXC_SPI_BUF_TX(u16)
MXC_SPI_BUF_RX(u32)
MXC_SPI_BUF_TX(u32)

static int spi_imx_bytes_per_word(const int bits_per_word)
{
	if (bits_per_word <= 8)
		return 1;
	else if (bits_per_word <= 16)
		return 2;
	else
		return 4;
}

void spi_imx_hook_slave_ready(struct spi_master *master, int (*slave_ready)(struct spi_device *spi))
{
	struct spi_imx_data *spi_imx = spi_master_get_devdata(master);
	spi_imx->slave_ready = slave_ready;
}
EXPORT_SYMBOL_GPL(spi_imx_hook_slave_ready);

static bool spi_imx_can_dma(struct spi_master *master, struct spi_device *spi, struct spi_transfer *transfer)
{
	struct spi_imx_data *spi_imx = spi_master_get_devdata(master);

	if (!master->dma_rx)
		return false;

	if (transfer->no_dma)
		return false;

	if (transfer->len < spi_imx->devtype_data->fifo_size)
		return false;

	spi_imx->dynamic_burst = 0;

	return true;
}

static void spi_imx_buf_rx_swap_u32(struct spi_imx_data *spi_imx)
{
	unsigned int val = readl(spi_imx->base + MXC_CSPIRXDATA);
#ifdef __LITTLE_ENDIAN
	unsigned int bytes_per_word;
#endif

	if (spi_imx->rx_buf) {
#ifdef __LITTLE_ENDIAN
		bytes_per_word = spi_imx_bytes_per_word(spi_imx->bits_per_word);
		if (bytes_per_word == 1)
			val = cpu_to_be32(val);
		else if (bytes_per_word == 2)
			val = (val << 16) | (val >> 16);
#endif
		*(u32 *)spi_imx->rx_buf = val;
		spi_imx->rx_buf += sizeof(u32);
	}

	spi_imx->remainder -= sizeof(u32);
}

static void spi_imx_buf_rx_swap(struct spi_imx_data *spi_imx)
{
	int unaligned;
	u32 val;

	unaligned = spi_imx->remainder % 4;

	if (!unaligned) {
		spi_imx_buf_rx_swap_u32(spi_imx);
		return;
	}

	if (spi_imx_bytes_per_word(spi_imx->bits_per_word) == 2) {
		spi_imx_buf_rx_u16(spi_imx);
		return;
	}

	val = readl(spi_imx->base + MXC_CSPIRXDATA);

	while (unaligned--) {
		if (spi_imx->rx_buf) {
			*(u8 *)spi_imx->rx_buf = (val >> (8 * unaligned)) & 0xff;
			spi_imx->rx_buf++;
		}
		spi_imx->remainder--;
	}
}

static void spi_imx_buf_tx_swap_u32(struct spi_imx_data *spi_imx)
{
	u32 val = 0;
#ifdef __LITTLE_ENDIAN
	unsigned int bytes_per_word;
#endif

	if (spi_imx->tx_buf) {
		val = *(u32 *)spi_imx->tx_buf;
		spi_imx->tx_buf += sizeof(u32);
	}

	spi_imx->count -= sizeof(u32);
#ifdef __LITTLE_ENDIAN
	bytes_per_word = spi_imx_bytes_per_word(spi_imx->bits_per_word);

	if (bytes_per_word == 1)
		val = cpu_to_be32(val);
	else if (bytes_per_word == 2)
		val = (val << 16) | (val >> 16);
#endif
	writel(val, spi_imx->base + MXC_CSPITXDATA);
}

static void spi_imx_buf_tx_swap(struct spi_imx_data *spi_imx)
{
	int unaligned;
	u32 val = 0;

	unaligned = spi_imx->count % 4;

	if (!unaligned) {
		spi_imx_buf_tx_swap_u32(spi_imx);
		return;
	}

	if (spi_imx_bytes_per_word(spi_imx->bits_per_word) == 2) {
		spi_imx_buf_tx_u16(spi_imx);
		return;
	}

	while (unaligned--) {
		if (spi_imx->tx_buf) {
			val |= *(u8 *)spi_imx->tx_buf << (8 * unaligned);
			spi_imx->tx_buf++;
		}
		spi_imx->count--;
	}

	writel(val, spi_imx->base + MXC_CSPITXDATA);
}

/* MX6Q eCSPI */
static unsigned int mx6q_ecspi_clkdiv(struct spi_imx_data *spi_imx, unsigned int fspi, unsigned int *fres)
{
	/*
	 * there are two 4-bit dividers, the pre-divider divides by
	 * $pre, the post-divider by 2^$post
	 */
	unsigned int pre, post;
	unsigned int fin = spi_imx->spi_clk;

	if (unlikely(fspi > fin))
		return 0;

	post = fls(fin) - fls(fspi);
	if (fin > fspi << post)
		post++;

	/* now we have: (fin <= fspi << post) with post being minimal */
	post = max(4U, post) - 4;
	if (unlikely(post > 0xf)) {
		dev_err(spi_imx->dev, "cannot set clock freq: %u (base freq: %u)\n", fspi, fin);
		return 0xff;
	}

	pre = DIV_ROUND_UP(fin, fspi << post) - 1;

	dev_dbg(spi_imx->dev, "%s: fin: %u, fspi: %u, post: %u, pre: %u\n", __func__, fin, fspi, post, pre);

	/* Resulting frequency for the SCLK line. */
	*fres = (fin / (pre + 1)) >> post;

	return (pre << MX6Q_ECSPI_CTRL_PREDIV_OFFSET) |
		(post << MX6Q_ECSPI_CTRL_POSTDIV_OFFSET);
}

static void mx6q_ecspi_intctrl(struct spi_imx_data *spi_imx, int enable)
{
	unsigned val = 0;

	if (enable & MXC_INT_TE)
		val |= MX6Q_ECSPI_INT_TEEN;

	if (enable & MXC_INT_RR)
		val |= MX6Q_ECSPI_INT_RREN;

	if (enable & MXC_INT_RDR)
		val |= MX6Q_ECSPI_INT_RDREN;

	writel(val, spi_imx->base + MX6Q_ECSPI_INT);
}

static void mx6q_ecspi_trigger(struct spi_imx_data *spi_imx)
{
	u32 reg;

	reg = readl(spi_imx->base + MX6Q_ECSPI_CTRL);
	reg |= MX6Q_ECSPI_CTRL_XCH;
	writel(reg, spi_imx->base + MX6Q_ECSPI_CTRL);
}

static void mx6q_ecspi_disable(struct spi_imx_data *spi_imx)
{
	u32 ctrl;

	ctrl = readl(spi_imx->base + MX6Q_ECSPI_CTRL);
	ctrl &= ~MX6Q_ECSPI_CTRL_ENABLE;
	writel(ctrl, spi_imx->base + MX6Q_ECSPI_CTRL);
}

static int mx6q_ecspi_config(struct spi_device *spi)
{
	struct spi_imx_data *spi_imx = spi_master_get_devdata(spi->master);
	u32 ctrl = MX6Q_ECSPI_CTRL_ENABLE;
	u32 delay;
	u32 reg;
	u32 clk = spi_imx->speed_hz;
	u32 cfg = readl(spi_imx->base + MX6Q_ECSPI_CONFIG);
	u32 period = 0;

	/* set Master mode */
	ctrl |= MX6Q_ECSPI_CTRL_MODE_MASK;

	/*
	 * Enable SPI_RDY handling (falling edge/level triggered).
	 */
	if (spi->mode & SPI_READY)
		ctrl |= MX6Q_ECSPI_CTRL_DRCTL(spi_imx->spi_drctl);

	if (spi_imx->dynamic_burst)
		period = MX6Q_ECSPI_PERIOD_CSRC_SCLK | MX6Q_ECSPI_PERIOD_SAMPLEPERIOD(0);

	/* set clock speed */
	ctrl |= mx6q_ecspi_clkdiv(spi_imx, spi_imx->speed_hz, &clk);
	spi_imx->spi_bus_clk = clk;

	/* set chip select to use */
	ctrl |= MX6Q_ECSPI_CTRL_CS(spi->chip_select);

	ctrl |= (spi_imx->bits_per_word - 1) << MX6Q_ECSPI_CTRL_BL_OFFSET;

	/* SPI burst completed when BURST_LENGTH + 1 bits are received */
	cfg |= MX6Q_ECSPI_CONFIG_SBBCTRL(spi->chip_select);

	if (spi->mode & SPI_CPHA)
		cfg |= MX6Q_ECSPI_CONFIG_SCLKPHA(spi->chip_select);
	else
		cfg &= ~MX6Q_ECSPI_CONFIG_SCLKPHA(spi->chip_select);

	if (spi->mode & SPI_CPOL) {
		cfg |= MX6Q_ECSPI_CONFIG_SCLKPOL(spi->chip_select);
		cfg |= MX6Q_ECSPI_CONFIG_SCLKCTL(spi->chip_select);
	} else {
		cfg &= ~MX6Q_ECSPI_CONFIG_SCLKPOL(spi->chip_select);
		cfg &= ~MX6Q_ECSPI_CONFIG_SCLKCTL(spi->chip_select);
	}
	if (spi->mode & SPI_CS_HIGH)
		cfg |= MX6Q_ECSPI_CONFIG_SSBPOL(spi->chip_select);
	else
		cfg &= ~MX6Q_ECSPI_CONFIG_SSBPOL(spi->chip_select);

	/* Idle data lines low */
	cfg |= MX6Q_ECSPI_CONFIG_DATACTL(spi->chip_select);

	if (spi_imx->usedma)
		ctrl |= MX6Q_ECSPI_CTRL_SMC;

	/* CTRL register always go first to bring out controller from reset */
	writel(ctrl, spi_imx->base + MX6Q_ECSPI_CTRL);

	reg = readl(spi_imx->base + MX6Q_ECSPI_TESTREG);
	if (spi->mode & SPI_LOOP)
		reg |= MX6Q_ECSPI_TESTREG_LBC;
	else
		reg &= ~MX6Q_ECSPI_TESTREG_LBC;
	writel(reg, spi_imx->base + MX6Q_ECSPI_TESTREG);
	writel(period, spi_imx->base + MX6Q_ECSPI_PERIOD);
	writel(cfg, spi_imx->base + MX6Q_ECSPI_CONFIG);

	/*
	 * Wait until the changes in the configuration register CONFIGREG
	 * propagate into the hardware. It takes exactly one tick of the
	 * SCLK clock, but we will wait two SCLK clock just to be sure. The
	 * effect of the delay it takes for the hardware to apply changes
	 * is noticable if the SCLK clock run very slow. In such a case, if
	 * the polarity of SCLK should be inverted, the GPIO ChipSelect might
	 * be asserted before the SCLK polarity changes, which would disrupt
	 * the SPI communication as the device on the other end would consider
	 * the change of SCLK polarity as a clock tick already.
	 */
	delay = (2 * 1000000) / clk;
	if (likely(delay < 10))	/* SCLK is faster than 100 kHz */
		udelay(delay);
	else			/* SCLK is _very_ slow */
		usleep_range(delay, delay + 10);

	return 0;
}

static void mx6q_setup_wml(struct spi_imx_data *spi_imx)
{
	/*
	 * Configure the DMA register: setup the watermark
	 * and enable DMA request.
	 */
	writel(MX6Q_ECSPI_DMA_RX_WML(spi_imx->wml - 1) |
		MX6Q_ECSPI_DMA_TX_WML(spi_imx->wml) |
		MX6Q_ECSPI_DMA_RXT_WML(spi_imx->wml) |
		MX6Q_ECSPI_DMA_TEDEN | MX6Q_ECSPI_DMA_RXDEN |
		MX6Q_ECSPI_DMA_RXTDEN, spi_imx->base + MX6Q_ECSPI_DMA);
}

static int mx6q_ecspi_rx_available(struct spi_imx_data *spi_imx)
{
	return readl(spi_imx->base + MX6Q_ECSPI_STAT) & MX6Q_ECSPI_STAT_RR;
}

static void mx6q_ecspi_reset(struct spi_imx_data *spi_imx)
{
	/* drain receive buffer */
	while (mx6q_ecspi_rx_available(spi_imx))
		readl(spi_imx->base + MXC_CSPIRXDATA);
}

static struct spi_imx_devtype_data imx6q_ecspi_devtype_data = {
	.intctrl = mx6q_ecspi_intctrl,
	.config = mx6q_ecspi_config,
	.trigger = mx6q_ecspi_trigger,
	.rx_available = mx6q_ecspi_rx_available,
	.reset = mx6q_ecspi_reset,
	.setup_wml = mx6q_setup_wml,
	.fifo_size = 64,
	.has_dmamode = true,
	.dynamic_burst = true,
	.disable = mx6q_ecspi_disable,
	.devtype = IMX6Q_ECSPI,
};

static const struct platform_device_id spi_imx_devtype[] = {
	{
		.name = "imx6q-ecspi",
		.driver_data = (kernel_ulong_t)&imx6q_ecspi_devtype_data,
	},
	{
		/* sentinel */
	}
};

static const struct of_device_id spi_imx_dt_ids[] = {
	{ .compatible = "fsl,imx6q-ecspi", .data = &imx6q_ecspi_devtype_data, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, spi_imx_dt_ids);

static void spi_imx_chipselect(struct spi_device *spi, bool is_active)
{
	int active = is_active != CS_INACTIVE;
	int dev_is_lowactive = !(spi->mode & SPI_CS_HIGH);

	if (spi->mode & SPI_NO_CS)
		return;

	if (!gpio_is_valid(spi->cs_gpio))
		return;

/*	ndelay(CS_DELAY); */
	gpio_set_value(spi->cs_gpio, dev_is_lowactive ^ active);
/*	ndelay(CS_DELAY); */
}

static void spi_imx_set_burst_len(struct spi_imx_data *spi_imx, int n_bits)
{
	u32 ctrl;

	ctrl = readl(spi_imx->base + MX6Q_ECSPI_CTRL);
	ctrl &= ~MX6Q_ECSPI_CTRL_BL_MASK;
	ctrl |= ((n_bits - 1) << MX6Q_ECSPI_CTRL_BL_OFFSET);
	writel(ctrl, spi_imx->base + MX6Q_ECSPI_CTRL);
}

static void spi_imx_push(struct spi_imx_data *spi_imx)
{
	unsigned int burst_len, fifo_words;

	if (spi_imx->dynamic_burst)
		fifo_words = 4;
	else
		fifo_words = spi_imx_bytes_per_word(spi_imx->bits_per_word);
	/*
	 * Reload the FIFO when the remaining bytes to be transferred in the
	 * current burst is 0. This only applies when bits_per_word is a
	 * multiple of 8.
	 */
	if (!spi_imx->remainder) {
		if (spi_imx->dynamic_burst) {

			/* We need to deal unaligned data first */
			burst_len = spi_imx->count % MXQL_ECSPI_CTRL_MAX_BURST;

			if (!burst_len)
				burst_len = MXQL_ECSPI_CTRL_MAX_BURST;

			spi_imx_set_burst_len(spi_imx, burst_len * 8);

			spi_imx->remainder = burst_len;
		} else {
			spi_imx->remainder = fifo_words;
		}
	}

	while (spi_imx->txfifo < spi_imx->devtype_data->fifo_size) {
		if (!spi_imx->count)
			break;
		if (spi_imx->dynamic_burst && spi_imx->txfifo >=  DIV_ROUND_UP(spi_imx->remainder, fifo_words))
			break;
		spi_imx->tx(spi_imx);
		spi_imx->txfifo++;
	}

	spi_imx->devtype_data->trigger(spi_imx);
}

static irqreturn_t spi_imx_isr(int irq, void *dev_id)
{
	struct spi_imx_data *spi_imx = dev_id;

	while (spi_imx->txfifo && spi_imx->devtype_data->rx_available(spi_imx)) {
		spi_imx->rx(spi_imx);
		spi_imx->txfifo--;
	}

	if (spi_imx->count) {
		spi_imx_push(spi_imx);
		return IRQ_HANDLED;
	}

	if (spi_imx->txfifo) {
		/* No data left to push, but still waiting for rx data,
		 * enable receive data available interrupt.
		 */
		spi_imx->devtype_data->intctrl(spi_imx, MXC_INT_RR);
		return IRQ_HANDLED;
	}

	spi_imx->devtype_data->intctrl(spi_imx, 0);
	complete(&spi_imx->xfer_done);

	return IRQ_HANDLED;
}

static int spi_imx_dma_configure(struct spi_master *master)
{
	int ret;
	enum dma_slave_buswidth buswidth;
	struct dma_slave_config rx = {}, tx = {};
	struct spi_imx_data *spi_imx = spi_master_get_devdata(master);

	switch (spi_imx_bytes_per_word(spi_imx->bits_per_word)) {
	case 4:
		buswidth = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;
	case 2:
		buswidth = DMA_SLAVE_BUSWIDTH_2_BYTES;
		break;
	case 1:
		buswidth = DMA_SLAVE_BUSWIDTH_1_BYTE;
		break;
	default:
		return -EINVAL;
	}

	tx.direction = DMA_MEM_TO_DEV;
	tx.dst_addr = spi_imx->base_phys + MXC_CSPITXDATA;
	tx.dst_addr_width = buswidth;
	tx.dst_maxburst = spi_imx->wml;
	ret = dmaengine_slave_config(master->dma_tx, &tx);
	if (ret) {
		dev_err(spi_imx->dev, "TX dma configuration failed with %d\n", ret);
		return ret;
	}

	rx.direction = DMA_DEV_TO_MEM;
	rx.src_addr = spi_imx->base_phys + MXC_CSPIRXDATA;
	rx.src_addr_width = buswidth;
	rx.src_maxburst = spi_imx->wml;
	ret = dmaengine_slave_config(master->dma_rx, &rx);
	if (ret) {
		dev_err(spi_imx->dev, "RX dma configuration failed with %d\n", ret);
		return ret;
	}

	return 0;
}

static void spi_imx_sdma_exit(struct spi_imx_data *spi_imx)
{
	struct spi_master *master = spi_imx->master;

	if (master->dma_rx) {
		dma_release_channel(master->dma_rx);
		master->dma_rx = NULL;
	}

	if (master->dma_tx) {
		dma_release_channel(master->dma_tx);
		master->dma_tx = NULL;
	}
}

static int spi_imx_sdma_init(struct device *dev, struct spi_imx_data *spi_imx, struct spi_master *master)
{
	int ret;

	/* use pio mode for i.mx6dl chip TKT238285 */
	if (of_machine_is_compatible("fsl,imx6dl"))
		return 0;

	spi_imx->wml = spi_imx->devtype_data->fifo_size / 2;

	/* Prepare for TX DMA: */
	master->dma_tx = dma_request_slave_channel_reason(dev, "tx");
	if (IS_ERR(master->dma_tx)) {
		ret = PTR_ERR(master->dma_tx);
		dev_dbg(dev, "can't get the TX DMA channel, error %d!\n", ret);
		master->dma_tx = NULL;
		goto err;
	}

	/* Prepare for RX : */
	master->dma_rx = dma_request_slave_channel_reason(dev, "rx");
	if (IS_ERR(master->dma_rx)) {
		ret = PTR_ERR(master->dma_rx);
		dev_dbg(dev, "can't get the RX DMA channel, error %d\n", ret);
		master->dma_rx = NULL;
		goto err;
	}

	init_completion(&spi_imx->dma_rx_completion);
	init_completion(&spi_imx->dma_tx_completion);
	master->can_dma = spi_imx_can_dma;
	master->max_dma_len = MAX_SDMA_BD_BYTES;
	spi_imx->master->flags = SPI_MASTER_MUST_RX | SPI_MASTER_MUST_TX;

	return 0;
err:
	spi_imx_sdma_exit(spi_imx);
	return ret;
}

static void spi_imx_dma_rx_callback(void *cookie)
{
	struct spi_imx_data *spi_imx = (struct spi_imx_data *)cookie;

	complete(&spi_imx->dma_rx_completion);
}

static void spi_imx_dma_tx_callback(void *cookie)
{
	struct spi_imx_data *spi_imx = (struct spi_imx_data *)cookie;

	complete(&spi_imx->dma_tx_completion);
}

static int spi_imx_calculate_timeout(struct spi_imx_data *spi_imx, int size)
{
	unsigned long timeout = 0;

	/* Time with actual data transfer and CS change delay related to HW */
	timeout = (8 + 4) * size / spi_imx->spi_bus_clk;

	/* Add extra second for scheduler related activities */
	timeout += 1;

	/* Double calculated timeout */
	return msecs_to_jiffies(2 * timeout * MSEC_PER_SEC);
}

static int spi_imx_dma_transfer(struct spi_imx_data *spi_imx, struct spi_transfer *transfer)
{
	struct dma_async_tx_descriptor *desc_tx;
	struct dma_async_tx_descriptor *desc_rx;
	unsigned long transfer_timeout;
	unsigned long timeout;
	struct spi_master *master = spi_imx->master;
	struct sg_table *tx = &transfer->tx_sg;
	struct sg_table *rx = &transfer->rx_sg;
	struct scatterlist *last_sg = sg_last(rx->sgl, rx->nents);
	unsigned int bytes_per_word, i;
	int ret;

	/* Get the right burst length from the last sg to ensure no tail data */
	bytes_per_word = spi_imx_bytes_per_word(transfer->bits_per_word);
	for (i = spi_imx->devtype_data->fifo_size / 2; i > 0; i--) {
		if (!(sg_dma_len(last_sg) % (i * bytes_per_word)))
			break;
	}
	/* Use 1 as wml in case no available burst length got */
	if (i == 0)
		i = 1;

	spi_imx->wml =  i;

	ret = spi_imx_dma_configure(master);
	if (ret)
		return ret;

	if (!spi_imx->devtype_data->setup_wml) {
		dev_err(spi_imx->dev, "No setup_wml()?\n");
		return -EINVAL;
	}
	spi_imx->devtype_data->setup_wml(spi_imx);

	/* Take a sledge hammer to the dma channel to ensure rx completion notification */
	dmaengine_terminate_all(master->dma_tx);
	dmaengine_terminate_all(master->dma_rx);

	/*
	 * The TX DMA setup starts the transfer, so make sure RX is configured
	 * before TX.
	 */
	desc_rx = dmaengine_prep_slave_sg(master->dma_rx, rx->sgl, rx->nents, DMA_DEV_TO_MEM, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc_rx)
		return -EINVAL;

	desc_rx->callback = spi_imx_dma_rx_callback;
	desc_rx->callback_param = (void *)spi_imx;
	dmaengine_submit(desc_rx);
	reinit_completion(&spi_imx->dma_rx_completion);
	dma_async_issue_pending(master->dma_rx);

	desc_tx = dmaengine_prep_slave_sg(master->dma_tx, tx->sgl, tx->nents, DMA_MEM_TO_DEV, DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc_tx) {
		dmaengine_terminate_all(master->dma_tx);
		return -EINVAL;
	}

	desc_tx->callback = spi_imx_dma_tx_callback;
	desc_tx->callback_param = (void *)spi_imx;
	dmaengine_submit(desc_tx);
	reinit_completion(&spi_imx->dma_tx_completion);
	dma_async_issue_pending(master->dma_tx);

	transfer_timeout = spi_imx_calculate_timeout(spi_imx, transfer->len);

	/* Wait SDMA to finish the data transfer.*/
	timeout = wait_for_completion_timeout(&spi_imx->dma_tx_completion, transfer_timeout);
	if (!timeout) {
		dev_err(spi_imx->dev, "I/O Error in DMA TX\n");
		dmaengine_terminate_all(master->dma_tx);
		dmaengine_terminate_all(master->dma_rx);
		return -ETIMEDOUT;
	}

	timeout = wait_for_completion_timeout(&spi_imx->dma_rx_completion, transfer_timeout);
	if (!timeout) {
		dev_err(&master->dev, "I/O Error in DMA RX\n");
		spi_imx->devtype_data->reset(spi_imx);
		dmaengine_terminate_all(master->dma_rx);
		return -ETIMEDOUT;
	}

	return transfer->len;
}

static int spi_imx_pio_transfer(struct spi_device *spi,
				struct spi_transfer *transfer)
{
	struct spi_imx_data *spi_imx = spi_master_get_devdata(spi->master);
	unsigned long transfer_timeout;
	unsigned long timeout;

	spi_imx->tx_buf = transfer->tx_buf;
	spi_imx->rx_buf = transfer->rx_buf;
	spi_imx->count = transfer->len;
	spi_imx->txfifo = 0;
	spi_imx->remainder = 0;

	reinit_completion(&spi_imx->xfer_done);

	spi_imx_push(spi_imx);

	spi_imx->devtype_data->intctrl(spi_imx, MXC_INT_TE);

	transfer_timeout = spi_imx_calculate_timeout(spi_imx, transfer->len);

	timeout = wait_for_completion_timeout(&spi_imx->xfer_done, transfer_timeout);
	if (!timeout) {
		dev_err(&spi->dev, "I/O Error in PIO\n");
		spi_imx->devtype_data->reset(spi_imx);
		return -ETIMEDOUT;
	}

	return transfer->len;
}

static int spi_imx_setup(struct spi_device *spi)
{
	dev_dbg(&spi->dev, "%s: mode %d, %u bpw, %d hz\n", __func__, spi->mode, spi->bits_per_word, spi->max_speed_hz);

	if (spi->mode & SPI_NO_CS)
		return 0;

	if (gpio_is_valid(spi->cs_gpio))
		gpio_direction_output(spi->cs_gpio,
				      spi->mode & SPI_CS_HIGH ? 0 : 1);

	spi_imx_chipselect(spi, CS_INACTIVE);

	return 0;
}

static void spi_imx_cleanup(struct spi_device *spi)
{
}

static int spi_imx_prepare_message(struct spi_master *master, struct spi_message *msg)
{
/*
	struct spi_imx_data *spi_imx = spi_master_get_devdata(master);
	int ret;

	ret = clk_enable(spi_imx->clk_per);
	if (ret)
		return ret;

	ret = clk_enable(spi_imx->clk_ipg);
	if (ret) {
		clk_disable(spi_imx->clk_per);
		return ret;
	}
*/
	return 0;
}

static int spi_imx_unprepare_message(struct spi_master *master, struct spi_message *msg)
{
/*
	struct spi_imx_data *spi_imx = spi_master_get_devdata(master);

	clk_disable(spi_imx->clk_ipg);
	clk_disable(spi_imx->clk_per);
*/
	return 0;
}

static int spi_imx_transfer_one(struct spi_master *master, struct spi_device *spi, struct spi_transfer *transfer)
{
	struct spi_imx_data *spi_imx = spi_master_get_devdata(master);
	int status = 0;
	int ready_status;

	if (!transfer)
		return 0;

	spi_imx->bits_per_word = transfer->bits_per_word;
	spi_imx->speed_hz  = transfer->speed_hz;

	/*
	 * Initialize the functions for transfer. To transfer non byte-aligned
	 * words, we have to use multiple word-size bursts, we can't use
	 * dynamic_burst in that case.
	 */
	if (spi_imx->devtype_data->dynamic_burst &&
	    (spi_imx->bits_per_word == 8 ||
	    spi_imx->bits_per_word == 16 ||
	    spi_imx->bits_per_word == 32)) {

		spi_imx->rx = spi_imx_buf_rx_swap;
		spi_imx->tx = spi_imx_buf_tx_swap;
		spi_imx->dynamic_burst = 1;

	} else {
		if (spi_imx->bits_per_word <= 8) {
			spi_imx->rx = spi_imx_buf_rx_u8;
			spi_imx->tx = spi_imx_buf_tx_u8;
		} else if (spi_imx->bits_per_word <= 16) {
			spi_imx->rx = spi_imx_buf_rx_u16;
			spi_imx->tx = spi_imx_buf_tx_u16;
		} else {
			spi_imx->rx = spi_imx_buf_rx_u32;
			spi_imx->tx = spi_imx_buf_tx_u32;
		}
		spi_imx->dynamic_burst = 0;
	}

	/* Check for DMA */
	spi_imx->usedma = spi_imx_can_dma(spi_imx->master, spi, transfer);

	/* Configure the hardware */
	spi_imx->devtype_data->config(spi);

	/* Only send if length is value */
	if (transfer->len) {

		/* flush rxfifo before transfer */
		while (spi_imx->devtype_data->rx_available(spi_imx))
			spi_imx->rx(spi_imx);

		if (spi_imx->usedma)
			status = spi_imx_dma_transfer(spi_imx, transfer);
		else
			status = spi_imx_pio_transfer(spi, transfer);

		/* Allow slave to control flow */
		if (spi_imx->slave_ready) {
			ready_status = spi_imx->slave_ready(spi);
			if (ready_status < 0) {
				status = ready_status;
				goto error_finalize;
			}
		}
	}

	/* Check the results of the transfer */
	if (status == transfer->len)
		status = 0;
	else if (status >= 0)
		status = -EREMOTEIO;

error_finalize:

	/* Notify the infrastructure */
	spi_finalize_current_transfer(master);

	return status;
}

static int spi_imx_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	const struct of_device_id *of_id = of_match_device(spi_imx_dt_ids, &pdev->dev);
	struct spi_imx_master *mxc_platform_info = dev_get_platdata(&pdev->dev);
	struct spi_master *master;
	struct spi_imx_data *spi_imx;
	struct resource *res;
	int i;
	int ret;
	int irq;
	int spi_drctl;
	const struct spi_imx_devtype_data *devtype_data = of_id ? of_id->data : (struct spi_imx_devtype_data *)pdev->id_entry->driver_data;

	if (!np && !mxc_platform_info) {
		dev_err(&pdev->dev, "can't get the platform data\n");
		return -EINVAL;
	}

	/* Allocate the master */
	master = spi_alloc_master(&pdev->dev, sizeof(struct spi_imx_data));
	if (!master)
		return -ENOMEM;

	ret = of_property_read_u32(np, "fsl,spi-rdy-drctl", &spi_drctl);
	if ((ret < 0) || (spi_drctl >= 0x3)) {
		/* '11' is reserved */
		spi_drctl = 0;
	}

	platform_set_drvdata(pdev, master);

	master->bits_per_word_mask = SPI_BPW_RANGE_MASK(1, 32);
	master->bus_num = np ? -1 : pdev->id;

	spi_imx = spi_master_get_devdata(master);
	spi_imx->master = master;
	spi_imx->dev = &pdev->dev;

	spi_imx->devtype_data = devtype_data;

	/* Get number of chip selects, either platform data or OF */
	if (mxc_platform_info) {
		master->num_chipselect = mxc_platform_info->num_chipselect;
		if (mxc_platform_info->chipselect) {
			master->cs_gpios = devm_kcalloc(&master->dev, master->num_chipselect, sizeof(int), GFP_KERNEL);
			if (!master->cs_gpios)
				return -ENOMEM;

			for (i = 0; i < master->num_chipselect; i++)
				master->cs_gpios[i] = mxc_platform_info->chipselect[i];
		}
	} else {
		u32 num_cs;
		if (!of_property_read_u32(np, "num-cs", &num_cs))
			master->num_chipselect = num_cs;
		/* If not preset, default value of 1 is used */
	}

	master->setup = spi_imx_setup;
	master->cleanup = spi_imx_cleanup;
	master->prepare_message = spi_imx_prepare_message;
	master->unprepare_message = spi_imx_unprepare_message;
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_NO_CS | SPI_LOOP | SPI_READY;
	master->transfer_one = spi_imx_transfer_one;
	master->set_cs = spi_imx_chipselect;

	spi_imx->spi_drctl = spi_drctl;

	init_completion(&spi_imx->xfer_done);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	spi_imx->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(spi_imx->base)) {
		ret = PTR_ERR(spi_imx->base);
		goto out_master_put;
	}
	spi_imx->base_phys = res->start;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto out_master_put;
	}

	ret = devm_request_irq(&pdev->dev, irq, spi_imx_isr, 0, dev_name(&pdev->dev), spi_imx);
	if (ret) {
		dev_err(&pdev->dev, "can't get irq%d: %d\n", irq, ret);
		goto out_master_put;
	}

	spi_imx->clk_ipg = devm_clk_get(&pdev->dev, "ipg");
	if (IS_ERR(spi_imx->clk_ipg)) {
		ret = PTR_ERR(spi_imx->clk_ipg);
		goto out_master_put;
	}

	spi_imx->clk_per = devm_clk_get(&pdev->dev, "per");
	if (IS_ERR(spi_imx->clk_per)) {
		ret = PTR_ERR(spi_imx->clk_per);
		goto out_master_put;
	}

	ret = clk_prepare_enable(spi_imx->clk_per);
	if (ret)
		goto out_master_put;

	ret = clk_prepare_enable(spi_imx->clk_ipg);
	if (ret)
		goto out_put_per;

	spi_imx->spi_clk = clk_get_rate(spi_imx->clk_per);
	/*
	 * Only validated on i.mx35 and i.mx6 now, can remove the constraint
	 * if validated on other chips.
	 */
	if (spi_imx->devtype_data->has_dmamode) {
		ret = spi_imx_sdma_init(&pdev->dev, spi_imx, master);
		if (ret == -EPROBE_DEFER)
			goto out_clk_put;

		if (ret < 0)
			dev_err(&pdev->dev, "dma setup error %d, use pio\n",
				ret);
	}

	spi_imx->devtype_data->reset(spi_imx);

	spi_imx->devtype_data->intctrl(spi_imx, 0);

	master->dev.of_node = pdev->dev.of_node;

	/* Request GPIO CS lines, if any */
	if (master->cs_gpios) {
		for (i = 0; i < master->num_chipselect; i++) {
			if (!gpio_is_valid(master->cs_gpios[i]))
				continue;

			ret = devm_gpio_request(&pdev->dev, master->cs_gpios[i], DRIVER_NAME);
			if (ret) {
				dev_err(&pdev->dev, "Can't get CS GPIO %i\n", master->cs_gpios[i]);
				goto out_spi_unregister;
			}
		}
	}

	/*
	 * driver may get busy before register() returns, especially
	 * if someone registered boardinfo for devices
	 */
	ret = spi_register_master(spi_master_get(master));
	if (ret)
		spi_master_put(master);

	dev_info(&pdev->dev, "probed\n");

/*
	clk_disable(spi_imx->clk_ipg);
	clk_disable(spi_imx->clk_per);
*/
	return ret;

out_spi_unregister:
	spi_unregister_master(master);
out_clk_put:
	clk_disable_unprepare(spi_imx->clk_ipg);
out_put_per:
	clk_disable_unprepare(spi_imx->clk_per);
out_master_put:
	spi_master_put(master);

	return ret;
}

static int spi_imx_remove(struct platform_device *pdev)
{
	struct spi_master *master = platform_get_drvdata(pdev);
	struct spi_imx_data *spi_imx = spi_master_get_devdata(master);
	int ret;

	spi_unregister_master(master);

	ret = clk_enable(spi_imx->clk_per);
	if (ret)
		return ret;

	ret = clk_enable(spi_imx->clk_ipg);
	if (ret) {
		clk_disable(spi_imx->clk_per);
		return ret;
	}

	writel(0, spi_imx->base + MXC_CSPICTRL);
	clk_disable_unprepare(spi_imx->clk_ipg);
	clk_disable_unprepare(spi_imx->clk_per);
	spi_imx_sdma_exit(spi_imx);
	spi_master_put(master);

	return 0;
}

static struct platform_driver spi_imx_shaper_driver = {
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = spi_imx_dt_ids,
		   },
	.id_table = spi_imx_devtype,
	.probe = spi_imx_probe,
	.remove = spi_imx_remove,
};
module_platform_driver(spi_imx_shaper_driver);

MODULE_DESCRIPTION("Shaper Patched SPI Controller driver");
MODULE_AUTHOR("Stephen Street");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:" DRIVER_NAME);
