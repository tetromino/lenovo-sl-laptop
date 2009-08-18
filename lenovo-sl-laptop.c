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

#define LENSL_LAPTOP_VERSION "0.03"

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
static int userspace_backlight = 1;
static int bluetooth_auto_enable = 1;
static int wwan_auto_enable = 1;
static int uwb_auto_enable = 1;
module_param(debug_ec, bool, S_IRUGO);
MODULE_PARM_DESC(debug_ec,
	"Present EC debugging interface in procfs. WARNING: writing to the "
	"EC can hang your system and possibly damage your hardware.");
module_param(control_backlight, bool, S_IRUGO);
MODULE_PARM_DESC(control_backlight,
	"Control backlight brightness; can conflict with ACPI video driver.");
module_param(userspace_backlight, bool, S_IRUGO);
/* Setting userspace_backlight_key to 0 is needed to work around an
   xf86-video-intel bug when kernel mode setting is used. */
MODULE_PARM_DESC(userspace_backlight,
	"Rely on userspace to change screen brightness when hotkey is "
	"pressed.");
module_param_named(debug, dbg_level, uint, S_IRUGO);
MODULE_PARM_DESC(debug,
	"Set debug verbosity level (0 = nothing, 7 = everything).");
module_param(bluetooth_auto_enable, bool, S_IRUGO);
MODULE_PARM_DESC(bluetooth_auto_enable,
	"Automatically enable bluetooth (if supported by hardware) when the "
	"module is loaded.");
module_param(wwan_auto_enable, bool, S_IRUGO);
MODULE_PARM_DESC(wwan_auto_enable,
	"Automatically enable WWAN (if supported by hardware) when the "
	"module is loaded.");
module_param(uwb_auto_enable, bool, S_IRUGO);
MODULE_PARM_DESC(wwan_auto_enable,
	"Automatically enable UWB (if supported by hardware) when the "
	"module is loaded.");

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
    Bluetooth, WWAN, UWB
 *************************************************************************/

enum {
	/* ACPI GBDC/SBDC, GWAN/SWAN, GUWB/SUWB bits */
	LENSL_RADIO_HWPRESENT	= 0x01, /* hardware is available */
	LENSL_RADIO_RADIOSSW	= 0x02, /* radio is enabled */
	LENSL_RADIO_RESUMECTRL	= 0x04, /* state at resume: off/last state */
};

typedef enum {
	LENSL_BLUETOOTH = 0,
	LENSL_WWAN,
	LENSL_UWB,
} lensl_radio_type;

/* pretend_blocked indicates whether we pretend that the device is
   hardware-blocked (used primarily to prevent the device from coming
   online when the module is loaded) */
struct lensl_radio {
	lensl_radio_type type;
	enum rfkill_type rfktype;
	int present;
	char *name;
	char *rfkname;
	struct rfkill *rfk;
	int (*get_acpi)(int *);
	int (*set_acpi)(int);
	int *auto_enable;
};

static inline int get_wlsw(int *value)
{
	return lensl_acpi_int_func(hkey_handle, "WLSW", value, 0);
}

static inline int get_gbdc(int *value)
{
	return lensl_acpi_int_func(hkey_handle, "GBDC", value, 0);
}

static inline int get_gwan(int *value)
{
	return lensl_acpi_int_func(hkey_handle, "GWAN", value, 0);
}

static inline int get_guwb(int *value)
{
	return lensl_acpi_int_func(hkey_handle, "GUWB", value, 0);
}

static inline int set_sbdc(int value)
{
	return lensl_acpi_int_func(hkey_handle, "SBDC", NULL, 1, value);
}

static inline int set_swan(int value)
{
	return lensl_acpi_int_func(hkey_handle, "SWAN", NULL, 1, value);
}

static inline int set_suwb(int value)
{
	return lensl_acpi_int_func(hkey_handle, "SUWB", NULL, 1, value);
}

static int lensl_radio_get(struct lensl_radio *radio, int *hw_blocked,
				int *value)
{
	int wlsw;

	*hw_blocked = 0;
	if (!radio)
		return -EINVAL;
	if (!radio->present)
		return -ENODEV;
	if (!get_wlsw(&wlsw) && !wlsw)
		*hw_blocked = 1;
	if (radio->get_acpi(value))
		return -EIO;
	return 0;
}

