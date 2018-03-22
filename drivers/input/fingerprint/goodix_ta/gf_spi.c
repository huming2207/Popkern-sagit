/*
 * TEE driver for goodix fingerprint sensor
 * Copyright (C) 2016 Goodix
 * Copyright (C) 2017 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/notifier.h>
#include <linux/fb.h>
#include <linux/wakelock.h>

#include "gf_spi.h"

#include <linux/platform_device.h>

#define	CHRD_DRIVER_NAME	"goodix_fp_spi"
#define	GF_INPUT_NAME		"uinput-goodix"
#define GF_DEV_NAME		"goodix_fp"
#define GF_SPIDEV_NAME		"goodix,fingerprint"

#define WAKELOCK_HOLD_TIME	500

#define N_SPI_MINORS		32	/* ... up to 256 */
static int SPIDEV_MAJOR;

static DECLARE_BITMAP(minors, N_SPI_MINORS);
static DEFINE_MUTEX(device_list_lock);
static LIST_HEAD(device_list);

static struct wake_lock fp_wakelock;
static struct gf_dev gf;

static unsigned int report_home_events = 1;
module_param(report_home_events, uint, S_IRUGO | S_IWUSR);

static void gf_hw_reset(struct gf_dev *gf_dev, unsigned int delay_ms)
{
	gpio_set_value(gf_dev->reset_gpio, 0);
	msleep(delay_ms);
	gpio_set_value(gf_dev->reset_gpio, 1);
	msleep(delay_ms);
}

static void gf_set_irq(struct gf_dev *gf_dev, bool state)
{
	if (state == gf_dev->irq_enabled) {
		pr_err("%s: IRQ already set to %d\n", __func__, state);
		return;
	}

	if (state)
		enable_irq(gf_dev->irq);
	else
		disable_irq(gf_dev->irq);

	gf_dev->irq_enabled = state;
}

static void gf_kernel_key_input(struct gf_dev *gf_dev, struct gf_key *gf_key)
{
	pr_debug("%s: received key, key=%d, value=%d\n",
			__func__, gf_key->key, gf_key->value);

	switch (gf_key->key) {
	case GF_KEY_HOME:
		if (!report_home_events)
			return;

		input_report_key(gf_dev->input, GF_KEY_INPUT_HOME, gf_key->value);
		input_sync(gf_dev->input);
		break;
	}
}

static long gf_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct gf_dev *gf_dev = &gf;
	struct gf_key gf_key;
	int retval = 0;
	u8 netlink_route = NETLINK_TEST;

	switch (cmd) {
	case GF_IOC_INIT:
		pr_debug("%s: GF_IOC_INIT\n", __func__);
		if (copy_to_user((void __user *)arg, (void *)&netlink_route, sizeof(u8))) {
			retval = -EFAULT;
			break;
		}
		break;
	case GF_IOC_DISABLE_IRQ:
		pr_debug("%s: GF_IOC_DISABEL_IRQ\n", __func__);
		gf_set_irq(gf_dev, 0);
		break;
	case GF_IOC_ENABLE_IRQ:
		pr_debug("%s: GF_IOC_ENABLE_IRQ\n", __func__);
		gf_set_irq(gf_dev, 1);
		break;
	case GF_IOC_RESET:
		pr_debug("%s: GF_IOC_RESET.\n", __func__);
		gf_hw_reset(gf_dev, 3);
		break;
	case GF_IOC_INPUT_KEY_EVENT:
		if (copy_from_user(&gf_key, (struct gf_key *)arg, sizeof(struct gf_key))) {
			pr_err("%s: failed to copy input key event\n");
			retval = -EFAULT;
			break;
		}

		gf_kernel_key_input(gf_dev, &gf_key);
		break;
	default:
		pr_debug("%s: unsupport cmd:0x%x\n", cmd);
	}

	return retval;
}

#ifdef CONFIG_COMPAT
static long gf_compat_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	return gf_ioctl(filp, cmd, (unsigned long)compat_ptr(arg));
}
#endif /*CONFIG_COMPAT*/

static irqreturn_t gf_irq(int irq, void *handle)
{
	char temp[4] = { 0x0 };
	temp[0] = GF_NET_EVENT_IRQ;
	wake_lock_timeout(&fp_wakelock, msecs_to_jiffies(WAKELOCK_HOLD_TIME));
	sendnlmsg(temp);

	return IRQ_HANDLED;
}

static int gf_open(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev;

	if (++gf_dev->users == 1) {
		gf_set_irq(gf_dev, 1);
		gf_hw_reset(gf_dev, 3);
	}

	filp->private_data = gf_dev;
	nonseekable_open(inode, filp);

	return 0;
}

