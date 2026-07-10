/*
 * SiS Touchconroller driver for I2C interface
 *
 * Copyright (C) 2018 MSC Technologies
 * Author: Dizni Premdas <Dizni.Premdas@msc-technologies.eu>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/device.h>
#include <linux/of_gpio.h>
#include <linux/platform_data/sis_i2c_95xx_ts.h>
#include <linux/crc-itu-t.h>
#include <linux/interrupt.h>
#include <linux/input/mt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/firmware.h>

#define MAJOR_NUM   '1'
#define MINOR_NUM   '0'
#define EXTRA_NUM   '8'


/* Touchscreen absolute values */
#define MAX_TOUCHES             10
#define MAX_AREA                0xff
#define MAX_X                   4096
#define MAX_Y                   4096

#define TOUCH_ACTIVE            0x01
#define TOUCH_INACTIVE          0x00

#define DATA_SIZE               64

#define BYTE_COUNT              0
#define REPORT_ID_BYTE          2

#define STATUS_TOUCH            0
#define CONTACT_ID              1
#define X_LSB                   2
#define X_MSB                   3
#define Y_LSB                   4
#define Y_MSB                   5

#define SIZE_OF_BYTE_COUNT      2
#define SIZE_OF_REPORT_ID       1
#define SIZE_OF_TOUCH_DATA      6
#define SIZE_OF_NUM_TOUCH       1
#define SIZE_OF_SCAN_TIME       2
#define SIZE_OF_CRC             2


#define IS_TOUCH(x)             (x & 0x1)
#define IS_AREA(x)              ((x >> 4) & 0x1)
#define IS_PRESSURE(x)          ((x >> 6) & 0x1)
#define IS_SCANTIME(x)          ((x >> 7) & 0x1)

#define OUTPUT_REG              0x0004

#define REPORTID_VENDOR_CMD     0x09
#define REPORTID_FROM_DEV       0x0a
#define CMD_WRITE_FINISH        0x81
#define CMD_RESET               0x82
#define CMD_WRITE_START         0x83
#define CMD_WRITE_DATA          0x84
#define CMD_CHANGE_MODE         0x85
#define CMD_READ                0x86
#define CMD_RECALIB             0x87

#define ENABLE_DIAGNOSIS_MODE   0x0121
#define DISABLE_DIAGNOSIS_MODE  0x0120
#define PWR_CMD_FWCTRL          0x0950
#define PWR_CMD_ACTIVE          0x0951
#define PWR_CMD_SLEEP           0x0952
#define CHG_OLD_MODE            0x0123

#define GOOD_CODE               0xBEEF
#define BAD_CODE                0xDEAD

#define MAX_WR_SIZE             60
#define MAX_RD_SIZE             60
#define DATA_UNIT               (MAX_RD_SIZE - 8)

#define UNIT_FLASH_SIZE         (8 * 1024)

#define FW_INFO_ADDR            0xA0004000
#define FW_PARMS_RD_ADDR        0xA0004000
#define FW_PARMS_WT_ADDR        0x00004000
#define FW_RD_ADDR              0xA0000000

#define VOLT_DATA_SIZE          (PAGE_SIZE - 10)
#define TARGET_VOLT_ADDR        0xA0004268  /* FW_INFO_ADDR + 0x0268 */
#define VOLT_DATA_ADDR          0x5A000000
#define RAW_DATA_ADDR           0x52000000
#define CYCLE_DATA_ADDR         0x54000000
#define DIFF_DATA_ADDR          0x53000000

#define FWMODE_ADDR             0x58000000

/* Each SiS controller connected has these data */
struct sis_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct sis_platform_data *pdata;
	char phys[64];
	uint8_t fw_info[128];
	uint8_t sensor_data_id;
	uint8_t fw_dump_id;

	unsigned int irq;

	struct mutex diag_mutex;
	char *fw_name;
	const struct firmware *fw;
};


static int __sis_read(struct i2c_client *client, u16 len, void *val)
{
	struct i2c_msg msg[1];

	/* Read data */
	msg[0].addr = client->addr;
	msg[0].flags = I2C_M_RD;
	msg[0].len = len;
	msg[0].buf = val;

	return i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
}

static int __sis_write(struct i2c_client *client, u16 len, void *val)
{
	struct i2c_msg msg[1];

	/* Write data */
	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = len;
	msg[0].buf = val;

	return i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg));
}

static void sis_ctrl_reset_hard(struct i2c_client *client,
	struct sis_platform_data *pdata)
{
	if (!gpio_is_valid(pdata->gpio_rst)) {
		dev_info(&client->dev, "Must have reset GPIO for hard reset\n");
		return;
	}

	/* reset with a pulse of 100ms */
	gpio_set_value(pdata->gpio_rst, 0);
	msleep(100); /* low pulse */
	gpio_set_value(pdata->gpio_rst, 1);
	msleep(2000); /* time to reboot */
}

static uint16_t num_of_tranfers(uint32_t size, uint32_t unit)
{
	return ((uint16_t)((size + unit - 1)/unit));
}

static int insert_crc(unsigned char *buffer, unsigned int size)
{
	uint8_t i;
	unsigned short crc = 0;

	if (size < 8)
		return -EINVAL;

	crc = crc_itu_t_byte(crc, buffer[6]);
	for (i = 0; i < (size - 8); i++)
		crc = crc_itu_t_byte(crc, buffer[8 + i]);

	buffer[7] = crc & 0xff;

	return 0;
}

