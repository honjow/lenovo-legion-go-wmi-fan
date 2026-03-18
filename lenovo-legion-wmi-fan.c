// SPDX-License-Identifier: GPL-2.0
/*
 * lenovo-legion-wmi-fan.c - Fan curve control for Lenovo Legion Go series
 *
 * Uses the GameZone WMI GUID (887B54E3-DDDC-4B2C-8B88-68A26A8835D0) via the
 * global wmi_evaluate_method() API.  This avoids binding to the WMI device
 * instance (which is already bound to lenovo_wmi_gamezone) and registers as
 * a platform driver instead.
 *
 * The mainline lenovo_wmi_gamezone driver uses method IDs 43/44/45 for
 * platform_profile switching.  This driver uses WMAB method IDs 5/6 and
 * WMAE method 0x12 — a completely disjoint set — so there is no conflict.
 *
 * Full-speed mode is toggled via the WMAE ACPI method at \_SB.GZFD.WMAE
 * (method 0x12, feature_id 0x04020000) which is a separate method from WMAB.
 *
 * Temperature set-points are fixed by firmware at 10,20,...,100 °C and cannot
 * be changed.  Fan RPM cannot be read on this hardware; only the curve
 * percentage values are accessible.
 *
 * Exposes:
 *   hwmon  – pwm1_enable, pwm1_auto_point{1-10}_{pwm,temp}
 *   sysfs  – fan_fullspeed
 *
 * Copyright (C) 2026 honjow
 */

#define pr_fmt(fmt) "legion_wmi_fan: " fmt

#include <linux/acpi.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

#define LENOVO_GAMEZONE_GUID	"887B54E3-DDDC-4B2C-8B88-68A26A8835D0"

/* WMI method IDs for WMAB (via GameZone GUID) */
#define FAN_METHOD_GET_CURVE		0x05	/* GetFanTableData  */
#define FAN_METHOD_SET_CURVE		0x06	/* SetFanTableData  */

/* WMAE ACPI path and method for full-speed feature toggle */
#define WMAE_ACPI_PATH		"\\_SB.GZFD.WMAE"
#define WMAE_METHOD_SET_FEATURE	0x12
#define FULLSPEED_FEATURE_ID	0x04020000U	/* SetFeatureStatus id */

#define FAN_CURVE_POINTS	10	/* fixed by firmware protocol */

/* pwm1_enable values (standard hwmon convention) */
#define PWM_ENABLE_FULLSPEED	0
#define PWM_ENABLE_MANUAL	1
#define PWM_ENABLE_AUTO		2

/* -------------------------------------------------------------------------
 * Driver private data
 * ------------------------------------------------------------------------- */

struct legion_fan_curve {
	u16 speeds[FAN_CURVE_POINTS];	/* 0–100 % */
};

/* Fixed temperature set-points imposed by firmware (°C) */
static const u16 legion_fixed_temps[FAN_CURVE_POINTS] = {
	10, 20, 30, 40, 50, 60, 70, 80, 90, 100
};

struct legion_wmi_fan {
	struct device		*hwmon_dev;
	struct mutex		 lock;		/* protects curve + pwm_enable */
	struct legion_fan_curve	 curve;
	u8			 pwm_enable;	/* PWM_ENABLE_* */
};

/* -------------------------------------------------------------------------
 * Standalone WMI evaluate helpers (uses global GUID-based API)
 * ------------------------------------------------------------------------- */

/**
 * legion_wmi_evaluate() - call a WMI method and optionally return the object
 * @method_id:	method identifier passed to the firmware
 * @in_buf:	pointer to input buffer (may be NULL if in_len == 0)
 * @in_len:	length of input buffer in bytes
 * @out:	if non-NULL, receives the allocated acpi_object (caller frees);
 *		if NULL the result is freed here
 *
 * Returns 0 on success, negative errno on failure.
 */
static int legion_wmi_evaluate(u8 method_id,
			       const void *in_buf, size_t in_len,
			       union acpi_object **out)
{
	struct acpi_buffer input  = { (acpi_size)in_len, (void *)in_buf };
	struct acpi_buffer result = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status;

