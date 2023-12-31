// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <asm/arch_timer.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msm_dsps.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#define IMAGE_LOAD_CMD		1
#define IMAGE_UNLOAD_CMD	0
#define SSR_RESET_CMD		1
#define CLASS_NAME			"ssc"
#define DRV_NAME			"sensors"
#define DRV_VERSION			"2.00"

#ifdef CONFIG_COMPAT
#define DSPS_IOCTL_READ_SLOW_TIMER32	_IOR(DSPS_IOCTL_MAGIC, 3, compat_uint_t)
#endif

#define QTICK_DIV_FACTOR	0x249F

struct sns_ssc_control_s {
	struct class *dev_class;
	dev_t dev_num;
	struct device *dev;
	struct cdev *cdev;
	struct platform_device *slpi_private;
	struct work_struct slpi_ldr_work;
};
static struct sns_ssc_control_s sns_ctl;

struct slpi_loader_private {
	void *pil_h;
	struct kobject *boot_slpi_obj;
	struct attribute_group *attr_group;
};

static void slpi_load_fw(struct work_struct *slpi_ldr_work)
{
	struct platform_device *pdev = sns_ctl.slpi_private;
	struct slpi_loader_private *priv = NULL;
	int ret;
	const char *firmware_name = NULL;
	phandle rproc_phandle;

	if (!pdev) {
		pr_err("%s: Platform device null\n", __func__);
		goto fail;
	}

	if (!pdev->dev.of_node) {
		pr_err("%s: Device tree information missing\n", __func__);
		goto fail;
	}

	ret = of_property_read_string(pdev->dev.of_node,
		"qcom,firmware-name", &firmware_name);
	if (ret < 0) {
		dev_err(&pdev->dev, "%s:can't get fw name, err: %d\n", __func__, ret);
		goto fail;
	}

	dev_dbg(&pdev->dev, "%s: firmware name %s\n", __func__, firmware_name);
	priv = platform_get_drvdata(pdev);
	if (!priv) {
		dev_err(&pdev->dev,
		" %s: Private data get failed\n", __func__);
		goto fail;
	}

	if (!priv->pil_h) {
		if (of_property_read_u32(pdev->dev.of_node, "qcom,rproc-handle",
					 &rproc_phandle)) {
			dev_err(&pdev->dev, "error reading rproc phandle\n");
			goto fail;
		}

		priv->pil_h = rproc_get_by_phandle(rproc_phandle);
		if (!priv->pil_h) {
			dev_err(&pdev->dev, "rproc not found\n");
			goto fail;
		}
	}

	ret = rproc_boot(priv->pil_h);
	if (ret) {
		dev_err(&pdev->dev, "%s: rproc boot failed, err: %d\n",
			__func__, ret);
		goto fail;
	}

	dev_info(&pdev->dev, "%s: SLPI image is loaded\n", __func__);
	return;

fail:
	dev_err(&pdev->dev, "%s: SLPI image loading failed\n", __func__);
}

static void slpi_loader_do(struct platform_device *pdev)
{
	dev_dbg(&pdev->dev, "%s: scheduling work to load SLPI fw\n", __func__);
	schedule_work(&sns_ctl.slpi_ldr_work);
}

static void slpi_loader_unload(struct platform_device *pdev)
{
	struct slpi_loader_private *priv = NULL;

	priv = platform_get_drvdata(pdev);

	if (!priv)
		return;

	if (priv->pil_h) {
		dev_dbg(&pdev->dev, "%s: calling subsystem put\n", __func__);
		rproc_shutdown(priv->pil_h);
		priv->pil_h = NULL;
	}
}

static ssize_t slpi_ssr_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf,
				size_t count)
{
	int ssr_cmd = 0;
	struct rproc *sns_dev = NULL;
	struct platform_device *pdev = sns_ctl.slpi_private;
	struct slpi_loader_private *priv = NULL;

	dev_dbg(&pdev->dev, "%s: going to call slpi_ssr\n", __func__);

	if (kstrtoint(buf, 10, &ssr_cmd) < 0)
		return -EINVAL;

	if (ssr_cmd != SSR_RESET_CMD)
		return -EINVAL;

	priv = platform_get_drvdata(pdev);
	if (!priv)
		return -EINVAL;

	sns_dev = (struct rproc *)priv->pil_h;
	if (!sns_dev)
		return -EINVAL;

	dev_dbg(&pdev->dev, "requesting for SLPI restart\n");

	rproc_shutdown(sns_dev);
	slpi_loader_do(pdev);

	dev_dbg(&pdev->dev, "SLPI restarted\n");
	return count;
}