static int response_valid(uint8_t *rsp)
{
	uint8_t i;
	uint16_t length;
	uint16_t code;
	uint16_t status;
	uint8_t crc_rec;
	uint16_t crc = 0;

	length = rsp[0] | (rsp[1] << 8);

	if (rsp[2] != REPORTID_FROM_DEV)
		return -EIO;

	crc_rec = rsp[3];

	for (i = 4; i < length; i++)
		crc = crc_itu_t_byte(crc, rsp[i]);

	if (crc_rec != (crc & 0x00FF))
		return -EIO;

	/* copy the status and code */
	code = rsp[4] | (rsp[5] << 8);
	status = rsp[6] | (rsp[7] << 8);

	if (code == BAD_CODE)
		return -EIO;

	return 0;
}

static int send_cmd81_write_finish(struct sis_data *data)
{
	uint8_t cmd[8];
	uint8_t rsp[MAX_RD_SIZE] = {0};
	int ret;

	cmd[0] = OUTPUT_REG & 0x00ff;
	cmd[1] = (OUTPUT_REG >> 8) & 0x00ff;
	cmd[2] = 0x06;
	cmd[3] = 0x00;
	cmd[4] = REPORTID_VENDOR_CMD;
	cmd[5] = 0x00;
	cmd[6] = CMD_WRITE_FINISH;

	ret = insert_crc(cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	ret = __sis_write(data->client, sizeof(cmd), cmd);
	if (ret < 0)
		return ret;

	ret = __sis_read(data->client, sizeof(rsp), rsp);
	if (ret < 0)
		return ret;

	return response_valid(rsp);
}

static int send_cmd82_reset_soft(struct sis_data *data)
{
	uint8_t cmd[8];
	int ret;

	cmd[0] = OUTPUT_REG & 0x00ff;
	cmd[1] = (OUTPUT_REG >> 8) & 0x00ff;
	cmd[2] = 0x06;
	cmd[3] = 0x00;
	cmd[4] = REPORTID_VENDOR_CMD;
	cmd[5] = 0x00;
	cmd[6] = CMD_RESET;

	ret = insert_crc(cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	ret = __sis_write(data->client, sizeof(cmd), cmd);
	if (ret < 0)
		return ret;

	/* No ACK for RESET */
	msleep(6000); /* time to reboot */

	return 0;
}

static int send_cmd83_write_start(struct sis_data *data, uint32_t addr,
		uint16_t len)
{
	uint8_t cmd[14];
	uint8_t rsp[MAX_RD_SIZE] = {0};
	int ret;

	cmd[0] = OUTPUT_REG & 0x00ff;
	cmd[1] = (OUTPUT_REG >> 8) & 0x00ff;
	cmd[2] = 0x0C;
	cmd[3] = 0x00;
	cmd[4] = REPORTID_VENDOR_CMD;
	cmd[5] = 0x00;
	cmd[6] = CMD_WRITE_START;

	cmd[8] = (addr & 0x00ff);
	cmd[9] = (addr >> 8) & 0x00ff;
	cmd[10] = (addr >> 16) & 0x00ff;
	cmd[11] = (addr >> 24) & 0x00ff;
	cmd[12] = (len & 0x00ff);
	cmd[13] = (len >> 8) & 0x00ff;

	ret = insert_crc(cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	ret = __sis_write(data->client, sizeof(cmd), cmd);
	if (ret < 0)
		return ret;

	ret = __sis_read(data->client, sizeof(rsp), rsp);
	if (ret < 0)
		return ret;

	return response_valid(rsp);
}

static int send_cmd84_write(struct sis_data *data, const uint8_t *buf,
		uint32_t offset, uint16_t len)
{
	uint8_t cmd[MAX_WR_SIZE] = {0};
	uint8_t rsp[MAX_RD_SIZE] = {0};
	uint16_t i;
	int ret;
	uint16_t num_trans;

	cmd[0] = OUTPUT_REG & 0x00ff;
	cmd[1] = (OUTPUT_REG >> 8) & 0x00ff;
	cmd[2] = len + 6;
	cmd[3] = 0x00;
	cmd[4] = REPORTID_VENDOR_CMD;
	cmd[5] = 0x00;
	cmd[6] = CMD_WRITE_DATA;

	num_trans = num_of_tranfers(len, 4);

	for (i = 0; i < num_trans; i++) {
		cmd[4 * i + 8] = *(buf + offset + (4 * i + 3));
		cmd[4 * i + 9] = *(buf + offset + (4 * i + 2));
		cmd[4 * i + 10] = *(buf + offset + (4 * i + 1));
		cmd[4 * i + 11] = *(buf + offset + (4 * i));
	}

	ret = insert_crc(cmd, len + 8);
	if (ret < 0)
		return ret;

	ret = __sis_write(data->client, sizeof(cmd), cmd);
	if (ret < 0)
		return ret;

	ret = __sis_read(data->client, sizeof(rsp), rsp);
	if (ret < 0)
		return ret;

	return response_valid(rsp);
}

static int send_cmd85_change_mode(struct sis_data *data, uint16_t mode)
{
	uint8_t cmd[10];
	uint8_t rsp[MAX_RD_SIZE] = {0};
	int ret;

	cmd[0] = OUTPUT_REG & 0x00ff;
	cmd[1] = (OUTPUT_REG >> 8) & 0x00ff;
	cmd[2] = 0x08;
	cmd[3] = 0x00;
	cmd[4] = REPORTID_VENDOR_CMD;
	cmd[5] = 0x00;
	cmd[6] = CMD_CHANGE_MODE;

	cmd[8] = (mode & 0x00ff);
	cmd[9] = (mode >>  8) & 0x00ff;

	ret = insert_crc(cmd, sizeof(cmd));
	if (ret < 0)
		return ret;

	ret = __sis_write(data->client, sizeof(cmd), cmd);
	if (ret < 0)
		return ret;

	ret = __sis_read(data->client, sizeof(rsp), rsp);
	if (ret < 0)
		return ret;

	return response_valid(rsp);
}

static int send_cmd86_read(struct sis_data *data, uint32_t addr,
		uint8_t *pbuf, uint16_t len)
{
	uint8_t cmd[14];
	int ret;
	uint8_t rsp[MAX_RD_SIZE] = {0};
	uint16_t num_trans;

	cmd[0] = OUTPUT_REG & 0x00ff;
	cmd[1] = (OUTPUT_REG >> 8) & 0x00ff;
	cmd[1] = 0x00;
	cmd[2] = 0x0c;
	cmd[3] = 0x00;
	cmd[4] = REPORTID_VENDOR_CMD;
	cmd[5] = 0x00;
	cmd[6] = CMD_READ;

	num_trans = num_of_tranfers(len, DATA_UNIT);

	while (num_trans) {
		cmd[8] = (addr & 0x00ff);
		cmd[9] = (addr >> 8) & 0x00ff;
		cmd[10] = (addr >> 16) & 0x00ff;
		cmd[11] = (addr >> 24) & 0x00ff;
		cmd[12] = (DATA_UNIT & 0x00ff);
		cmd[13] = (DATA_UNIT >> 8) & 0x00ff;

		ret = insert_crc(cmd, sizeof(cmd));
		if (ret < 0)
			return ret;

		ret = __sis_write(data->client, sizeof(cmd), cmd);
		if (ret < 0)
			return ret;

		ret = __sis_read(data->client, sizeof(rsp), rsp);
		if (ret < 0)
			return ret;

		ret = response_valid(rsp);
		if (ret < 0)
			return -EINVAL;

		if (num_trans == 1)
			memcpy(pbuf, rsp + 8, len % DATA_UNIT);
		else
			memcpy(pbuf, rsp + 8, DATA_UNIT);

		num_trans--;
		addr += DATA_UNIT;
		pbuf += DATA_UNIT;
	}

	return 0;
}

static int update_fw_info(struct sis_data *data)
{
	int ret = 0;

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		return ret;

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		return ret;

	ret = send_cmd86_read(data, FW_INFO_ADDR, data->fw_info,
			sizeof(data->fw_info));
	if (ret < 0)
		return ret;

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		return ret;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		return ret;

	return 0;
}

static irqreturn_t sis_int_irq_handler(int irq, void *devid)
{
	struct sis_data *data = (struct sis_data *)devid;
	struct input_dev *input_dev = data->input_dev;
	uint8_t tmpbuf[DATA_SIZE] = {0};
	uint8_t i;
	int ret;

	uint8_t count;
	uint8_t report_id = 0;
	uint8_t num_touches;

	uint8_t status;
	uint8_t id;
	uint16_t x;
	uint16_t y;
	uint8_t n_touch = 0;

	do {
		ret = __sis_read(data->client, sizeof(tmpbuf), tmpbuf);
		if (ret < 0) {
			dev_err(&data->client->dev, "Error in sis read %d",
					ret);
			goto err_ctrl_read;
		}

		count = tmpbuf[BYTE_COUNT];

		if (count == 3)
			goto empty_packet;

		report_id = tmpbuf[REPORT_ID_BYTE];

		if (!IS_TOUCH(report_id))
			goto err_wrong_report;

		if (IS_SCANTIME(report_id))
			count -= SIZE_OF_SCAN_TIME;

		num_touches = tmpbuf[count - 1];

		if (num_touches > MAX_TOUCHES) {
			dev_err(&data->client->dev,
					"Error %d num of touches not supported",
					num_touches);
			goto err_num_touches;
		}

		count -= SIZE_OF_NUM_TOUCH;
		count -= SIZE_OF_BYTE_COUNT;
		count -= SIZE_OF_REPORT_ID;

		if (!n_touch)
			n_touch = num_touches;

		i = SIZE_OF_BYTE_COUNT + SIZE_OF_REPORT_ID;
		/* NOTE: If actual touch data is less than 5, the rest of touch
		 * data would be set zero in each touch packet for keeping
		 * the fixed length.
		 */
		for ( ; (i < count) && (n_touch > 0); i += SIZE_OF_TOUCH_DATA) {
			n_touch--;

			id = tmpbuf[i + CONTACT_ID];
			input_mt_slot(input_dev, id);

			status = tmpbuf[i + STATUS_TOUCH];
			if (status == TOUCH_ACTIVE) {
				x = tmpbuf[i + X_LSB] | (tmpbuf[i + X_MSB] << 8);
				y = tmpbuf[i + Y_LSB] | (tmpbuf[i + Y_MSB] << 8);

				/* Touch active */
				input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 1);
				input_report_abs(input_dev, ABS_MT_POSITION_X, x);
				input_report_abs(input_dev, ABS_MT_POSITION_Y, y);
				input_report_abs(input_dev, ABS_MT_PRESSURE, 1);
				input_report_abs(input_dev, ABS_MT_TOUCH_MAJOR, 1);
				input_report_key(input_dev, BTN_TOUCH, 1);
				input_report_abs(input_dev, ABS_X, x);
				input_report_abs(input_dev, ABS_Y, y);
				input_report_abs(input_dev, ABS_PRESSURE, 1);
			} else if (status == TOUCH_INACTIVE) {
				/* Touch no longer active, close out slot */
				input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 0);
				input_report_abs(input_dev, ABS_MT_PRESSURE, 0);
				input_report_abs(input_dev, ABS_PRESSURE, 0);
				input_report_key(input_dev, BTN_TOUCH, 0);
			}
		}
	} while (num_touches > 5);

	input_sync(data->input_dev);
	return IRQ_HANDLED;

empty_packet:
	input_mt_report_slot_state(input_dev, MT_TOOL_FINGER, 0);
	input_sync(data->input_dev);

err_wrong_report:
err_num_touches:
err_ctrl_read:
	return IRQ_HANDLED;
}

static int sis_initialize_input_device(struct sis_data *data)
{
	struct device *dev = &data->client->dev;
	struct input_dev *input_dev;
	int error;
	unsigned int mt_flags = 0;

	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	input_dev->name = "SiS Touch";
	input_dev->phys = data->phys;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = dev;

	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(EV_KEY, input_dev->evbit);

	mt_flags = INPUT_MT_DIRECT;

	/* For single touch */
	input_set_abs_params(input_dev, ABS_X, 0, MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, 255, 0, 0);

	/* For multi touch */
	error = input_mt_init_slots(input_dev, MAX_TOUCHES, mt_flags);
	if (error) {
		dev_err(dev, "Error %d initialising slots\n", error);
		goto err_free_mem;
	}

	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, MAX_AREA, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);

	input_set_drvdata(input_dev, data);

	error = input_register_device(input_dev);
	if (error) {
		dev_err(dev, "Error %d registering input device\n", error);
		goto err_free_mem;
	}

	data->input_dev = input_dev;

	return 0;

err_free_mem:
	input_free_device(input_dev);
	return error;
}