	status = wmi_evaluate_method(LENOVO_GAMEZONE_GUID, 0, method_id,
				     &input, &result);
	if (ACPI_FAILURE(status))
		return -EIO;

	if (out)
		*out = result.pointer;
	else
		kfree(result.pointer);

	return 0;
}

/* -------------------------------------------------------------------------
 * Fan curve read / write
 * ------------------------------------------------------------------------- */

static int legion_get_fan_curve(struct legion_wmi_fan *lf)
{
	/*
	 * GetFanTableData (method 0x05) response format (44 bytes):
	 *   [0-3]   count = 10 (u32 LE)
	 *   [4-43]  speeds[0..9] (u32 LE each, value 0-100 %)
	 *
	 * Input: 4 zero bytes.  No fan_id/sensor_id header in either direction.
	 */
	u8 in[4] = { 0 };
	union acpi_object *obj = NULL;
	u32 count;
	u8 *buf;
	int ret, i;

	ret = legion_wmi_evaluate(FAN_METHOD_GET_CURVE,
				  in, sizeof(in), &obj);
	if (ret)
		return ret;
	if (!obj)
		return -ENODATA;

	if (obj->type != ACPI_TYPE_BUFFER) {
		ret = -ENXIO;
		goto out;
	}

	buf = obj->buffer.pointer;

	/* Need at least the count field (4 bytes) + one speed entry (4 bytes) */
	if (obj->buffer.length < sizeof(u32) + sizeof(u32)) {
		ret = -ENODATA;
		goto out;
	}

	count = get_unaligned_le32(buf);
	if (count == 0 || count > FAN_CURVE_POINTS) {
		ret = -ERANGE;
		goto out;
	}

	/* Validate buffer contains all speed data */
	if (obj->buffer.length < sizeof(u32) + count * sizeof(u32)) {
		ret = -ENODATA;
		goto out;
	}

	for (i = 0; i < FAN_CURVE_POINTS; i++) {
		if (i < (int)count)
			lf->curve.speeds[i] = (u16)get_unaligned_le32(
				buf + sizeof(u32) + i * sizeof(u32));
		else
			lf->curve.speeds[i] = lf->curve.speeds[count - 1];
	}
out:
	kfree(obj);
	return ret;
}

static int legion_set_fan_curve(struct legion_wmi_fan *lf)
{
	/*
	 * SetFanTableData (method 0x06) input buffer (52 bytes total).
	 * Layout matches the hhd firmware protocol exactly:
	 *
	 *   Offset  Size  Content
	 *   0       2     0x00 0x00  header/padding
	 *   2       4     speed_count = 10 (u32 LE)
	 *   6       20    speeds[0..9] as u16 LE (2 bytes each)
	 *   26      1     0x00       padding
	 *   27      4     temp_count = 10 (u32 LE)
	 *   31      20    fixed temps 10..100 °C as u16 LE (2 bytes each)
	 *   51      1     0x00       trailing byte
	 *   Total: 52 bytes
	 */
	u8 buf[52] = { 0 };
	int i;

	/* speed_count at offset 2 */
	put_unaligned_le32(FAN_CURVE_POINTS, buf + 2);

	/* speeds[0..9] at offset 6, each u16 LE */
	for (i = 0; i < FAN_CURVE_POINTS; i++)
		put_unaligned_le16(lf->curve.speeds[i], buf + 6 + i * sizeof(u16));

	/* temp_count at offset 27 */
	put_unaligned_le32(FAN_CURVE_POINTS, buf + 27);

	/* fixed temperatures at offset 31, each u16 LE */
	for (i = 0; i < FAN_CURVE_POINTS; i++)
		put_unaligned_le16(legion_fixed_temps[i], buf + 31 + i * sizeof(u16));

	return legion_wmi_evaluate(FAN_METHOD_SET_CURVE,
				   buf, sizeof(buf), NULL);
}