static int lensl_radio_set_on(struct lensl_radio *radio, int *hw_blocked,
				bool on)
{
	int value, ret;
	if ((ret = lensl_radio_get(radio, hw_blocked, &value)) < 0)
		return ret;
	/* WLSW overrides radio in firmware/hardware, but there is
	   no reason to risk weird behaviour. */
	if (*hw_blocked)
		return ret;
	if (on)
		value |= LENSL_RADIO_RADIOSSW;
	else
		value &= ~LENSL_RADIO_RADIOSSW;
	if (radio->set_acpi(value))
		return -EIO;
	return 0;
}

/* Bluetooth/WWAN/UWB rfkill interface */

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,30)

static int lensl_radio_rfkill_get_state(void *data, enum rfkill_state *state)
{
	int ret, value, hw_blocked = 0;
	ret = lensl_radio_get((struct lensl_radio *)data,
		&hw_blocked, &value);

	if (hw_blocked) {
		*state = RFKILL_STATE_HARD_BLOCKED;
		return 0;
	}

	if (ret)
		return ret;

	if (value & LENSL_RADIO_RADIOSSW)
		*state = RFKILL_STATE_UNBLOCKED;
	else
		*state = RFKILL_STATE_SOFT_BLOCKED;
	return 0;
}

static int lensl_radio_rfkill_toggle_radio(void *data, enum rfkill_state state)
{
	int ret, value, hw_blocked = 0;
	ret = lensl_radio_get((struct lensl_radio *)data,
		&hw_blocked, &value);

	if (state == RFKILL_STATE_UNBLOCKED) {
		if (hw_blocked)
			return -EPERM;
		if (ret)
			return ret;
		value = 1;
	} else {
		if (ret && !hw_blocked)
			return ret;
		value = 0;
	}

	ret = lensl_radio_set_on((struct lensl_radio *)data,
		&hw_blocked, value);

	if (hw_blocked)
		return 0;
	return ret;
}

static int lensl_radio_new_rfkill(struct lensl_radio *radio,
			struct rfkill **rfk, bool sw_blocked,
			bool hw_blocked)
{
	int res;

	*rfk = rfkill_allocate(&lensl_pdev->dev, radio->rfktype);
	if (!*rfk) {
		vdbg_printk(LENSL_ERR,
			"Failed to allocate memory for rfkill class\n");
		return -ENOMEM;
	}

	(*rfk)->name = radio->rfkname;
	(*rfk)->get_state = lensl_radio_rfkill_get_state;
	(*rfk)->toggle_radio = lensl_radio_rfkill_toggle_radio;
	(*rfk)->data = radio;

	if (hw_blocked)
		(*rfk)->state = RFKILL_STATE_HARD_BLOCKED;
	else if (sw_blocked)
		(*rfk)->state = RFKILL_STATE_SOFT_BLOCKED;
	else
		(*rfk)->state = RFKILL_STATE_UNBLOCKED;

	res = rfkill_register(*rfk);
	if (res < 0) {
		vdbg_printk(LENSL_ERR,
			"Failed to register %s rfkill switch: %d\n",
			radio->rfkname, res);
		rfkill_free(*rfk);
		*rfk = NULL;
		return res;
	}

	return 0;
}

#else

static void lensl_radio_rfkill_query(struct rfkill *rfk, void *data)
{
	int ret, value = 0;
	ret = get_wlsw(&value);
	if (ret)
		return;
	rfkill_set_hw_state(rfk, !value);
}

static int lensl_radio_rfkill_set_block(void *data, bool blocked)
{
	int ret, hw_blocked = 0;
	ret = lensl_radio_set_on((struct lensl_radio *)data,
		&hw_blocked, !blocked);
	/* rfkill spec: just return 0 on hard block */
	return ret;
}

static struct rfkill_ops rfkops = {
	NULL,
	lensl_radio_rfkill_query,
	lensl_radio_rfkill_set_block,
};

