#ifndef HW_SPI_H
#define HW_SPI_H

extern const struct snd_sof_dsp_ops snd_sof_spi_ops;

struct sof_spi_dev {
	int gpio;
	unsigned int active;
};

#endif