static struct sis_platform_data *sis_parse_dt(struct i2c_client *client)
{
	struct sis_platform_data *pdata;

	if (!client->dev.of_node)
		return ERR_PTR(-ENODEV);

	pdata = devm_kzalloc(&client->dev, sizeof(struct sis_platform_data),
			GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->gpio_rst = 0;
	pdata->gpio_int = 0;

	/* reset gpio */
	pdata->gpio_rst = of_get_named_gpio(client->dev.of_node,
			"sis,rst-gpio", 0);

	/* interrupt gpio */
	pdata->gpio_int = of_get_named_gpio(client->dev.of_node,
			"sis,int-gpio", 0);

	return pdata;
}

static ssize_t ver_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	uint8_t ver[] = { MAJOR_NUM, '.', MINOR_NUM, '.', EXTRA_NUM };
	uint8_t len = sizeof(ver);

	memcpy(buf, ver, len);

	return len;
}

static ssize_t fw_info_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct sis_data *data = dev_get_drvdata(dev);
	uint8_t len = sizeof(data->fw_info);
	int ret;

	ret = mutex_lock_interruptible(&data->diag_mutex);
	if (ret)
		return ret;

	memcpy(buf, data->fw_info, len);

	mutex_unlock(&data->diag_mutex);

	return len;
}