static int gf_release(struct inode *inode, struct file *filp)
{
	struct gf_dev *gf_dev;

	gf_dev = filp->private_data;
	filp->private_data = NULL;

	if (--gf_dev->users == 0)
		gf_set_irq(gf_dev, 0);

	return 0;
}

static const struct file_operations gf_fops = {
	.owner = THIS_MODULE,
	/* REVISIT switch to aio primitives, so that userspace
	 * gets more complete API coverage.  It'll simplify things
	 * too, except for the locking.
	 */
	.unlocked_ioctl = gf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = gf_compat_ioctl,
#endif /*CONFIG_COMPAT*/
	.open = gf_open,
	.release = gf_release,
};

static void gf_fb_state_worker(struct work_struct *work)
{
	struct gf_dev *gf_dev = container_of(work, typeof(*gf_dev), fb_state_work);
	char temp[4] = {0x0};

	temp[0] = gf_dev->fb_state;
	sendnlmsg(temp);
}

static int gf_fb_state_callback(struct notifier_block *nb,
		unsigned long type, void *data)
{
	struct fb_event *evdata = data;
	struct gf_dev *gf_dev;
	unsigned int blank;

	if (type != FB_EVENT_BLANK)
		goto end;

	if (!evdata || !evdata->data)
		goto end;

	pr_debug("%s: type=%d\n", __func__, (int)type);

	gf_dev = container_of(nb, struct gf_dev, notifier);

	blank = *(int *)(evdata->data);
	switch (blank) {
	case FB_BLANK_POWERDOWN:
		gf_dev->fb_state = GF_NET_EVENT_FB_BLACK;
		schedule_work(&gf_dev->fb_state_work);
		break;
	case FB_BLANK_UNBLANK:
		gf_dev->fb_state = GF_NET_EVENT_FB_UNBLACK;
		schedule_work(&gf_dev->fb_state_work);
		break;
	}

end:
	return NOTIFY_OK;
}

static struct notifier_block goodix_notifier = {
	.notifier_call = gf_fb_state_callback,
};

