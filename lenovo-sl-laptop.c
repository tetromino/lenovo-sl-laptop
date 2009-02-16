/*
 *  lenovo-sl-laptop.c - Lenovo ThinkPad SL Series Extras Driver
 *
 *
 *  Copyright (C) 2008-2009 Alexandre Rostovtsev <tetromino@gmail.com>
 *
 *  Largely based on thinkpad_acpi.c, eeepc-laptop.c, and video.c which
 *  are copyright their respective authors.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 *  02110-1301, USA.
 *
 */

#define LENSL_LAPTOP_VERSION "0.02"

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/pci_ids.h>
#include <linux/rfkill.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>

#include <linux/input.h>
#include <linux/kthread.h>
#include <linux/freezer.h>

#include <linux/proc_fs.h>
#include <linux/uaccess.h>

#define LENSL_MODULE_DESC "Lenovo ThinkPad SL Series Extras driver"
#define LENSL_MODULE_NAME "lenovo-sl-laptop"

MODULE_AUTHOR("Alexandre Rostovtsev");
MODULE_DESCRIPTION(LENSL_MODULE_DESC);
MODULE_LICENSE("GPL");

/* #define instead of enum needed for macro */
#define LENSL_EMERG	0
#define LENSL_ALERT	1
#define LENSL_CRIT	2
#define LENSL_ERR	3
#define LENSL_WARNING	4
#define LENSL_NOTICE	5
#define LENSL_INFO	6
#define LENSL_DEBUG	7