static ssize_t slpi_boot_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf,
				size_t count)
{
	int boot = 0;

	if (kstrtoint(buf, 10, &boot) < 0)
		return -EINVAL;

	if (boot == IMAGE_LOAD_CMD) {
		pr_debug("%s: going to call slpi_loader_do\n", __func__);
		slpi_loader_do(sns_ctl.slpi_private);
	} else if (boot == IMAGE_UNLOAD_CMD) {
		pr_debug("%s: going to call slpi_unloader\n", __func__);
		slpi_loader_unload(sns_ctl.slpi_private);
	}
	return count;
}

static struct kobj_attribute slpi_boot_attribute =
	__ATTR(boot, 0220, NULL, slpi_boot_store);

static struct kobj_attribute slpi_ssr_attribute =
	__ATTR(ssr, 0220, NULL, slpi_ssr_store);

static struct attribute *attrs[] = {
	&slpi_boot_attribute.attr,
	&slpi_ssr_attribute.attr,
	NULL,
};

static int slpi_loader_init_sysfs(struct platform_device *pdev)
{
	struct slpi_loader_private *priv = NULL;
	int ret = -EINVAL;

	sns_ctl.slpi_private = NULL;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, priv);

	priv->attr_group = devm_kzalloc(&pdev->dev,
				sizeof(*(priv->attr_group)),
				GFP_KERNEL);
	if (!priv->attr_group) {
		dev_err(&pdev->dev, "%s: malloc attr_group failed\n",
						__func__);
		return -ENOMEM;
	}

	priv->attr_group->attrs = attrs;

	priv->boot_slpi_obj = kobject_create_and_add("boot_slpi", kernel_kobj);
	if (!priv->boot_slpi_obj) {
		dev_err(&pdev->dev, "%s: sysfs create and add failed\n",
						__func__);
		return -ENOMEM;
	}

	ret = sysfs_create_group(priv->boot_slpi_obj, priv->attr_group);
	if (ret) {
		dev_err(&pdev->dev, "%s: sysfs create group failed %d\n",
							__func__, ret);
		goto error_return;
	}

	sns_ctl.slpi_private = pdev;

	return 0;

error_return:

	if (priv->boot_slpi_obj) {
		kobject_del(priv->boot_slpi_obj);
		priv->boot_slpi_obj = NULL;
	}

	return ret;
}

static int slpi_loader_remove(struct platform_device *pdev)
{
	struct slpi_loader_private *priv = NULL;

	priv = platform_get_drvdata(pdev);

	if (!priv)
		return 0;

	if (priv->pil_h) {
		rproc_shutdown(priv->pil_h);
		priv->pil_h = NULL;
	}

	if (priv->boot_slpi_obj) {
		sysfs_remove_group(priv->boot_slpi_obj, priv->attr_group);
		kobject_del(priv->boot_slpi_obj);
		priv->boot_slpi_obj = NULL;
	}

	return 0;
}

/*
 * Read virtual QTimer clock ticks and scale down to 32KHz clock as used
 * in DSPS
 */
static u32 sns_read_qtimer(void)
{
	u64 val;

	val = __arch_counter_get_cntvct();
	/*
	 * To convert ticks from 19.2 Mhz clock to 32768 Hz clock:
	 * x = (value * 32768) / 19200000
	 * This is same as first left shift the value by 4 bits, i.e. multiply
	 * by 16, and then divide by 0x249F. The latter is preferable since
	 * QTimer tick (value) is 56-bit, so (value * 32768) could overflow,
	 * while (value * 16) will never do
	 */
	val <<= 4;
	do_div(val, QTICK_DIV_FACTOR);

	return (u32)val;
}

static int sensors_ssc_open(struct inode *ip, struct file *fp)
{
	return 0;
}

static int sensors_ssc_release(struct inode *inode, struct file *file)
{
	return 0;
}