static int send_cmd86_read_debug(struct sis_data *data, uint32_t d_addr,
		uint8_t *pbuf, uint32_t len)
{
	uint8_t cmd[14];
	int ret;
	uint8_t rsp[MAX_RD_SIZE] = {0};
	uint16_t num_trans;
	uint16_t i;
	uint32_t addr;

	cmd[0] = OUTPUT_REG & 0x00ff;
	cmd[1] = (OUTPUT_REG >> 8) & 0x00ff;
	cmd[2] = 0x0c;
	cmd[3] = 0x00;
	cmd[4] = REPORTID_VENDOR_CMD;
	cmd[5] = 0x00;
	cmd[6] = CMD_READ;

	num_trans = num_of_tranfers(len, DATA_UNIT);

	for (i = 0; i < num_trans; i++) {
		addr = d_addr + (i * 13);

		cmd[8] = (addr & 0x00ff);
		cmd[9] = (addr >> 8) & 0x00ff;
		cmd[10] = (addr >> 16) & 0x00ff;
		cmd[11] = (addr >> 24) & 0x00ff;
		cmd[12] = (DATA_UNIT & 0x00ff);
		cmd[13] = (DATA_UNIT >> 8) & 0x00ff;

		ret = insert_crc(cmd, sizeof(cmd));
		if (ret < 0)
			return ret;

		ret = __sis_write(data->client, sizeof(cmd), cmd);
		if (ret < 0)
			return ret;

		ret = __sis_read(data->client, sizeof(rsp), rsp);
		if (ret < 0)
			return ret;

		ret = response_valid(rsp);
		if (ret < 0)
			return -EINVAL;

		if (num_trans == 1)
			memcpy(pbuf, rsp + 8, len % DATA_UNIT);
		else
			memcpy(pbuf, rsp + 8, DATA_UNIT);

		pbuf += DATA_UNIT;
		mdelay(5);
	}

	return 0;
}


