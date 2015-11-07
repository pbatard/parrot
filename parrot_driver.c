/*
 * Linux 2.6 and 3.0 'parrot' sample device driver
 *
 * Copyright (c) 2011, Pete Batard <pete@akeo.ie>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/kfifo.h>
#include "parrot_driver.h"

/* Module information */
MODULE_AUTHOR(AUTHOR);
MODULE_DESCRIPTION(DESCRIPTION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");

/* Device variables */
static struct class* parrot_class = NULL;
static struct device* parrot_device = NULL;
static int parrot_major;
/* Flag used with the one_shot mode */
static bool message_read;
/* A mutex will ensure that only one process accesses our device */
static DEFINE_MUTEX(parrot_device_mutex);
/* Use a Kernel FIFO for read operations */
static DECLARE_KFIFO(parrot_msg_fifo, char, PARROT_MSG_FIFO_SIZE);
/* This table keeps track of each message length in the FIFO */
static unsigned int parrot_msg_len[PARROT_MSG_FIFO_MAX];
/* Read and write index for the table above */
static int parrot_msg_idx_rd, parrot_msg_idx_wr;

/* Module parameters that can be provided on insmod */
static bool debug = false;	/* print extra debug info */
module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "enable debug info (default: false)");
static bool one_shot = true;	/* only read a single message after open() */
module_param(one_shot, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "disable the readout of multiple messages at once (default: true)");


static int parrot_device_open(struct inode* inode, struct file* filp)
{
	dbg("");

	/* Our sample device does not allow write access */
	if ( ((filp->f_flags & O_ACCMODE) == O_WRONLY)
	  || ((filp->f_flags & O_ACCMODE) == O_RDWR) ) {
		warn("write access is prohibited\n");
		return -EACCES;
	}

	/* Ensure that only one process has access to our device at any one time
 	* For more info on concurrent accesses, see http://lwn.net/images/pdf/LDD3/ch05.pdf */
	if (!mutex_trylock(&parrot_device_mutex)) {
		warn("another process is accessing the device\n");
		return -EBUSY;
	}

	message_read = false;
	return 0;
}

static int parrot_device_close(struct inode* inode, struct file* filp)
{
	dbg("");
	mutex_unlock(&parrot_device_mutex);
	return 0;
}

static ssize_t parrot_device_read(struct file* filp, char __user *buffer, size_t length, loff_t* offset)
{
	int retval;
	unsigned int copied;

	/* The default from 'cat' is to issue multiple reads until the FIFO is depleted
	 * one_shot avoids that */
	if (one_shot && message_read) return 0;
	dbg("");

	if (kfifo_is_empty(&parrot_msg_fifo)) {
		dbg("no message in fifo\n");
		return 0;
	}

	retval = kfifo_to_user(&parrot_msg_fifo, buffer, parrot_msg_len[parrot_msg_idx_rd], &copied);
	/* Ignore short reads (but warn about them) */
	if (parrot_msg_len[parrot_msg_idx_rd] != copied) {
		warn("short read detected\n");
	}
	/* loop into the message length table */
	parrot_msg_idx_rd = (parrot_msg_idx_rd+1)%PARROT_MSG_FIFO_MAX;
	message_read = true;

	return retval ? retval : copied;
}

/* The file_operation scructure tells the kernel which device operations are handled.
 * For a list of available file operations, see http://lwn.net/images/pdf/LDD3/ch03.pdf */
static struct file_operations fops = {
	.read = parrot_device_read,
	.open = parrot_device_open,
	.release = parrot_device_close
};

/* Placing data into the read FIFO is done through sysfs */
static ssize_t sys_add_to_fifo(struct device* dev, struct device_attribute* attr, const char* buf, size_t count)
{
	unsigned int copied;

	dbg("");
	if (kfifo_avail(&parrot_msg_fifo) < count) {
		warn("not enough space left on fifo\n");
		return -ENOSPC;
	}
	if ((parrot_msg_idx_wr+1)%PARROT_MSG_FIFO_MAX == parrot_msg_idx_rd) {
		/* We've looped into our message length table */
		warn("message length table is full\n");
		return -ENOSPC;
	}

	/* The buffer is already in kernel space, so no need for ..._from_user() */
	copied = kfifo_in(&parrot_msg_fifo, buf, count);
	parrot_msg_len[parrot_msg_idx_wr] = copied;
	if (copied != count) {
		warn("short write detected\n");
	}
	parrot_msg_idx_wr = (parrot_msg_idx_wr+1)%PARROT_MSG_FIFO_MAX;

	return copied;
}