static long sensors_ssc_ioctl(struct file *file,
			unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	u32 val = 0;

	switch (cmd) {
	case DSPS_IOCTL_READ_SLOW_TIMER:
#ifdef CONFIG_COMPAT
	case DSPS_IOCTL_READ_SLOW_TIMER32:
#endif
		val = sns_read_qtimer();
		ret = put_user(val, (u32 __user *) arg);
		break;

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

const struct file_operations sensors_ssc_fops = {
	.owner = THIS_MODULE,
	.open = sensors_ssc_open,
	.release = sensors_ssc_release,
#ifdef CONFIG_COMPAT
	.compat_ioctl = sensors_ssc_ioctl,
#endif
	.unlocked_ioctl = sensors_ssc_ioctl
};

static int sensors_ssc_probe(struct platform_device *pdev)
{
	phandle rproc_phandle;
	struct property *prop;
	int size;
	struct rproc *slpi;
	int ret;
	struct slpi_loader_private *priv;

	prop = of_find_property(pdev->dev.of_node, "qcom,rproc-handle",
				&size);
	if (!prop) {
		dev_err(&pdev->dev, "Missing remotproc handle\n");
		return -ENOPARAM;
	}
	rproc_phandle = be32_to_cpup(prop->value);
	slpi = rproc_get_by_phandle(rproc_phandle);
	if (!slpi) {
		dev_err(&pdev->dev, "fail to get rproc\n", __func__);
		return -EPROBE_DEFER;
	}

	ret = slpi_loader_init_sysfs(pdev);

	if (ret) {
		dev_err(&pdev->dev, "%s: Error in initing sysfs, err:\n", __func__, ret);
		return ret;
	}

	sns_ctl.dev_class = class_create(THIS_MODULE, CLASS_NAME);
	if (sns_ctl.dev_class == NULL) {
		dev_err(&pdev->dev, "%s: class_create fail\n", __func__);
		goto class_create_err;
	}

	ret = alloc_chrdev_region(&sns_ctl.dev_num, 0, 1, DRV_NAME);
	if (ret) {
		dev_err(&pdev->dev, "%s: alloc_chrdev_region fail, err:%d\n", __func__, ret);
		goto alloc_chrdev_region_err;
	}

	sns_ctl.dev = device_create(sns_ctl.dev_class, NULL,
				     sns_ctl.dev_num,
				     &sns_ctl, DRV_NAME);
	if (IS_ERR(sns_ctl.dev)) {
		dev_err(&pdev->dev, "%s: device_create fail\n", __func__);
		goto device_create_err;
	}

	sns_ctl.cdev = cdev_alloc();
	if (sns_ctl.cdev == NULL) {
		dev_err(&pdev->dev, "%s: cdev_alloc fail\n", __func__);
		goto cdev_alloc_err;
	}
	cdev_init(sns_ctl.cdev, &sensors_ssc_fops);
	sns_ctl.cdev->owner = THIS_MODULE;

	ret = cdev_add(sns_ctl.cdev, sns_ctl.dev_num, 1);
	if (ret) {
		dev_err(&pdev->dev, "%s: cdev_add fail\n", __func__);
		goto cdev_add_err;
	}

	INIT_WORK(&sns_ctl.slpi_ldr_work, slpi_load_fw);

	return 0;

cdev_add_err:
	kfree(sns_ctl.cdev);
cdev_alloc_err:
	device_destroy(sns_ctl.dev_class, sns_ctl.dev_num);
device_create_err:
	unregister_chrdev_region(sns_ctl.dev_num, 1);
alloc_chrdev_region_err:
	class_destroy(sns_ctl.dev_class);
class_create_err:
	priv = platform_get_drvdata(pdev);
	sysfs_remove_group(priv->boot_slpi_obj, priv->attr_group);
	kobject_del(priv->boot_slpi_obj);
	return -ENODEV;
}

static int sensors_ssc_remove(struct platform_device *pdev)
{
	slpi_loader_remove(pdev);
	cdev_del(sns_ctl.cdev);
	kfree(sns_ctl.cdev);
	sns_ctl.cdev = NULL;
	device_destroy(sns_ctl.dev_class, sns_ctl.dev_num);
	unregister_chrdev_region(sns_ctl.dev_num, 1);
	class_destroy(sns_ctl.dev_class);

	return 0;
}

static const struct of_device_id msm_ssc_sensors_dt_match[] = {
	{.compatible = "qcom,msm-ssc-sensors"},
	{}
};
MODULE_DEVICE_TABLE(of, msm_ssc_sensors_dt_match);

static struct platform_driver sensors_ssc_driver = {
	.driver = {
		.name = "sensors-ssc",
		.of_match_table = msm_ssc_sensors_dt_match,
	},
	.probe = sensors_ssc_probe,
	.remove = sensors_ssc_remove,
};
module_platform_driver(sensors_ssc_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Sensors SSC driver");