static int read_sensor_data(struct sis_data *data, uint32_t addr, char *pbuf)
{
	uint8_t *volt_data;
	uint8_t temp_buf[4];
	uint8_t ntx;
	uint8_t nrx;
	uint8_t allign_rx;
	uint32_t total_len;
	uint32_t i;
	uint32_t index = 0;
	int data_len;
	int ret;

	disable_irq(data->irq);

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		goto err_comm_fail;

	msleep(100);

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, CHG_OLD_MODE);
	if (ret < 0)
		goto err_comm_fail;

	msleep(200); /* time to change to old mode */

	/* read tx and rx */
	ret = send_cmd86_read(data, VOLT_DATA_ADDR, temp_buf, sizeof(temp_buf));
	if (ret < 0)
		goto err_comm_fail;

	ntx = temp_buf[1];
	nrx = temp_buf[3];
	if (nrx % 4)
		allign_rx  = nrx + (4 - (nrx % 4));
	else
		allign_rx = nrx;

	total_len = ntx * allign_rx;
	data_len = ntx * nrx;

	/* check if enough memory */
	if (total_len > VOLT_DATA_SIZE) {
		pr_info("Not enough memory in sysfs file\n");
		ret = -ENOMEM;
		goto err_comm_fail;
	}

	/* copy x, y, length and target volt to file */
	if (addr == RAW_DATA_ADDR) {
		uint8_t buf_volt[4];
		uint16_t t_volt;

		/* read target volt */
		ret = send_cmd86_read(data, TARGET_VOLT_ADDR, buf_volt,
				sizeof(buf_volt));
		if (ret < 0)
			goto err_comm_fail;


		/* copy x, y, length and target volt to file */
		pbuf[0] = ntx;
		pbuf[1] = nrx;
		t_volt =  ((buf_volt[3]<<0) | (buf_volt[2]<<8)
				| (buf_volt[1]<<16) | (buf_volt[0]<<24)) >> 4;
		pbuf[2] = (t_volt >> 8) & 0x00ff;
		pbuf[3] = t_volt & 0x00ff;
		index = 4;
		data_len += index;
	}

	/* read raw/cycle data */
	volt_data = kmalloc(total_len + 1, GFP_KERNEL);
	if (!volt_data) {
		ret = -ENOMEM;
		goto err_comm_fail;
	}

	ret = send_cmd86_read_debug(data, addr, volt_data, total_len);
	if (ret < 0)
		goto err_comm_fail;

	/* copy data to file, remove the holes */
	for (i = 0; i < total_len; i += allign_rx, index += nrx)
		memcpy(&pbuf[index], &volt_data[i], nrx);

	kfree(volt_data);

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd82_reset_soft(data);
	if (ret < 0)
		goto err_comm_fail;

	/* return data length on SUCCESS */
	ret = data_len;

err_comm_fail:
	enable_irq(data->irq);
	return ret;
}

static ssize_t sensor_data_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	uint32_t data_addr[] = { RAW_DATA_ADDR, CYCLE_DATA_ADDR };
	struct sis_data *data = dev_get_drvdata(dev);
	uint8_t id = data->sensor_data_id;
	int len;

	/* read raw/cycle data */
	len = read_sensor_data(data, data_addr[id], buf);
	if (len < 0) {
		pr_info("failed %d\n", len);
		data->sensor_data_id = 0;
		return len;
	}

	id++;
	data->sensor_data_id = (id > 1) ? 0 : id;

	return len;
}

static int sis_update_file_name(struct device *dev, char **file_name,
		const char *buf, size_t count)
{
	char *file_name_tmp;

	/* Simple sanity check */
	if (count > 64) {
		dev_err(dev, "File name too long\n");
		return -EINVAL;
	}

	file_name_tmp = krealloc(*file_name, count + 1, GFP_KERNEL);
	if (!file_name_tmp)
		return -ENOMEM;

	*file_name = file_name_tmp;
	memcpy(*file_name, buf, count);

	/* Echo into the sysfs entry may append newline at the end of buf */
	if (buf[count - 1] == '\n')
		(*file_name)[count - 1] = '\0';
	else
		(*file_name)[count] = '\0';

	return 0;
}

static int cmd_update_flash(struct sis_data *data, uint32_t addr, uint32_t len)
{
	int ret;
	uint16_t i;
	uint16_t num_blocks;
	uint32_t curr_len;
	uint32_t num_bytes = len;
	uint32_t offset = (uint32_t)(addr & 0xFFFFFFFF);
	uint32_t wr_addr;
	uint32_t index = 0;
	uint32_t remain_bytes;
	uint16_t data_len;

	while (num_bytes) {
		curr_len = (num_bytes > UNIT_FLASH_SIZE) ? UNIT_FLASH_SIZE : num_bytes;
		num_blocks = num_of_tranfers(curr_len, DATA_UNIT);
		wr_addr = addr + index;
		offset = (uint32_t)(wr_addr & 0xFFFFFFFF);

		pr_info("\nWriting at address=%08X blocks=%d\n", wr_addr, num_blocks);
		ret = send_cmd83_write_start(data, wr_addr, num_blocks);
		if (ret < 0)
			goto update_failed;

		for (i = 0; i < num_blocks; i++) {
			remain_bytes = curr_len - (i * DATA_UNIT);
			data_len = (remain_bytes > DATA_UNIT) ? DATA_UNIT : remain_bytes;

			ret = send_cmd84_write(data, data->fw->data, offset, data_len);
			if (ret < 0)
				goto update_failed;

			offset += DATA_UNIT;
		}

		pr_info("Wrote all %d blocks ...\n", num_blocks);

		ret = send_cmd81_write_finish(data);
		if (ret < 0)
			goto update_failed;

		msleep(4000); /* finalise write in update mode 0 */

		index += curr_len;
		num_bytes -= curr_len;
		pr_info("Successfully\n");
	}

	ret = 0;

update_failed:

	return ret;
}

