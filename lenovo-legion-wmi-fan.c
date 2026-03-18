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
 * platform_profile switching.  This driver uses IDs 5/6/16/18/35/36 — a
 * completely disjoint set — so there is no conflict.
 *
 * Exposes:
 *   hwmon  – fan1_input, pwm1_enable, pwm1_auto_point{1-10}_{pwm,temp}
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

/* WMI method IDs (decimal == what the ACPI table uses) */
#define FAN_METHOD_GET_CURVE		0x05	/* GetFanTableData   */
#define FAN_METHOD_SET_CURVE		0x06	/* SetFanTableData   */
#define FAN_METHOD_GET_SPEED		0x10	/* GetCurrentFanSpeed */
#define FAN_METHOD_SET_FEATURE		0x12	/* SetFeatureStatus  */
#define FAN_METHOD_GET_COUNT		0x23	/* GetFanCount       */
#define FAN_METHOD_GET_MAX_SPEED	0x24	/* GetFanMaxSpeed    */

#define FAN_CURVE_POINTS	10	/* fixed by firmware protocol */
#define FAN_DEFAULT_FAN_ID	0
#define FAN_DEFAULT_SENSOR_ID	0

/* GetFanTableData response buffer offsets */
#define FAN_BUF_FAN_ID_OFF	0	/* u16 LE */
#define FAN_BUF_SENSOR_ID_OFF	2	/* u16 LE */
#define FAN_BUF_SPEED_CNT_OFF	4	/* u32 LE: number of speed entries */
#define FAN_BUF_SPEED_DATA_OFF	8	/* u16 LE * speed_count */
/* temp_count u32 LE follows immediately after speed data */

/* pwm1_enable values (standard hwmon convention) */
#define PWM_ENABLE_FULLSPEED	0
#define PWM_ENABLE_MANUAL	1
#define PWM_ENABLE_AUTO		2

/* -------------------------------------------------------------------------
 * Driver private data
 * ------------------------------------------------------------------------- */