#define vdbg_printk_(a_dbg_level, format, arg...) \
	do { if (dbg_level >= a_dbg_level) \
		printk("<" #a_dbg_level ">" LENSL_MODULE_NAME ": " \
			format, ## arg); \
	} while (0)
#define vdbg_printk(a_dbg_level, format, arg...) \
	vdbg_printk_(a_dbg_level, format, ## arg)

#define LENSL_HKEY_FILE LENSL_MODULE_NAME
#define LENSL_DRVR_NAME LENSL_MODULE_NAME

/* FIXME : we use "thinkpad_screen" for now to ensure compatibility with
   the xf86-video-intel driver (it checks the name against a fixed list
   of strings, see i830_lvds.c) but this is obviously suboptimal since
   this string is usually used by thinkpad_acpi.c */
#define LENSL_BACKLIGHT_NAME "thinkpad_screen"

#define LENSL_HKEY_POLL_KTHREAD_NAME "klensl_hkeyd"
#define LENSL_WORKQUEUE_NAME "klensl_wq"

#define LENSL_EC0 "\\_SB.PCI0.SBRG.EC0"
#define LENSL_HKEY LENSL_EC0 ".HKEY"
#define LENSL_LCDD "\\_SB.PCI0.VGA.LCDD"

#define LENSL_MAX_ACPI_ARGS 3

/* parameters */

static unsigned int dbg_level = LENSL_INFO;
static int debug_ec;
static int control_backlight;
static int bluetooth_auto_enable = 1;
module_param(debug_ec, bool, S_IRUGO);
MODULE_PARM_DESC(debug_ec,
	"Present EC debugging interface in procfs. WARNING: writing to the "
	"EC can hang your system and possibly damage your hardware.");
module_param(control_backlight, bool, S_IRUGO);
MODULE_PARM_DESC(control_backlight,
	"Control backlight brightness; can conflict with ACPI video driver");
module_param_named(debug, dbg_level, uint, S_IRUGO);
MODULE_PARM_DESC(debug,
	"Set debug verbosity level (0 = nothing, 7 = everything)");
module_param(bluetooth_auto_enable, bool, S_IRUGO);
MODULE_PARM_DESC(bluetooth_auto_enable,
	"Automatically enable bluetooth (if supported by hardware) when the "
	"module is loaded");

/* general */

static acpi_handle hkey_handle, ec0_handle;
static struct platform_device *lensl_pdev;
static struct input_dev *hkey_inputdev;
static struct workqueue_struct *lensl_wq;

static int parse_strtoul(const char *buf,
		unsigned long max, unsigned long *value)
{
	int res;

	res = strict_strtoul(buf, 0, value);
	if (res)
		return res;
	if (*value > max)
		return -EINVAL;
	return 0;
}

static int lensl_acpi_int_func(acpi_handle handle, char *pathname, int *ret,
				int n_arg, ...)
{
	acpi_status status;
	struct acpi_object_list params;
	union acpi_object in_obj[LENSL_MAX_ACPI_ARGS], out_obj;
	struct acpi_buffer result, *resultp;
	int i;
	va_list ap;

	if (!handle)
		return -EINVAL;
	if (n_arg < 0 || n_arg > LENSL_MAX_ACPI_ARGS)
		return -EINVAL;
	va_start(ap, n_arg);
	for (i = 0; i < n_arg; i++) {
		in_obj[i].integer.value = va_arg(ap, int);
		in_obj[i].type = ACPI_TYPE_INTEGER;
	}
	va_end(ap);
	params.count = n_arg;
	params.pointer = in_obj;

	if (ret) {
		result.length = sizeof(out_obj);
		result.pointer = &out_obj;
		resultp = &result;
	} else
		resultp = NULL;

	status = acpi_evaluate_object(handle, pathname, &params, resultp);
	if (ACPI_FAILURE(status))
		return -EIO;
	if (ret)
		*ret = out_obj.integer.value;

	vdbg_printk(LENSL_DEBUG, "ACPI : %s(", pathname);
	if (dbg_level >= LENSL_DEBUG) {
		for (i = 0; i < n_arg; i++) {
			if (i)
				printk(", ");
			printk("%d", (int)in_obj[i].integer.value);
		}
		printk(")");
		if (ret)
			printk(" == %d", *ret);
		printk("\n");
	}
	return 0;
}

/*************************************************************************
    bluetooth - copied nearly verbatim from thinkpad_acpi.c
 *************************************************************************/

enum {
	LENSL_RFK_BLUETOOTH_SW_ID = 0,
	LENSL_RFK_WWAN_SW_ID,
};

enum {
	/* ACPI GBDC/SBDC bits */
	TP_ACPI_BLUETOOTH_HWPRESENT	= 0x01,	/* Bluetooth hw available */
	TP_ACPI_BLUETOOTH_RADIOSSW	= 0x02,	/* Bluetooth radio enabled */
	TP_ACPI_BLUETOOTH_UNK		= 0x04,	/* unknown function */
};

static struct rfkill *bluetooth_rfkill;
static int bluetooth_present;
static int bluetooth_pretend_blocked;

static inline int get_wlsw(int *value)
{
	return lensl_acpi_int_func(hkey_handle, "WLSW", value, 0);
}

static inline int get_gbdc(int *value)
{
	return lensl_acpi_int_func(hkey_handle, "GBDC", value, 0);
}

static inline int set_sbdc(int value)
{
	return lensl_acpi_int_func(hkey_handle, "SBDC", NULL, 1, value);
}

static int bluetooth_get_radiosw(void)
{
	int value = 0;

	if (!bluetooth_present)
		return -ENODEV;

	/* WLSW overrides bluetooth in firmware/hardware, reflect that */
	if (bluetooth_pretend_blocked || (!get_wlsw(&value) && !value))
		return RFKILL_STATE_HARD_BLOCKED;

	if (get_gbdc(&value))
		return -EIO;

	return ((value & TP_ACPI_BLUETOOTH_RADIOSSW) != 0) ?
		RFKILL_STATE_UNBLOCKED : RFKILL_STATE_SOFT_BLOCKED;
}

static void bluetooth_update_rfk(void)
{
	int result;

	if (!bluetooth_rfkill)
		return;

	result = bluetooth_get_radiosw();
	if (result < 0)
		return;
	rfkill_force_state(bluetooth_rfkill, result);
}

static int bluetooth_set_radiosw(int radio_on, int update_rfk)
{
	int value;

	if (!bluetooth_present)
		return -ENODEV;

	/* WLSW overrides bluetooth in firmware/hardware, but there is no
	 * reason to risk weird behaviour. */
	if (get_wlsw(&value) && !value && radio_on)
		return -EPERM;

	if (get_gbdc(&value))
		return -EIO;
	if (radio_on)
		value |= TP_ACPI_BLUETOOTH_RADIOSSW;
	else
		value &= ~TP_ACPI_BLUETOOTH_RADIOSSW;
	if (set_sbdc(value))
		return -EIO;

	if (update_rfk)
		bluetooth_update_rfk();

	return 0;
}

/*************************************************************************
    bluetooth sysfs - copied nearly verbatim from thinkpad_acpi.c
 *************************************************************************/

static ssize_t bluetooth_enable_show(struct device *dev,
			   struct device_attribute *attr,
			   char *buf)
{
	int status;

	status = bluetooth_get_radiosw();
	if (status < 0)
		return status;

	return snprintf(buf, PAGE_SIZE, "%d\n",
			(status == RFKILL_STATE_UNBLOCKED) ? 1 : 0);
}

static ssize_t bluetooth_enable_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	unsigned long t;
	int res;

	if (parse_strtoul(buf, 1, &t))
		return -EINVAL;

	res = bluetooth_set_radiosw(t, 1);

	return (res) ? res : count;
}

static struct device_attribute dev_attr_bluetooth_enable =
	__ATTR(bluetooth_enable, S_IWUSR | S_IRUGO,
		bluetooth_enable_show, bluetooth_enable_store);

static struct attribute *bluetooth_attributes[] = {
	&dev_attr_bluetooth_enable.attr,
	NULL
};

static const struct attribute_group bluetooth_attr_group = {
	.attrs = bluetooth_attributes,
};

static int bluetooth_rfk_get(void *data, enum rfkill_state *state)
{
	int bts = bluetooth_get_radiosw();

	if (bts < 0)
		return bts;

	*state = bts;
	return 0;
}

static int bluetooth_rfk_set(void *data, enum rfkill_state state)
{
	return bluetooth_set_radiosw((state == RFKILL_STATE_UNBLOCKED), 0);
}

static int lensl_new_rfkill(const unsigned int id,
			struct rfkill **rfk,
			const enum rfkill_type rfktype,
			const char *name,
			int (*toggle_radio)(void *, enum rfkill_state),
			int (*get_state)(void *, enum rfkill_state *))
{
	int res;
	enum rfkill_state initial_state;

	*rfk = rfkill_allocate(&lensl_pdev->dev, rfktype);
	if (!*rfk) {
		vdbg_printk(LENSL_ERR,
			"Failed to allocate memory for rfkill class\n");
		return -ENOMEM;
	}

	(*rfk)->name = name;
	(*rfk)->get_state = get_state;
	(*rfk)->toggle_radio = toggle_radio;

	if (!get_state(NULL, &initial_state))
		(*rfk)->state = initial_state;

	res = rfkill_register(*rfk);
	if (res < 0) {
		vdbg_printk(LENSL_ERR,
			"Failed to register %s rfkill switch: %d\n",
			name, res);
		rfkill_free(*rfk);
		*rfk = NULL;
		return res;
	}

	return 0;
}

static void bluetooth_exit(void)
{
	if (bluetooth_rfkill)
		rfkill_unregister(bluetooth_rfkill);

	sysfs_remove_group(&lensl_pdev->dev.kobj,
			&bluetooth_attr_group);
}

static int bluetooth_init(void)
{
	int value, res;
	bluetooth_present = 0;
	if (!hkey_handle)
		return -ENODEV;
	if (get_gbdc(&value))
		return -EIO;
	if (!(value & TP_ACPI_BLUETOOTH_HWPRESENT))
		return -ENODEV;
	bluetooth_present = 1;

	res = sysfs_create_group(&lensl_pdev->dev.kobj,
				&bluetooth_attr_group);
	if (res)
		return res;

	bluetooth_pretend_blocked = !bluetooth_auto_enable;
	res = lensl_new_rfkill(LENSL_RFK_BLUETOOTH_SW_ID,
				&bluetooth_rfkill,
				RFKILL_TYPE_BLUETOOTH,
				"lensl_bluetooth_sw",
				bluetooth_rfk_set,
				bluetooth_rfk_get);
	bluetooth_pretend_blocked = 0;
	if (res) {
		bluetooth_exit();
		return res;
	}

	return 0;
}

/*************************************************************************
    backlight control - based on video.c
 *************************************************************************/

/* NB: the reason why this needs to be implemented here is that the SL series
   uses the ACPI interface for controlling the backlight in a non-standard
   manner. See http://bugzilla.kernel.org/show_bug.cgi?id=12249  */

static acpi_handle lcdd_handle;
static struct backlight_device *backlight;
static struct lensl_vector {
	int count;
	int *values;
} backlight_levels;

static int get_bcl(struct lensl_vector *levels)
{
	int i, status;
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *o, *obj;

	if (!levels)
		return -EINVAL;
	if (levels->count) {
		levels->count = 0;
		kfree(levels->values);
	}

	/* _BCL returns an array sorted from high to low; the first two values
	   are *not* special (non-standard behavior) */
	status = acpi_evaluate_object(lcdd_handle, "_BCL", NULL, &buffer);
	if (!ACPI_SUCCESS(status))
		return status;
	obj = (union acpi_object *)buffer.pointer;
	if (!obj || (obj->type != ACPI_TYPE_PACKAGE)) {
		vdbg_printk(LENSL_ERR, "Invalid _BCL data\n");
		status = -EFAULT;
		goto out;
	}

	levels->count = obj->package.count;
	if (!levels->count)
		goto out;
	levels->values = kmalloc(levels->count * sizeof(int), GFP_KERNEL);
	if (!levels->values) {
		vdbg_printk(LENSL_ERR,
			"Failed to allocate memory for brightness levels\n");
		status = -ENOMEM;
		goto out;
	}

	for (i = 0; i < obj->package.count; i++) {
		o = (union acpi_object *)&obj->package.elements[i];
		if (o->type != ACPI_TYPE_INTEGER) {
			vdbg_printk(LENSL_ERR, "Invalid brightness data\n");
			goto err;
		}
		levels->values[i] = (int) o->integer.value;
	}
	goto out;

err:
	levels->count = 0;
	kfree(levels->values);

out:
	kfree(buffer.pointer);

	return status;
}

static inline int set_bcm(int level)
{
	/* standard behavior */
	return lensl_acpi_int_func(lcdd_handle, "_BCM", NULL, 1, level);
}

static inline int get_bqc(int *level)
{
	/* returns an index from the bottom into the _BCL package
	   (non-standard behavior) */
	return lensl_acpi_int_func(lcdd_handle, "_BQC", level, 0);
}

/* backlight device sysfs support */
static int lensl_bd_get_brightness(struct backlight_device *bd)
{
	int level = 0;

	if (get_bqc(&level))
		return 0;

	return level;
}

static int lensl_bd_set_brightness_int(int request_level)
{
	int n;
	n = backlight_levels.count - request_level - 1;
	if (n >= 0 && n < backlight_levels.count)
		return set_bcm(backlight_levels.values[n]);

	return -EINVAL;
}

static int lensl_bd_set_brightness(struct backlight_device *bd)
{
	if (!bd)
		return -EINVAL;

	return lensl_bd_set_brightness_int(bd->props.brightness);
}

static struct backlight_ops lensl_backlight_ops = {
	.get_brightness = lensl_bd_get_brightness,
	.update_status  = lensl_bd_set_brightness,
};

static void backlight_exit(void)
{
	backlight_device_unregister(backlight);
	backlight = NULL;
	if (backlight_levels.count) {
		kfree(backlight_levels.values);
		backlight_levels.count = 0;
	}
}

static int backlight_init(void)
{
	int status = 0;

	lcdd_handle = NULL;
	backlight = NULL;
	backlight_levels.count = 0;
	backlight_levels.values = NULL;

	status = acpi_get_handle(NULL, LENSL_LCDD, &lcdd_handle);
	if (ACPI_FAILURE(status)) {
		vdbg_printk(LENSL_ERR,
			"Failed to get ACPI handle for %s\n", LENSL_LCDD);
		return -EIO;
	}

	status = get_bcl(&backlight_levels);
	if (status || !backlight_levels.count)
		goto err;

	backlight = backlight_device_register(LENSL_BACKLIGHT_NAME,
			NULL, NULL, &lensl_backlight_ops);
	backlight->props.max_brightness = backlight_levels.count - 1;
	backlight->props.brightness = lensl_bd_get_brightness(backlight);
	vdbg_printk(LENSL_INFO, "Started backlight brightness control\n");
	goto out;
err:
	if (backlight_levels.count) {
		kfree(backlight_levels.values);
		backlight_levels.count = 0;
	}
out:
	return status;
}

/*************************************************************************
    LEDs
 *************************************************************************/

#ifdef CONFIG_NEW_LEDS

#define LENSL_LED_TV_OFF   0
#define LENSL_LED_TV_ON    0x02
#define LENSL_LED_TV_BLINK 0x01
#define LENSL_LED_TV_DIM   0x100

/* equivalent to the ThinkVantage LED on other ThinkPads */
#define LENSL_LED_TV_NAME "lensl::lenovocare"

struct {
	struct led_classdev cdev;
	enum led_brightness brightness;
	int supported, new_code;
	struct work_struct work;
} led_tv;

static inline int set_tvls(int code)
{
	return lensl_acpi_int_func(hkey_handle, "TVLS", NULL, 1, code);
}

static void led_tv_worker(struct work_struct *work)
{
	if (!led_tv.supported)
		return;
	set_tvls(led_tv.new_code);
	if (led_tv.new_code)
		led_tv.brightness = LED_FULL;
	else
		led_tv.brightness = LED_OFF;
}

static void led_tv_brightness_set_sysfs(struct led_classdev *led_cdev,
				enum led_brightness brightness)
{
	switch (brightness) {
	case LED_OFF:
		led_tv.new_code = LENSL_LED_TV_OFF;
		break;
	case LED_FULL:
		led_tv.new_code = LENSL_LED_TV_ON;
		break;
	default:
		return;
	}
	queue_work(lensl_wq, &led_tv.work);
}

static enum led_brightness led_tv_brightness_get_sysfs(
					struct led_classdev *led_cdev)
{
	return led_tv.brightness;
}

static int led_tv_blink_set_sysfs(struct led_classdev *led_cdev,
			unsigned long *delay_on, unsigned long *delay_off)
{
	if (*delay_on == 0 && *delay_off == 0) {
		/* If we can choose the flash rate, use dimmed blinking --
		   it looks better */
		led_tv.new_code = LENSL_LED_TV_ON |
			LENSL_LED_TV_BLINK | LENSL_LED_TV_DIM;
		*delay_on = 2000;
		*delay_off = 2000;
	} else if (*delay_on + *delay_off == 4000) {
		/* User wants dimmed blinking */
		led_tv.new_code = LENSL_LED_TV_ON |
			LENSL_LED_TV_BLINK | LENSL_LED_TV_DIM;
	} else if (*delay_on == 7250 && *delay_off == 500) {
		/* User wants standard blinking mode */
		led_tv.new_code = LENSL_LED_TV_ON | LENSL_LED_TV_BLINK;
	} else
		return -EINVAL;
	queue_work(lensl_wq, &led_tv.work);
	return 0;
}

static void led_exit(void)
{
	if (led_tv.supported) {
		led_classdev_unregister(&led_tv.cdev);
		led_tv.supported = 0;
		set_tvls(LENSL_LED_TV_OFF);
	}
}

static int led_init(void)
{
	int res;

	memset(&led_tv, 0, sizeof(led_tv));
	led_tv.cdev.brightness_get = led_tv_brightness_get_sysfs;
	led_tv.cdev.brightness_set = led_tv_brightness_set_sysfs;
	led_tv.cdev.blink_set = led_tv_blink_set_sysfs;
	led_tv.cdev.name = LENSL_LED_TV_NAME;
	INIT_WORK(&led_tv.work, led_tv_worker);
	set_tvls(LENSL_LED_TV_OFF);
	res = led_classdev_register(&lensl_pdev->dev, &led_tv.cdev);
	if (res) {
		vdbg_printk(LENSL_WARNING, "Failed to register LED device\n");
		return res;
	}
	led_tv.supported = 1;
	return 0;
}

#else /* CONFIG_NEW_LEDS */

static void led_exit(void)
{
}

static int led_init(void)
{
	return -ENODEV;
}

#endif /* CONFIG_NEW_LEDS */

/*************************************************************************
    hwmon & fans
 *************************************************************************/

static struct device *lensl_hwmon_device;
/* we do not have a reliable way of reading it from ACPI */
static int pwm1_value = -1;
/* corresponds to ~2700 rpm */
#define DEFAULT_PWM1 126

static inline int get_tach(int *value, int fan)
{
	return lensl_acpi_int_func(ec0_handle, "TACH", value, 1, fan);
}

static inline int get_decf(int *value)
{
	return lensl_acpi_int_func(ec0_handle, "DECF", value, 0);
}

/* speed must be in range 0 .. 255 */
static inline int set_sfnv(int action, int speed)
{
	return lensl_acpi_int_func(ec0_handle, "SFNV", NULL, 2, action, speed);
}

static int pwm1_enable_get_current(void)
{
	int res;
	int value;

	res = get_decf(&value);
	if (res)
		return res;
	if (value & 1)
		return 1;
	return 0;
}

static ssize_t fan1_input_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int res;
	int rpm;

	res = get_tach(&rpm, 0);
	if (res)
		return res;
	return snprintf(buf, PAGE_SIZE, "%u\n", rpm);
}