/* -------------------------------------------------------------------------
 * Full-speed mode via WMAE (\_SB.GZFD.WMAE method 0x12)
 *
 * From the hhd reference implementation (confirmed working on hardware):
 *   set_feature(0x04020000, 1)  → full speed on
 *   set_feature(0x04020000, 0)  → full speed off
 *
 * Input to WMAE method 0x12: 8 bytes = feature_id(u32 LE) + value(u32 LE)
 *
 * WMAE is a different ACPI method from WMAB (which handles the fan curve).
 * We call it directly via acpi_evaluate_object() to avoid needing its WMI
 * GUID, since the path \_SB.GZFD.WMAE is stable across firmware versions.
 * ------------------------------------------------------------------------- */

static int legion_wmae_set_feature(u32 feature_id, u32 value)
{
	struct acpi_object_list input;
	union acpi_object params[3];
	acpi_handle handle;
	acpi_status status;
	u8 buf[8];

	status = acpi_get_handle(NULL, WMAE_ACPI_PATH, &handle);
	if (ACPI_FAILURE(status))
		return -ENODEV;

	put_unaligned_le32(feature_id, buf);
	put_unaligned_le32(value,      buf + sizeof(u32));

	params[0].type          = ACPI_TYPE_INTEGER;
	params[0].integer.value = 0;			/* instance */
	params[1].type          = ACPI_TYPE_INTEGER;
	params[1].integer.value = WMAE_METHOD_SET_FEATURE;
	params[2].type             = ACPI_TYPE_BUFFER;
	params[2].buffer.length    = sizeof(buf);
	params[2].buffer.pointer   = buf;

	input.count   = 3;
	input.pointer = params;

	status = acpi_evaluate_object(handle, NULL, &input, NULL);
	return ACPI_FAILURE(status) ? -EIO : 0;
}

static int legion_set_fullspeed(struct legion_wmi_fan *lf, bool enable)
{
	return legion_wmae_set_feature(FULLSPEED_FEATURE_ID, enable ? 1 : 0);
}

/* -------------------------------------------------------------------------
 * hwmon ops (pwm1_enable)
 * ------------------------------------------------------------------------- */

static umode_t legion_hwmon_is_visible(const void *data,
				       enum hwmon_sensor_types type,
				       u32 attr, int channel)
{
	if (type == hwmon_pwm && attr == hwmon_pwm_enable)
		return 0644;
	return 0;
}

static int legion_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);

	if (type != hwmon_pwm || attr != hwmon_pwm_enable)
		return -EOPNOTSUPP;

	mutex_lock(&lf->lock);
	*val = lf->pwm_enable;
	mutex_unlock(&lf->lock);
	return 0;
}

static int legion_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long val)
{
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);
	int ret = 0;

	if (type != hwmon_pwm || attr != hwmon_pwm_enable)
		return -EOPNOTSUPP;
	if (val < PWM_ENABLE_FULLSPEED || val > PWM_ENABLE_AUTO)
		return -EINVAL;

	mutex_lock(&lf->lock);
	switch (val) {
	case PWM_ENABLE_FULLSPEED:
		ret = legion_set_fullspeed(lf, true);
		if (!ret)
			lf->pwm_enable = PWM_ENABLE_FULLSPEED;
		break;
	case PWM_ENABLE_MANUAL:
		ret = legion_set_fullspeed(lf, false);
		if (ret)
			break;
		ret = legion_set_fan_curve(lf);
		if (!ret)
			lf->pwm_enable = PWM_ENABLE_MANUAL;
		break;
	case PWM_ENABLE_AUTO:
		ret = legion_set_fullspeed(lf, false);
		if (!ret)
			lf->pwm_enable = PWM_ENABLE_AUTO;
		break;
	}
	mutex_unlock(&lf->lock);
	return ret;
}

static const struct hwmon_channel_info * const legion_hwmon_info[] = {
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_ENABLE),
	NULL,
};

static const struct hwmon_ops legion_hwmon_ops = {
	.is_visible = legion_hwmon_is_visible,
	.read       = legion_hwmon_read,
	.write      = legion_hwmon_write,
};

static const struct hwmon_chip_info legion_hwmon_chip = {
	.ops  = &legion_hwmon_ops,
	.info = legion_hwmon_info,
};

