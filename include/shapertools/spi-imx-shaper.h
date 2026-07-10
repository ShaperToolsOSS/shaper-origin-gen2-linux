#ifndef _SHAPERTOOLS_SPI_IMX_SHAPER_H_
#define _SHAPERTOOLS_SPI_IMX_SHAPER_H_

void spi_imx_hook_slave_ready(struct spi_master *master, int (*slave_ready)(struct spi_device *spi));

#endif