static ssize_t pwm1_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	if (pwm1_value > -1)
		return snprintf(buf, PAGE_SIZE, "%u\n", pwm1_value);
	return -EPERM;
}

static ssize_t pwm1_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int status, res = 0;
	unsigned long speed;
	if (parse_strtoul(buf, 255, &speed))
		return -EINVAL;
	status = pwm1_enable_get_current();
	if (status < 0)
		return status;
	if (status > 0)
		res = set_sfnv(1, speed);

	if (res)
		return res;
	pwm1_value = speed;
	return count;
}

static ssize_t pwm1_enable_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	int status;
	status = pwm1_enable_get_current();
	if (status < 0)
		return status;
	return snprintf(buf, PAGE_SIZE, "%u\n", status);
}

static ssize_t pwm1_enable_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	int res, speed;
	unsigned long status;

	if (parse_strtoul(buf, 1, &status))
		return -EINVAL;

	if (status && pwm1_value > -1)
		speed = pwm1_value;
	else
		speed = DEFAULT_PWM1;

	res = set_sfnv(status, speed);

	if (res)
		return res;
	pwm1_value = speed;
	return count;
}

static struct device_attribute dev_attr_fan1_input =
	__ATTR(fan1_input, S_IRUGO,
		fan1_input_show, NULL);