/* -------------------------------------------------------------------------
 * pwm1_auto_point{1..10}_{pwm,temp} sysfs attributes (via extra_groups)
 *
 * These live on the hwmon device; dev_get_drvdata(dev) returns lf.
 * Temperatures use the hwmon convention: millidegrees Celsius.
 * PWM values use the hwmon convention: 0–255 (mapped from 0–100 %).
 * ------------------------------------------------------------------------- */

static ssize_t pwm_auto_point_pwm_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	int idx = sda->index;
	long pwm;

	mutex_lock(&lf->lock);
	pwm = (long)lf->curve.speeds[idx] * 255 / 100;
	mutex_unlock(&lf->lock);

	return sysfs_emit(buf, "%ld\n", pwm);
}

static ssize_t pwm_auto_point_pwm_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	int idx = sda->index;
	long val;
	int ret;

	ret = kstrtol(buf, 10, &val);
	if (ret)
		return ret;
	if (val < 0 || val > 255)
		return -EINVAL;

	mutex_lock(&lf->lock);
	lf->curve.speeds[idx] = (u16)(val * 100 / 255);
	ret = 0;
	if (lf->pwm_enable == PWM_ENABLE_MANUAL)
		ret = legion_set_fan_curve(lf);
	mutex_unlock(&lf->lock);

	return ret ? ret : (ssize_t)count;
}

static ssize_t pwm_auto_point_temp_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	int idx = sda->index;

	/* Temperatures are fixed by firmware; no need to lock or access lf */
	return sysfs_emit(buf, "%ld\n", (long)legion_fixed_temps[idx] * 1000);
}

/* Expand one auto_point index (1-based for user, 0-based for array) */
#define LEGION_AUTO_POINT(_n)						\
static SENSOR_DEVICE_ATTR(pwm1_auto_point##_n##_pwm, 0644,		\
			  pwm_auto_point_pwm_show,			\
			  pwm_auto_point_pwm_store, (_n) - 1);		\
static SENSOR_DEVICE_ATTR(pwm1_auto_point##_n##_temp, 0444,		\
			  pwm_auto_point_temp_show,			\
			  NULL, (_n) - 1)

LEGION_AUTO_POINT(1);
LEGION_AUTO_POINT(2);
LEGION_AUTO_POINT(3);
LEGION_AUTO_POINT(4);
LEGION_AUTO_POINT(5);
LEGION_AUTO_POINT(6);
LEGION_AUTO_POINT(7);
LEGION_AUTO_POINT(8);
LEGION_AUTO_POINT(9);
LEGION_AUTO_POINT(10);

static struct attribute *legion_auto_point_attrs[] = {
	&sensor_dev_attr_pwm1_auto_point1_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point1_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point2_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point3_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point4_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point5_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point6_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point7_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point7_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point8_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point8_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point9_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point9_temp.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point10_pwm.dev_attr.attr,
	&sensor_dev_attr_pwm1_auto_point10_temp.dev_attr.attr,
	NULL,
};

static const struct attribute_group legion_auto_point_group = {
	.attrs = legion_auto_point_attrs,
};

static const struct attribute_group *legion_hwmon_extra_groups[] = {
	&legion_auto_point_group,
	NULL,
};

/* -------------------------------------------------------------------------
 * fan_fullspeed sysfs attribute on the WMI device node
 * ------------------------------------------------------------------------- */

static ssize_t fan_fullspeed_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n",
			  lf->pwm_enable == PWM_ENABLE_FULLSPEED ? 1 : 0);
}

static ssize_t fan_fullspeed_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	mutex_lock(&lf->lock);
	ret = legion_set_fullspeed(lf, enable);
	if (!ret)
		lf->pwm_enable = enable ? PWM_ENABLE_FULLSPEED : PWM_ENABLE_AUTO;
	mutex_unlock(&lf->lock);

	return ret ? ret : (ssize_t)count;
}

static DEVICE_ATTR_RW(fan_fullspeed);

static struct attribute *legion_wmi_fan_attrs[] = {
	&dev_attr_fan_fullspeed.attr,
	NULL,
};

static const struct attribute_group legion_wmi_fan_group = {
	.attrs = legion_wmi_fan_attrs,
};