struct legion_fan_curve {
	u16 temps[FAN_CURVE_POINTS];	/* °C */
	u16 speeds[FAN_CURVE_POINTS];	/* 0–100 % */
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

/**
 * legion_wmi_evaluate_int() - call a WMI method and return an integer result
 *
 * Handles both ACPI_TYPE_INTEGER and short ACPI_TYPE_BUFFER responses
 * (some firmware versions return a 4-byte buffer instead of an integer).
 */
static int legion_wmi_evaluate_int(u8 method_id,
				   const void *in_buf, size_t in_len,
				   u64 *out_val)
{
	union acpi_object *obj = NULL;
	int ret;

	ret = legion_wmi_evaluate(method_id, in_buf, in_len, &obj);
	if (ret)
		return ret;
	if (!obj)
		return -ENODATA;

	if (obj->type == ACPI_TYPE_INTEGER) {
		if (out_val)
			*out_val = obj->integer.value;
	} else if (obj->type == ACPI_TYPE_BUFFER &&
		   obj->buffer.length >= sizeof(u32)) {
		if (out_val)
			*out_val = get_unaligned_le32(obj->buffer.pointer);
	} else {
		kfree(obj);
		return -ENXIO;
	}

	kfree(obj);
	return 0;
}

/* -------------------------------------------------------------------------
 * Fan curve read / write
 * ------------------------------------------------------------------------- */

static int legion_get_fan_curve(struct legion_wmi_fan *lf)
{
	u8 in[4] = { FAN_DEFAULT_FAN_ID, 0, FAN_DEFAULT_SENSOR_ID, 0 };
	union acpi_object *obj = NULL;
	size_t temp_cnt_off, expected;
	u32 speed_cnt, temp_cnt, cnt;
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

	/* Need at least header (8 bytes) + one speed (2 bytes) */
	if (obj->buffer.length < FAN_BUF_SPEED_DATA_OFF + sizeof(u16)) {
		ret = -ENODATA;
		goto out;
	}

	speed_cnt = get_unaligned_le32(buf + FAN_BUF_SPEED_CNT_OFF);
	if (speed_cnt == 0 || speed_cnt > FAN_CURVE_POINTS) {
		ret = -ERANGE;
		goto out;
	}

	temp_cnt_off = FAN_BUF_SPEED_DATA_OFF + speed_cnt * sizeof(u16);
	expected = temp_cnt_off + sizeof(u32);
	if (obj->buffer.length < expected) {
		ret = -ENODATA;
		goto out;
	}

	temp_cnt = get_unaligned_le32(buf + temp_cnt_off);
	if (temp_cnt == 0 || temp_cnt > FAN_CURVE_POINTS) {
		ret = -ERANGE;
		goto out;
	}

	expected = temp_cnt_off + sizeof(u32) + temp_cnt * sizeof(u16);
	if (obj->buffer.length < expected) {
		ret = -ENODATA;
		goto out;
	}

	cnt = min(speed_cnt, temp_cnt);
	for (i = 0; i < (int)cnt; i++) {
		lf->curve.speeds[i] = get_unaligned_le16(
			buf + FAN_BUF_SPEED_DATA_OFF + i * sizeof(u16));
		lf->curve.temps[i]  = get_unaligned_le16(
			buf + temp_cnt_off + sizeof(u32) + i * sizeof(u16));
	}
	/* Pad remaining points with the last valid value */
	for (i = cnt; i < FAN_CURVE_POINTS; i++) {
		lf->curve.speeds[i] = lf->curve.speeds[cnt - 1];
		lf->curve.temps[i]  = lf->curve.temps[cnt - 1];
	}
out:
	kfree(obj);
	return ret;
}

static int legion_set_fan_curve(struct legion_wmi_fan *lf)
{
	/*
	 * Buffer layout (48 bytes total):
	 *   fan_id   (2 bytes LE)
	 *   sensor_id (2 bytes LE)
	 *   speed_count (4 bytes LE)
	 *   speeds[10]  (10 × 2 bytes LE)
	 *   temp_count  (4 bytes LE)
	 *   temps[10]   (10 × 2 bytes LE)
	 */
	u8 buf[4 + 4 + FAN_CURVE_POINTS * 2 + 4 + FAN_CURVE_POINTS * 2];
	u8 *p = buf;
	int i;

	put_unaligned_le16(FAN_DEFAULT_FAN_ID,    p); p += sizeof(u16);
	put_unaligned_le16(FAN_DEFAULT_SENSOR_ID, p); p += sizeof(u16);
	put_unaligned_le32(FAN_CURVE_POINTS, p);       p += sizeof(u32);
	for (i = 0; i < FAN_CURVE_POINTS; i++) {
		put_unaligned_le16(lf->curve.speeds[i], p);
		p += sizeof(u16);
	}
	put_unaligned_le32(FAN_CURVE_POINTS, p);       p += sizeof(u32);
	for (i = 0; i < FAN_CURVE_POINTS; i++) {
		put_unaligned_le16(lf->curve.temps[i], p);
		p += sizeof(u16);
	}

	return legion_wmi_evaluate(FAN_METHOD_SET_CURVE,
				   buf, sizeof(buf), NULL);
}

/* -------------------------------------------------------------------------
 * Full-speed mode
 *
 * From ACPI reverse engineering (corando98/LLG_Dev_scripts):
 *   enable:  WMAE 0 0x12 0x0104020100  → bytes [01 04 02 01 00]
 *   disable: WMAE 0 0x12 0x0004020000  → bytes [00 04 02 00 00]
 * ------------------------------------------------------------------------- */

static int legion_set_fullspeed(struct legion_wmi_fan *lf, bool enable)
{
	u8 buf[5];

	buf[0] = enable ? 0x01 : 0x00;
	buf[1] = 0x04;
	buf[2] = 0x02;
	buf[3] = enable ? 0x01 : 0x00;
	buf[4] = 0x00;

	return legion_wmi_evaluate(FAN_METHOD_SET_FEATURE,
				   buf, sizeof(buf), NULL);
}

/* -------------------------------------------------------------------------
 * hwmon ops (fan1_input and pwm1_enable)
 * ------------------------------------------------------------------------- */

static umode_t legion_hwmon_is_visible(const void *data,
				       enum hwmon_sensor_types type,
				       u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_input)
			return 0444;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_enable)
			return 0644;
		break;
	default:
		break;
	}
	return 0;
}