static struct device_attribute dev_attr_pwm1 =
	__ATTR(pwm1, S_IWUSR | S_IRUGO,
		pwm1_show, pwm1_store);
static struct device_attribute dev_attr_pwm1_enable =
	__ATTR(pwm1_enable, S_IWUSR | S_IRUGO,
		pwm1_enable_show, pwm1_enable_store);

static struct attribute *hwmon_attributes[] = {
	&dev_attr_pwm1_enable.attr, &dev_attr_pwm1.attr,
	&dev_attr_fan1_input.attr,
	NULL
};

static const struct attribute_group hwmon_attr_group = {
	.attrs = hwmon_attributes,
};

static void hwmon_exit(void)
{
	if (!lensl_hwmon_device)
		return;

	sysfs_remove_group(&lensl_hwmon_device->kobj,
			   &hwmon_attr_group);
	hwmon_device_unregister(lensl_hwmon_device);
	lensl_hwmon_device = NULL;
}

static int hwmon_init(void)
{
	int res;

	pwm1_value = -1;
	lensl_hwmon_device = hwmon_device_register(&lensl_pdev->dev);
	if (!lensl_hwmon_device) {
		vdbg_printk(LENSL_ERR, "Failed to register hwmon device\n");
		return -ENODEV;
	}

	res = sysfs_create_group(&lensl_hwmon_device->kobj,
				 &hwmon_attr_group);
	if (res < 0) {
		vdbg_printk(LENSL_ERR, "Failed to create hwmon sysfs group\n");
		hwmon_device_unregister(lensl_hwmon_device);
		lensl_hwmon_device = NULL;
		return -ENODEV;
	}
	return 0;
}

