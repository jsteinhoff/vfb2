/****
 * Virtual frame buffer device
 *
 *  Copyright (c) 2005 Jan Steinhoff <jan.steinhoff@uni-jena.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This driver can provide a frame buffer for all displays that can not be
 * io-mapped (e.g. usb displays).
 * It is actually a virtual frame buffer, the display driver can send its
 * content periodically to the display, or only on demand.
 * 
 * based on drivers/video/vfb.c and drivers/usb/media/vicam.c (mmap function)
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fb.h>
#include <linux/init.h>
#include <linux/rwsem.h>
#include <asm/atomic.h>

#include <linux/version.h>
#include "vfb2.h"

#if !defined(CONFIG_FB)
#error : No frame buffer support. Compile kernel with CONFIG_FB. 
#endif
#if !defined(CONFIG_FB_VESA)
#warning : Make sure you have cfbfillrect, cfbcopyarea and cfbimgblt in the kernel or as modules (If unsure, compile kernel with CONFIG_FB_VESA).
#endif

#define err(format, arg...) printk(KERN_ERR "vfb2: " format "\n" , ## arg)

#define VFB2_PRESENT		1
#define VFB2_NOT_PRESENT	0
#define VFB2_ERROR_ON_REGISTER	-1

struct vfb2_device {
	struct vfb2_init init;
	int present;
	int table_index;
	atomic_t open;
	int current_mode;
	void *videomemory;
	struct fb_info *info;
	struct rw_semaphore ioctl_sem;
};

#define VFB2_MAX_DEVICES	FB_MAX
static struct vfb2_device *vfb2_table[VFB2_MAX_DEVICES] = { 0 };
static DECLARE_RWSEM(vfb2_table_sem);

static void vfb2_remove(struct vfb2_device *dev);

static int vfb2_find_dev(struct vfb2_device *dev)
{
	int i;
	int ret = -1;

	for (i=0; i<VFB2_MAX_DEVICES; i++)
		if (vfb2_table[i] == dev) {
			ret = i; 
			break;
		}

	return ret;
}

static inline struct vfb2_device *vfb2_get_dev(struct fb_info *info)
{
	struct vfb2_device *dev;

	if (!info)
		return NULL;
	dev = (struct vfb2_device *)info->par;
	if (!dev)
		return NULL;
	if (vfb2_find_dev(dev) < 0)
		return NULL;
	return dev;
}

static struct vfb2_device *vfb2_get_present_dev(struct fb_info *info)
{
	struct vfb2_device *dev;

	down_read(&vfb2_table_sem);
	dev = vfb2_get_dev(info);
	if (!dev)
		goto exit;
	if (dev->present == VFB2_NOT_PRESENT)
		dev = NULL;
exit:
	up_read(&vfb2_table_sem);
	return dev;
}

static int vfb2_match_mode(struct vfb2_device *dev,
			   struct fb_var_screeninfo *var)
{
	struct vfb2_mode *mode_table = dev->init.mode_table;
	int i;
	int mode = -1;

	for (i=0; mode_table[i].xres; i++)
		if ((mode_table[i].xres == var->xres) &&
		    (mode_table[i].yres == var->yres)) {
			mode = i;
			break;
		}

	for (i=0; mode_table[i].xres; i++)
		if ((mode_table[i].xres == var->xres) &&
		    (mode_table[i].yres == var->yres) &&
		    (mode_table[i].bpp == var->bits_per_pixel)) {
			mode = i;
			break;
		}

	return mode;
}

static inline void vfb2_set_mode(struct vfb2_device *dev,
				 struct fb_var_screeninfo *var, int mode)
{
	struct vfb2_mode *mode_table = dev->init.mode_table;

	var->xres = mode_table[mode].xres;
	var->yres = mode_table[mode].yres;
	var->bits_per_pixel = mode_table[mode].bpp;
}

static inline u_long vfb2_line_length(struct vfb2_device *dev, int mode)
{
	struct vfb2_mode *mode_table = dev->init.mode_table;
	u_long length;

	length = mode_table[mode].xres * mode_table[mode].bpp;
	length = (length + 7) & ~7;
	length >>= 3;
	return length;
}

static void vfb2_set_bitfields(struct fb_var_screeninfo *var, int mode_16bpp)
{
	var->red.offset = 0;
	var->transp.offset = 0;
	var->transp.length = 0;
	switch (var->bits_per_pixel) {
	case 1:
	case 8:
		var->red.length = var->bits_per_pixel;
		var->green.offset = 0;
		var->green.length = var->bits_per_pixel;
		var->blue.offset = 0;
		var->blue.length = var->bits_per_pixel;
		break;
	case 16:
		var->red.length = 5;
		var->green.offset = 5;
		if (mode_16bpp == VFB2_16BPP_TRANSP) {
			var->green.length = 5;
			var->blue.offset = 10;
			var->blue.length = 5;
			var->transp.offset = 15;
			var->transp.length = 1;
		} else {
			var->green.length = 6;
			var->blue.offset = 11;
			var->blue.length = 5;
		}
		break;
	case 32:
		var->transp.offset = 24;
		var->transp.length = 8;
	case 24:
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;
		break;
	}
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;
}

static int vfb2_check_var_helper(struct fb_var_screeninfo *var,
				 struct vfb2_device *dev)
{
	int mode;

	mode = vfb2_match_mode(dev, var);
	if (mode < 0)
		mode = dev->current_mode;
	vfb2_set_mode(dev, var, mode);

	if ((!var->xres) || (!var->yres))
		return -EINVAL;
	if ((var->bits_per_pixel != 1) &&
	    (var->bits_per_pixel != 8) &&
	    (var->bits_per_pixel != 16) &&
	    (var->bits_per_pixel != 24) &&
	    (var->bits_per_pixel != 32))
		return -ENOTSUPP;

	if (vfb2_line_length(dev, mode) * var->yres > dev->init.vmem_len)
		return -ENOMEM;

	var->xres_virtual = var->xres;
	var->yres_virtual = var->yres;
	var->xoffset = 0;
	var->yoffset = 0;
	var->grayscale = 0;
	var->activate = FB_ACTIVATE_NOW;
	var->vmode = FB_VMODE_NONINTERLACED;
	vfb2_set_bitfields(var, dev->init.mode_table[mode].transp_mode);

	return 0;
}

static int vfb2_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct vfb2_device *dev = vfb2_get_present_dev(info);

	if (!dev)
		return -ENODEV;
	return vfb2_check_var_helper(var, dev);
}

static int vfb2_set_par_helper(struct fb_info *info, struct vfb2_device *dev)
{
	int mode;

	mode = vfb2_match_mode(dev, &info->var);
	if (mode < 0)
		return -EINVAL;

	info->fix.line_length = vfb2_line_length(dev, mode);
	info->fix.visual = dev->init.mode_table[mode].visual;
	dev->current_mode = mode;

	return 0;
}

static int vfb2_set_par(struct fb_info *info)
{
	struct vfb2_device *dev = vfb2_get_present_dev(info);

	if (!dev)
		return -ENODEV;
	return vfb2_set_par_helper(info, dev);
}

static int vfb2_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			  u_int transp, struct fb_info *info)
{
	struct vfb2_device *dev = vfb2_get_present_dev(info);

	if (!dev)
		return -ENODEV;

	if (regno >= 256)
		return 1;

#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW

	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
		switch (info->var.bits_per_pixel) {
		case 16:
		case 24:
		case 32:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}
	return 0;
}

static int vfb2_open(struct fb_info *info, int user)
{
	struct vfb2_device *dev;
	int ret = -ENODEV;

	down_read(&vfb2_table_sem);
	dev = vfb2_get_dev(info);
	if (!dev)
		goto error;
	if (dev->present == VFB2_NOT_PRESENT)
		goto error;
	atomic_inc(&dev->open);
	ret = 0;
error:
	up_read(&vfb2_table_sem);
	return ret;
}

static int vfb2_release(struct fb_info *info, int user)
{
	struct vfb2_device *dev;
	int remove = 0;
	int ret = 0;

	down_read(&vfb2_table_sem);
	dev = vfb2_get_dev(info);
	if (!dev) {
		ret = -ENODEV;
		goto error;
	}
	if (atomic_dec_and_test(&dev->open) &&
	    (dev->present == VFB2_NOT_PRESENT))
		remove = 1;
error:
	up_read(&vfb2_table_sem);

	if (remove)
		vfb2_remove(dev);

	return ret;
}

static int vfb2_ioctl(struct inode *inode, struct file *file,
		      unsigned int cmd, unsigned long arg,
		      struct fb_info *info)
{
	struct vfb2_device *dev = vfb2_get_present_dev(info);
	int ret = -ENODEV;

	if (!dev)
		return -ENODEV;

	down_read(&dev->ioctl_sem);
	if (dev->present == VFB2_NOT_PRESENT)
		goto error;

	if (dev->init.vfb2_ioctl)
		ret = dev->init.vfb2_ioctl(inode, file, cmd, arg,
					   dev->table_index);
	else
		ret = -ENOIOCTLCMD;
error:
	up_read(&dev->ioctl_sem);
	return ret;
}

static int vfb2_mmap(struct fb_info *info, struct file *file,
		     struct vm_area_struct *vma)
{
	unsigned long page, pos;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,10)
	unsigned long kva;
#endif
	unsigned long start = vma->vm_start;
	unsigned long size  = vma->vm_end-vma->vm_start;
	struct vfb2_device *dev = vfb2_get_present_dev(info);

	if (!dev)
		return -ENODEV;

	if (size > info->fix.smem_len)
		return -EINVAL;

	pos = (unsigned long) info->screen_base;
	while (size > 0) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,10)
		page = page_to_pfn(vmalloc_to_page((void *)pos));
		if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED))
			return -EAGAIN;
#else
		kva = (unsigned long)page_address(vmalloc_to_page((void *)pos));
		kva |= pos & (PAGE_SIZE-1);
		page = __pa(kva);
		if (remap_page_range(vma, start, page, PAGE_SIZE, PAGE_SHARED))
			return -EAGAIN;
#endif

		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	return 0;
}

static struct fb_ops vfb2_ops = {
	.owner		= THIS_MODULE,
	.fb_setcolreg	= vfb2_setcolreg,
	.fb_check_var	= vfb2_check_var,
	.fb_set_par	= vfb2_set_par,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
	.fb_cursor	= soft_cursor,
	.fb_open	= vfb2_open,
	.fb_release	= vfb2_release,
	.fb_mmap	= vfb2_mmap,
	.fb_ioctl	= vfb2_ioctl,
};

static inline int vfb2_alloc_vmem(struct vfb2_device *dev)
{
	void *adr;
	long size = dev->init.vmem_len;

	if (size % PAGE_SIZE) {
		size = ((size % PAGE_SIZE) + 1) * PAGE_SIZE;
		dev->init.vmem_len = size;
	}

	if (!(dev->videomemory = vmalloc(size)))
		return -ENOMEM;

	memset(dev->videomemory, 0, size);
	adr = dev->videomemory;
	while (size > 0) {
		SetPageReserved(vmalloc_to_page(adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	return 0;
}

static inline void vfb2_free_vmem(struct vfb2_device *dev)
{
	void *adr = dev->videomemory;
	long size = dev->init.vmem_len;

	if (!adr)
		return;

	while (size > 0) {
		ClearPageReserved(vmalloc_to_page(adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}
	vfree(dev->videomemory);
	dev->videomemory = NULL;
}

static void vfb2_remove(struct vfb2_device *dev)
{
	if (!dev)
		return;

	down_write(&vfb2_table_sem);
	if (dev->table_index >= 0) {
		if (dev->present == VFB2_PRESENT) {
			err("vfb2_remove called, but device still present!");
			goto error;
		}
		if (atomic_read(&dev->open)) {
			err("vfb2_remove called, but device still open!");
			goto error;
		}
		if (dev->present != VFB2_ERROR_ON_REGISTER)
			unregister_framebuffer(dev->info);

		vfb2_table[dev->table_index] = NULL;
		dev->table_index = -1;
		dev->info->par = NULL;
	}
	up_write(&vfb2_table_sem);

	if (dev->info) {
		if (dev->info->cmap.len)
			fb_dealloc_cmap(&dev->info->cmap);

		framebuffer_release(dev->info);
		dev->info = NULL;
	}

	vfb2_free_vmem(dev);

	if (dev->init.mode_table)
		kfree(dev->init.mode_table);
	kfree(dev);
	return;
error:
	up_write(&vfb2_table_sem);
}

static inline int vfb2_num_modes(struct vfb2_mode *mode_table)
{
	int i;

	for (i=0; mode_table[i].xres; i++) ;
	return i;
}

static inline struct vfb2_device *vfb2_init_dev(struct vfb2_init *init)
{
	struct vfb2_device *dev;
	int mtable_size = (vfb2_num_modes(init->mode_table)+1)
			  * sizeof(struct vfb2_mode);

	dev = kmalloc(sizeof(struct vfb2_device), GFP_KERNEL);
	if (!dev)
		goto error;
	memcpy(&dev->init, init, sizeof(struct vfb2_init));

	dev->init.mode_table = kmalloc(mtable_size, GFP_KERNEL);
	if (!dev->init.mode_table) {
		kfree(dev);
		dev = NULL;
		goto error;
	}
	memcpy(dev->init.mode_table, init->mode_table, mtable_size);

	dev->info = NULL;
	dev->videomemory = NULL;
	dev->current_mode = 0;
	dev->present = VFB2_ERROR_ON_REGISTER;
	dev->table_index = -1;
	atomic_set(&dev->open, 0);
	init_rwsem(&dev->ioctl_sem);
error:
	return dev;
}

int vfb2_register(struct vfb2_init *init)
{
	int i;
	struct fb_info *info;
	int res = -ENOMEM;
	struct vfb2_device *dev;

	if (!init || !init->mode_table)
		return -EINVAL;	

	dev = vfb2_init_dev(init);
	if (!dev)
		goto error;

	if (vfb2_alloc_vmem(dev))
		goto error;

	info = framebuffer_alloc(sizeof(__u32) * 256, NULL);
	if (!info)
		goto error;
	info->pseudo_palette = info->par;
	info->par = dev;
	dev->info = info;

	res = fb_alloc_cmap(&info->cmap, 256, 0);
	if (res < 0)
		goto error;

	info->screen_base = dev->videomemory;
	info->fbops = &vfb2_ops;
	info->flags = FBINFO_FLAG_DEFAULT;
	strcpy(info->fix.id, "vfb2");
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.accel = FB_ACCEL_NONE;
	info->fix.smem_len = dev->init.vmem_len;
	vfb2_set_mode(dev, &info->var, 0);
	res = vfb2_check_var_helper(&info->var, dev);
	if (res < 0)
		goto error;
	res = vfb2_set_par_helper(info, dev);
	if (res < 0)
		goto error;

	down_write(&vfb2_table_sem);
	for (i=0; i<VFB2_MAX_DEVICES; i++)
		if (vfb2_table[i] == NULL) {
			dev->table_index = i;
			vfb2_table[i] = dev;
			break;
		}
	if (dev->table_index == -1) {
		err("vfb2_table is full!");
		res = -EBUSY;
		goto error2;
	}

	res = register_framebuffer(info);
	if (!res) {
		dev->present = VFB2_PRESENT;
		res = dev->table_index;
	}
error2:
	up_write(&vfb2_table_sem);
error:
	if (res < 0)
		vfb2_remove(dev);
	return res;
}

static struct vfb2_device *vfb2_index_to_dev(int index)
{
	struct vfb2_device *dev;

	if ((index < 0) || (index >= VFB2_MAX_DEVICES))
		return NULL;
	dev = vfb2_table[index];
	return dev;
}

void vfb2_unregister(int table_index)
{
	struct vfb2_device *dev;
	int open;

	down_write(&vfb2_table_sem);
	dev = vfb2_index_to_dev(table_index);
	if (!dev) {
		up_write(&vfb2_table_sem);
		err("vfb2_unregister: invalid table_index");
		return;
	}
	dev->present = VFB2_NOT_PRESENT;
	open = atomic_read(&dev->open);
	up_write(&vfb2_table_sem);

	/* wait for ioctl to finish */
	down_write(&dev->ioctl_sem);
	up_write(&dev->ioctl_sem);

	if (!open)
		vfb2_remove(dev);
}