static struct class *gf_class;
static int gf_probe(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;
	unsigned long minor;
	int rc = 0;

	gf_dev->spi = pdev;
	gf_dev->irq_enabled = false;

	gf_dev->reset_gpio = of_get_named_gpio(gf_dev->spi->dev.of_node,
			"fp-gpio-reset", 0);
	if (!gpio_is_valid(gf_dev->reset_gpio)) {
		pr_err("%s: failed to get reset_gpio, rc = %d\n", __func__, rc);
		rc = -EINVAL;
		goto error_end;
	}

	gf_dev->irq_gpio = of_get_named_gpio(gf_dev->spi->dev.of_node,
			"fp-gpio-irq", 0);
	if (!gpio_is_valid(gf_dev->irq_gpio)) {
		pr_err("%s: failed to get irq_gpio, rc = %d\n", __func__, rc);
		rc = -EINVAL;
		goto error_end;
	}

	rc = gpio_request(gf_dev->reset_gpio, "goodix_reset");
	if (rc) {
		pr_err("%s: failed to request reset_gpio, rc = %d\n", __func__, rc);
		goto error_gpio;
	}
	gpio_direction_output(gf_dev->reset_gpio, 1);

	rc = gpio_request(gf_dev->irq_gpio, "goodix_irq");
	if (rc) {
		pr_err("%s: failed to request irq_gpio, rc = %d\n", __func__, rc);
		goto error_gpio;
	}
	gpio_direction_input(gf_dev->irq_gpio);

	gf_dev->irq = gpio_to_irq(gf_dev->irq_gpio);

	rc = request_threaded_irq(gf_dev->irq, NULL, gf_irq,
			IRQF_TRIGGER_RISING | IRQF_ONESHOT,
			"gf", gf_dev);
	if (rc) {
		pr_err("%s: failed to request threaded irq, rc = %d\n", __func__, rc);
		goto error_gpio;
	}

	enable_irq_wake(gf_dev->irq);

	/*
	 * If we can allocate a minor number, hook up this device.
	 * Reusing minors is fine so long as udev or mdev is working.
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SPI_MINORS);
	if (minor < N_SPI_MINORS) {
		struct device *dev;

		gf_dev->devt = MKDEV(SPIDEV_MAJOR, minor);
		dev = device_create(gf_class, &gf_dev->spi->dev, gf_dev->devt,
				gf_dev, GF_DEV_NAME);
		rc = IS_ERR(dev) ? PTR_ERR(dev) : 0;
	} else {
		dev_dbg(&gf_dev->spi->dev, "no minor number available\n");
		rc = -ENODEV;
	}

	if (rc) {
		goto error_dev_lock;
	}

	set_bit(minor, minors);
	INIT_LIST_HEAD(&gf_dev->device_entry);
	list_add(&gf_dev->device_entry, &device_list);
	mutex_unlock(&device_list_lock);

	gf_dev->input = input_allocate_device();
	if (!gf_dev->input) {
		pr_err("%s: failed to allocate input device\n", __func__);
		rc = -ENOMEM;
		goto error_dev;
	}

	input_set_capability(gf_dev->input, EV_KEY, GF_KEY_INPUT_HOME);

	gf_dev->input->name = GF_INPUT_NAME;
	rc = input_register_device(gf_dev->input);
	if (rc) {
		pr_err("%s: failed to register input device\n", __func__);
		goto error_input_alloc;
	}

	gf_dev->notifier = goodix_notifier;
	fb_register_client(&gf_dev->notifier);
	INIT_WORK(&gf_dev->fb_state_work, gf_fb_state_worker);

	wake_lock_init(&fp_wakelock, WAKE_LOCK_SUSPEND, "fp_wakelock");

	return 0;

error_input_alloc:
	input_free_device(gf_dev->input);
error_dev:
	mutex_lock(&device_list_lock);
	list_del(&gf_dev->device_entry);
	device_destroy(gf_class, gf_dev->devt);
	clear_bit(MINOR(gf_dev->devt), minors);
error_dev_lock:
	mutex_unlock(&device_list_lock);
error_gpio:
	gpio_free(gf_dev->irq_gpio);
	gpio_free(gf_dev->reset_gpio);
error_end:
	return rc;
}

static int gf_remove(struct platform_device *pdev)
{
	struct gf_dev *gf_dev = &gf;

	wake_lock_destroy(&fp_wakelock);

	free_irq(gf_dev->irq, gf_dev);

	input_unregister_device(gf_dev->input);

	mutex_lock(&device_list_lock);
	list_del(&gf_dev->device_entry);
	device_destroy(gf_class, gf_dev->devt);
	clear_bit(MINOR(gf_dev->devt), minors);
	mutex_unlock(&device_list_lock);

	fb_unregister_client(&gf_dev->notifier);

	gpio_free(gf_dev->irq_gpio);
	gpio_free(gf_dev->reset_gpio);

	return 0;
}

static struct of_device_id gx_match_table[] = {
	{ .compatible = GF_SPIDEV_NAME },
	{},
};

static struct platform_driver gf_driver = {
	.driver = {
		.name = GF_DEV_NAME,
		.owner = THIS_MODULE,
		.of_match_table = gx_match_table,
	},
	.probe = gf_probe,
	.remove = gf_remove,
};

static int __init gf_init(void)
{
	int rc;

	/* Claim our 256 reserved device numbers.  Then register a class
	 * that will key udev/mdev to add/remove /dev nodes.  Last, register
	 * the driver which manages those device numbers.
	 */

	BUILD_BUG_ON(N_SPI_MINORS > 256);
	rc = register_chrdev(SPIDEV_MAJOR, CHRD_DRIVER_NAME, &gf_fops);
	if (rc < 0) {
		pr_err("%s: failed to register char device\n", __func__);
		goto error_end;
	}
	SPIDEV_MAJOR = rc;

	gf_class = class_create(THIS_MODULE, GF_DEV_NAME);
	if (IS_ERR(gf_class)) {
		pr_err("%s: failed to create device class\n", __func__);
		rc = PTR_ERR(gf_class);
		goto error_chardev;
	}

	rc = platform_driver_register(&gf_driver);
	if (rc < 0) {
		pr_err("%s: failed to register spi driver\n", __func__);
		goto error_class;

	}

	netlink_init();

	return 0;

error_class:
	class_destroy(gf_class);
error_chardev:
	unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
error_end:
	return rc;
}
module_init(gf_init);

static void __exit gf_exit(void)
{
	netlink_exit();
	platform_driver_unregister(&gf_driver);
	class_destroy(gf_class);
	unregister_chrdev(SPIDEV_MAJOR, gf_driver.driver.name);
}
module_exit(gf_exit);

MODULE_AUTHOR("Jiangtao Yi, <yijiangtao@goodix.com>");
MODULE_AUTHOR("Jandy Gou, <gouqingsong@goodix.com>");
MODULE_DESCRIPTION("goodix fingerprint sensor device driver");
MODULE_LICENSE("GPL");