/*************************************************************************
    hotkeys
 *************************************************************************/

static int hkey_poll_hz = 5;
static u8 hkey_ec_prev_offset;
static struct mutex hkey_poll_mutex;
static struct task_struct *hkey_poll_task;

struct key_entry {
	char type;
	u8 scancode;
	int keycode;
};

enum { KE_KEY, KE_END };

static struct key_entry ec_keymap[] = {
	/* Fn F2 */
	{KE_KEY, 0x0B, KEY_COFFEE },
	/* Fn F3 */
	{KE_KEY, 0x0C, KEY_BATTERY },
	/* Fn F4; dispatches an ACPI event */
	{KE_KEY, 0x0D, /* KEY_SLEEP */ KEY_RESERVED },
	/* Fn F5; FIXME: should this be KEY_BLUETOOTH? */
	{KE_KEY, 0x0E, KEY_WLAN },
	/* Fn F7; dispatches an ACPI event */
	{KE_KEY, 0x10, /* KEY_SWITCHVIDEOMODE */ KEY_RESERVED },
	/* Fn F8 - ultranav; FIXME: find some keycode that fits this properly */
	{KE_KEY, 0x11, KEY_PROG1 },
	/* Fn F9 */
	{KE_KEY, 0x12, KEY_EJECTCD },
	/* Fn F12 */
	{KE_KEY, 0x15, KEY_SUSPEND },
	{KE_KEY, 0x69, KEY_VOLUMEUP },
	{KE_KEY, 0x6A, KEY_VOLUMEDOWN },
	{KE_KEY, 0x6B, KEY_MUTE },
	/* Fn Home; dispatches an ACPI event */
	{KE_KEY, 0x6C, KEY_BRIGHTNESSDOWN /*KEY_RESERVED*/ },
	/* Fn End; dispatches an ACPI event */
	{KE_KEY, 0x6D, KEY_BRIGHTNESSUP /*KEY_RESERVED*/ },
	/* Fn spacebar - zoom */
	{KE_KEY, 0x71, KEY_ZOOM },
	/* Lenovo Care key */
	{KE_KEY, 0x80, KEY_VENDOR },
	{KE_END, 0},
};