/* -------------------------------------------------------------------------
 * DMI match table — restrict to Legion Go family
 * ------------------------------------------------------------------------- */

static const struct dmi_system_id legion_go_dmi_table[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8APU1"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go S 8APU1"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go S 8ARP1"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8ASP2"),
		},
	},
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8AHP2"),
		},
	},
	{ }
};

/* -------------------------------------------------------------------------
 * Platform driver probe / remove
 * ------------------------------------------------------------------------- */

static int legion_wmi_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct legion_wmi_fan *lf;
	int ret;

	lf = devm_kzalloc(dev, sizeof(*lf), GFP_KERNEL);
	if (!lf)
		return -ENOMEM;

	lf->pwm_enable = PWM_ENABLE_AUTO;
	mutex_init(&lf->lock);
	dev_set_drvdata(dev, lf);

	/*
	 * Validate firmware support by reading the current fan curve.
	 * GetFanCount (0x23) always returns 0 on Legion Go hardware, so we
	 * use GetFanTableData (0x05) as the probe check instead.
	 */
	ret = legion_get_fan_curve(lf);
	if (ret) {
		dev_warn(dev, "Failed to read fan curve from firmware (%d)\n",
			 ret);
		return ret;
	}

	/* Register the hwmon device */
	lf->hwmon_dev = devm_hwmon_device_register_with_info(
		dev, "legion_wmi_fan", lf,
		&legion_hwmon_chip, legion_hwmon_extra_groups);
	if (IS_ERR(lf->hwmon_dev)) {
		ret = PTR_ERR(lf->hwmon_dev);
		dev_warn(dev, "hwmon registration failed (%d)\n", ret);
		return ret;
	}

	/* fan_fullspeed sysfs attribute on the platform device itself */
	ret = devm_device_add_group(dev, &legion_wmi_fan_group);
	if (ret) {
		dev_warn(dev, "sysfs group creation failed (%d)\n", ret);
		return ret;
	}

	dev_info(dev, "Lenovo Legion Go WMI fan driver loaded\n");
	return 0;
}

static void legion_wmi_fan_remove(struct platform_device *pdev)
{
	dev_info(&pdev->dev, "Lenovo Legion Go WMI fan driver unloaded\n");
}

/* -------------------------------------------------------------------------
 * Module infrastructure
 * ------------------------------------------------------------------------- */

static struct platform_driver legion_wmi_fan_driver = {
	.driver = {
		.name = "legion-wmi-fan",
	},
	.probe  = legion_wmi_fan_probe,
	.remove = legion_wmi_fan_remove,
};

static struct platform_device *legion_wmi_fan_pdev;

static int __init legion_wmi_fan_init(void)
{
	int ret;

	if (!dmi_check_system(legion_go_dmi_table)) {
		pr_info("not a supported Legion Go device\n");
		return -ENODEV;
	}

	if (!wmi_has_guid(LENOVO_GAMEZONE_GUID)) {
		pr_info("GameZone WMI GUID not found\n");
		return -ENODEV;
	}

	legion_wmi_fan_pdev = platform_device_register_simple(
		"legion-wmi-fan", PLATFORM_DEVID_NONE, NULL, 0);
	if (IS_ERR(legion_wmi_fan_pdev)) {
		ret = PTR_ERR(legion_wmi_fan_pdev);
		pr_warn("platform device creation failed (%d)\n", ret);
		return ret;
	}

	ret = platform_driver_register(&legion_wmi_fan_driver);
	if (ret) {
		pr_warn("platform driver registration failed (%d)\n", ret);
		platform_device_unregister(legion_wmi_fan_pdev);
		return ret;
	}

	return 0;
}

static void __exit legion_wmi_fan_exit(void)
{
	platform_driver_unregister(&legion_wmi_fan_driver);
	platform_device_unregister(legion_wmi_fan_pdev);
}

module_init(legion_wmi_fan_init);
module_exit(legion_wmi_fan_exit);

MODULE_AUTHOR("honjow");
MODULE_DESCRIPTION("Lenovo Legion Go WMI fan curve driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");
