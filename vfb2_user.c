/*
 * User space interface to vfb2.c
 *
 *  Copyright (c) 2005 Jan Steinhoff <jan.steinhoff@uni-jena.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This is usefull if the display driver is implemented in user space (e.g.
 * with libusb).
 */

#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,15)
#include <linux/config.h>
#endif

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include "vfb2.h"
#include "vfb2_user.h"

#define err(format, arg...) printk(KERN_ERR "vfb2_user: " format "\n" , ## arg)

static atomic_t uvfb2_number = ATOMIC_INIT(0);

struct uvfb2_device {
	int vfb2_index;
	int table_length;
	struct vfb2_mode *mode_table;
	int modes;
};

static int uvfb2_open(struct inode *inode, struct file *file)
{
	struct uvfb2_device *dev;

	dev = kmalloc(sizeof(struct uvfb2_device), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	memset(dev, 0x00, sizeof(struct uvfb2_device));
	dev->vfb2_index = -1;
	file->private_data = dev;

	return 0;
}

static int uvfb2_release(struct inode *inode, struct file *file)
{
	struct uvfb2_device *dev = (struct uvfb2_device *)file->private_data;

	file->private_data = NULL;

	if (dev->vfb2_index >= 0) {
		vfb2_unregister(dev->vfb2_index);
		atomic_dec(&uvfb2_number);
	}
	if (dev->mode_table)
		kfree(dev->mode_table);
	kfree(dev);

	return 0;
}

static int uvfb2_read(struct file *file, char __user *buf,
		      size_t nbytes, loff_t *ppos)
{
	char *page = (char*) __get_free_page(GFP_KERNEL);
	char *page_pos = page;
	int retval;

	if (!page)
		return -ENOMEM;

	page_pos += sprintf(page_pos, "number of user space fb: %i\n",
			    atomic_read(&uvfb2_number));

	retval = min(max((int)(page_pos - page - *ppos), 0), (int)nbytes);
	if (retval == 0)
		goto error;
	if (copy_to_user(buf, page + *ppos, retval)) {
		retval = -EFAULT;
		goto error;
	}
	*ppos += retval;
error:
	free_page((unsigned long) page);
	return retval;
}

static int uvfb2_mk_table(struct uvfb2_device *dev, int i)
{
	int size = sizeof(struct vfb2_mode) * (i+1);

	dev->mode_table	= kmalloc(size, GFP_KERNEL);
	if (!dev->mode_table)
		return -ENOMEM;
	memset(dev->mode_table, 0x00, size);
	dev->table_length = i;

	return 0;
}

static int uvfb2_ioctl(struct inode *inode, struct file *file,
		       unsigned int cmd, unsigned long arg)
{
	struct uvfb2_device *dev = (struct uvfb2_device *)file->private_data;
	int i;
	int res;
	struct vfb2_init init;
	struct fb_info *info;

	switch (cmd) {
	case UVFB2_NUM_MODES:
		if (dev->vfb2_index >= 0)
			return -EBUSY;
		if (dev->mode_table)
			return -EINVAL;
		if (get_user(i, (int *)arg))
			return -EFAULT;
		if (i <= 0)
			return -EINVAL;
		return uvfb2_mk_table(dev, i);

	case UVFB2_ADD_MODE:
		if (dev->vfb2_index >= 0)
			return -EBUSY;
		if (dev->table_length == 0) {
			res = uvfb2_mk_table(dev, UVFB2_DEF_NUM_MODES);
			if (res)
				return res;
		}
		if (dev->table_length == dev->modes)
			return -EINVAL;
		if (copy_from_user(&dev->mode_table[dev->modes], (void *)arg,
				   sizeof(struct vfb2_mode))) {
			memset(&dev->mode_table[dev->modes], 0x00,
			       sizeof(struct vfb2_mode));
			return -EFAULT;
		}
		dev->modes++;
		return 0;

	case UVFB2_VMEM_SIZE:
		if (dev->vfb2_index >= 0)
			return -EBUSY;
		if (dev->modes == 0)
			return -EINVAL;
		if (get_user(init.vmem_len, (__u32 *)arg))
			return -EFAULT;
		init.mode_table = dev->mode_table;
		init.vfb2_ioctl = NULL;
		init.private = NULL;
		dev->vfb2_index = vfb2_register(&init);
		if (dev->vfb2_index < 0)
			return dev->vfb2_index;
		atomic_inc(&uvfb2_number);
		return 0;

	case UVFB2_MODE:
		if (dev->vfb2_index < 0)
			return -EINVAL;
		res = vfb2_current_mode(dev->vfb2_index);
		if (res < 0)
			return res;
		if (put_user(res, (int *)arg))
			return -EFAULT;
		return 0;

	case UVFB2_NODE:
		if (dev->vfb2_index < 0)
			return -EINVAL;
		info = vfb2_fb_info(dev->vfb2_index);
		if (!info)
			return -EINVAL;
		if (put_user(info->node, (int *)arg))
			return -EFAULT;
		return 0;
	}

	return -ENOIOCTLCMD;
}

struct file_operations uvfb2_fops = {
	.owner = THIS_MODULE,
	.open = uvfb2_open,
	.release = uvfb2_release,
	.read = uvfb2_read,
	.ioctl = uvfb2_ioctl,
};

static int __init uvfb2_init(void)
{
	struct proc_dir_entry *pentry;

	pentry = create_proc_entry(UVFB2_DEVICE,
				   S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP,
				   NULL);
	if (!pentry) {
		err("can not create /proc/" UVFB2_DEVICE);
		return -EPERM;
	}
	pentry->owner = THIS_MODULE;
	pentry->proc_fops = &uvfb2_fops;
	return 0;
}

static void __exit uvfb2_exit(void)
{
	remove_proc_entry(UVFB2_DEVICE, NULL);
}

module_init(uvfb2_init);
module_exit(uvfb2_exit);

MODULE_LICENSE ("GPL");