static int ec_scancode_to_keycode(u8 scancode)
{
	struct key_entry *key;

	for (key = ec_keymap; key->type != KE_END; key++)
		if (scancode == key->scancode)
			return key->keycode;

	return -EINVAL;
}

static int hkey_inputdev_getkeycode(struct input_dev *dev, int scancode,
					int *keycode)
{
	int result;

	if (!dev)
		return -EINVAL;

	result = ec_scancode_to_keycode(scancode);
	if (result >= 0) {
		*keycode = result;
		return 0;
	}
	return result;
}

static int hkey_inputdev_setkeycode(struct input_dev *dev, int scancode,
					int keycode)
{
	struct key_entry *key;

	if (!dev)
		return -EINVAL;

	for (key = ec_keymap; key->type != KE_END; key++)
		if (scancode == key->scancode) {
			clear_bit(key->keycode, dev->keybit);
			key->keycode = keycode;
			set_bit(key->keycode, dev->keybit);
			return 0;
		}

	return -EINVAL;
}

static int hkey_ec_get_offset(void)
{
	/* Hotkey events are stored in EC registers 0x0A .. 0x11
	 * Address of last event is stored in EC registers 0x12 and
	 * 0x14; if address is 0x01, last event is in register 0x0A;
	 * if address is 0x07, last event is in register 0x10;
	 * if address is 0x00, last event is in register 0x11 */

	u8 offset;

	if (ec_read(0x12, &offset))
		return -EINVAL;
	if (!offset)
		offset = 8;
	offset -= 1;
	if (offset > 7)
		return -EINVAL;
	return offset;
}