static int lensl_radio_new_rfkill(struct lensl_radio *radio,
			struct rfkill **rfk, bool sw_blocked,
			bool hw_blocked)
{
	int res;		
		
	*rfk = rfkill_alloc(radio->rfkname, &lensl_pdev->dev, radio->rfktype,
			&rfkops, radio);
	if (!*rfk) {
		vdbg_printk(LENSL_ERR,
			"Failed to allocate memory for rfkill class\n");
		return -ENOMEM;
	}

	rfkill_set_hw_state(*rfk, hw_blocked);
	rfkill_set_sw_state(*rfk, sw_blocked);

	res = rfkill_register(*rfk);
	if (res < 0) {
		vdbg_printk(LENSL_ERR,
			"Failed to register %s rfkill switch: %d\n",
			radio->rfkname, res);
		rfkill_destroy(*rfk);
		*rfk = NULL;
		return res;
	}

	return 0;
}

#endif /* LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,30) */

/* Bluetooth/WWAN/UWB init and exit */

static struct lensl_radio lensl_radios[3] = {
	{
		LENSL_BLUETOOTH,
		RFKILL_TYPE_BLUETOOTH,
		0,
		"bluetooth",
		"lensl_bluetooth_sw",
		NULL,
		get_gbdc,
		set_sbdc,
		&bluetooth_auto_enable,
	},
	{
		LENSL_WWAN,
		RFKILL_TYPE_WWAN,
		0,
		"WWAN",
		"lensl_wwan_sw",
		NULL,
		get_gwan,
		set_swan,
		&wwan_auto_enable,
	},
	{
		LENSL_UWB,
		RFKILL_TYPE_UWB,
		0,
		"UWB",
		"lensl_uwb_sw",
		NULL,
		get_guwb,
		set_suwb,
		&uwb_auto_enable,
	},
};

static void radio_exit(lensl_radio_type type)
{
	if (lensl_radios[type].rfk)
		rfkill_unregister(lensl_radios[type].rfk);
}

static int radio_init(lensl_radio_type type)
{
	int value, res, hw_blocked = 0, sw_blocked;

	if (!hkey_handle)
		return -ENODEV;
	lensl_radios[type].present = 1; /* need for lensl_radio_get */
	res = lensl_radio_get(&lensl_radios[type], &hw_blocked, &value);
	lensl_radios[type].present = 0;
	if (res && !hw_blocked)
		return -EIO;
	if (!(value & LENSL_RADIO_HWPRESENT))
		return -ENODEV;
	lensl_radios[type].present = 1;

	if (*lensl_radios[type].auto_enable) {
		sw_blocked = 0;
		value |= LENSL_RADIO_RADIOSSW;
		lensl_radios[type].set_acpi(value);
	} else {
		sw_blocked = 1;
		value &= ~LENSL_RADIO_RADIOSSW;
		lensl_radios[type].set_acpi(value);
	}

	res = lensl_radio_new_rfkill(&lensl_radios[type], &lensl_radios[type].rfk,
					sw_blocked, hw_blocked);

	if (res) {
		radio_exit(type);
		return res;
	}
	vdbg_printk(LENSL_DEBUG, "Initialized %s subdriver\n",
		lensl_radios[type].name);

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
	vdbg_printk(LENSL_ERR,
		"Failed to start backlight brightness control\n");
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
	vdbg_printk(LENSL_DEBUG, "Initialized LED subdriver\n");
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
	/* switch fans to automatic mode on module unload */
	set_sfnv(0, DEFAULT_PWM1);
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
	vdbg_printk(LENSL_DEBUG, "Initialized hwmon subdriver\n");
	return 0;
}

/*************************************************************************
    hotkeys
 *************************************************************************/

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