static ssize_t sis_update_fw(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct sis_data *data = dev_get_drvdata(dev);
	int ret;
	int i;
	uint32_t addr;

	ret = mutex_lock_interruptible(&data->diag_mutex);
	if (ret)
		return ret;

	ret = sis_update_file_name(dev, &data->fw_name, buf, count);
	if (ret)
		return ret;

	dev_info(dev, "Firmware file name %s\n", data->fw_name);

	ret = request_firmware(&data->fw, data->fw_name, dev);
	if (ret) {
		dev_err(dev, "Unable to open file %s with error %d\n",
				data->fw_name, ret);
		goto err_fw_not_found;
	}

	dev_info(dev, "File %s opened successfully\n", data->fw_name);

	disable_irq(data->irq);

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

#if 0

	/* 1. Clear boot-flag */
	ret = cmd_update_flash(data, 0x0001e000, 0x1000);
	if (ret < 0)
		goto err_comm_fail;

	/* 2. Update Main code 1 */
	ret = cmd_update_flash(data, 0x00007000, 0x16000);
	if (ret < 0)
		goto err_comm_fail;

	/* 3. Update Main code 2 */
	ret = cmd_update_flash(data, 0x00006000, 0x1000);
	if (ret < 0)
		goto err_comm_fail;

	/* 4. Update FW info to USB descriptor */
	ret = cmd_update_flash(data, 0x00004000, 0x2000);
	if (ret < 0)
		goto err_comm_fail;

	/* 5. Update boot-code */
	ret = cmd_update_flash(data, 0x00000000, 0x4000);
	if (ret < 0)
		goto err_comm_fail;

	/* 6. Update rodata-code */
	ret = cmd_update_flash(data, 0x0001d000, 0x2000);
	if (ret < 0)
		goto err_comm_fail;
#endif


	/* 1. Clear boot-flag */
	ret = cmd_update_flash(data, 0x0001e000, 0x1000);
	if (ret < 0)
		goto err_comm_fail;

	 /* 2. Update Main code 1 */
	addr = 0x00007000;
	for (i = 0; i < 22; i++) {
		ret = cmd_update_flash(data, addr, 0x1000);
		if (ret < 0)
			goto err_comm_fail;
		addr += 0x1000;
	}

	/* 3. Update Main code 2 */
	ret = cmd_update_flash(data, 0x00006000, 0x1000);
	if (ret < 0)
		goto err_comm_fail;

	/* 4. Update FW info to USB descriptor */
	addr = 0x00004000;
	for (i = 0; i < 2; i++) {
		ret = cmd_update_flash(data, addr, 0x1000);
		if (ret < 0)
			goto err_comm_fail;
		addr += 0x1000;
	}


	/* 5. Update boot-code */
	addr = 0x00000000;
	for (i = 0; i < 4; i++) {
		ret = cmd_update_flash(data, addr, 0x1000);
		if (ret < 0)
			goto err_comm_fail;
		addr += 0x1000;
	}


	/* 6. Update rodata-code */
	addr = 0x0001d000;
	for (i = 0; i < 2; i++) {
		ret = cmd_update_flash(data, addr, 0x1000);
		if (ret < 0)
			goto err_comm_fail;
		addr += 0x1000;
	}


	msleep(10000); /* Time to finalize the fw */

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	release_firmware(data->fw);

	dev_info(dev, "Resetting device");
	ret = send_cmd82_reset_soft(data);
	if (ret < 0)
		goto err_comm_fail;

	dev_info(dev, "Reset complete");

	ret = update_fw_info(data);
	if (ret < 0)
		goto err_comm_fail;

	dev_info(dev, "Firmware update of the device was successful");

	enable_irq(data->irq);

	mutex_unlock(&data->diag_mutex);

	return count;

err_comm_fail:
	dev_err(dev, "Firmware update of the device failed");
	release_firmware(data->fw);

err_fw_not_found:
	mutex_unlock(&data->diag_mutex);
	return ret;
}

static ssize_t sis_reset(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct sis_data *data = dev_get_drvdata(dev);
	int ret;

	ret = mutex_lock_interruptible(&data->diag_mutex);
	if (ret)
		return ret;

	dev_info(dev, "Resetting controller");
	disable_irq(data->irq);

	ret = send_cmd82_reset_soft(data);
	if (ret < 0)
		goto err_comm_fail;

	dev_info(dev, "Reset complete");

	ret = update_fw_info(data);
	if (ret < 0)
		goto err_comm_fail;

	dev_info(dev, "Reset successful");

	enable_irq(data->irq);

	mutex_unlock(&data->diag_mutex);

	return count;

err_comm_fail:
	dev_err(dev, "Reset failed");

	mutex_unlock(&data->diag_mutex);
	return ret;
}

static ssize_t fw_mode_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	uint8_t fw_mode[32];
	uint8_t len = sizeof(fw_mode);
	int ret = 0;

	struct sis_data *data = dev_get_drvdata(dev);

	disable_irq(data->irq);

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd86_read(data, FWMODE_ADDR, fw_mode, len);
	if (ret < 0)
		goto err_comm_fail;

	memcpy(buf, fw_mode, len);

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	 ret = len;

err_comm_fail:
	enable_irq(data->irq);
	return ret;
}