static int hkey_poll_kthread(void *data)
{
	unsigned long t = 0;
	int offset, level;
	unsigned int keycode;
	u8 scancode;

	mutex_lock(&hkey_poll_mutex);

	offset = hkey_ec_get_offset();
	if (offset < 0) {
		vdbg_printk(LENSL_WARNING,
			"Failed to read hotkey register offset from EC\n");
		hkey_ec_prev_offset = 0;
	} else
		hkey_ec_prev_offset = offset;

	while (!kthread_should_stop() && hkey_poll_hz) {
		if (t == 0)
			t = 1000/hkey_poll_hz;
		t = msleep_interruptible(t);
		if (unlikely(kthread_should_stop()))
			break;
		try_to_freeze();
		if (t > 0)
			continue;
		offset = hkey_ec_get_offset();
		if (offset < 0) {
			vdbg_printk(LENSL_WARNING,
			   "Failed to read hotkey register offset from EC\n");
			continue;
		}
		if (offset == hkey_ec_prev_offset)
			continue;

		if (ec_read(0x0A + offset, &scancode)) {
			vdbg_printk(LENSL_WARNING,
				"Failed to read hotkey code from EC\n");
			continue;
		}
		keycode = ec_scancode_to_keycode(scancode);
		vdbg_printk(LENSL_DEBUG,
		   "Got hotkey keycode %d (scancode %d)\n", keycode, scancode);

		/* Special handling for brightness keys. We do it here and not
		   via an ACPI notifier in order to prevent possible conflicts
		   with video.c */
		if (keycode == KEY_BRIGHTNESSDOWN) {
			if (control_backlight && backlight) {
				level = lensl_bd_get_brightness(backlight);
				if (0 <= --level)
					lensl_bd_set_brightness_int(level);
			} else
				keycode = KEY_RESERVED;
		} else if (keycode == KEY_BRIGHTNESSUP) {
			if (control_backlight && backlight) {
				level = lensl_bd_get_brightness(backlight);
				if (backlight_levels.count > ++level)
					lensl_bd_set_brightness_int(level);
			} else
				keycode = KEY_RESERVED;
		}

		if (keycode != KEY_RESERVED) {
			input_report_key(hkey_inputdev, keycode, 1);
			input_sync(hkey_inputdev);
			input_report_key(hkey_inputdev, keycode, 0);
			input_sync(hkey_inputdev);
		}
		hkey_ec_prev_offset = offset;
	}

	mutex_unlock(&hkey_poll_mutex);
	return 0;
}

static void hkey_poll_start(void)
{
	hkey_ec_prev_offset = 0;
	mutex_lock(&hkey_poll_mutex);
	hkey_poll_task = kthread_run(hkey_poll_kthread,
		NULL, LENSL_HKEY_POLL_KTHREAD_NAME);
	if (IS_ERR(hkey_poll_task)) {
		hkey_poll_task = NULL;
		vdbg_printk(LENSL_ERR,
			"Could not create kernel thread for hotkey polling\n");
	}
	mutex_unlock(&hkey_poll_mutex);
}

static void hkey_poll_stop(void)
{
	if (hkey_poll_task) {
		if (frozen(hkey_poll_task) || freezing(hkey_poll_task))
			thaw_process(hkey_poll_task);

		kthread_stop(hkey_poll_task);
		hkey_poll_task = NULL;
		mutex_lock(&hkey_poll_mutex);
		/* at this point, the thread did exit */
		mutex_unlock(&hkey_poll_mutex);
	}
}

static void hkey_inputdev_exit(void)
{
	if (hkey_inputdev)
		input_unregister_device(hkey_inputdev);
	hkey_inputdev = NULL;
}

static int hkey_inputdev_init(void)
{
	int result;
	struct key_entry *key;

	hkey_inputdev = input_allocate_device();
	if (!hkey_inputdev) {
		vdbg_printk(LENSL_ERR,
			"Failed to allocate hotkey input device\n");
		return -ENODEV;
	}
	hkey_inputdev->name = "Lenovo ThinkPad SL Series extra buttons";
	hkey_inputdev->phys = LENSL_HKEY_FILE "/input0";
	hkey_inputdev->uniq = LENSL_HKEY_FILE;
	hkey_inputdev->id.bustype = BUS_HOST;
	hkey_inputdev->id.vendor = PCI_VENDOR_ID_LENOVO;
	hkey_inputdev->getkeycode = hkey_inputdev_getkeycode;
	hkey_inputdev->setkeycode = hkey_inputdev_setkeycode;
	set_bit(EV_KEY, hkey_inputdev->evbit);

	for (key = ec_keymap; key->type != KE_END; key++)
		set_bit(key->keycode, hkey_inputdev->keybit);

	result = input_register_device(hkey_inputdev);
	if (result) {
		vdbg_printk(LENSL_ERR,
			"Failed to register hotkey input device\n");
		input_free_device(hkey_inputdev);
		hkey_inputdev = NULL;
		return -ENODEV;
	}
	return 0;
}


/*************************************************************************
    procfs debugging interface
 *************************************************************************/

#define LENSL_PROC_EC "ec0"
#define LENSL_PROC_DIRNAME LENSL_MODULE_NAME

static struct proc_dir_entry *proc_dir;

int lensl_ec_read_procmem(char *buf, char **start, off_t offset,
		int count, int *eof, void *data)
{
	int err, len = 0;
	u8 i, result;
	/* note: ec_read at i = 255 locks up my SL300 hard. -AR */
	for (i = 0; i < 255; i++) {
		if (!(i % 16)) {
			if (i)
				len += sprintf(buf+len, "\n");
			len += sprintf(buf+len, "%02X:", i);
		}
		err = ec_read(i, &result);
		if (!err)
			len += sprintf(buf+len, " %02X", result);
		else
			len += sprintf(buf+len, " **");
	}
	len += sprintf(buf+len, "\n");
	*eof = 1;
	return len;
}