int vfb2_current_mode(int table_index)
{
	struct vfb2_device *dev;
	int ret = -EINVAL;

	down_read(&vfb2_table_sem);
	dev = vfb2_index_to_dev(table_index);
	if (!dev)
		goto error;
	ret = dev->current_mode;
error:
	up_read(&vfb2_table_sem);
	return ret;
}

void *vfb2_videomemory(int table_index)
{
	struct vfb2_device *dev;
	void *ret = NULL;

	down_read(&vfb2_table_sem);
	dev = vfb2_index_to_dev(table_index);
	if (!dev)
		goto error;
	ret = dev->videomemory;
error:
	up_read(&vfb2_table_sem);
	return ret;
}

struct fb_info *vfb2_fb_info(int table_index)
{
	struct vfb2_device *dev;
	struct fb_info *ret = NULL;

	down_read(&vfb2_table_sem);
	dev = vfb2_index_to_dev(table_index);
	if (!dev)
		goto error;
	ret = dev->info;
error:
	up_read(&vfb2_table_sem);
	return ret;
}

void *vfb2_private(int table_index)
{
	struct vfb2_device *dev;
	void *ret = NULL;

	down_read(&vfb2_table_sem);
	dev = vfb2_index_to_dev(table_index);
	if (!dev)
		goto error;
	ret = dev->init.private;
error:
	up_read(&vfb2_table_sem);
	return ret;
}

MODULE_LICENSE ("GPL");

EXPORT_SYMBOL(vfb2_register);
EXPORT_SYMBOL(vfb2_unregister);
EXPORT_SYMBOL(vfb2_current_mode);
EXPORT_SYMBOL(vfb2_videomemory);
EXPORT_SYMBOL(vfb2_fb_info);
EXPORT_SYMBOL(vfb2_private);