static ssize_t diff_data_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	uint8_t *volt_data;
	uint8_t temp_buf[4];
	uint8_t ntx;
	uint8_t nrx;
	uint8_t allign_rx;
	uint8_t is_16_bit;
	uint32_t total_len;
	uint32_t i;
	uint32_t index = 0;
	int data_len;
	int ret;

	struct sis_data *data = dev_get_drvdata(dev);

	disable_irq(data->irq);

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		goto err_comm_fail;

	msleep(100);

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	/* read tx and rx */
	ret = send_cmd86_read(data, VOLT_DATA_ADDR, temp_buf, sizeof(temp_buf));
	if (ret < 0)
		goto err_comm_fail;

	ntx = temp_buf[1];
	nrx = temp_buf[3];

	is_16_bit = data->fw_info[0x1D];

	if (is_16_bit) {
		if (nrx % 2)
			allign_rx  = nrx + (2 - (nrx % 2));
		else
			allign_rx = nrx;

		total_len = ntx * allign_rx * 2;
		data_len   = ntx * nrx * 2;
	} else {
		if (nrx % 4)
			allign_rx  = nrx + (4 - (nrx % 4));
		else
			allign_rx = nrx;

		total_len = ntx * allign_rx;
		data_len = ntx * nrx;
	}

	/* check if enough memory */
	if (total_len > VOLT_DATA_SIZE) {
		pr_info("Not enough memory in sysfs file\n");
		ret = -ENOMEM;
		goto err_comm_fail;
	}

	/* copy x, y, bit8/16 info to file */
	buf[0] = ntx;
	buf[1] = nrx;
	buf[2] = is_16_bit;
	index = 3;
	data_len += index;

	/* read diff data */
	volt_data = kmalloc(total_len + 1, GFP_KERNEL);
	if (!volt_data) {
		ret = -ENOMEM;
		goto err_comm_fail;
	}

	ret = send_cmd86_read_debug(data, DIFF_DATA_ADDR, volt_data, total_len);
	if (ret < 0)
		goto err_comm_fail;

	/* copy data to file, remove the holes */
	if (is_16_bit) {
		allign_rx *= 2;
		nrx *= 2;
	}
	for (i = 0; i < total_len; i += allign_rx, index += nrx)
		memcpy(&buf[index], &volt_data[i], nrx);

	kfree(volt_data);

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	/* return data length on SUCCESS */
	ret = data_len;

err_comm_fail:
	enable_irq(data->irq);
	return ret;
}

static ssize_t fw_parms_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct sis_data *data = dev_get_drvdata(dev);
	int ret;

	ret = mutex_lock_interruptible(&data->diag_mutex);
	if (ret)
		return ret;

	disable_irq(data->irq);

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd86_read(data, FW_PARMS_RD_ADDR, buf, PAGE_SIZE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	/* return data length on SUCCESS */
	ret = PAGE_SIZE-1;

err_comm_fail:
	enable_irq(data->irq);
	mutex_unlock(&data->diag_mutex);
	return ret;
}

static ssize_t fw_parms_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	struct sis_data *data = dev_get_drvdata(dev);
	int ret;

	uint16_t i;
	uint16_t num_blocks;
	uint32_t num_bytes = count + 1;
	uint32_t offset = (uint32_t)(FW_PARMS_WT_ADDR & 0xFFFFFFFF);
	uint32_t remain_bytes;
	uint16_t data_len;

	ret = mutex_lock_interruptible(&data->diag_mutex);
	if (ret)
		return ret;

	disable_irq(data->irq);

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	num_blocks = num_of_tranfers(num_bytes, DATA_UNIT);
	offset = 0;

	pr_info("\nInitiating parms update....\n");
	ret = send_cmd83_write_start(data, FW_PARMS_WT_ADDR, num_blocks);
	if (ret < 0)
		goto err_comm_fail;

	for (i = 0; i < num_blocks; i++) {
		remain_bytes = num_bytes - (i * DATA_UNIT);
		data_len = (remain_bytes > DATA_UNIT) ? DATA_UNIT : remain_bytes;

		ret = send_cmd84_write(data, buf, offset, data_len);
		if (ret < 0)
			goto err_comm_fail;

		offset += DATA_UNIT;
	}

	ret = send_cmd81_write_finish(data);
	if (ret < 0)
		goto err_comm_fail;

	msleep(4000); /* finalise write in update mode 0 */

	msleep(10000);

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	dev_info(dev, "Parms updated");
	dev_info(dev, "Resetting device");
	ret = send_cmd82_reset_soft(data);
	if (ret < 0)
		goto err_comm_fail;
	dev_info(dev, "Reset complete");

	ret = update_fw_info(data);
	if (ret < 0)
		goto err_comm_fail;

	enable_irq(data->irq);

	mutex_unlock(&data->diag_mutex);

	return count;

err_comm_fail:
	dev_err(dev, "Parms update failed");
	enable_irq(data->irq);
	mutex_unlock(&data->diag_mutex);
	return ret;
}

static ssize_t dump_fw_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct sis_data *data = dev_get_drvdata(dev);
	int ret;

	uint16_t data_len = 0xF00;
	uint8_t id = data->fw_dump_id;
	uint32_t addr = FW_RD_ADDR + (id * data_len);

	/* total read = 34 (0x1F000 / 0xF00) */
	data->fw_dump_id = (id == 33) ? 0 : (id + 1);
	if (id == 33)
		data_len = 0x100;
	pr_info("addr: %X", addr);

	ret = mutex_lock_interruptible(&data->diag_mutex);
	if (ret)
		return ret;

	disable_irq(data->irq);

	ret = send_cmd85_change_mode(data, PWR_CMD_ACTIVE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, ENABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd86_read(data, addr, buf, data_len);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, PWR_CMD_FWCTRL);
	if (ret < 0)
		goto err_comm_fail;

	ret = send_cmd85_change_mode(data, DISABLE_DIAGNOSIS_MODE);
	if (ret < 0)
		goto err_comm_fail;

	/* return data length on SUCCESS */
	ret = data_len;