/* we expect input in the format "%02X %02X", where the first number is
   the EC register and the second is the value to be written */
int lensl_ec_write_procmem(struct file *file, const char *buffer,
				unsigned long count, void *data)
{
	char s[7];
	unsigned int reg, val;

	if (count > 6)
		return -EINVAL;
	memset(s, 0, 7);
	if (copy_from_user(s, buffer, count))
		return -EFAULT;
	if (sscanf(s, "%02X %02X", &reg, &val) < 2)
		return -EINVAL;
	if (reg > 255 || val > 255)
		return -EINVAL;
	if (ec_write(reg, val))
		return -EIO;
	return count;
}

static void lenovo_sl_procfs_exit(void)
{
	if (proc_dir) {
		remove_proc_entry(LENSL_PROC_EC, proc_dir);
		remove_proc_entry(LENSL_PROC_DIRNAME, acpi_root_dir);
	}
}

static int lenovo_sl_procfs_init(void)
{
	struct proc_dir_entry *proc_ec;

	proc_dir = proc_mkdir(LENSL_PROC_DIRNAME, acpi_root_dir);
	if (!proc_dir) {
		vdbg_printk(LENSL_ERR,
		   "Failed to create proc dir acpi/%s/\n", LENSL_PROC_DIRNAME);
		return -ENOENT;
	}
	proc_dir->owner = THIS_MODULE;
	proc_ec = create_proc_entry(LENSL_PROC_EC, 0600, proc_dir);
	if (!proc_ec) {
		vdbg_printk(LENSL_ERR,
			"Failed to create proc entry acpi/%s/%s\n",
			LENSL_PROC_DIRNAME, LENSL_PROC_EC);
		return -ENOENT;
	}
	proc_ec->read_proc = lensl_ec_read_procmem;
	proc_ec->write_proc = lensl_ec_write_procmem;

	return 0;
}

/*************************************************************************
    init/exit
 *************************************************************************/

static int __init lenovo_sl_laptop_init(void)
{
	int ret;
	acpi_status status;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,28)
	if (!acpi_video_backlight_support())
		control_backlight = 1;
#endif

	hkey_handle = ec0_handle = NULL;

	if (acpi_disabled)
		return -ENODEV;

	lensl_wq = create_singlethread_workqueue(LENSL_WORKQUEUE_NAME);
	if (!lensl_wq) {
		vdbg_printk(LENSL_ERR, "Failed to create a workqueue\n");
		return -ENOMEM;
	}

	status = acpi_get_handle(NULL, LENSL_HKEY, &hkey_handle);
	if (ACPI_FAILURE(status)) {
		vdbg_printk(LENSL_ERR,
			"Failed to get ACPI handle for %s\n", LENSL_HKEY);
		return -ENODEV;
	}
	status = acpi_get_handle(NULL, LENSL_EC0, &ec0_handle);
	if (ACPI_FAILURE(status)) {
		vdbg_printk(LENSL_ERR,
			"Failed to get ACPI handle for %s\n", LENSL_EC0);
		return -ENODEV;
	}

	lensl_pdev = platform_device_register_simple(LENSL_DRVR_NAME, -1,
							NULL, 0);
	if (IS_ERR(lensl_pdev)) {
		ret = PTR_ERR(lensl_pdev);
		lensl_pdev = NULL;
		vdbg_printk(LENSL_ERR, "Failed to register platform device\n");
		return ret;
	}

	ret = hkey_inputdev_init();
	if (ret)
		return -ENODEV;

	bluetooth_init();
	if (control_backlight)
		backlight_init();

	led_init();
	mutex_init(&hkey_poll_mutex);
	hkey_poll_start();
	hwmon_init();

	if (debug_ec)
		lenovo_sl_procfs_init();

	vdbg_printk(LENSL_INFO, "Loaded Lenovo ThinkPad SL Series driver\n");
	return 0;
}

static void __exit lenovo_sl_laptop_exit(void)
{
	lenovo_sl_procfs_exit();
	hwmon_exit();
	hkey_poll_stop();
	led_exit();
	backlight_exit();
	bluetooth_exit();
	hkey_inputdev_exit();
	if (lensl_pdev)
		platform_device_unregister(lensl_pdev);
	destroy_workqueue(lensl_wq);
	vdbg_printk(LENSL_INFO, "Unloaded Lenovo ThinkPad SL Series driver\n");
}

MODULE_ALIAS("dmi:bvnLENOVO:*:svnLENOVO:*:pvrThinkPad SL*:rvnLENOVO:*");

module_init(lenovo_sl_laptop_init);
module_exit(lenovo_sl_laptop_exit);