static int hkey_action(void *data)
{
	int keycode;
	int level;

	if (!data)
		return -EINVAL;
	keycode = *(int *)data;

	/* Special handling for brightness keys. Unfortunately, this is
	   the only way to get brightness hotkeys working in X with intel
	   video and kernel mode setting. */
	if (keycode == KEY_BRIGHTNESSDOWN) {
		if (!userspace_backlight && control_backlight && backlight) {
			level = lensl_bd_get_brightness(backlight);
			if (0 <= --level)
				lensl_bd_set_brightness_int(level);
		}
	} else if (keycode == KEY_BRIGHTNESSUP) {
		if (!userspace_backlight && control_backlight && backlight) {
			level = lensl_bd_get_brightness(backlight);
			if (backlight_levels.count > ++level)
			lensl_bd_set_brightness_int(level);
		}
	}

	if (keycode != KEY_RESERVED) {
	        input_report_key(hkey_inputdev, keycode, 1);
	        input_sync(hkey_inputdev);
	        input_report_key(hkey_inputdev, keycode, 0);
	        input_sync(hkey_inputdev);
	}

	return 0;
}

typedef int (*acpi_ec_query_func) (void *data);
extern int acpi_ec_add_query_handler(void *ec, u8 query_bit, acpi_handle handle,
	acpi_ec_query_func func,void *data);
static int hkey_add(struct acpi_device *device)
{
	int result;
	struct key_entry *key;

	for (key = ec_keymap; key->type != KE_END; key++) {
		result = acpi_ec_add_query_handler(
			acpi_driver_data(device->parent),
			key->scancode, NULL, hkey_action,
			&(key->keycode));
		if (result) {
			vdbg_printk(LENSL_ERR,
				"Failed to register hotkey notification.\n");
			return -ENODEV;
		}
	}
	return 0;
}

extern void acpi_ec_remove_query_handler(void *ec, u8 query_bit);
static int hkey_remove(struct acpi_device *device, int type)
{
	struct key_entry *key;

	for (key = ec_keymap; key->type != KE_END; key++) {
		acpi_ec_remove_query_handler(acpi_driver_data(device->parent),
			key->scancode);
	}
	return 0;
}

static const struct acpi_device_id hkey_ids[] = {
	{"LEN0014",0},
	{"", 0},
};

static struct acpi_driver hkey_driver = {
	.name = "lenovo-sl-laptop-hotkey",
	.class = "lenovo",
	.ids = hkey_ids,
	.ops = {
		.add = hkey_add,
		.remove = hkey_remove,
	},
};

static void hkey_register_notify(void)
{
	int result;
	result = acpi_bus_register_driver(&hkey_driver);
	if (result) {
		vdbg_printk(LENSL_ERR,"Failed to register hotkey driver\n");
	}
	return;
}

static void hkey_unregister_notify(void)
{
	acpi_bus_unregister_driver(&hkey_driver);
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
	vdbg_printk(LENSL_DEBUG, "Initialized hotkey subdriver\n");
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
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,29)
	proc_dir->owner = THIS_MODULE;
#endif
	proc_ec = create_proc_entry(LENSL_PROC_EC, 0600, proc_dir);
	if (!proc_ec) {
		vdbg_printk(LENSL_ERR,
			"Failed to create proc entry acpi/%s/%s\n",
			LENSL_PROC_DIRNAME, LENSL_PROC_EC);
		return -ENOENT;
	}
	proc_ec->read_proc = lensl_ec_read_procmem;
	proc_ec->write_proc = lensl_ec_write_procmem;
	vdbg_printk(LENSL_DEBUG, "Initialized procfs debugging interface\n");

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

	radio_init(LENSL_BLUETOOTH);
	radio_init(LENSL_WWAN);
	radio_init(LENSL_UWB);
	if (control_backlight)
		backlight_init();

	led_init();
	hkey_register_notify();
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
	hkey_unregister_notify();
	led_exit();
	backlight_exit();
	radio_exit(LENSL_UWB);
	radio_exit(LENSL_WWAN);
	radio_exit(LENSL_BLUETOOTH);
	hkey_inputdev_exit();
	if (lensl_pdev)
		platform_device_unregister(lensl_pdev);
	destroy_workqueue(lensl_wq);
	vdbg_printk(LENSL_INFO, "Unloaded Lenovo ThinkPad SL Series driver\n");
}

MODULE_ALIAS("dmi:bvnLENOVO:*:svnLENOVO*:*:pvrThinkPad SL*:rvnLENOVO:*");

module_init(lenovo_sl_laptop_init);
module_exit(lenovo_sl_laptop_exit);