err_comm_fail:
	if (ret < 0)
		data->fw_dump_id = 0;

	enable_irq(data->irq);
	mutex_unlock(&data->diag_mutex);
	return ret;
}


static DEVICE_ATTR(ver, S_IRUSR, ver_show, NULL);
static DEVICE_ATTR(fw_info, S_IRUSR, fw_info_show, NULL);
static DEVICE_ATTR(update_fw, S_IWUSR, NULL, sis_update_fw);
static DEVICE_ATTR(reset_sw, S_IWUSR, NULL, sis_reset);
static DEVICE_ATTR(sensor_data, S_IRUSR, sensor_data_show, NULL);
static DEVICE_ATTR(fw_mode, S_IRUSR, fw_mode_show, NULL);
static DEVICE_ATTR(diff_data, S_IRUSR, diff_data_show, NULL);
static DEVICE_ATTR(fw_parms, S_IRUSR|S_IWUSR, fw_parms_show, fw_parms_store);
static DEVICE_ATTR(dump_fw, S_IRUSR, dump_fw_show, NULL);

static struct attribute *sis_attrs[] = {
	&dev_attr_ver.attr,
	&dev_attr_fw_info.attr,
	&dev_attr_update_fw.attr,
	&dev_attr_reset_sw.attr,
	&dev_attr_sensor_data.attr,
	&dev_attr_fw_mode.attr,
	&dev_attr_diff_data.attr,
	&dev_attr_fw_parms.attr,
	&dev_attr_dump_fw.attr,
	NULL,   /* need to NULL terminate the list of attributes */
};

static const struct attribute_group sis_attr_group = {
	.attrs = sis_attrs,
};

static int sis_probe(struct i2c_client *client)
{
	struct sis_data *data;
	struct sis_platform_data *pdata;
	int ret;

	pdata = sis_parse_dt(client);
	if (IS_ERR(pdata))
		return PTR_ERR(pdata);

	data = devm_kzalloc(&client->dev, sizeof(struct sis_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	snprintf(data->phys, sizeof(data->phys), "i2c-%u-%04x",
			client->adapter->nr, client->addr);

	dev_info(&client->dev, "%s\n", data->phys);

	data->client = client;
	data->pdata = pdata;
	i2c_set_clientdata(client, data);
	mutex_init(&data->diag_mutex);

	ret = devm_gpio_request(&client->dev, pdata->gpio_int, "sis-int");
	if (ret < 0) {
		dev_err(&client->dev, "Failed to request gpio %ld, error %d\n",
				pdata->gpio_int, data->irq);
		return ret;
	}

	ret = gpio_direction_input(pdata->gpio_int);
	if (ret < 0) {
		dev_err(&client->dev, "Failed to set direction for gpio %ld, error %d\n",
				pdata->gpio_int, data->irq);
		return ret;
	}

	data->irq = gpio_to_irq(pdata->gpio_int);
	if (data->irq < 0) {
		dev_err(&client->dev, "Failed to get irq number for gpio %ld, error %d\n",
				pdata->gpio_int, data->irq);
		return data->irq;
	}

	sis_ctrl_reset_hard(client, pdata);

	ret = update_fw_info(data);
	if (ret < 0) {
		dev_err(&client->dev, "SiS touch controller at address %x init failed",
				client->addr);
		return ret;
	}

	ret = sis_initialize_input_device(data);
	if (ret < 0) {
		dev_err(&client->dev, "Input device initialisation falied %d\n", ret);
		return ret;
	}

	ret = request_threaded_irq(data->irq, NULL, sis_int_irq_handler,
			IRQF_TRIGGER_LOW|IRQF_ONESHOT, data->client->name,
			data);
	if (ret) {
		dev_err(&client->dev, "Can't request irq %d because of %d\n", data->irq, ret);
		return ret;
	}
	data->client->irq = data->irq;

	ret = sysfs_create_group(&client->dev.kobj, &sis_attr_group);
	if (ret) {
		dev_err(&client->dev, "Failure %d creating sysfs group\n", ret);
		goto err_free_irq;
	}

	return 0;

err_free_irq:
	if (data->irq)
		free_irq(data->irq, data);

	return ret;
}

static const struct of_device_id sis_of_match[] = {
	{ .compatible = "sis,i2c_touch_driver", },
	{},
};
MODULE_DEVICE_TABLE(of, sis_of_match);

static const struct i2c_device_id sis_id[] = {
	{ "sis_i2c_95xx_ts", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, sis_id);

static struct i2c_driver sis_driver = {
	.driver = {
		.name   = "sis_i2c_95xx_ts",
		.owner  = THIS_MODULE,
		.of_match_table = of_match_ptr(sis_of_match),
	},
	.probe      = sis_probe,
	.id_table   = sis_id,
};

module_i2c_driver(sis_driver);

/* Module information */
MODULE_AUTHOR("Dizni Premdas <Dizni.Premdas@msc-technologies.eu>");
MODULE_DESCRIPTION("SiS Touchconroller I2C driver");
MODULE_LICENSE("GPL");