static int legion_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);
	u8 in[4] = { FAN_DEFAULT_FAN_ID, 0, 0, 0 };
	u64 speed;
	int ret;

	switch (type) {
	case hwmon_fan:
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		mutex_lock(&lf->lock);
		ret = legion_wmi_evaluate_int(FAN_METHOD_GET_SPEED,
					      in, sizeof(in), &speed);
		mutex_unlock(&lf->lock);
		if (ret)
			return ret;
		*val = (long)speed;
		return 0;

	case hwmon_pwm:
		if (attr != hwmon_pwm_enable)
			return -EOPNOTSUPP;
		mutex_lock(&lf->lock);
		*val = lf->pwm_enable;
		mutex_unlock(&lf->lock);
		return 0;

	default:
		return -EOPNOTSUPP;
	}
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
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
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
	struct legion_wmi_fan *lf = dev_get_drvdata(dev);
	struct sensor_device_attribute *sda = to_sensor_dev_attr(attr);
	int idx = sda->index;
	long temp_mc;

	mutex_lock(&lf->lock);
	temp_mc = (long)lf->curve.temps[idx] * 1000;
	mutex_unlock(&lf->lock);

	return sysfs_emit(buf, "%ld\n", temp_mc);
}

static ssize_t pwm_auto_point_temp_store(struct device *dev,
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
	/* 0 °C to 120 °C in millidegrees */
	if (val < 0 || val > 120000)
		return -EINVAL;

	mutex_lock(&lf->lock);
	lf->curve.temps[idx] = (u16)(val / 1000);
	ret = 0;
	if (lf->pwm_enable == PWM_ENABLE_MANUAL)
		ret = legion_set_fan_curve(lf);
	mutex_unlock(&lf->lock);

	return ret ? ret : (ssize_t)count;
}

/* Expand one auto_point index (1-based for user, 0-based for array) */
#define LEGION_AUTO_POINT(_n)						\
static SENSOR_DEVICE_ATTR(pwm1_auto_point##_n##_pwm, 0644,		\
			  pwm_auto_point_pwm_show,			\
			  pwm_auto_point_pwm_store, (_n) - 1);		\
static SENSOR_DEVICE_ATTR(pwm1_auto_point##_n##_temp, 0644,		\
			  pwm_auto_point_temp_show,			\
			  pwm_auto_point_temp_store, (_n) - 1)

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
	u8 in[4] = { 0 };
	u64 fan_count = 0;
	int ret;

	lf = devm_kzalloc(dev, sizeof(*lf), GFP_KERNEL);
	if (!lf)
		return -ENOMEM;

	lf->pwm_enable = PWM_ENABLE_AUTO;
	mutex_init(&lf->lock);
	dev_set_drvdata(dev, lf);

	/* Verify the firmware supports fan control */
	ret = legion_wmi_evaluate_int(FAN_METHOD_GET_COUNT,
				      in, sizeof(in), &fan_count);
	if (ret) {
		dev_warn(dev, "Failed to get fan count (%d)\n", ret);
		return ret;
	}
	if (fan_count == 0) {
		dev_warn(dev, "Firmware reports zero fans\n");
		return -ENODEV;
	}
	dev_info(dev, "Fan count: %llu\n", fan_count);

	/* Snapshot the current fan curve from firmware */
	ret = legion_get_fan_curve(lf);
	if (ret)
		dev_warn(dev, "Could not read initial fan curve (%d); using defaults\n",
			 ret);

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

	dev_info(dev, "Lenovo Legion Go WMI fan driver loaded (%llu fan(s))\n",
		 fan_count);
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
