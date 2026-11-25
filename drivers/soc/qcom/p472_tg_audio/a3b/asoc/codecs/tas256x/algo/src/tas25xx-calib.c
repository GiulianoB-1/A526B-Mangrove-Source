/*
 ** ============================================================================
 ** Copyright (c) 2017  Texas Instruments Inc.
 **
 ** This program is free software; you can redistribute it and/or modify it
 ** under
 ** the terms of the GNU General Public License as published by the Free
 ** Software Foundation; version 2.
 **
 ** This program is distributed in the hope that it will be useful, but WITHOUT
 ** ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ** FITNESS
 ** FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 ** details
 **
 ** File:
 **     tas25xx-calib.c
 **
 ** Description:
 **     misc driver for interfacing for  Texas Instruments TAS25xx algorithm
 **
 ** ============================================================================
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include "algo/inc/tas_smart_amp_v2.h"
#include "tas25xx-algo-intf.h"

#define TAS_PAYLOAD_SIZE 14

/* Holds the Packet data required for processing */
struct tas_dsp_pkt {
	u8 device_id;
	u8 book;
	u8 page;
	u8 offset;
	u8 data[TAS_PAYLOAD_SIZE * 4];
};

/* Bounded read-only diagnostic ABI: count includes four-byte header.
 * No observed TG runtime client; never copy more than the supplied count.
 */
static int smartamp_params_get(char __user *input, size_t count)
{
	struct tas_dsp_pkt packet = {0};
	u32 index, length, channel, paramid;
	int ret;

	if (count < 8 || count > sizeof(packet) || (count & 3))
		return -EINVAL;
	if (copy_from_user(&packet, input, 4))
		return -EFAULT;
	if (!packet.page || packet.offset < 8 ||
	    packet.offset > 124 || (packet.offset & 3) || packet.book)
		return -EINVAL;
	index = (packet.page - 1) * 30 + (packet.offset - 8) / 4;
	switch (index) {
	case TAS_SA_GET_F0:
	case TAS_SA_GET_Q:
	case TAS_SA_GET_TV:
	case TAS_SA_GET_RE:
	case TAS_SA_GET_STATUS:
	case TAS_SA_EXC_TEMP_STAT:
	case TAS_SA_GET_VALID_STATUS:
	case TAS_SA_GET_OP_MODE:
	case TAS_SA_GET_RE_RANGE:
		break;
	default:
		return -EPERM;
	}
	if (packet.device_id == DEVICE1)
		channel = CHANNEL0;
	else if (packet.device_id == DEVICE2)
		channel = CHANNEL1;
	else
		return -EINVAL;
	length = (count - 4) / 4;
	paramid = TAS_CALC_PARAM_IDX(index, length, channel);
	ret = tas25xx_smartamp_algo_ctrl(packet.data, paramid, TAS_GET_PARAM,
					length * 4, TISA_MOD_RX);
	if (ret)
		return ret < 0 ? ret : -EIO;
	if (copy_to_user(input, &packet, count))
		return -EFAULT;
	return 0;
}

static int tas_calib_open(struct inode *inode, struct file *fd)
{
	return 0;
}

static ssize_t tas_calib_write(struct file *file,
		const char __user *buffer, size_t count, loff_t *offp)
{
	/* P487 final policy: no TG runtime userspace SET writer. */
	return -EPERM;
}

static ssize_t tas_calib_read(struct file *file, char __user *buffer,
		size_t count, loff_t *ptr)
{
	int rc;

	rc = smartamp_params_get(buffer, count);
	return rc ? rc : count;
}

static long tas_calib_ioctl(struct file *filp, uint cmd, ulong arg)
{
	return -EPERM;
}

static int tas_calib_release(struct inode *inode, struct file *fd)
{
	return 0;
}

const struct file_operations tas_calib_fops = {
	.owner			= THIS_MODULE,
	.open			= tas_calib_open,
	.write			= tas_calib_write,
	.read			= tas_calib_read,
	.release		= tas_calib_release,
	.unlocked_ioctl	= tas_calib_ioctl,
};

static struct miscdevice tas_calib_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "tas_calib",
	.fops = &tas_calib_fops,
};

int tas_calib_init(void)
{
	int rc;

	pr_info("TI-SmartPA: %s", __func__);
	rc = misc_register(&tas_calib_misc);
	if (rc)
		pr_err("TI-SmartPA: %s: register calib misc failed\n",
			__func__);
	return rc;
}

void tas_calib_exit(void)
{
	misc_deregister(&tas_calib_misc);
}

/*
 *MODULE_AUTHOR("Texas Instruments Inc.");
 *MODULE_DESCRIPTION("tas2560 Misc driver");
 *MODULE_LICENSE("GPL v2");
 */