/* This sysfs entry resets the FIFO */
static ssize_t sys_reset(struct device* dev, struct device_attribute* attr, const char* buf, size_t count)
{
	dbg("");

	/* Ideally, we would have a mutex around the FIFO, to ensure that we don't reset while in use.
	 * To keep this sample simple, and because this is a sysfs operation, we don't do that */
	kfifo_reset(&parrot_msg_fifo);
	parrot_msg_idx_rd = parrot_msg_idx_wr = 0;

	return count;
}

/* Declare the sysfs entries. The macros create instances of dev_attr_fifo and dev_attr_reset */
static DEVICE_ATTR(fifo, S_IWUSR, NULL, sys_add_to_fifo);
static DEVICE_ATTR(reset, S_IWUSR, NULL, sys_reset);

/* Module initialization and release */
static int __init parrot_module_init(void)
{
	int retval;
	dbg("");

	/* First, see if we can dynamically allocate a major for our device */
	parrot_major = register_chrdev(0, DEVICE_NAME, &fops);
	if (parrot_major < 0) {
		err("failed to register device: error %d\n", parrot_major);
		retval = parrot_major;
		goto failed_chrdevreg;
	}

	/* We can either tie our device to a bus (existing, or one that we create)
	 * or use a "virtual" device class. For this example, we choose the latter */
	parrot_class = class_create(THIS_MODULE, CLASS_NAME);
	if (IS_ERR(parrot_class)) {
		err("failed to register device class '%s'\n", CLASS_NAME);
		retval = PTR_ERR(parrot_class);
		goto failed_classreg;
	}

	/* With a class, the easiest way to instantiate a device is to call device_create() */
	parrot_device = device_create(parrot_class, NULL, MKDEV(parrot_major, 0), NULL, CLASS_NAME "_" DEVICE_NAME);
	if (IS_ERR(parrot_device)) {
		err("failed to create device '%s_%s'\n", CLASS_NAME, DEVICE_NAME);
		retval = PTR_ERR(parrot_device);
		goto failed_devreg;
	}

	/* Now we can create the sysfs endpoints (don't care about errors).
	 * dev_attr_fifo and dev_attr_reset come from the DEVICE_ATTR(...) earlier */
	retval = device_create_file(parrot_device, &dev_attr_fifo);
	if (retval < 0) {
		warn("failed to create write /sys endpoint - continuing without\n");
	}
	retval = device_create_file(parrot_device, &dev_attr_reset);
	if (retval < 0) {
		warn("failed to create reset /sys endpoint - continuing without\n");
	}

	mutex_init(&parrot_device_mutex);
	/* This device uses a Kernel FIFO for its read operation */
	INIT_KFIFO(parrot_msg_fifo);
	parrot_msg_idx_rd = parrot_msg_idx_wr = 0;

	return 0;

failed_devreg:
	class_unregister(parrot_class);
	class_destroy(parrot_class);
failed_classreg:
	unregister_chrdev(parrot_major, DEVICE_NAME);
failed_chrdevreg:
	return -1;
}

static void __exit parrot_module_exit(void)
{
	dbg("");
	device_remove_file(parrot_device, &dev_attr_fifo);
	device_remove_file(parrot_device, &dev_attr_reset);
	device_destroy(parrot_class, MKDEV(parrot_major, 0));
	class_unregister(parrot_class);
	class_destroy(parrot_class);
	unregister_chrdev(parrot_major, DEVICE_NAME);
}

/* Let the kernel know the calls for module init and exit */
module_init(parrot_module_init);
module_exit(parrot_module_exit);
