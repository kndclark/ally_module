// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for Asus notebook built-in keyboard.
 *  Fixes small logical maximum to match usage maximum.
 *
 *  Currently supported devices are:
 *    EeeBook X205TA
 *    VivoBook E200HA
 *
 *  Copyright (c) 2016 Yusuke Fujimaki <usk.fujimaki@gmail.com>
 *
 *  This module based on hid-ortek by
 *  Copyright (c) 2010 Johnathon Harris <jmharris@gmail.com>
 *  Copyright (c) 2011 Jiri Kosina
 *
 *  This module has been updated to add support for Asus i2c touchpad.
 *
 *  Copyright (c) 2016 Brendan McGrath <redmcg@redmandi.dyndns.org>
 *  Copyright (c) 2016 Victor Vlasenko <victor.vlasenko@sysgears.com>
 *  Copyright (c) 2016 Frederik Wenigwieser <frederik.wenigwieser@gmail.com>
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/dmi.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/platform_data/x86/asus-wmi.h>
#include <linux/input/mt.h>
#include <linux/usb.h> /* For to_usb_interface for T100 touchpad intf check */
#include <linux/power_supply.h>
#include <linux/leds.h>
#include <linux/led-dynamic-lighting.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "hid-ids.h"

MODULE_AUTHOR("Yusuke Fujimaki <usk.fujimaki@gmail.com>");
MODULE_AUTHOR("Brendan McGrath <redmcg@redmandi.dyndns.org>");
MODULE_AUTHOR("Victor Vlasenko <victor.vlasenko@sysgears.com>");
MODULE_AUTHOR("Frederik Wenigwieser <frederik.wenigwieser@gmail.com>");
MODULE_AUTHOR("Marco Scardovi <scardracs@disroot.org>");
MODULE_AUTHOR("Denis Benato <denis.benato@linux.dev>");
MODULE_DESCRIPTION("Asus HID Keyboard and TouchPad");

#define T100_TPAD_INTF 2
#define MEDION_E1239T_TPAD_INTF 1

#define E1239T_TP_TOGGLE_REPORT_ID 0x05
#define T100CHI_MOUSE_REPORT_ID 0x06
#define FEATURE_REPORT_ID 0x0d
#define INPUT_REPORT_ID 0x5d
#define FEATURE_KBD_REPORT_ID 0x5a
#define FEATURE_KBD_REPORT_SIZE 64
#define FEATURE_KBD_LED_REPORT_ID1 0x5d
#define FEATURE_KBD_LED_REPORT_ID2 0x5e
#define ROG_ALLY_REPORT_SIZE 64
#define ROG_ALLY_X_MIN_MCU 313
#define ROG_ALLY_MIN_MCU 319

#define AURA_FEATURE_REPORT_SIZE	64
#define ASUS_AURA_MAX_EFFECTS		13

/*
 * AniMe Matrix (USB 0x193b, feature report 0x5e). Packet layout and DMI
 * board matching follow asusctl rog-anime / reverse-engineering notes.
 */
#define ASUS_ANIME_PACKET_SIZE		640
#define ASUS_ANIME_BLOCK_START		7
#define ASUS_ANIME_BLOCK_END		634
#define ASUS_ANIME_PANE_LEN		(ASUS_ANIME_BLOCK_END - ASUS_ANIME_BLOCK_START)
#define ASUS_ANIME_STRIX_PANE1_LEN	490
#define ASUS_ANIME_MAX_LEDS		(ASUS_ANIME_PANE_LEN * 3)
#define ASUS_ANIME_MAX_EFFECTS		9

enum asus_anime_type {
	ASUS_ANIME_GA401 = 0,
	ASUS_ANIME_GA402,
	ASUS_ANIME_GU604,
	ASUS_ANIME_G635L,
	ASUS_ANIME_G835L,
	ASUS_ANIME_UNKNOWN,
};

enum asus_pid_193b_kind {
	ASUS_PID_193B_NONE = 0,
	ASUS_PID_193B_SLASH,
	ASUS_PID_193B_ANIME,
};

#define AURA_CMD_PROBE			0x05
#define AURA_CMD_RUN_MODE		0x9e
#define AURA_CMD_SET_EFFECT		0xb3
#define AURA_CMD_COMMIT			0xb4
#define AURA_CMD_SET			0xb5
#define AURA_CMD_ZONE_ENABLE	0xc0
#define AURA_CMD_DIRECT			0xbc
#define AURA_CMD_POWER			0xbd

#define AURA_ZONE_ACTIVATE_KEYBOARD	0x00
#define AURA_ZONE_ACTIVATE_LIGHTBAR	0x01

#define AURA_POWER_CMD_ENABLE		0x01

/* Power Byte 0: Logo (even bits) & Keyboard (odd bits) */
#define AURA_POWER_LOGO_BOOT		BIT(0)
#define AURA_POWER_KBD_BOOT			BIT(1)
#define AURA_POWER_LOGO_AWAKE		BIT(2)
#define AURA_POWER_KBD_AWAKE		BIT(3)
#define AURA_POWER_LOGO_SLEEP		BIT(4)
#define AURA_POWER_KBD_SLEEP		BIT(5)
#define AURA_POWER_LOGO_SHUTDOWN	BIT(6)
#define AURA_POWER_KBD_SHUTDOWN	BIT(7)
#define AURA_POWER_MASK_LOGO_ALL	(AURA_POWER_LOGO_BOOT | \
					 AURA_POWER_LOGO_AWAKE | \
					 AURA_POWER_LOGO_SLEEP | \
					 AURA_POWER_LOGO_SHUTDOWN)
#define AURA_POWER_MASK_KBD_LOGO_ALL	0xff

/* Power Byte 1: Chassis Lightbar */
#define AURA_POWER_LB_AUX		BIT(0)
#define AURA_POWER_LB_BOOT		BIT(1)
#define AURA_POWER_LB_AWAKE		BIT(2)
#define AURA_POWER_LB_SLEEP		BIT(3)
#define AURA_POWER_LB_SHUTDOWN		BIT(4)
#define AURA_POWER_MASK_LIGHTBAR_ALL	0x1f

/* Power Byte 2: Lid Display Bezel / Lid segments */
#define AURA_POWER_LID_BOOT			BIT(0)
#define AURA_POWER_LID_AWAKE		BIT(1)
#define AURA_POWER_LID_SLEEP		BIT(2)
#define AURA_POWER_LID_SHUTDOWN		BIT(3)
#define AURA_POWER_LID_PERSISTENCE	0xd0
#define AURA_POWER_MASK_LID_ALL		(AURA_POWER_LID_PERSISTENCE | 0x0f)

/* Power Byte 3: Rear Glow */
#define AURA_POWER_REAR_BOOT		BIT(0)
#define AURA_POWER_REAR_AWAKE		BIT(1)
#define AURA_POWER_REAR_SLEEP		BIT(2)
#define AURA_POWER_REAR_SHUTDOWN	BIT(3)
#define AURA_POWER_MASK_REAR_ALL	0x0f

#define AURA_ZONE_ALL			0x00
#define AURA_ZONE_KEY1			0x01
#define AURA_ZONE_KEY2			0x02
#define AURA_ZONE_KEY3			0x03
#define AURA_ZONE_KEY4			0x04
#define AURA_ZONE_LOGO			0x05
#define AURA_ZONE_BAR_LEFT		0x06
#define AURA_ZONE_BAR_RIGHT		0x07
#define AURA_ZONE_KEYBOARD_CHANNEL	0x01
#define AURA_ZONE_LIGHTBAR_CHANNEL	0x04

#define AURA_EFFECT_STATIC		0x00
#define AURA_EFFECT_BREATHING		0x01
#define AURA_EFFECT_SPECTRUM_CYCLE	0x02
#define AURA_EFFECT_RAINBOW		0x03
#define AURA_EFFECT_STARS			0x04
#define AURA_EFFECT_RAIN			0x05
#define AURA_EFFECT_REACTIVE		0x06
#define AURA_EFFECT_LASER			0x07
#define AURA_EFFECT_RIPPLE		0x08
#define AURA_EFFECT_PULSE			0x0a
#define AURA_EFFECT_COMET			0x0b
#define AURA_EFFECT_FLASH			0x0c

#define AURA_SPEED_SLOW			0xe1
#define AURA_SPEED_MED			0xeb
#define AURA_SPEED_FAST			0xf5

#define ROG_STRIX_LEDS_PER_PKT		16
#define ROG_STRIX_PERKEY_FULL_PKTS	10
#define ROG_STRIX_PERKEY_FINAL_PKT_LEDS	8
#define ROG_STRIX_PERKEY_PACKETS	(ROG_STRIX_PERKEY_FULL_PKTS + 1)
#define ROG_STRIX_DIRECT_LEDS \
	((ROG_STRIX_PERKEY_FULL_PKTS * ROG_STRIX_LEDS_PER_PKT) + \
	 ROG_STRIX_PERKEY_FINAL_PKT_LEDS)
#define ROG_STRIX_DIRECT_BUF_SIZE	(ROG_STRIX_DIRECT_LEDS * 3)
#define ROG_STRIX_LIGHTBAR_LEDS		12
#define ROG_STRIX_LIGHTBAR_BUF_SIZE	(ROG_STRIX_LIGHTBAR_LEDS * 3)
#define ROG_STRIX_4ZONE_KBD_LEDS	4
#define ROG_STRIX_4ZONE_KBD_BUF_SIZE	(ROG_STRIX_4ZONE_KBD_LEDS * 3)
#define ROG_STRIX_4ZONE_LIGHTBAR_LEDS	6
#define ROG_STRIX_4ZONE_LIGHTBAR_BUF_SIZE \
					(ROG_STRIX_4ZONE_LIGHTBAR_LEDS * 3)
#define ROG_STRIX_4ZONE_DIRECT_KBD_OFFSET	9
#define ROG_STRIX_4ZONE_DIRECT_LB_OFFSET	27
#define ROG_STRIX_PERKEY_DIRECT_PAYLOAD_OFFSET	9

#define AURA_DIRECT_FRAME_PERKEY	0x00
#define AURA_DIRECT_FRAME_ZONED		0x01
#define AURA_DIRECT_ROUTING_DEFAULT	0x01
#define AURA_DIRECT_CHUNK_FLAG		0x01

/*
 * Microsoft HID Lighting Illumination / LampArray (Usage Page 0x59).
 *
 * Linux Dynamic Lighting (led-class-dynamic / asus::*) is the userspace ABI.
 * On some Strix N-KEY devices (e.g. G614PR) Aura feature 0xBC cannot address
 * the chassis lightbar independently; the sibling LampArray interface is the
 * correct direct-RGB backend (same path Windows DL / G-Helper LampArray use).
 * When LampArray is absent, callers fall back to Aura 0xBC. Firmware effects
 * (0xb3) remain on the Aura report ID 0x5d interface.
 *
 * The LampArray USB interface is bound without hidraw so lighting stays
 * exclusively under this driver.
 */
#define ASUS_LAMPARRAY_MAX_LAMPS	64
#define ASUS_LAMPARRAY_MULTI_MAX	8
#define ASUS_LAMPARRAY_PURPOSE_CONTROL	0x01
#define ASUS_LAMPARRAY_FLAG_COMPLETE	0x01
#define ASUS_LAMPARRAY_RID_ATTR		0x01
#define ASUS_LAMPARRAY_RID_REQUEST	0x02
#define ASUS_LAMPARRAY_RID_RESPONSE	0x03
#define ASUS_LAMPARRAY_RID_MULTI	0x04
#define ASUS_LAMPARRAY_RID_CONTROL	0x06
#define AURA_ZONE_ACTIVATE_LAMPARRAY	0x03
#define AURA_ZONE_RELEASE_LAMPARRAY	0x04

/* Spurious HID codes sent by QUIRK_ROG_NKEY_KEYBOARD devices */
#define ASUS_SPURIOUS_CODE_0XEA 0xea
#define ASUS_SPURIOUS_CODE_0XEC 0xec
#define ASUS_SPURIOUS_CODE_0X02 0x02
#define ASUS_SPURIOUS_CODE_0X8A 0x8a
#define ASUS_SPURIOUS_CODE_0X9E 0x9e

/* Special key codes */
#define ASUS_FAN_CTRL_KEY_CODE 0xae

#define SUPPORT_KBD_BACKLIGHT BIT(0)

#define MAX_TOUCH_MAJOR 8
#define MAX_PRESSURE 128

#define BTN_LEFT_MASK 0x01
#define CONTACT_TOOL_TYPE_MASK 0x80
#define CONTACT_X_MSB_MASK 0xf0
#define CONTACT_Y_MSB_MASK 0x0f
#define CONTACT_TOUCH_MAJOR_MASK 0x07
#define CONTACT_PRESSURE_MASK 0x7f

#define	BATTERY_REPORT_ID	(0x03)
#define	BATTERY_REPORT_SIZE	(1 + 8)
#define	BATTERY_LEVEL_MAX	((u8)255)
#define	BATTERY_STAT_DISCONNECT	(0)
#define	BATTERY_STAT_CHARGING	(1)
#define	BATTERY_STAT_FULL	(2)

#define QUIRK_FIX_NOTEBOOK_REPORT	BIT(0)
#define QUIRK_NO_INIT_REPORTS		BIT(1)
#define QUIRK_SKIP_INPUT_MAPPING	BIT(2)
#define QUIRK_IS_MULTITOUCH		BIT(3)
#define QUIRK_NO_CONSUMER_USAGES	BIT(4)
#define QUIRK_USE_KBD_BACKLIGHT		BIT(5)
#define QUIRK_T100_KEYBOARD		BIT(6)
#define QUIRK_T100CHI			BIT(7)
#define QUIRK_G752_KEYBOARD		BIT(8)
#define QUIRK_T90CHI			BIT(9)
#define QUIRK_MEDION_E1239T		BIT(10)
#define QUIRK_ROG_NKEY_KEYBOARD		BIT(11)
#define QUIRK_ROG_CLAYMORE_II_KEYBOARD	BIT(12)
#define QUIRK_ROG_ALLY_XPAD		BIT(13)
#define QUIRK_HID_FN_LOCK		BIT(14)
#define QUIRK_FILTER_CAMERA_COMPANION	BIT(15)

#define I2C_KEYBOARD_QUIRKS			(QUIRK_FIX_NOTEBOOK_REPORT | \
						 QUIRK_NO_INIT_REPORTS | \
						 QUIRK_NO_CONSUMER_USAGES)
#define I2C_TOUCHPAD_QUIRKS			(QUIRK_NO_INIT_REPORTS | \
						 QUIRK_SKIP_INPUT_MAPPING | \
						 QUIRK_IS_MULTITOUCH)

#define TRKID_SGN       ((TRKID_MAX + 1) >> 1)

enum asus_work_action_type {
	FN_LOCK_SYNC,
	BRIGHTNESS_SET,
	WMI_FAN,
};

struct hid_raw_event_data {
	u8 report_data[FEATURE_KBD_REPORT_SIZE];
	size_t report_size;
};

struct asus_work_action {
	struct list_head node;
	enum asus_work_action_type type;
	union {
		/* Data for BRIGHTNESS_SET */
		unsigned int brightness;

		/* Data for FN_LOCK_SYNC */
		bool fn_lock;

		/* Data for WMI_FAN */
		struct hid_raw_event_data fan_hid_data;
	} data;
};

struct asus_worker {
	struct hid_device *hdev;
	struct work_struct work;
	struct list_head actions;
	spinlock_t lock;
	bool removed;
};

struct asus_touchpad_info {
	int max_x;
	int max_y;
	int res_x;
	int res_y;
	int contact_size;
	int max_contacts;
	int report_size;
};

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)
struct asus_lamparray_lamp {
	u16 id;
	s32 x;
	bool keyboard;
};
#endif

struct asus_drvdata {
	unsigned long quirks;
	struct led_classdev slash_led;
	bool has_slash_led;
	u8 slash_mode;
	u8 slash_brightness;
	u8 slash_interval;
	struct hid_device *hdev;
	struct input_dev *input;
	struct input_dev *tp_kbd_input;
	struct asus_worker *worker;
	unsigned int kbd_backlight_brightness;
	const struct asus_touchpad_info *tp;
	struct power_supply *battery;
	struct power_supply_desc battery_desc;
	int battery_capacity;
	int battery_stat;
	bool battery_in_query;
	unsigned long battery_next_query;
	struct asus_hid_listener listener;
	bool fn_lock;
#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)
	struct mutex aura_lock; /* Serializes Aura HID reports and buffers */
	u8 *aura_buf;
	/*
	 * 0 until the first successful write on that report id, then the HID
	 * path that worked. 0x5d and 0x5e are cached separately.
	 */
	u8 aura_write_5d;
	u8 aura_write_5e;
	bool kbd_power_on;
	bool lb_power_on;
	u32 kbd_power_states;
	u32 lb_power_states;
	struct led_classdev_dynamic dldev_kbd;
	struct led_classdev_dynamic dldev_lightbar;
	struct dl_rgb kbd_palette[2];
	struct dl_rgb lightbar_palette[2];
	const char *effects_kbd[ASUS_AURA_MAX_EFFECTS];
	const char *effects_lightbar[ASUS_AURA_MAX_EFFECTS];
	bool has_dldev_kbd;
	bool has_dldev_lightbar;
	bool kbd_effect_set;
	bool lightbar_effect_set;
	bool boot_effect_valid;
	u8 boot_zone;
	u8 boot_mode;
	u8 boot_r;
	u8 boot_g;
	u8 boot_b;
	u8 boot_r2;
	u8 boot_g2;
	u8 boot_b2;
	u8 boot_speed;
	u8 boot_direction;
	bool is_strix_4zone;
	bool has_lightbar;
	u8 kbd_direct_buf[ROG_STRIX_4ZONE_KBD_BUF_SIZE];
	u8 lb_direct_buf[ROG_STRIX_LIGHTBAR_BUF_SIZE];
	bool kbd_direct_valid;
	bool lb_direct_valid;
	struct hid_device *lamparray_hdev;
	u8 *lamparray_buf;
	size_t lamparray_buf_len;
	u8 lamparray_rid_base;
	unsigned int lamparray_count;
	bool lamparray_controlled;
	bool lamparray_unavailable;
	struct asus_lamparray_lamp *lamparray_lamps;
	struct led_classdev_dynamic dldev_anime;
	const char *effects_anime[ASUS_ANIME_MAX_EFFECTS];
	bool has_dldev_anime;
	enum asus_anime_type anime_type;
	u8 anime_boot;
	u8 anime_awake;
	u8 anime_sleep;
	u8 anime_shutdown;
	bool anime_builtin;
	u8 *anime_buf;
#endif
};

static int asus_report_battery(struct asus_drvdata *, u8 *, int);

static const struct asus_touchpad_info asus_i2c_tp = {
	.max_x = 2794,
	.max_y = 1758,
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t100ta_tp = {
	.max_x = 2240,
	.max_y = 1120,
	.res_x = 30, /* units/mm */
	.res_y = 27, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t100ha_tp = {
	.max_x = 2640,
	.max_y = 1320,
	.res_x = 30, /* units/mm */
	.res_y = 29, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t200ta_tp = {
	.max_x = 3120,
	.max_y = 1716,
	.res_x = 30, /* units/mm */
	.res_y = 28, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 28 /* 2 byte header + 5 * 5 + 1 byte footer */,
};

static const struct asus_touchpad_info asus_t100chi_tp = {
	.max_x = 2640,
	.max_y = 1320,
	.res_x = 31, /* units/mm */
	.res_y = 29, /* units/mm */
	.contact_size = 3,
	.max_contacts = 4,
	.report_size = 15 /* 2 byte header + 3 * 4 + 1 byte footer */,
};

static const struct asus_touchpad_info medion_e1239t_tp = {
	.max_x = 2640,
	.max_y = 1380,
	.res_x = 29, /* units/mm */
	.res_y = 28, /* units/mm */
	.contact_size = 5,
	.max_contacts = 5,
	.report_size = 32 /* 2 byte header + 5 * 5 + 5 byte footer */,
};

static const u8 asus_report_id_init[] = {
	FEATURE_KBD_REPORT_ID,
	FEATURE_KBD_LED_REPORT_ID1,
	FEATURE_KBD_LED_REPORT_ID2
};

/*
 * Send events to asus-wmi driver for handling special keys
 */
static int asus_wmi_send_event(struct asus_drvdata *drvdata, u8 code)
{
	int err;
	u32 retval;

	err = asus_wmi_evaluate_method(ASUS_WMI_METHODID_DEVS,
				       ASUS_WMI_METHODID_NOTIF, code, &retval);
	if (err) {
		pr_warn("Failed to notify asus-wmi: %d\n", err);
		return err;
	}

	if (retval != 0) {
		pr_warn("Failed to notify asus-wmi (retval): 0x%x\n", retval);
		return -EIO;
	}

	return 0;
}

static void asus_report_contact_down(struct asus_drvdata *drvdat,
		int toolType, u8 *data)
{
	struct input_dev *input = drvdat->input;
	int touch_major, pressure, x, y;

	x = (data[0] & CONTACT_X_MSB_MASK) << 4 | data[1];
	y = drvdat->tp->max_y - ((data[0] & CONTACT_Y_MSB_MASK) << 8 | data[2]);

	input_report_abs(input, ABS_MT_POSITION_X, x);
	input_report_abs(input, ABS_MT_POSITION_Y, y);

	if (drvdat->tp->contact_size < 5)
		return;

	if (toolType == MT_TOOL_PALM) {
		touch_major = MAX_TOUCH_MAJOR;
		pressure = MAX_PRESSURE;
	} else {
		touch_major = (data[3] >> 4) & CONTACT_TOUCH_MAJOR_MASK;
		pressure = data[4] & CONTACT_PRESSURE_MASK;
	}

	input_report_abs(input, ABS_MT_TOUCH_MAJOR, touch_major);
	input_report_abs(input, ABS_MT_PRESSURE, pressure);
}

/* Required for Synaptics Palm Detection */
static void asus_report_tool_width(struct asus_drvdata *drvdat)
{
	struct input_mt *mt = drvdat->input->mt;
	struct input_mt_slot *oldest;
	int oldid, i;

	if (drvdat->tp->contact_size < 5)
		return;

	oldest = NULL;
	oldid = mt->trkid;

	for (i = 0; i < mt->num_slots; ++i) {
		struct input_mt_slot *ps = &mt->slots[i];
		int id = input_mt_get_value(ps, ABS_MT_TRACKING_ID);

		if (id < 0)
			continue;
		if ((id - oldid) & TRKID_SGN) {
			oldest = ps;
			oldid = id;
		}
	}

	if (oldest) {
		input_report_abs(drvdat->input, ABS_TOOL_WIDTH,
			input_mt_get_value(oldest, ABS_MT_TOUCH_MAJOR));
	}
}

static int asus_report_input(struct asus_drvdata *drvdat, u8 *data, int size)
{
	int i, toolType = MT_TOOL_FINGER;
	u8 *contactData = data + 2;

	if (size != drvdat->tp->report_size)
		return 0;

	for (i = 0; i < drvdat->tp->max_contacts; i++) {
		bool down = !!(data[1] & BIT(i+3));

		if (drvdat->tp->contact_size >= 5)
			toolType = contactData[3] & CONTACT_TOOL_TYPE_MASK ?
						MT_TOOL_PALM : MT_TOOL_FINGER;

		input_mt_slot(drvdat->input, i);
		input_mt_report_slot_state(drvdat->input, toolType, down);

		if (down) {
			asus_report_contact_down(drvdat, toolType, contactData);
			contactData += drvdat->tp->contact_size;
		}
	}

	input_report_key(drvdat->input, BTN_LEFT, data[1] & BTN_LEFT_MASK);
	asus_report_tool_width(drvdat);

	input_mt_sync_frame(drvdat->input);
	input_sync(drvdat->input);

	return 1;
}

static int asus_e1239t_event(struct asus_drvdata *drvdat, u8 *data, int size)
{
	if (size != 3)
		return 0;

	/* Handle broken mute key which only sends press events */
	if (!drvdat->tp &&
	    data[0] == 0x02 && data[1] == 0xe2 && data[2] == 0x00) {
		input_report_key(drvdat->input, KEY_MUTE, 1);
		input_sync(drvdat->input);
		input_report_key(drvdat->input, KEY_MUTE, 0);
		input_sync(drvdat->input);
		return 1;
	}

	/* Handle custom touchpad toggle key which only sends press events */
	if (drvdat->tp_kbd_input &&
	    data[0] == 0x05 && data[1] == 0x02 && data[2] == 0x28) {
		input_report_key(drvdat->tp_kbd_input, KEY_F21, 1);
		input_sync(drvdat->tp_kbd_input);
		input_report_key(drvdat->tp_kbd_input, KEY_F21, 0);
		input_sync(drvdat->tp_kbd_input);
		return 1;
	}

	return 0;
}

/*
 * Used in atomic contexts to schedule work involving sleeps operations or
 * asus-wmi interactions.
 *
 * Caller is responsible to store relevant data in the structure to carry out
 * the required action.
 *
 * This function must be called while the spin lock protecting the workqueue
 * is already being held.
 */
static void asus_worker_schedule(struct asus_worker *worker, struct asus_work_action *action)
{
	if (worker->removed) {
		kfree(action);
		return;
	}

	list_add_tail(&action->node, &worker->actions);
	schedule_work(&worker->work);
}

static int asus_kbd_fn_lock_set(struct asus_drvdata *drvdata, bool enabled)
{
	struct asus_work_action *action;
	unsigned long flags;

	action = kzalloc_obj(struct asus_work_action, GFP_ATOMIC);
	if (!action)
		return -ENOMEM;

	drvdata->fn_lock = enabled;
	action->type = FN_LOCK_SYNC;
	action->data.fn_lock = drvdata->fn_lock;
	INIT_LIST_HEAD(&action->node);

	spin_lock_irqsave(&drvdata->worker->lock, flags);
	asus_worker_schedule(drvdata->worker, action);
	spin_unlock_irqrestore(&drvdata->worker->lock, flags);

	return 0;
}

static int asus_kbd_wmi_fan_send(struct asus_drvdata *drvdata, u8 *report_data,
				 size_t report_size)
{
	struct asus_work_action *action;
	unsigned long flags;

	if (report_size > FEATURE_KBD_REPORT_SIZE) {
		hid_err(drvdata->hdev, "Invalid report size for fan event: %zu\n", report_size);
		return -EINVAL;
	}

	action = kzalloc_obj(struct asus_work_action, GFP_NOWAIT);
	if (!action)
		return -ENOMEM;

	action->type = WMI_FAN;
	action->data.fan_hid_data.report_size = report_size;
	memcpy(action->data.fan_hid_data.report_data, report_data, report_size);
	INIT_LIST_HEAD(&action->node);

	spin_lock_irqsave(&drvdata->worker->lock, flags);
	asus_worker_schedule(drvdata->worker, action);
	spin_unlock_irqrestore(&drvdata->worker->lock, flags);

	return 0;
}

static int asus_event(struct hid_device *hdev, struct hid_field *field,
		      struct hid_usage *usage, __s32 value)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_ASUSVENDOR &&
	    (usage->hid & HID_USAGE) != 0x00 &&
	    (usage->hid & HID_USAGE) != 0xff && !usage->type) {
		hid_warn(hdev, "Unmapped Asus vendor usagepage code 0x%02x\n",
			 usage->hid & HID_USAGE);
	}

	if (usage->type == EV_KEY && value) {
		switch (usage->code) {
		case KEY_KBDILLUMUP:
			return !asus_hid_event(ASUS_EV_BRTUP);
		case KEY_KBDILLUMDOWN:
			return !asus_hid_event(ASUS_EV_BRTDOWN);
		case KEY_KBDILLUMTOGGLE:
			return !asus_hid_event(ASUS_EV_BRTTOGGLE);
		case KEY_FN_ESC:
			if (drvdata->quirks & QUIRK_HID_FN_LOCK) {
				ret = asus_kbd_fn_lock_set(drvdata, !drvdata->fn_lock);
				if (ret) {
					hid_err(hdev, "Error while toggling FN lock: %d\n", ret);
					return ret;
				}
			}
			break;
		}
	}

	return 0;
}

static int asus_raw_event(struct hid_device *hdev,
		struct hid_report *report, u8 *data, int size)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (size < 2) {
		hid_dbg(hdev, "Unexpected keyboard report size %d\n", size);
		return 0;
	}

	if (drvdata->battery && data[0] == BATTERY_REPORT_ID)
		return asus_report_battery(drvdata, data, size);

	if (drvdata->tp && data[0] == INPUT_REPORT_ID)
		return asus_report_input(drvdata, data, size);

	if (drvdata->quirks & QUIRK_MEDION_E1239T)
		return asus_e1239t_event(drvdata, data, size);

	/*
	 * Skip these report ID, the device emits a continuous stream associated
	 * with the AURA mode it is in which looks like an 'echo'.
	 */
	if (report->id == FEATURE_KBD_LED_REPORT_ID1 || report->id == FEATURE_KBD_LED_REPORT_ID2)
		return -1;
	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD) {
		if (report->id == FEATURE_KBD_REPORT_ID) {
			/*
			 * Fn+F5 fan control key - try to send WMI event to toggle fan mode.
			 * If successful, block the event from reaching userspace.
			 * If asus-wmi is unavailable or the call fails, let the event
			 * pass to userspace so it can implement its own fan control.
			 */
			if (data[1] == ASUS_FAN_CTRL_KEY_CODE) {
				ret = asus_kbd_wmi_fan_send(drvdata, data, size);

				/* if execution deferred successfully block event */
				if (ret == 0)
					return -1;

				return ret;
			}

			/*
			 * ASUS ROG laptops send these codes during normal operation
			 * with no discernable reason. Filter them out to avoid
			 * unmapped warning messages.
			 */
			if (data[1] == ASUS_SPURIOUS_CODE_0XEA ||
			    data[1] == ASUS_SPURIOUS_CODE_0XEC ||
			    data[1] == ASUS_SPURIOUS_CODE_0X02 ||
			    data[1] == ASUS_SPURIOUS_CODE_0X8A ||
			    data[1] == ASUS_SPURIOUS_CODE_0X9E) {
				return -1;
			}
		}

		/*
		 * G713 and G733 send these codes on some keypresses, depending on
		 * the key pressed it can trigger a shutdown event if not caught.
		 */
		if (data[0] == 0x02 && data[1] == 0x30)
			return -1;
	}

	if (drvdata->quirks & QUIRK_ROG_CLAYMORE_II_KEYBOARD) {
		/*
		 * CLAYMORE II keyboard sends this packet when it goes to sleep
		 * this causes the whole system to go into suspend.
		 */
		if (size == 2 && data[0] == 0x02 && data[1] == 0x00)
			return -1;
	}

	/*
	 * The camera-toggle key reports its vendor usage (0x85) together with a
	 * companion state byte in the same array report, e.g. "5a 85 01" and
	 * "5a 85 10" for the two toggle positions. The 0x10 companion aliases the
	 * brightness-down vendor usage and would spuriously dim the panel, so drop
	 * the companion slots and leave only the camera usage for input mapping.
	 */
	if (drvdata->quirks & QUIRK_FILTER_CAMERA_COMPANION &&
	    report->id == FEATURE_KBD_REPORT_ID && size >= 3 && data[1] == 0x85)
		memset(&data[2], 0, size - 2);

	return 0;
}

static int asus_kbd_set_report(struct hid_device *hdev, const u8 *buf, size_t buf_size)
{
	u8 *dmabuf __free(kfree) = kmemdup(buf, buf_size, GFP_KERNEL);
	int ret;

	if (!dmabuf)
		return -ENOMEM;

	/*
	 * For Aura/Slash LED reports (0x5d/0x5e), match asus_aura_set_feature:
	 * interrupt OUT, then raw OUTPUT, then raw FEATURE.
	 */
	if (buf[0] == FEATURE_KBD_LED_REPORT_ID1 ||
	    buf[0] == FEATURE_KBD_LED_REPORT_ID2) {
		ret = hid_hw_output_report(hdev, dmabuf, buf_size);
		if (ret >= 0)
			return 0;

		ret = hid_hw_raw_request(hdev, buf[0], dmabuf, buf_size,
					 HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
		if (ret >= 0)
			return 0;
	}

	/*
	 * The report ID should be set from the incoming buffer due to LED and key
	 * interfaces having different pages
	 */
	ret = hid_hw_raw_request(hdev, buf[0], dmabuf, buf_size, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;

	return 0;
}

static int asus_kbd_init(struct hid_device *hdev, u8 report_id)
{
	/*
	 * The handshake is first sent as a set_report, then retrieved
	 * from a get_report. They should be equal.
	 */
	const u8 buf[] = { report_id, 0x41, 0x53, 0x55, 0x53, 0x20, 0x54,
		     0x65, 0x63, 0x68, 0x2e, 0x49, 0x6e, 0x63, 0x2e, 0x00 };
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0) {
		hid_err(hdev, "Asus handshake %02x failed to send: %d\n",
			report_id, ret);
		return ret;
	}

	u8 *readbuf __free(kfree) = kzalloc(FEATURE_KBD_REPORT_SIZE, GFP_KERNEL);
	if (!readbuf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, report_id, readbuf,
				 FEATURE_KBD_REPORT_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_warn(hdev, "Asus handshake %02x failed to receive ack: %d\n",
			 report_id, ret);
	} else if (memcmp(readbuf, buf, sizeof(buf)) != 0) {
		hid_warn(hdev, "Asus handshake %02x returned invalid response: %*ph\n",
			 report_id, FEATURE_KBD_REPORT_SIZE, readbuf);
	}

	/*
	 * Do not return error if handshake is wrong until this is
	 * verified to work for all devices.
	 */
	return 0;
}

static int asus_kbd_get_functions(struct hid_device *hdev,
				  unsigned char *kbd_func,
				  u8 report_id)
{
	const u8 buf[] = { report_id, 0x05, 0x20, 0x31, 0x00, 0x08 };
	u8 *readbuf;
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0) {
		hid_err(hdev, "Asus failed to send configuration command: %d\n", ret);
		return ret;
	}

	readbuf = kzalloc(FEATURE_KBD_REPORT_SIZE, GFP_KERNEL);
	if (!readbuf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, report_id, readbuf,
				 FEATURE_KBD_REPORT_SIZE, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (ret < 0) {
		hid_err(hdev, "Asus failed to request functions: %d\n", ret);
		kfree(readbuf);
		return ret;
	}

	*kbd_func = readbuf[6];

	kfree(readbuf);
	return ret;
}

static int asus_kbd_disable_oobe(struct hid_device *hdev)
{
	const u8 init[][6] = {
		{ FEATURE_KBD_REPORT_ID, 0x05, 0x20, 0x31, 0x00, 0x08 },
		{ FEATURE_KBD_REPORT_ID, 0xBA, 0xC5, 0xC4 },
		{ FEATURE_KBD_REPORT_ID, 0xD0, 0x8F, 0x01 },
		{ FEATURE_KBD_REPORT_ID, 0xD0, 0x85, 0xFF }
	};
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(init); i++) {
		ret = asus_kbd_set_report(hdev, init[i], sizeof(init[i]));
		if (ret < 0)
			return ret;
	}

	hid_info(hdev, "Disabled OOBE for keyboard\n");
	return 0;
}

static void asus_kbd_set_fn_lock(struct hid_device *hdev, bool enabled)
{
	const u8 buf[FEATURE_KBD_REPORT_SIZE] = { FEATURE_KBD_REPORT_ID, 0xd0, 0x4e, !!enabled };
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0)
		hid_err(hdev, "Asus failed to set fn lock: %d\n", ret);
}

static void asus_kbd_set_brightness(struct hid_device *hdev, u8 brightness)
{
	const u8 buf[FEATURE_KBD_REPORT_SIZE] = {
		FEATURE_KBD_REPORT_ID, 0xba, 0xc5, 0xc4, brightness
	};
	int ret;

	ret = asus_kbd_set_report(hdev, buf, sizeof(buf));
	if (ret < 0)
		hid_err(hdev, "Asus failed to set keyboard backlight: %d\n", ret);
}

static void asus_kbd_wmi_fan(struct hid_device *hdev, struct hid_raw_event_data *data)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	ret = asus_wmi_send_event(drvdata, ASUS_FAN_CTRL_KEY_CODE);

	/*
	 * Warn if asus-wmi failed (but not if it's unavailable).
	 * Let the event reach userspace in all failure cases.
	 */
	switch (ret) {
	case -ENODEV:
		break;
	case 0:
		return;
	default:
		hid_warn(hdev, "Failed to notify asus-wmi: %d\n", ret);
		break;
	}

	/*
	 * Fallback: pass the raw event to the HID core; to avoid
	 * racing against the hid_report_raw_event() that generated
	 * this event use the same locking mechanism and wait for
	 * that function to terminate and signal the deferred execution
	 * before raising the stored event.
	 */
	down(&hdev->driver_input_lock);
	hid_report_raw_event(hdev, HID_INPUT_REPORT,
			     data->report_data, data->report_size,
			     data->report_size, 1);
	up(&hdev->driver_input_lock);
}

static void asus_kbd_backlight_set(struct asus_hid_listener *listener, int brightness)
{
	struct asus_drvdata *drvdata = container_of(listener, struct asus_drvdata, listener);
	struct asus_worker *worker = drvdata->worker;
	struct asus_work_action *action;
	unsigned long flags;

	drvdata->kbd_backlight_brightness = brightness;

	action = kzalloc_obj(struct asus_work_action, GFP_NOWAIT);
	if (!action)
		return;

	action->type = BRIGHTNESS_SET;
	action->data.brightness = brightness;
	INIT_LIST_HEAD(&action->node);

	spin_lock_irqsave(&worker->lock, flags);
	asus_worker_schedule(worker, action);
	spin_unlock_irqrestore(&worker->lock, flags);
}

static void asus_work(struct work_struct *work)
{
	struct asus_worker *worker = container_of(work, struct asus_worker, work);
	struct asus_work_action *action = NULL;
	unsigned long flags;

	/* Save the action to be performed and clear the flag */
	spin_lock_irqsave(&worker->lock, flags);
	if (!list_empty(&worker->actions)) {
		action = list_first_entry(&worker->actions,
					  struct asus_work_action, node);
		list_del(&action->node);
	}
	spin_unlock_irqrestore(&worker->lock, flags);

	if (!action)
		return;

	switch (action->type) {
	case BRIGHTNESS_SET:
		asus_kbd_set_brightness(worker->hdev, action->data.brightness);
		break;
	case FN_LOCK_SYNC:
		asus_kbd_set_fn_lock(worker->hdev, action->data.fn_lock);
		break;
	case WMI_FAN:
		asus_kbd_wmi_fan(worker->hdev, &action->data.fan_hid_data);
		break;
	default:
		hid_err(worker->hdev, "Invalid action type: %d\n", action->type);
		break;
	}

	kfree(action);

	/* Re-schedule if there are more pending actions */
	spin_lock_irqsave(&worker->lock, flags);
	if (!list_empty(&worker->actions))
		schedule_work(&worker->work);
	spin_unlock_irqrestore(&worker->lock, flags);
}

static int asus_worker_create(struct hid_device *hdev, struct asus_drvdata *drvdata)
{
	drvdata->worker = devm_kzalloc(&hdev->dev, sizeof(struct asus_worker), GFP_KERNEL);
	if (!drvdata->worker)
		return -ENOMEM;

	drvdata->worker->removed = false;
	drvdata->worker->hdev = hdev;
	INIT_LIST_HEAD(&drvdata->worker->actions);

	INIT_WORK(&drvdata->worker->work, asus_work);
	spin_lock_init(&drvdata->worker->lock);

	return 0;
}

static void asus_worker_stop(struct asus_worker *worker)
{
	struct asus_work_action *action, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&worker->lock, flags);
	worker->removed = true;
	list_for_each_entry_safe(action, tmp, &worker->actions, node) {
		list_del(&action->node);
		kfree(action);
	}
	spin_unlock_irqrestore(&worker->lock, flags);

	cancel_work_sync(&worker->work);
}

/*
 * We don't care about any other part of the string except the version section.
 * Example strings: FGA80100.RC72LA.312_T01, FGA80100.RC71LS.318_T01
 * The bytes "5a 05 03 31 00 1a 13" and possibly more come before the version
 * string, and there may be additional bytes after the version string such as
 * "75 00 74 00 65 00" or a postfix such as "_T01"
 */
static int mcu_parse_version_string(const u8 *response, size_t response_size)
{
	const u8 *end = response + response_size;
	const u8 *p = response;
	int dots, err, version;
	char buf[4];

	dots = 0;
	while (p < end && dots < 2) {
		if (*p++ == '.')
			dots++;
	}

	if (dots != 2 || end - p < 3)
		return -EINVAL;

	memcpy(buf, p, 3);
	buf[3] = '\0';

	err = kstrtoint(buf, 10, &version);
	if (err || version < 0)
		return -EINVAL;

	return version;
}

static int mcu_request_version(struct hid_device *hdev)
{
	u8 *response __free(kfree) = kzalloc(ROG_ALLY_REPORT_SIZE, GFP_KERNEL);
	const u8 request[] = { 0x5a, 0x05, 0x03, 0x31, 0x00, 0x20 };
	int ret;

	if (!response)
		return -ENOMEM;

	ret = asus_kbd_set_report(hdev, request, sizeof(request));
	if (ret < 0)
		return ret;

	ret = hid_hw_raw_request(hdev, FEATURE_REPORT_ID, response,
				ROG_ALLY_REPORT_SIZE, HID_FEATURE_REPORT,
				HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;

	ret = mcu_parse_version_string(response, ROG_ALLY_REPORT_SIZE);
	if (ret < 0) {
		pr_err("Failed to parse MCU version: %d\n", ret);
		print_hex_dump(KERN_ERR, "MCU: ", DUMP_PREFIX_NONE,
			      16, 1, response, ROG_ALLY_REPORT_SIZE, false);
	}

	return ret;
}

static void validate_mcu_fw_version(struct hid_device *hdev, int idProduct)
{
	int min_version, version;

	version = mcu_request_version(hdev);
	if (version < 0)
		return;

	switch (idProduct) {
	case USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY:
		min_version = ROG_ALLY_MIN_MCU;
		break;
	case USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X:
		min_version = ROG_ALLY_X_MIN_MCU;
		break;
	default:
		min_version = 0;
	}

	if (version < min_version) {
		hid_warn(hdev,
			"The MCU firmware version must be %d or greater to avoid issues with suspend.\n",
			min_version);
	} else {
		set_ally_mcu_hack(ASUS_WMI_ALLY_MCU_HACK_DISABLED);
		set_ally_mcu_powersave(true);
	}
}

static bool asus_has_report_id(struct hid_device *hdev, u16 report_id)
{
	struct hid_report *report;
	int t;

	for (t = HID_INPUT_REPORT; t <= HID_FEATURE_REPORT; t++) {
		list_for_each_entry(report, &hdev->report_enum[t].report_list, list) {
			if (report->id == report_id)
				return true;
		}
	}

	return false;
}

/*
 * Sibling USB interface with HID Lighting (LampArray) only — no Aura 0x5d.
 * Bound by this driver without hidraw so lighting stays in-kernel DL.
 */
static int asus_lamparray_detect_base(struct hid_device *hdev)
{
	u8 base;

	if (asus_has_report_id(hdev, FEATURE_KBD_LED_REPORT_ID1) ||
	    asus_has_report_id(hdev, FEATURE_KBD_LED_REPORT_ID2))
		return -ENODEV;

	for (base = 0; base <= 0x40; base += 0x40) {
		if (asus_has_report_id(hdev, base + ASUS_LAMPARRAY_RID_ATTR) &&
		    asus_has_report_id(hdev, base + ASUS_LAMPARRAY_RID_MULTI) &&
		    asus_has_report_id(hdev, base + ASUS_LAMPARRAY_RID_CONTROL))
			return base;
	}

	return -ENODEV;
}

static bool asus_is_lamparray_interface(struct hid_device *hdev)
{
	return asus_lamparray_detect_base(hdev) >= 0;
}

static size_t asus_feature_report_len(struct hid_device *hdev, u8 id)
{
	struct hid_report *report;

	report = hdev->report_enum[HID_FEATURE_REPORT].report_id_hash[id];
	if (!report)
		return 0;
	return hid_report_len(report);
}

static bool asus_dmi_is_anime_board(void)
{
	const char *board = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board)
		return false;

	return strstr(board, "GA401I") || strstr(board, "GA401Q") ||
	       strstr(board, "GA402R") || strstr(board, "GA402X") ||
	       strstr(board, "GA402N") || strstr(board, "GU604V") ||
	       strstr(board, "G635L") || strstr(board, "G835L");
}

/*
 * Boards that ship a standalone Slash MCU on 0x193b and never AniMe.
 * Keep this list conservative: unknown boards fall through to -ENODEV
 * when report size alone cannot decide.
 */
static bool asus_dmi_is_slash_only_board(void)
{
	const char *board = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board)
		return false;

	return strstr(board, "G614") || strstr(board, "G614J") ||
	       strstr(board, "G614JZ") || strstr(board, "G614JI") ||
	       strstr(board, "G614JV") || strstr(board, "G614PR");
}

static enum asus_anime_type asus_anime_type_from_dmi(void)
{
	const char *board = dmi_get_system_info(DMI_BOARD_NAME);

	if (!board)
		return ASUS_ANIME_UNKNOWN;

	if (strstr(board, "GA401I") || strstr(board, "GA401Q"))
		return ASUS_ANIME_GA401;
	if (strstr(board, "GA402R") || strstr(board, "GA402X") ||
	    strstr(board, "GA402N"))
		return ASUS_ANIME_GA402;
	if (strstr(board, "GU604V"))
		return ASUS_ANIME_GU604;
	if (strstr(board, "G635L"))
		return ASUS_ANIME_G635L;
	if (strstr(board, "G835L"))
		return ASUS_ANIME_G835L;

	return ASUS_ANIME_UNKNOWN;
}

/*
 * USB 0x193b is shared by Slash and AniMe Matrix. Discriminate in order:
 * 1) feature report 0x5e size (AniMe ~640 B, Slash Aura-sized)
 * 2) known DMI boards when size is ambiguous
 * 3) otherwise refuse to guess (-ENODEV / NONE)
 */
static enum asus_pid_193b_kind asus_classify_pid_193b(struct hid_device *hdev)
{
	size_t len_5e;
	bool anime_dmi = asus_dmi_is_anime_board();
	bool slash_dmi = asus_dmi_is_slash_only_board();

	if (hdev->product != USB_DEVICE_ID_ASUSTEK_ROG_SLASH)
		return ASUS_PID_193B_NONE;

	len_5e = asus_feature_report_len(hdev, FEATURE_KBD_LED_REPORT_ID2);

	if (len_5e >= ASUS_ANIME_PACKET_SIZE)
		return ASUS_PID_193B_ANIME;

	if (len_5e > 0 && len_5e < ASUS_ANIME_PACKET_SIZE) {
		if (anime_dmi)
			return ASUS_PID_193B_NONE;
		return ASUS_PID_193B_SLASH;
	}

	/* No usable 0x5e length: Slash may talk on 0x5d only. */
	if (asus_has_report_id(hdev, FEATURE_KBD_LED_REPORT_ID1)) {
		if (anime_dmi)
			return ASUS_PID_193B_NONE;
		return ASUS_PID_193B_SLASH;
	}

	if (anime_dmi)
		return ASUS_PID_193B_ANIME;
	if (slash_dmi)
		return ASUS_PID_193B_SLASH;

	return ASUS_PID_193B_NONE;
}

/*
 * Slash on Aura keyboards exposes report 0x5e. Standalone Slash MCU reuses
 * USB 0x193b and is accepted only when classify_pid_193b says SLASH.
 */
static bool asus_device_has_slash(struct hid_device *hdev)
{
	if (hdev->product == USB_DEVICE_ID_ASUSTEK_ROG_SLASH)
		return asus_classify_pid_193b(hdev) == ASUS_PID_193B_SLASH;

	return asus_has_report_id(hdev, FEATURE_KBD_LED_REPORT_ID2);
}

static int asus_kbd_register_leds(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct usb_interface *intf;
	struct usb_device *udev;
	unsigned char kbd_func;
	int ret;

	/* Get keyboard functions */
	ret = asus_kbd_get_functions(hdev, &kbd_func, FEATURE_KBD_REPORT_ID);
	if (ret < 0)
		return ret;

	/* Check for backlight support */
	if (!(kbd_func & SUPPORT_KBD_BACKLIGHT))
		return -ENODEV;

	if (dmi_match(DMI_PRODUCT_FAMILY, "ProArt P16")) {
		ret = asus_kbd_disable_oobe(hdev);
		if (ret < 0)
			return ret;
	}

	if ((drvdata->quirks & QUIRK_ROG_ALLY_XPAD) && hid_is_usb(hdev)) {
		intf = to_usb_interface(hdev->dev.parent);
		udev = interface_to_usbdev(intf);
		validate_mcu_fw_version(hdev,
			le16_to_cpu(udev->descriptor.idProduct));
	}

	drvdata->listener.brightness_set = asus_kbd_backlight_set;
	ret = asus_hid_register_listener(&drvdata->listener);
	if (ret < 0) {
		hid_err(hdev, "Unable to register kbd brightness listener: %d\n", ret);
		drvdata->listener.brightness_set = NULL;
	}

	return ret;
}

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)

/*
 * Missing transfer type. A timeout or I/O error keeps the cached path.
 * usbhid returns -ENOSYS when urbout is absent (no Interrupt OUT), which is
 * the common case on Aura N-KEY interfaces that only expose Interrupt IN.
 */
static bool asus_aura_path_missing(int ret)
{
	return ret == -ENOSYS || ret == -EOPNOTSUPP || ret == -EINVAL ||
	       ret == -EPIPE;
}

static u8 *asus_aura_write_slot(struct asus_drvdata *drvdata, u8 report_id)
{
	if (report_id == FEATURE_KBD_LED_REPORT_ID2)
		return &drvdata->aura_write_5e;

	return &drvdata->aura_write_5d;
}

static int asus_aura_try_path(struct asus_drvdata *drvdata, u8 path)
{
	switch (path) {
	case 1:
		if (!drvdata->hdev->ll_driver ||
		    !drvdata->hdev->ll_driver->output_report)
			return -EOPNOTSUPP;
		return hid_hw_output_report(drvdata->hdev, drvdata->aura_buf,
					    AURA_FEATURE_REPORT_SIZE);
	case 2:
		return hid_hw_raw_request(drvdata->hdev, drvdata->aura_buf[0],
					  drvdata->aura_buf,
					  AURA_FEATURE_REPORT_SIZE,
					  HID_OUTPUT_REPORT,
					  HID_REQ_SET_REPORT);
	case 3:
		return hid_hw_raw_request(drvdata->hdev, drvdata->aura_buf[0],
					  drvdata->aura_buf,
					  AURA_FEATURE_REPORT_SIZE,
					  HID_FEATURE_REPORT,
					  HID_REQ_SET_REPORT);
	default:
		return -EINVAL;
	}
}

static int asus_aura_scan_paths(struct asus_drvdata *drvdata, u8 *slot)
{
	static const u8 order[] = { 1, 2, 3 };
	unsigned int i;
	int ret = -EIO;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		ret = asus_aura_try_path(drvdata, order[i]);
		hid_info(drvdata->hdev, "DEBUG scan_paths: id=%02x path=%u ret=%d\n",
			 drvdata->aura_buf[0], order[i], ret);
		if (ret >= 0) {
			*slot = order[i];
			return 0;
		}
		if (!asus_aura_path_missing(ret))
			return ret;
	}

	return ret;
}

static int asus_aura_set_feature_unlocked(struct asus_drvdata *drvdata,
					  const u8 *buf, size_t buf_size)
{
	u8 *slot;
	int ret;

	if (buf_size > AURA_FEATURE_REPORT_SIZE)
		return -EINVAL;

	memcpy(drvdata->aura_buf, buf, buf_size);
	if (buf_size < AURA_FEATURE_REPORT_SIZE)
		memset(drvdata->aura_buf + buf_size, 0,
		       AURA_FEATURE_REPORT_SIZE - buf_size);

	/*
	 * Match Armoury Crate: interrupt OUT, then raw OUTPUT, then FEATURE.
	 * Remember the path that works for this report id. A later timeout
	 * does not abandon it; only a missing transfer type does.
	 */
	slot = asus_aura_write_slot(drvdata, buf[0]);
	if (*slot) {
		ret = asus_aura_try_path(drvdata, *slot);
		if (ret >= 0)
			return 0;
		if (!asus_aura_path_missing(ret))
			return ret;
		*slot = 0;
	}

	return asus_aura_scan_paths(drvdata, slot);
}

static int asus_aura_set_feature(struct asus_drvdata *drvdata,
				 const u8 *buf, size_t buf_size)
{
	guard(mutex)(&drvdata->aura_lock);

	return asus_aura_set_feature_unlocked(drvdata, buf, buf_size);
}

static int asus_aura_get_feature(struct asus_drvdata *drvdata,
				 u8 *buf, size_t buf_size)
{
	int ret;

	if (buf_size > AURA_FEATURE_REPORT_SIZE)
		return -EINVAL;

	guard(mutex)(&drvdata->aura_lock);

	memset(drvdata->aura_buf, 0, AURA_FEATURE_REPORT_SIZE);
	drvdata->aura_buf[0] = buf[0];

	ret = hid_hw_raw_request(drvdata->hdev, buf[0], drvdata->aura_buf,
				 AURA_FEATURE_REPORT_SIZE,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0)
		return ret;

	memcpy(buf, drvdata->aura_buf, min_t(size_t, buf_size, ret));
	return ret;
}

static int asus_aura_commit(struct asus_drvdata *drvdata)
{
	u8 buf_set[AURA_FEATURE_REPORT_SIZE] = {
		FEATURE_KBD_LED_REPORT_ID1,
		AURA_CMD_SET,
	};
	u8 buf_apply[AURA_FEATURE_REPORT_SIZE] = {
		FEATURE_KBD_LED_REPORT_ID1,
		AURA_CMD_COMMIT,
	};
	int ret;

	/*
	 * Apply staged 0xb3 effect programming:
	 * First send 0xb5 (AURA_CMD_SET) to latch parameters,
	 * then send 0xb4 (AURA_CMD_COMMIT) to apply them to hardware,
	 * then send 0xb5 (AURA_CMD_SET) to settle as captured in firmware traces.
	 */
	ret = asus_aura_set_feature(drvdata, buf_set, sizeof(buf_set));
	if (ret < 0)
		return ret;

	ret = asus_aura_set_feature(drvdata, buf_apply, sizeof(buf_apply));
	if (ret < 0)
		return ret;

	return asus_aura_set_feature(drvdata, buf_set, sizeof(buf_set));
}

static int asus_aura_query_run_mode(struct asus_drvdata *drvdata, u8 selector,
				    u8 *buf, size_t buf_size)
{
	u8 req[AURA_FEATURE_REPORT_SIZE] = {
		FEATURE_KBD_LED_REPORT_ID1,
		AURA_CMD_RUN_MODE,
		0x01,
		selector,
	};
	int ret;

	ret = asus_aura_set_feature(drvdata, req, sizeof(req));
	if (ret < 0)
		return ret;

	memset(buf, 0, buf_size);
	buf[0] = FEATURE_KBD_LED_REPORT_ID1;

	ret = asus_aura_get_feature(drvdata, buf, buf_size);
	if (ret < 0)
		return ret;

	if (ret < 5 || buf[1] != AURA_CMD_RUN_MODE || buf[2] != 0x01 ||
	    buf[3] != selector || buf[4] != 0x01)
		return -ENODATA;

	return ret;
}

static int asus_aura_get_effect_mask(struct asus_drvdata *drvdata, u8 effect_mask[2])
{
	u8 buf[AURA_FEATURE_REPORT_SIZE];
	int ret;

	ret = asus_aura_query_run_mode(drvdata, 0x20, buf, sizeof(buf));
	if (ret >= 22)
		goto found;

	ret = asus_aura_query_run_mode(drvdata, 0x15, buf, sizeof(buf));
	if (ret < 0)
		return ret;
	if (ret < 22)
		return -ENODATA;

found:
	effect_mask[0] = buf[20];
	effect_mask[1] = buf[21];

	return 0;
}

/*
 * Notebook 0x9e capability bits (Armoury Crate). hw_mode is the 0xb3 opcode;
 * cap_bit is the bit in bytes 20..21. They match for modes 0..8, then skip the
 * unused byte21 bit2: Pulse is mode 0x0a but cap bit 9, not BIT(0x0a).
 */
static const struct {
	const char *name;
	u8 hw_mode;
	u16 cap_bit;
} asus_aura_effect_map[] = {
	{ "static",		AURA_EFFECT_STATIC,		BIT(0) },
	{ "breathe",		AURA_EFFECT_BREATHING,		BIT(1) },
	{ "rainbow_cycle",	AURA_EFFECT_SPECTRUM_CYCLE,	BIT(2) },
	{ "rainbow_wave",	AURA_EFFECT_RAINBOW,		BIT(3) },
	{ "star",		AURA_EFFECT_STARS,		BIT(4) },
	{ "rain",		AURA_EFFECT_RAIN,		BIT(5) },
	{ "highlight",		AURA_EFFECT_REACTIVE,		BIT(6) },
	{ "laser",		AURA_EFFECT_LASER,		BIT(7) },
	{ "ripple",		AURA_EFFECT_RIPPLE,		BIT(8) },
	{ "pulse",		AURA_EFFECT_PULSE,		BIT(9) },
	{ "comet",		AURA_EFFECT_COMET,		BIT(11) },
	{ "flash",		AURA_EFFECT_FLASH,		BIT(12) },
};

static unsigned int asus_aura_fill_effects(const char **dst,
					   const u8 *effect_mask,
					   bool direct_capable)
{
	unsigned int i, n = 0;
	u16 mask = effect_mask ? (effect_mask[0] | ((u16)effect_mask[1] << 8))
			       : (u16)~0;

	for (i = 0; i < ARRAY_SIZE(asus_aura_effect_map); i++) {
		if (mask & asus_aura_effect_map[i].cap_bit)
			dst[n++] = asus_aura_effect_map[i].name;
	}
	if (direct_capable)
		dst[n++] = "direct";

	return n;
}

static int asus_aura_hw_mode_from_name(const char *name, u8 *aura_mode)
{
	unsigned int i;

	if (!name)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(asus_aura_effect_map); i++) {
		if (!strcmp(name, asus_aura_effect_map[i].name)) {
			*aura_mode = asus_aura_effect_map[i].hw_mode;
			return 0;
		}
	}

	return -EINVAL;
}

static bool asus_aura_hw_mode_known(u8 mode)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(asus_aura_effect_map); i++) {
		if (asus_aura_effect_map[i].hw_mode == mode)
			return true;
	}

	return false;
}

static bool asus_aura_speed_known(u8 speed)
{
	return speed == 0 || speed == AURA_SPEED_SLOW ||
	       speed == AURA_SPEED_MED || speed == AURA_SPEED_FAST;
}

static const u8 asus_aura_hw_speed[] = {
	[0] = AURA_SPEED_SLOW,
	[1] = AURA_SPEED_MED,
	[2] = AURA_SPEED_FAST,
};

static u8 asus_aura_speed_to_hw(unsigned int speed)
{
	if (speed < ARRAY_SIZE(asus_aura_hw_speed))
		return asus_aura_hw_speed[speed];

	return AURA_SPEED_MED;
}

static unsigned int asus_aura_speed_from_hw(u8 speed)
{
	if (speed == AURA_SPEED_SLOW)
		return 0;
	if (speed == AURA_SPEED_FAST)
		return 2;

	return 1;
}

/* Hardware direction byte for enum dl_direction. */
static const u8 asus_aura_hw_direction[] = {
	[DL_DIRECTION_LEFT]	= 1,
	[DL_DIRECTION_RIGHT]	= 0,
	[DL_DIRECTION_UP]	= 2,
	[DL_DIRECTION_DOWN]	= 3,
};

static u8 asus_aura_direction_to_hw(enum dl_direction direction)
{
	if (direction < ARRAY_SIZE(asus_aura_hw_direction))
		return asus_aura_hw_direction[direction];

	return asus_aura_hw_direction[DL_DIRECTION_RIGHT];
}

static enum dl_direction asus_aura_direction_from_hw(u8 direction)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(asus_aura_hw_direction); i++) {
		if (asus_aura_hw_direction[i] == direction)
			return i;
	}

	return DL_DIRECTION_RIGHT;
}

/*
 * Aura 0xb3 is write-only. Read the feature report before any SET in probe:
 * some firmware still holds the last committed Set Effect packet. Adopt that
 * into the software node when the payload is a known Set Effect. Never write
 * a color from probe: userspace owns the first commit.
 */
static void asus_aura_capture_boot_effect(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	u8 buf[AURA_FEATURE_REPORT_SIZE];
	int ret;

	memset(buf, 0, sizeof(buf));
	buf[0] = FEATURE_KBD_LED_REPORT_ID1;
	ret = asus_aura_get_feature(drvdata, buf, sizeof(buf));
	if (ret < 0)
		return;

	if (ret < 13 || buf[1] != AURA_CMD_SET_EFFECT ||
	    buf[2] > AURA_ZONE_BAR_RIGHT ||
	    !asus_aura_hw_mode_known(buf[3]) ||
	    !asus_aura_speed_known(buf[7]))
		return;

	drvdata->boot_effect_valid = true;
	drvdata->boot_zone = buf[2];
	drvdata->boot_mode = buf[3];
	drvdata->boot_r = buf[4];
	drvdata->boot_g = buf[5];
	drvdata->boot_b = buf[6];
	drvdata->boot_speed = buf[7];
	drvdata->boot_direction = buf[8];
	drvdata->boot_r2 = buf[10];
	drvdata->boot_g2 = buf[11];
	drvdata->boot_b2 = buf[12];
}

static int asus_aura_activate_zone_unlocked(struct asus_drvdata *drvdata, u8 zone)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE] = { 0 };

	buf[0] = FEATURE_KBD_LED_REPORT_ID1;
	buf[1] = AURA_CMD_ZONE_ENABLE;
	buf[2] = zone;
	buf[3] = 0x01;
	buf[4] = 0x01;

	return asus_aura_set_feature_unlocked(drvdata, buf, sizeof(buf));
}

static int asus_aura_activate_zone(struct asus_drvdata *drvdata, u8 zone)
{
	guard(mutex)(&drvdata->aura_lock);

	return asus_aura_activate_zone_unlocked(drvdata, zone);
}

static int asus_aura_activate_zones(struct asus_drvdata *drvdata)
{
	int ret;

	ret = asus_aura_activate_zone(drvdata, AURA_ZONE_ACTIVATE_KEYBOARD);
	if (ret < 0)
		return ret;
	if (drvdata->has_lightbar)
		return asus_aura_activate_zone(drvdata, AURA_ZONE_ACTIVATE_LIGHTBAR);
	return 0;
}

static int asus_aura_wake_all_zones(struct asus_drvdata *drvdata)
{
	u8 buf_pwr[AURA_FEATURE_REPORT_SIZE] = {
		FEATURE_KBD_LED_REPORT_ID1,
		AURA_CMD_POWER,
		AURA_POWER_CMD_ENABLE,
		AURA_POWER_MASK_KBD_LOGO_ALL,
		AURA_POWER_MASK_LIGHTBAR_ALL,
		AURA_POWER_MASK_LID_ALL,
		AURA_POWER_MASK_REAR_ALL,
		0x00,
	};
	int ret;

	/* Unmute power gating across keyboard, lightbar, logo, lid, and rear-glow */
	ret = asus_aura_set_feature(drvdata, buf_pwr, sizeof(buf_pwr));
	if (ret < 0)
		return ret;

	/* Activate keyboard zone */
	ret = asus_aura_activate_zone(drvdata, AURA_ZONE_ACTIVATE_KEYBOARD);
	if (ret < 0)
		return ret;

	if (!drvdata->has_lightbar)
		return 0;

	return asus_aura_activate_zone(drvdata, AURA_ZONE_ACTIVATE_LIGHTBAR);
}

static int asus_aura_apply_effect(struct led_classdev_dynamic *ldev);

static void asus_aura_mark_effect_set(struct asus_drvdata *drvdata,
				      struct led_classdev_dynamic *ldev)
{
	if (ldev == &drvdata->dldev_kbd)
		drvdata->kbd_effect_set = true;
	else if (ldev == &drvdata->dldev_lightbar)
		drvdata->lightbar_effect_set = true;
}

static int asus_aura_write_zone_effect(struct asus_drvdata *drvdata, u8 zone,
				       u8 aura_mode, u8 r, u8 g, u8 b,
				       u8 speed, u8 direction,
				       u8 r2, u8 g2, u8 b2,
				       bool commit)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE] = { 0 };
	int ret;

	if (zone == AURA_ZONE_BAR_LEFT || zone == AURA_ZONE_BAR_RIGHT) {
		ret = asus_aura_activate_zone(drvdata, AURA_ZONE_ACTIVATE_LIGHTBAR);
		if (ret < 0)
			return ret;
	}

	buf[0] = FEATURE_KBD_LED_REPORT_ID1;
	buf[1] = AURA_CMD_SET_EFFECT;
	buf[2] = zone;
	buf[3] = aura_mode;
	buf[4] = r;
	buf[5] = g;
	buf[6] = b;
	buf[7] = speed;
	buf[8] = direction;
	buf[9] = 0x00;
	buf[10] = r2;
	buf[11] = g2;
	buf[12] = b2;

	ret = asus_aura_set_feature(drvdata, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	if (commit)
		return asus_aura_commit(drvdata);

	return 0;
}

static size_t asus_lamparray_report_len(struct hid_device *hdev, u8 id)
{
	struct hid_report *report;

	report = hdev->report_enum[HID_FEATURE_REPORT].report_id_hash[id];
	if (!report)
		return 0;
	return hid_report_len(report);
}

static u8 asus_lamparray_rid(struct asus_drvdata *drvdata, u8 offset)
{
	return drvdata->lamparray_rid_base + offset;
}

static int asus_lamparray_raw(struct asus_drvdata *drvdata, u8 *buf, size_t len,
			      bool get)
{
	int ret;

	if (!drvdata->lamparray_hdev || !len)
		return -ENODEV;

	ret = hid_hw_raw_request(drvdata->lamparray_hdev, buf[0], buf, len,
				 HID_FEATURE_REPORT,
				 get ? HID_REQ_GET_REPORT : HID_REQ_SET_REPORT);
	return ret < 0 ? ret : 0;
}

static void asus_lamparray_prepare(struct asus_drvdata *drvdata, u8 rid)
{
	memset(drvdata->lamparray_buf, 0, drvdata->lamparray_buf_len);
	drvdata->lamparray_buf[0] = rid;
}

static struct hid_device *asus_find_lamparray_sibling(struct hid_device *hdev)
{
	struct usb_interface *intf;
	struct usb_device *udev;
	struct usb_host_config *config;
	unsigned int i;

	if (!hid_is_usb(hdev))
		return NULL;

	intf = to_usb_interface(hdev->dev.parent);
	udev = interface_to_usbdev(intf);
	if (!udev->actconfig)
		return NULL;

	config = udev->actconfig;
	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		struct usb_interface *other = config->interface[i];
		struct hid_device *other_hdev;

		if (!other || other == intf)
			continue;
		if (!other->dev.driver || other->dev.driver != intf->dev.driver)
			continue;

		other_hdev = usb_get_intfdata(other);
		if (!other_hdev)
			continue;
		if (other_hdev->vendor != hdev->vendor ||
		    other_hdev->product != hdev->product)
			continue;
		if (asus_is_lamparray_interface(other_hdev))
			return other_hdev;
	}

	return NULL;
}

static void asus_lamparray_unbind_from_owners(struct hid_device *la_hdev)
{
	struct usb_interface *intf;
	struct usb_device *udev;
	struct usb_host_config *config;
	unsigned int i;

	if (!hid_is_usb(la_hdev))
		return;

	intf = to_usb_interface(la_hdev->dev.parent);
	udev = interface_to_usbdev(intf);
	if (!udev->actconfig)
		return;

	config = udev->actconfig;
	for (i = 0; i < config->desc.bNumInterfaces; i++) {
		struct usb_interface *other = config->interface[i];
		struct hid_device *other_hdev;
		struct asus_drvdata *owner;

		if (!other || other == intf)
			continue;
		if (!other->dev.driver || other->dev.driver != intf->dev.driver)
			continue;

		other_hdev = usb_get_intfdata(other);
		if (!other_hdev || other_hdev->driver != la_hdev->driver)
			continue;

		owner = hid_get_drvdata(other_hdev);
		if (!owner)
			continue;

		mutex_lock(&owner->aura_lock);
		if (owner->lamparray_hdev == la_hdev) {
			owner->lamparray_controlled = false;
			owner->lamparray_count = 0;
			owner->lamparray_lamps = NULL;
			owner->lamparray_hdev = NULL;
			owner->lamparray_unavailable = true;
			mutex_unlock(&owner->aura_lock);
			put_device(&la_hdev->dev);
			return;
		}
		mutex_unlock(&owner->aura_lock);
	}
}

static int asus_lamparray_cmp_x(const void *a, const void *b)
{
	const struct asus_lamparray_lamp *la = a;
	const struct asus_lamparray_lamp *lb = b;

	return cmp_int(la->x, lb->x);
}

static int asus_lamparray_init_from_hdev(struct asus_drvdata *drvdata,
					 struct hid_device *la_hdev)
{
	u8 *buf;
	size_t attr_len, req_len, resp_len, multi_len, ctrl_len, max_len;
	unsigned int count, i;
	int base, ret;

	base = asus_lamparray_detect_base(la_hdev);
	if (base < 0)
		return base;

	attr_len = asus_lamparray_report_len(la_hdev, base + ASUS_LAMPARRAY_RID_ATTR);
	req_len = asus_lamparray_report_len(la_hdev, base + ASUS_LAMPARRAY_RID_REQUEST);
	resp_len = asus_lamparray_report_len(la_hdev, base + ASUS_LAMPARRAY_RID_RESPONSE);
	multi_len = asus_lamparray_report_len(la_hdev, base + ASUS_LAMPARRAY_RID_MULTI);
	ctrl_len = asus_lamparray_report_len(la_hdev, base + ASUS_LAMPARRAY_RID_CONTROL);
	max_len = max3(max(attr_len, req_len), max(resp_len, multi_len), ctrl_len);
	if (attr_len < 3 || req_len < 3 || resp_len < 23 || multi_len < 51 ||
	    ctrl_len < 2 || !max_len)
		return -EPROTO;

	buf = devm_kzalloc(&drvdata->hdev->dev, max_len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	drvdata->lamparray_hdev = la_hdev;
	drvdata->lamparray_buf = buf;
	drvdata->lamparray_buf_len = max_len;
	drvdata->lamparray_rid_base = base;

	asus_lamparray_prepare(drvdata, base + ASUS_LAMPARRAY_RID_ATTR);
	ret = asus_lamparray_raw(drvdata, buf, attr_len, true);
	if (ret < 0)
		return ret;

	count = get_unaligned_le16(buf + 1);
	if (!count || count > ASUS_LAMPARRAY_MAX_LAMPS)
		return -EPROTO;

	drvdata->lamparray_lamps = devm_kcalloc(&drvdata->hdev->dev, count,
						sizeof(*drvdata->lamparray_lamps),
						GFP_KERNEL);
	if (!drvdata->lamparray_lamps)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		u32 purposes;

		asus_lamparray_prepare(drvdata, base + ASUS_LAMPARRAY_RID_REQUEST);
		put_unaligned_le16(i, buf + 1);
		ret = asus_lamparray_raw(drvdata, buf, req_len, false);
		if (ret < 0)
			return ret;

		asus_lamparray_prepare(drvdata, base + ASUS_LAMPARRAY_RID_RESPONSE);
		ret = asus_lamparray_raw(drvdata, buf, resp_len, true);
		if (ret < 0)
			return ret;

		/* LampId@1, PositionX@3, LampPurposes@19 (HID Lighting) */
		drvdata->lamparray_lamps[i].id = get_unaligned_le16(buf + 1);
		drvdata->lamparray_lamps[i].x = (s32)get_unaligned_le32(buf + 3);
		purposes = get_unaligned_le32(buf + 19);
		drvdata->lamparray_lamps[i].keyboard =
			!!(purposes & ASUS_LAMPARRAY_PURPOSE_CONTROL);
	}

	drvdata->lamparray_count = count;
	sort(drvdata->lamparray_lamps, count, sizeof(*drvdata->lamparray_lamps),
	     asus_lamparray_cmp_x, NULL);
	get_device(&la_hdev->dev);
	hid_info(drvdata->hdev,
		 "LampArray direct RGB backend (%u lamps, rid_base=0x%02x)\n",
		 count, base);
	return 0;
}

static void asus_lamparray_try_init_unlocked(struct asus_drvdata *drvdata)
{
	struct hid_device *sibling;
	int ret;

	if (drvdata->lamparray_count || drvdata->lamparray_unavailable)
		return;

	sibling = asus_find_lamparray_sibling(drvdata->hdev);
	if (!sibling)
		return;

	ret = asus_lamparray_init_from_hdev(drvdata, sibling);
	if (ret < 0) {
		hid_warn(drvdata->hdev, "LampArray init failed: %d\n", ret);
		drvdata->lamparray_hdev = NULL;
		drvdata->lamparray_lamps = NULL;
		drvdata->lamparray_count = 0;
		drvdata->lamparray_unavailable = true;
	}
}

static int asus_lamparray_set_control_unlocked(struct asus_drvdata *drvdata,
					       bool autonomous)
{
	u8 rid = asus_lamparray_rid(drvdata, ASUS_LAMPARRAY_RID_CONTROL);
	size_t len = asus_lamparray_report_len(drvdata->lamparray_hdev, rid);

	if (len < 2)
		return -EPROTO;

	asus_lamparray_prepare(drvdata, rid);
	drvdata->lamparray_buf[1] = autonomous ? 0x01 : 0x00;
	return asus_lamparray_raw(drvdata, drvdata->lamparray_buf, len, false);
}

static int asus_lamparray_aura_handoff_unlocked(struct asus_drvdata *drvdata,
						u8 zone, bool release)
{
	u8 aura[AURA_FEATURE_REPORT_SIZE] = {
		FEATURE_KBD_LED_REPORT_ID1,
		AURA_CMD_ZONE_ENABLE,
		zone,
		0x01,
	};

	if (release)
		aura[4] = 0x01;

	return asus_aura_set_feature_unlocked(drvdata, aura, sizeof(aura));
}

static int asus_lamparray_take_control_unlocked(struct asus_drvdata *drvdata)
{
	int ret;

	if (drvdata->lamparray_controlled)
		return 0;

	ret = asus_lamparray_aura_handoff_unlocked(drvdata,
						   AURA_ZONE_ACTIVATE_LAMPARRAY,
						   false);
	if (ret < 0)
		return ret;

	/* Match G-Helper: pulse then clear AutonomousMode for host control */
	ret = asus_lamparray_set_control_unlocked(drvdata, true);
	if (ret < 0)
		return ret;
	ret = asus_lamparray_set_control_unlocked(drvdata, false);
	if (ret < 0)
		return ret;

	drvdata->lamparray_controlled = true;
	return 0;
}

static void asus_lamparray_release_unlocked(struct asus_drvdata *drvdata)
{
	if (!drvdata->lamparray_count || !drvdata->lamparray_controlled)
		return;

	asus_lamparray_set_control_unlocked(drvdata, true);
	asus_lamparray_aura_handoff_unlocked(drvdata,
					     AURA_ZONE_RELEASE_LAMPARRAY,
					     true);
	drvdata->lamparray_controlled = false;
}

static void asus_lamparray_sample_rgb(const u8 *buf, unsigned int nleds,
				      unsigned int idx, unsigned int n,
				      u8 *r, u8 *g, u8 *b)
{
	unsigned int zi;

	if (!buf || !nleds || !n) {
		*r = *g = *b = 0;
		return;
	}

	zi = (idx * nleds) / n;
	if (zi >= nleds)
		zi = nleds - 1;
	*r = buf[zi * 3];
	*g = buf[zi * 3 + 1];
	*b = buf[zi * 3 + 2];
}

static void asus_lamparray_fill_solid(u8 *buf, unsigned int nleds,
				      u8 r, u8 g, u8 b)
{
	unsigned int i;

	for (i = 0; i < nleds; i++) {
		buf[i * 3 + 0] = r;
		buf[i * 3 + 1] = g;
		buf[i * 3 + 2] = b;
	}
}

static unsigned int asus_aura_lb_led_count(struct asus_drvdata *drvdata)
{
	return drvdata->is_strix_4zone ? ROG_STRIX_4ZONE_LIGHTBAR_LEDS :
					 ROG_STRIX_LIGHTBAR_LEDS;
}

/*
 * Update only the lamps for one zone. MultiUpdate lists those lamp ids, so
 * the other zone keeps whatever the controller already shows. The direct
 * buffers start empty; they are not prefilled red.
 */
static int asus_lamparray_apply_unlocked(struct asus_drvdata *drvdata,
					 bool keyboard)
{
	unsigned int match[ASUS_LAMPARRAY_MAX_LAMPS];
	unsigned int nmatch = 0, idx = 0, i;
	unsigned int src_leds;
	const u8 *src;
	u8 *buf = drvdata->lamparray_buf;
	u8 rid_multi = asus_lamparray_rid(drvdata, ASUS_LAMPARRAY_RID_MULTI);
	size_t multi_len;
	int ret;

	for (i = 0; i < drvdata->lamparray_count; i++) {
		if (drvdata->lamparray_lamps[i].keyboard == keyboard)
			match[nmatch++] = i;
	}
	/*
	 * No lamps for this zone: let callers fall back to Aura 0xBC.
	 * Check before take_control so an empty match does not claim the
	 * LampArray sibling and then report success.
	 */
	if (!nmatch)
		return -ENODEV;

	ret = asus_lamparray_take_control_unlocked(drvdata);
	if (ret < 0)
		return ret;

	src = keyboard ? drvdata->kbd_direct_buf : drvdata->lb_direct_buf;
	src_leds = keyboard ? ROG_STRIX_4ZONE_KBD_LEDS :
			      asus_aura_lb_led_count(drvdata);

	multi_len = asus_lamparray_report_len(drvdata->lamparray_hdev, rid_multi);
	if (multi_len < 51)
		return -EPROTO;

	for (i = 0; i < nmatch; i += ASUS_LAMPARRAY_MULTI_MAX) {
		unsigned int n = min_t(unsigned int, ASUS_LAMPARRAY_MULTI_MAX,
				       nmatch - i);
		unsigned int j;
		unsigned int id_off = 3;
		unsigned int col_off = 3 + ASUS_LAMPARRAY_MULTI_MAX * 2;

		asus_lamparray_prepare(drvdata, rid_multi);
		buf[1] = n;
		buf[2] = (i + n >= nmatch) ? ASUS_LAMPARRAY_FLAG_COMPLETE : 0;

		for (j = 0; j < n; j++) {
			unsigned int lamp = match[i + j];
			u8 r, g, b;
			u16 id = drvdata->lamparray_lamps[lamp].id;

			asus_lamparray_sample_rgb(src, src_leds, idx++, nmatch,
						  &r, &g, &b);
			put_unaligned_le16(id, buf + id_off + j * 2);
			buf[col_off + j * 4 + 0] = r;
			buf[col_off + j * 4 + 1] = g;
			buf[col_off + j * 4 + 2] = b;
			buf[col_off + j * 4 + 3] = 0xff;
		}

		ret = asus_lamparray_raw(drvdata, buf, multi_len, false);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/* Prefer LampArray; callers keep Aura 0xBC as fallback on -ENODEV. */
static int asus_lamparray_try_apply_unlocked(struct asus_drvdata *drvdata,
					     bool keyboard)
{
	asus_lamparray_try_init_unlocked(drvdata);
	if (!drvdata->lamparray_count)
		return -ENODEV;
	return asus_lamparray_apply_unlocked(drvdata, keyboard);
}

static int asus_aura_write_4zone_direct_bc_unlocked(struct asus_drvdata *drvdata)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE];

	memset(buf, 0, sizeof(buf));
	buf[0] = FEATURE_KBD_LED_REPORT_ID1;
	buf[1] = AURA_CMD_DIRECT;
	buf[2] = AURA_DIRECT_FRAME_ZONED;
	buf[3] = AURA_DIRECT_ROUTING_DEFAULT;
	buf[4] = AURA_ZONE_LIGHTBAR_CHANNEL;
	if (drvdata->kbd_direct_valid)
		memcpy(&buf[ROG_STRIX_4ZONE_DIRECT_KBD_OFFSET],
		       drvdata->kbd_direct_buf,
		       sizeof(drvdata->kbd_direct_buf));
	if (drvdata->lb_direct_valid)
		memcpy(&buf[ROG_STRIX_4ZONE_DIRECT_LB_OFFSET],
		       drvdata->lb_direct_buf,
		       ROG_STRIX_4ZONE_LIGHTBAR_BUF_SIZE);

	return asus_aura_set_feature_unlocked(drvdata, buf, sizeof(buf));
}

/*
 * Push a cached direct-RGB frame for one zone. LampArray MultiUpdate keeps the
 * sibling zone untouched; the 4-zone 0xBC fallback rewrites both zone slots
 * from the cached buffers.
 */
static int asus_aura_push_direct_unlocked(struct asus_drvdata *drvdata,
					  bool keyboard)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE];
	size_t payload_len;
	int ret;

	if (keyboard) {
		if (!drvdata->kbd_direct_valid)
			return 0;
	} else if (!drvdata->lb_direct_valid) {
		return 0;
	}

	ret = asus_lamparray_try_apply_unlocked(drvdata, keyboard);
	if (ret != -ENODEV)
		return ret;

	if (drvdata->is_strix_4zone)
		return asus_aura_write_4zone_direct_bc_unlocked(drvdata);

	/* Per-key keyboard frames are not retained in kbd_direct_buf. */
	if (keyboard)
		return 0;

	payload_len = asus_aura_lb_led_count(drvdata) * 3;
	payload_len = min_t(size_t, payload_len, ROG_STRIX_LIGHTBAR_BUF_SIZE);

	memset(buf, 0, sizeof(buf));
	buf[0] = FEATURE_KBD_LED_REPORT_ID1;
	buf[1] = AURA_CMD_DIRECT;
	buf[2] = AURA_DIRECT_FRAME_PERKEY;
	buf[3] = AURA_DIRECT_ROUTING_DEFAULT;
	buf[4] = AURA_ZONE_LIGHTBAR_CHANNEL;
	buf[5] = AURA_DIRECT_CHUNK_FLAG;
	buf[6] = 0x00;
	buf[7] = (u8)(payload_len / 3);
	memcpy(&buf[ROG_STRIX_PERKEY_DIRECT_PAYLOAD_OFFSET],
	       drvdata->lb_direct_buf, payload_len);

	return asus_aura_set_feature_unlocked(drvdata, buf, sizeof(buf));
}

static int asus_aura_strix_write_direct(struct asus_drvdata *drvdata,
					const u8 *buffer, size_t size)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE];
	unsigned int i;
	int ret;

	guard(mutex)(&drvdata->aura_lock);

	if (drvdata->is_strix_4zone) {
		unsigned int leds = min_t(size_t, size / 3,
					  ROG_STRIX_4ZONE_KBD_LEDS);

		if (buffer != drvdata->kbd_direct_buf)
			memcpy(drvdata->kbd_direct_buf, buffer, leds * 3);
		drvdata->kbd_direct_valid = true;

		ret = asus_lamparray_try_apply_unlocked(drvdata, true);
		if (ret != -ENODEV)
			return ret;

		return asus_aura_write_4zone_direct_bc_unlocked(drvdata);
	}

	/*
	 * Stream ROG Strix per-key matrix in 16-LED chunks using
	 * Aura HID Feature Reports with opcode 0xbc:
	 * [0]    = Report ID (0x5d)
	 * [1]    = Direct frame command (0xbc)
	 * [2..5] = Routing header (0x00, 0x01, 0x01, 0x01)
	 * [6]    = Start LED index (0, 16, 32, ..., 160)
	 * [7]    = Number of LEDs in chunk (16 for chunks 0..9, 8 for chunk 10)
	 * [8]    = Reserved / 0x00
	 * [9..]  = RGB payload (3 bytes per LED)
	 *
	 * Total LEDs: 168 (11 packets). Strictly terminate at packet 10;
	 * sending a 12th packet triggers a firmware defect that shuts off the
	 * rear and front lightbars. No 0xb4 commit command is issued for raw
	 * direct frames to avoid stepping hardware animation registers.
	 */
	for (i = 0; i < ROG_STRIX_DIRECT_LEDS; i += ROG_STRIX_LEDS_PER_PKT) {
		unsigned int leds = min_t(unsigned int, ROG_STRIX_DIRECT_LEDS - i,
					  ROG_STRIX_LEDS_PER_PKT);
		size_t payload_len = leds * 3;

		memset(buf, 0, sizeof(buf));
		buf[0] = FEATURE_KBD_LED_REPORT_ID1;
		buf[1] = AURA_CMD_DIRECT;
		buf[2] = AURA_DIRECT_FRAME_PERKEY;
		buf[3] = AURA_DIRECT_ROUTING_DEFAULT;
		buf[4] = AURA_ZONE_KEYBOARD_CHANNEL;
		buf[5] = AURA_DIRECT_CHUNK_FLAG;
		buf[6] = (u8)i;
		buf[7] = (u8)leds;
		buf[8] = 0x00;
		memcpy(&buf[ROG_STRIX_PERKEY_DIRECT_PAYLOAD_OFFSET],
		       buffer + (i * 3), payload_len);

		ret = asus_aura_set_feature_unlocked(drvdata, buf, sizeof(buf));
		if (ret < 0)
			return ret;
	}

	return 0;
}

static bool asus_aura_is_lightbar(struct asus_drvdata *drvdata,
				  struct led_classdev_dynamic *ldev)
{
	return ldev == &drvdata->dldev_lightbar;
}

static int asus_aura_write_zone_range(struct asus_drvdata *drvdata,
				      u8 zone_first, u8 zone_last,
				      u8 aura_effect, u8 r, u8 g, u8 b,
				      u8 speed, u8 direction,
				      u8 r2, u8 g2, u8 b2)
{
	u8 z;
	int ret;

	for (z = zone_first; z <= zone_last; z++) {
		ret = asus_aura_write_zone_effect(drvdata, z, aura_effect,
						  r, g, b, speed, direction,
						  r2, g2, b2, false);
		if (ret < 0)
			return ret;
	}

	return asus_aura_commit(drvdata);
}

static int asus_aura_strix_set_direct(struct led_classdev_dynamic *ldev,
				      const u8 *buffer, size_t size)
{
	struct asus_drvdata *drvdata = ldev->driver_data;
	int ret;

	if (size != ldev->led_count * 3)
		return -EINVAL;

	if (!led_dynamic_effect_is(ldev, "direct")) {
		ret = asus_aura_activate_zone(drvdata, AURA_ZONE_ACTIVATE_KEYBOARD);
		if (ret < 0)
			return ret;
	}

	return asus_aura_strix_write_direct(drvdata, buffer, size);
}

static int asus_aura_lightbar_write_packet(struct asus_drvdata *drvdata,
					   const u8 *buffer, size_t size)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE];
	size_t payload_len;
	int ret;

	guard(mutex)(&drvdata->aura_lock);

	if (drvdata->is_strix_4zone) {
		unsigned int leds = min_t(size_t, size / 3,
					  ROG_STRIX_4ZONE_LIGHTBAR_LEDS);

		if (buffer != drvdata->lb_direct_buf)
			memcpy(drvdata->lb_direct_buf, buffer, leds * 3);
		drvdata->lb_direct_valid = true;

		ret = asus_lamparray_try_apply_unlocked(drvdata, false);
		if (ret != -ENODEV)
			return ret;

		return asus_aura_write_4zone_direct_bc_unlocked(drvdata);
	}

	payload_len = min_t(size_t, size, ROG_STRIX_LIGHTBAR_BUF_SIZE);

	/*
	 * Per-Key models use buf[2] = 0x00 (chunked packet), so the MCU
	 * expects valid chunk headers in bytes 5-7. Without them it reads
	 * "0 LEDs to update" and silently drops the packet.
	 */
	memset(buf, 0, sizeof(buf));
	buf[0] = FEATURE_KBD_LED_REPORT_ID1;
	buf[1] = AURA_CMD_DIRECT;
	buf[2] = AURA_DIRECT_FRAME_PERKEY;
	buf[3] = AURA_DIRECT_ROUTING_DEFAULT;
	buf[4] = AURA_ZONE_LIGHTBAR_CHANNEL;
	buf[5] = AURA_DIRECT_CHUNK_FLAG;
	buf[6] = 0x00;             /* start LED index */
	buf[7] = (u8)(payload_len / 3); /* number of LEDs in this packet */

	if (buffer != drvdata->lb_direct_buf)
		memcpy(drvdata->lb_direct_buf, buffer, payload_len);
	drvdata->lb_direct_valid = true;
	memcpy(&buf[ROG_STRIX_PERKEY_DIRECT_PAYLOAD_OFFSET],
	       drvdata->lb_direct_buf, payload_len);

	return asus_aura_set_feature_unlocked(drvdata, buf, sizeof(buf));
}

static int asus_aura_lightbar_set_direct(struct led_classdev_dynamic *ldev,
					 const u8 *buffer, size_t size)
{
	struct asus_drvdata *drvdata = ldev->driver_data;
	int ret;

	if (size != ldev->led_count * 3)
		return -EINVAL;

	if (!led_dynamic_effect_is(ldev, "direct")) {
		ret = asus_aura_activate_zone(drvdata, AURA_ZONE_ACTIVATE_LIGHTBAR);
		if (ret < 0)
			return ret;
	}

	return asus_aura_lightbar_write_packet(drvdata, buffer, size);
}

/*
 * Commit a firmware effect with the color stored in the palette. Brightness
 * and enabled do not scale or clear this packet: 0xb3 is what the controller
 * keeps across reboot, so it must stay the unscaled color.
 */
static bool asus_aura_zone_wants_direct(struct asus_drvdata *drvdata,
					bool keyboard)
{
	struct led_classdev_dynamic *ldev = keyboard ? &drvdata->dldev_kbd :
						      &drvdata->dldev_lightbar;
	bool registered = keyboard ? drvdata->has_dldev_kbd :
				     drvdata->has_dldev_lightbar;

	return registered && ldev->enabled &&
	       led_dynamic_effect_is(ldev, "direct");
}

static int asus_aura_commit_effect(struct led_classdev_dynamic *ldev,
				   const char *name, unsigned int speed,
				   enum dl_direction direction,
				   const struct dl_rgb *palette,
				   unsigned int num_entries)
{
	struct asus_drvdata *drvdata = ldev->driver_data;
	bool lightbar = asus_aura_is_lightbar(drvdata, ldev);
	u8 aura_mode;
	u8 r = 0, g = 0, b = 0;
	u8 r2 = 0, g2 = 0, b2 = 0;
	int ret;

	if (name && !strcmp(name, "direct")) {
		guard(mutex)(&drvdata->aura_lock);

		return asus_aura_push_direct_unlocked(drvdata, !lightbar);
	}

	if (asus_aura_hw_mode_from_name(name, &aura_mode))
		return -EINVAL;

	if (palette && num_entries > 0) {
		r = palette[0].r;
		g = palette[0].g;
		b = palette[0].b;
		if (num_entries > 1) {
			r2 = palette[1].r;
			g2 = palette[1].g;
			b2 = palette[1].b;
		}
	}

	/*
	 * Static and animations are firmware effects: commit them with 0xb3
	 * so the controller keeps the color across reboot. LampArray stays
	 * the direct-RGB backend and must be released first. Re-apply the
	 * sibling zone afterwards if it is still in host direct mode.
	 */
	scoped_guard(mutex, &drvdata->aura_lock) {
		asus_lamparray_try_init_unlocked(drvdata);
		if (drvdata->lamparray_count)
			asus_lamparray_release_unlocked(drvdata);
	}

	if (lightbar)
		ret = asus_aura_write_zone_range(drvdata, AURA_ZONE_BAR_LEFT,
						 AURA_ZONE_BAR_RIGHT, aura_mode,
						 r, g, b,
						 asus_aura_speed_to_hw(speed),
						 asus_aura_direction_to_hw(direction),
						 r2, g2, b2);
	else if (drvdata->is_strix_4zone)
		ret = asus_aura_write_zone_range(drvdata, AURA_ZONE_KEY1,
						 AURA_ZONE_KEY4, aura_mode,
						 r, g, b,
						 asus_aura_speed_to_hw(speed),
						 asus_aura_direction_to_hw(direction),
						 r2, g2, b2);
	else
		ret = asus_aura_write_zone_effect(drvdata, AURA_ZONE_ALL,
						  aura_mode, r, g, b,
						  asus_aura_speed_to_hw(speed),
						  asus_aura_direction_to_hw(direction),
						  r2, g2, b2, true);
	if (ret < 0)
		return ret;

	/*
	 * Release above drops LampArray for every zone. If the sibling is
	 * still in direct mode, push its cached frame again so host control
	 * is not lost when this zone switches to a firmware effect.
	 */
	scoped_guard(mutex, &drvdata->aura_lock) {
		if (asus_aura_zone_wants_direct(drvdata, lightbar)) {
			ret = asus_aura_push_direct_unlocked(drvdata, lightbar);
			if (ret < 0)
				return ret;
		}
	}

	asus_aura_mark_effect_set(drvdata, ldev);
	return 0;
}

static int asus_aura_apply_effect(struct led_classdev_dynamic *ldev)
{
	return asus_aura_commit_effect(ldev, led_dynamic_effect_name(ldev),
				       ldev->speed, ldev->direction,
				       ldev->palette, ldev->num_palette_entries);
}

static void asus_aura_restore(struct asus_drvdata *drvdata)
{
	int ret;

	if (!drvdata->kbd_effect_set && !drvdata->lightbar_effect_set &&
	    !drvdata->kbd_direct_valid && !drvdata->lb_direct_valid)
		return;

	ret = asus_aura_activate_zones(drvdata);
	if (ret < 0)
		hid_warn(drvdata->hdev, "Failed to activate Aura zones: %d\n", ret);

	if (drvdata->has_dldev_kbd && drvdata->dldev_kbd.enabled &&
	    ((led_dynamic_effect_is(&drvdata->dldev_kbd, "direct") &&
	      drvdata->kbd_direct_valid) ||
	     (!led_dynamic_effect_is(&drvdata->dldev_kbd, "direct") &&
	      drvdata->kbd_effect_set))) {
		ret = asus_aura_apply_effect(&drvdata->dldev_kbd);
		if (ret < 0)
			hid_warn(drvdata->hdev,
				 "Failed to restore keyboard effect: %d\n", ret);
	}
	if (drvdata->has_dldev_lightbar && drvdata->dldev_lightbar.enabled &&
	    ((led_dynamic_effect_is(&drvdata->dldev_lightbar, "direct") &&
	      drvdata->lb_direct_valid) ||
	     (!led_dynamic_effect_is(&drvdata->dldev_lightbar, "direct") &&
	      drvdata->lightbar_effect_set))) {
		ret = asus_aura_apply_effect(&drvdata->dldev_lightbar);
		if (ret < 0)
			hid_warn(drvdata->hdev,
				 "Failed to restore lightbar effect: %d\n", ret);
	}
}

static int asus_aura_set_effect(struct led_classdev_dynamic *ldev,
				unsigned int index)
{
	if (!ldev->enabled)
		return 0;

	if (index >= ldev->num_effects)
		return -EINVAL;

	return asus_aura_commit_effect(ldev, ldev->effects[index],
				       ldev->speed, ldev->direction,
				       ldev->palette, ldev->num_palette_entries);
}

static int asus_aura_sync_power(struct asus_drvdata *drvdata,
				struct led_classdev_dynamic *which,
				bool enabled, u32 states);

static int asus_aura_set_enabled(struct led_classdev_dynamic *ldev, bool enabled)
{
	struct asus_drvdata *drvdata = ldev->driver_data;
	bool lightbar = asus_aura_is_lightbar(drvdata, ldev);
	int ret;

	ret = asus_aura_sync_power(drvdata, ldev, enabled,
				   ldev->active_power_states);
	if (ret < 0)
		return ret;

	if (!enabled) {
		/*
		 * Core updates ldev->enabled only after this callback returns.
		 * Blank the zone under LampArray/direct control, then release
		 * host control when the sibling does not still need it.
		 */
		guard(mutex)(&drvdata->aura_lock);

		if (drvdata->lamparray_controlled ||
		    (lightbar ? drvdata->lb_direct_valid :
				drvdata->kbd_direct_valid)) {
			if (lightbar) {
				memset(drvdata->lb_direct_buf, 0,
				       sizeof(drvdata->lb_direct_buf));
				drvdata->lb_direct_valid = true;
			} else {
				memset(drvdata->kbd_direct_buf, 0,
				       sizeof(drvdata->kbd_direct_buf));
				drvdata->kbd_direct_valid = true;
			}

			ret = asus_aura_push_direct_unlocked(drvdata, !lightbar);
			if (ret < 0)
				return ret;

			if (lightbar)
				drvdata->lb_direct_valid = false;
			else
				drvdata->kbd_direct_valid = false;

			if (!asus_aura_zone_wants_direct(drvdata, lightbar))
				asus_lamparray_release_unlocked(drvdata);
		}
		return 0;
	}

	return asus_aura_apply_effect(ldev);
}

static int asus_aura_set_speed(struct led_classdev_dynamic *ldev,
			       unsigned int speed)
{
	if (!ldev->enabled)
		return 0;

	return asus_aura_commit_effect(ldev, led_dynamic_effect_name(ldev),
				       speed, ldev->direction,
				       ldev->palette, ldev->num_palette_entries);
}

static int asus_aura_set_direction(struct led_classdev_dynamic *ldev,
				   enum dl_direction direction)
{
	if (!ldev->enabled)
		return 0;

	return asus_aura_commit_effect(ldev, led_dynamic_effect_name(ldev),
				       ldev->speed, direction,
				       ldev->palette, ldev->num_palette_entries);
}

static int asus_aura_set_palette(struct led_classdev_dynamic *ldev,
				 const struct dl_rgb *palette,
				 unsigned int num_entries)
{
	if (!palette || !num_entries || num_entries > ldev->max_palette_entries)
		return -EINVAL;

	if (!ldev->enabled)
		return 0;

	return asus_aura_commit_effect(ldev, led_dynamic_effect_name(ldev),
				       ldev->speed, ldev->direction,
				       palette, num_entries);
}

static int asus_aura_brightness_set_blocking(struct led_classdev *cdev,
					     enum led_brightness brightness)
{
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	struct asus_drvdata *drvdata = ldev->driver_data;

	guard(mutex)(&ldev->lock);

	/*
	 * Firmware backlight only. Zone power belongs to enabled. brightness
	 * 0 must not gate the zone: led_trigger_remove() calls this with
	 * LED_OFF, and the persisted 0xb3 color stays in place.
	 */
	if (ldev == &drvdata->dldev_kbd)
		asus_kbd_set_brightness(drvdata->hdev, brightness);

	return 0;
}

static void asus_aura_power_bits(u32 states, u8 *kbd, u8 *lightbar)
{
	*kbd = 0;
	*lightbar = 0;

	if (states & DL_POWER_STATE_BOOT) {
		*kbd |= AURA_POWER_KBD_BOOT | AURA_POWER_LOGO_BOOT;
		*lightbar |= AURA_POWER_LB_BOOT;
	}
	if (states & DL_POWER_STATE_AWAKE) {
		*kbd |= AURA_POWER_KBD_AWAKE | AURA_POWER_LOGO_AWAKE;
		*lightbar |= AURA_POWER_LB_AWAKE;
	}
	if (states & DL_POWER_STATE_SLEEP) {
		*kbd |= AURA_POWER_KBD_SLEEP | AURA_POWER_LOGO_SLEEP;
		*lightbar |= AURA_POWER_LB_SLEEP;
	}
	if (states & DL_POWER_STATE_SHUTDOWN) {
		*kbd |= AURA_POWER_KBD_SHUTDOWN | AURA_POWER_LOGO_SHUTDOWN;
		*lightbar |= AURA_POWER_LB_SHUTDOWN;
	}
}

static int asus_aura_write_power_unlocked(struct asus_drvdata *drvdata,
					  u8 kbd, u8 lightbar)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE] = {
		FEATURE_KBD_LED_REPORT_ID1,
		AURA_CMD_POWER,
		AURA_POWER_CMD_ENABLE,
	};

	/*
	 * Logo bits are already in @kbd and track the keyboard states.
	 * Lid and rear stay enabled so unmanaged zones are not gated off.
	 */
	buf[3] = kbd;
	buf[4] = drvdata->has_lightbar ? lightbar : 0;
	buf[5] = AURA_POWER_MASK_LID_ALL;
	buf[6] = AURA_POWER_MASK_REAR_ALL;

	return asus_aura_set_feature_unlocked(drvdata, buf, sizeof(buf));
}

static int asus_aura_sync_power(struct asus_drvdata *drvdata,
				struct led_classdev_dynamic *which,
				bool enabled, u32 states)
{
	u8 kbd = 0, lightbar = 0, unused;

	guard(mutex)(&drvdata->aura_lock);

	if (which == &drvdata->dldev_kbd) {
		drvdata->kbd_power_on = enabled;
		drvdata->kbd_power_states = states;
	} else if (which == &drvdata->dldev_lightbar) {
		drvdata->lb_power_on = enabled;
		drvdata->lb_power_states = states;
	}

	if (drvdata->kbd_power_on)
		asus_aura_power_bits(drvdata->kbd_power_states, &kbd, &unused);
	if (drvdata->lb_power_on)
		asus_aura_power_bits(drvdata->lb_power_states, &unused, &lightbar);

	return asus_aura_write_power_unlocked(drvdata, kbd, lightbar);
}

static int asus_aura_set_power_states(struct led_classdev_dynamic *ldev,
				      u32 active_states)
{
	struct asus_drvdata *drvdata = ldev->driver_data;

	return asus_aura_sync_power(drvdata, ldev, ldev->enabled, active_states);
}

static const struct led_dynamic_ops asus_aura_ops = {
	.set_effect	= asus_aura_set_effect,
	.set_speed	= asus_aura_set_speed,
	.set_direction	= asus_aura_set_direction,
	.set_palette	= asus_aura_set_palette,
	.set_enabled	= asus_aura_set_enabled,
	.set_power_states = asus_aura_set_power_states,
	.direct_write	= asus_aura_strix_set_direct,
};

static const struct led_dynamic_ops asus_aura_lightbar_ops = {
	.set_effect	= asus_aura_set_effect,
	.set_speed	= asus_aura_set_speed,
	.set_direction	= asus_aura_set_direction,
	.set_palette	= asus_aura_set_palette,
	.set_enabled	= asus_aura_set_enabled,
	.set_power_states = asus_aura_set_power_states,
	.direct_write	= asus_aura_lightbar_set_direct,
};

static int asus_aura_discover(struct asus_drvdata *drvdata, bool *has_lightbar,
			      bool *is_strix_4zone, bool *is_per_key)
{
	u8 buf[AURA_FEATURE_REPORT_SIZE] = {
		FEATURE_KBD_LED_REPORT_ID1,
		AURA_CMD_PROBE,
		0x20,
		0x31,
		0x00,
		0x20,
	};
	int ret;

	*has_lightbar = false;
	*is_strix_4zone = false;
	*is_per_key = false;

	/*
	 * Query hardware configuration via Report 0x5D opcode 0x05.
	 * Byte 9 describes the keyboard layout class (0x02 = 4-zone,
	 * 0x03 = per-key) and byte 13 is the physical-region bitmap
	 * (bit 1 = lightbar present).
	 */
	ret = asus_aura_set_feature(drvdata, buf, sizeof(buf));
	hid_info(drvdata->hdev, "DEBUG discover: set ret=%d path5d=%u\n",
		 ret, drvdata->aura_write_5d);
	if (ret < 0)
		return ret;

	memset(buf, 0, sizeof(buf));
	buf[0] = FEATURE_KBD_LED_REPORT_ID1;
	ret = asus_aura_get_feature(drvdata, buf, sizeof(buf));
	if (ret < 0)
		return ret;

	hid_info(drvdata->hdev, "DEBUG discover: get ret=%d reply=%*ph\n",
		 ret, 16, buf);

	if (ret < 14)
		return -EPROTO;

	if (buf[1] != AURA_CMD_PROBE || buf[2] != 0x20 || buf[3] != 0x31)
		return -ENODEV;

	*is_strix_4zone = (buf[9] == 0x02);
	*is_per_key = (buf[9] == 0x03);
	*has_lightbar = !!(buf[13] & 0x02);

	return 0;
}

static const struct asus_slash_mode {
	const char *name;
	u8 mode;
} asus_slash_modes[] = {
	{ "Static", 0x06 },
	{ "Bounce", 0x10 },
	{ "Slash", 0x12 },
	{ "Loading", 0x13 },
	{ "BitStream", 0x1d },
	{ "Transmission", 0x1a },
	{ "Flow", 0x19 },
	{ "Flux", 0x25 },
	{ "Phantom", 0x24 },
	{ "Spectrum", 0x26 },
	{ "Hazard", 0x32 },
	{ "Interfacing", 0x33 },
	{ "Ramp", 0x34 },
	{ "GameOver", 0x42 },
	{ "Start", 0x43 },
	{ "Buzzer", 0x44 },
};

static inline u8 asus_slash_report_id(struct asus_drvdata *drvdata)
{
	if (asus_has_report_id(drvdata->hdev, FEATURE_KBD_LED_REPORT_ID2))
		return FEATURE_KBD_LED_REPORT_ID2;
	return FEATURE_KBD_LED_REPORT_ID1;
}

static int asus_slash_init_unlocked(struct asus_drvdata *drvdata)
{
	u8 rpt = asus_slash_report_id(drvdata);
	u8 pkt1[] = { rpt, 0xd7, 0x00, 0x00, 0x01, 0xac };
	u8 pkt2[] = { rpt, 0xd2, 0x02, 0x01, 0x08, 0xab };
	int ret;

	ret = asus_aura_set_feature_unlocked(drvdata, pkt1, sizeof(pkt1));
	if (ret < 0)
		return ret;

	return asus_aura_set_feature_unlocked(drvdata, pkt2, sizeof(pkt2));
}

static int asus_slash_set_options_unlocked(struct asus_drvdata *drvdata, bool enabled,
					   u8 brightness, u8 interval)
{
	u8 rpt = asus_slash_report_id(drvdata);
	u8 pkt[] = {
		rpt, 0xd3, 0x03, 0x01, 0x08, 0xab, 0xff, 0x01,
		enabled ? 1 : 0, 0x06, brightness, 0xff, interval
	};

	return asus_aura_set_feature_unlocked(drvdata, pkt, sizeof(pkt));
}

static int asus_slash_set_mode_unlocked(struct asus_drvdata *drvdata, u8 mode)
{
	u8 rpt = asus_slash_report_id(drvdata);
	u8 pkt1[] = { rpt, 0xd2, 0x03, 0x00, 0x0c };
	u8 pkt2[] = {
		rpt, 0xd3, 0x04, 0x00, 0x0c, 0x01, mode, 0x02,
		0x19, 0x03, 0x13, 0x04, 0x11, 0x05, 0x12, 0x06, 0x13
	};
	int ret;

	ret = asus_aura_set_feature_unlocked(drvdata, pkt1, sizeof(pkt1));
	if (ret < 0)
		return ret;

	return asus_aura_set_feature_unlocked(drvdata, pkt2, sizeof(pkt2));
}

static int asus_slash_save_unlocked(struct asus_drvdata *drvdata)
{
	u8 rpt = asus_slash_report_id(drvdata);
	u8 pkt[] = { rpt, 0xd4, 0x00, 0x00, 0x01, 0xab };

	return asus_aura_set_feature_unlocked(drvdata, pkt, sizeof(pkt));
}

static int asus_slash_brightness_set_blocking(struct led_classdev *led_cdev,
					      enum led_brightness brightness)
{
	struct asus_drvdata *drvdata = container_of(led_cdev, struct asus_drvdata, slash_led);
	int ret;

	guard(mutex)(&drvdata->aura_lock);

	drvdata->slash_brightness = brightness;
	ret = asus_slash_set_options_unlocked(drvdata, brightness > 0, (u8)brightness,
					      drvdata->slash_interval);
	if (ret < 0)
		return ret;

	return asus_slash_save_unlocked(drvdata);
}

static ssize_t slash_mode_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct led_classdev *led = dev_get_drvdata(dev);
	struct asus_drvdata *drvdata = container_of(led, struct asus_drvdata, slash_led);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(asus_slash_modes); i++) {
		if (asus_slash_modes[i].mode == drvdata->slash_mode)
			return sysfs_emit(buf, "%s\n", asus_slash_modes[i].name);
	}

	return sysfs_emit(buf, "0x%02x\n", drvdata->slash_mode);
}

static ssize_t slash_mode_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct led_classdev *led = dev_get_drvdata(dev);
	struct asus_drvdata *drvdata = container_of(led, struct asus_drvdata, slash_led);
	char mode_str[32];
	unsigned int i;
	u8 mode_val = 0;
	int ret;

	if (sscanf(buf, "%31s", mode_str) != 1)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(asus_slash_modes); i++) {
		if (sysfs_streq(mode_str, asus_slash_modes[i].name)) {
			mode_val = asus_slash_modes[i].mode;
			break;
		}
	}

	if (!mode_val) {
		if (kstrtou8(mode_str, 0, &mode_val))
			return -EINVAL;
	}

	guard(mutex)(&drvdata->aura_lock);

	ret = asus_slash_set_mode_unlocked(drvdata, mode_val);
	if (ret < 0)
		return ret;

	ret = asus_slash_save_unlocked(drvdata);
	if (ret < 0)
		return ret;

	drvdata->slash_mode = mode_val;
	return count;
}
static DEVICE_ATTR_RW(slash_mode);

static ssize_t slash_mode_index_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf,
			  "Static Bounce Slash Loading BitStream Transmission Flow Flux Phantom Spectrum Hazard Interfacing Ramp GameOver Start Buzzer\n");
}
static DEVICE_ATTR_RO(slash_mode_index);

static ssize_t slash_interval_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct led_classdev *led = dev_get_drvdata(dev);
	struct asus_drvdata *drvdata = container_of(led, struct asus_drvdata, slash_led);

	return sysfs_emit(buf, "%u\n", drvdata->slash_interval);
}

static ssize_t slash_interval_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct led_classdev *led = dev_get_drvdata(dev);
	struct asus_drvdata *drvdata = container_of(led, struct asus_drvdata, slash_led);
	u8 interval;
	int ret;

	if (kstrtou8(buf, 0, &interval))
		return -EINVAL;

	guard(mutex)(&drvdata->aura_lock);

	drvdata->slash_interval = interval;
	ret = asus_slash_set_options_unlocked(drvdata, drvdata->slash_brightness > 0,
					      drvdata->slash_brightness, interval);
	if (ret < 0)
		return ret;

	ret = asus_slash_save_unlocked(drvdata);
	if (ret < 0)
		return ret;

	return count;
}
static DEVICE_ATTR_RW(slash_interval);

static struct attribute *asus_slash_attrs[] = {
	&dev_attr_slash_mode.attr,
	&dev_attr_slash_mode_index.attr,
	&dev_attr_slash_interval.attr,
	NULL,
};

static const struct attribute_group asus_slash_group = {
	.attrs = asus_slash_attrs,
};

static const struct attribute_group *asus_slash_groups[] = {
	&asus_slash_group,
	NULL,
};

static int asus_init_slash(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	int ret;

	if (!asus_device_has_slash(hdev))
		return 0;

	drvdata->slash_led.name = "asus::slash";
	drvdata->slash_led.max_brightness = 255;
	drvdata->slash_led.brightness = 255;
	drvdata->slash_led.brightness_set_blocking = asus_slash_brightness_set_blocking;
	drvdata->slash_led.groups = asus_slash_groups;

	drvdata->slash_brightness = 255;
	drvdata->slash_interval = 0;

	scoped_guard(mutex, &drvdata->aura_lock) {
		ret = asus_slash_init_unlocked(drvdata);
		if (ret < 0) {
			hid_warn(hdev, "Failed to initialize Slash lighting: %d\n", ret);
			return ret;
		}
	}

	ret = devm_led_classdev_register(&hdev->dev, &drvdata->slash_led);
	if (ret < 0) {
		hid_warn(hdev, "Failed to register Slash LED classdev: %d\n", ret);
		return ret;
	}

	drvdata->has_slash_led = true;
	hid_info(hdev, "Registered Slash lighting LED: asus::slash\n");

	return 0;
}

static void asus_aura_dldev_set_default_palette(struct led_classdev_dynamic *ldev,
						struct dl_rgb *palette)
{
	ldev->palette = palette;
	palette[0].r = 255;
	palette[0].g = 0;
	palette[0].b = 0;
	ldev->num_palette_entries = 1;
}

static void asus_aura_dldev_common_init(struct led_classdev_dynamic *ldev,
					struct asus_drvdata *drvdata,
					const char *name,
					const struct led_dynamic_ops *ops,
					const char **effects,
					unsigned int num_effects)
{
	unsigned int i;

	ldev->cdev.name = name;
	ldev->cdev.max_brightness = 255;
	ldev->cdev.brightness = 255;
	ldev->cdev.brightness_set_blocking = asus_aura_brightness_set_blocking;
	ldev->ops = ops;
	ldev->driver_data = drvdata;
	ldev->speed = 1;
	ldev->speed_min = 0;
	ldev->speed_max = 2;
	ldev->direction = DL_DIRECTION_RIGHT;
	ldev->supported_directions = BIT(DL_DIRECTION_RIGHT) |
				     BIT(DL_DIRECTION_LEFT);
	ldev->max_palette_entries = 2;
	ldev->enabled = true;
	ldev->effects = (const char * const *)effects;
	ldev->num_effects = num_effects;
	ldev->current_effect = 0;
	for (i = 0; i < num_effects; i++) {
		if (!strcmp(effects[i], "static")) {
			ldev->current_effect = i;
			break;
		}
	}
	ldev->supported_power_states = DL_POWER_STATE_ALL;
	ldev->active_power_states = DL_POWER_STATE_ALL;
}

static int asus_aura_effect_index(struct led_classdev_dynamic *ldev, u8 hw_mode)
{
	const char *name = NULL;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(asus_aura_effect_map); i++) {
		if (asus_aura_effect_map[i].hw_mode == hw_mode) {
			name = asus_aura_effect_map[i].name;
			break;
		}
	}
	if (!name)
		return -1;

	for (i = 0; i < ldev->num_effects; i++) {
		if (!strcmp(ldev->effects[i], name))
			return i;
	}

	return -1;
}

static void asus_aura_apply_boot_levels(struct led_classdev_dynamic *ldev,
					struct asus_drvdata *drvdata)
{
	ldev->speed = asus_aura_speed_from_hw(drvdata->boot_speed);
	ldev->direction = asus_aura_direction_from_hw(drvdata->boot_direction);

	if (ldev->supported_directions &&
	    !(ldev->supported_directions & BIT(ldev->direction)))
		ldev->direction = DL_DIRECTION_RIGHT;
}

static bool asus_aura_adopt_node(struct asus_drvdata *drvdata,
				 struct led_classdev_dynamic *ldev)
{
	int idx;

	if (!ldev->palette)
		return false;

	idx = asus_aura_effect_index(ldev, drvdata->boot_mode);
	if (idx < 0)
		return false;

	ldev->current_effect = idx;
	ldev->palette[0].r = drvdata->boot_r;
	ldev->palette[0].g = drvdata->boot_g;
	ldev->palette[0].b = drvdata->boot_b;
	ldev->num_palette_entries = 1;
	if (drvdata->boot_r2 || drvdata->boot_g2 || drvdata->boot_b2) {
		ldev->palette[1].r = drvdata->boot_r2;
		ldev->palette[1].g = drvdata->boot_g2;
		ldev->palette[1].b = drvdata->boot_b2;
		ldev->num_palette_entries = 2;
	}
	asus_aura_apply_boot_levels(ldev, drvdata);
	return true;
}

static bool asus_aura_adopt_boot_effect(struct asus_drvdata *drvdata)
{
	u8 zone = drvdata->boot_zone;
	bool kbd = zone == AURA_ZONE_ALL ||
		   (zone >= AURA_ZONE_KEY1 && zone <= AURA_ZONE_KEY4);
	bool lightbar = zone == AURA_ZONE_ALL ||
			zone == AURA_ZONE_BAR_LEFT ||
			zone == AURA_ZONE_BAR_RIGHT;
	bool any = false;

	if (kbd && drvdata->dldev_kbd.palette &&
	    asus_aura_adopt_node(drvdata, &drvdata->dldev_kbd)) {
		drvdata->kbd_effect_set = true;
		any = true;
	}

	if (lightbar && drvdata->dldev_lightbar.palette &&
	    asus_aura_adopt_node(drvdata, &drvdata->dldev_lightbar)) {
		drvdata->lightbar_effect_set = true;
		any = true;
	}

	if (kbd && drvdata->kbd_effect_set) {
		asus_lamparray_fill_solid(drvdata->kbd_direct_buf,
					  ROG_STRIX_4ZONE_KBD_LEDS,
					  drvdata->boot_r, drvdata->boot_g,
					  drvdata->boot_b);
		drvdata->kbd_direct_valid = true;
	}
	if (lightbar && drvdata->lightbar_effect_set) {
		asus_lamparray_fill_solid(drvdata->lb_direct_buf,
					  asus_aura_lb_led_count(drvdata),
					  drvdata->boot_r, drvdata->boot_g,
					  drvdata->boot_b);
		drvdata->lb_direct_valid = true;
	}

	return any;
}

static int asus_init_dynamic_lighting(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	unsigned int kbd_n, lightbar_n;
	u8 effect_mask[2];
	const u8 *mask = NULL;
	bool is_per_key = false;
	bool has_lightbar = false;
	bool kbd_direct;
	bool lightbar_direct;
	int ret;

	ret = asus_aura_discover(drvdata, &has_lightbar, &drvdata->is_strix_4zone,
				 &is_per_key);
	if (ret == -ENODEV)
		return 0;
	if (ret < 0) {
		hid_warn(hdev, "Aura device discovery failed: %d\n", ret);
		return 0;
	}

	drvdata->has_lightbar = has_lightbar;

	ret = asus_aura_wake_all_zones(drvdata);
	if (ret < 0)
		hid_warn(hdev, "Failed to wake Aura hardware zones: %d\n", ret);

	scoped_guard(mutex, &drvdata->aura_lock)
		asus_lamparray_try_init_unlocked(drvdata);

	kbd_direct = is_per_key || drvdata->is_strix_4zone ||
		     drvdata->lamparray_hdev;
	lightbar_direct = has_lightbar;
	ret = asus_aura_get_effect_mask(drvdata, effect_mask);
	if (ret < 0) {
		hid_warn(hdev,
			 "Aura 0x9e capability probe failed: %d, using fallback effect list\n",
			 ret);
	} else {
		mask = effect_mask;
		hid_info(hdev, "Aura effect capability mask %02x %02x\n",
			 effect_mask[0], effect_mask[1]);
	}

	kbd_n = asus_aura_fill_effects(drvdata->effects_kbd, mask, kbd_direct);
	lightbar_n = asus_aura_fill_effects(drvdata->effects_lightbar, mask,
					    lightbar_direct);

	/* Keyboard Dynamic Lighting zone: always registered on Aura models. */
	asus_aura_dldev_common_init(&drvdata->dldev_kbd, drvdata, "asus::keyboard",
				    &asus_aura_ops,
				    drvdata->effects_kbd, kbd_n);
	asus_aura_dldev_set_default_palette(&drvdata->dldev_kbd,
					    drvdata->kbd_palette);
	drvdata->kbd_power_on = true;
	drvdata->kbd_power_states = DL_POWER_STATE_ALL;
	if (kbd_direct) {
		drvdata->dldev_kbd.zone_type = drvdata->is_strix_4zone ?
			"keyboard" : "keyboard_per_key";
		drvdata->dldev_kbd.led_count = drvdata->is_strix_4zone ?
			ROG_STRIX_4ZONE_KBD_LEDS : ROG_STRIX_DIRECT_LEDS;
	} else {
		drvdata->dldev_kbd.zone_type = "keyboard";
		if (drvdata->is_strix_4zone)
			drvdata->dldev_kbd.led_count = ROG_STRIX_4ZONE_KBD_LEDS;
	}

	if (has_lightbar) {
		asus_aura_dldev_common_init(&drvdata->dldev_lightbar, drvdata,
					    "asus::lightbar",
					    &asus_aura_lightbar_ops,
					    drvdata->effects_lightbar, lightbar_n);
		drvdata->dldev_lightbar.zone_type = "lightbar";
		drvdata->dldev_lightbar.led_count = asus_aura_lb_led_count(drvdata);
		asus_aura_dldev_set_default_palette(&drvdata->dldev_lightbar,
						    drvdata->lightbar_palette);
		drvdata->lb_power_on = true;
		drvdata->lb_power_states = DL_POWER_STATE_ALL;
	}

	/*
	 * Adopt or keep the default palette before sysfs exists, so a
	 * concurrent reader cannot observe a half-initialized node.
	 */
	if (drvdata->boot_effect_valid)
		asus_aura_adopt_boot_effect(drvdata);

	ret = devm_led_classdev_dynamic_register(&hdev->dev, &drvdata->dldev_kbd);
	if (ret < 0) {
		hid_warn(hdev, "Failed to register kbd dynamic lighting: %d\n", ret);
		return ret;
	}
	drvdata->has_dldev_kbd = true;

	if (has_lightbar) {
		ret = devm_led_classdev_dynamic_register(&hdev->dev,
							 &drvdata->dldev_lightbar);
		if (ret < 0) {
			hid_warn(hdev, "Failed to register lightbar dynamic lighting: %d\n",
				 ret);
		} else {
			drvdata->has_dldev_lightbar = true;
		}
	}

	hid_info(hdev, "Registered dynamic lighting zones: kbd=%d, lightbar=%d\n",
		 drvdata->has_dldev_kbd, drvdata->has_dldev_lightbar);

	if (drvdata->boot_effect_valid &&
	    (drvdata->kbd_effect_set || drvdata->lightbar_effect_set))
		hid_info(hdev, "Preserving programmed Aura effect\n");
	else
		hid_info(hdev, "Leaving Aura controller state unchanged\n");

	return 0;
}

static unsigned int asus_anime_led_count(enum asus_anime_type type)
{
	switch (type) {
	case ASUS_ANIME_GA401:
		return ASUS_ANIME_PANE_LEN * 2;
	case ASUS_ANIME_G635L:
	case ASUS_ANIME_G835L:
		return 810;
	case ASUS_ANIME_GA402:
	case ASUS_ANIME_GU604:
	case ASUS_ANIME_UNKNOWN:
	default:
		/* Safe GA402-like 3-pane layout for unknown AniMe boards. */
		return ASUS_ANIME_PANE_LEN * 3;
	}
}

static unsigned int asus_anime_pane_count(enum asus_anime_type type)
{
	switch (type) {
	case ASUS_ANIME_GA401:
	case ASUS_ANIME_G635L:
	case ASUS_ANIME_G835L:
		return 2;
	default:
		return 3;
	}
}

static void asus_anime_fill_prefix(enum asus_anime_type type, unsigned int pane,
				   u8 *hdr)
{
	static const u8 ga_pfx[3][7] = {
		{ 0x5e, 0xc0, 0x02, 0x01, 0x00, 0x73, 0x02 },
		{ 0x5e, 0xc0, 0x02, 0x74, 0x02, 0x73, 0x02 },
		{ 0x5e, 0xc0, 0x02, 0xe7, 0x04, 0x73, 0x02 },
	};
	static const u8 strix_pfx[2][7] = {
		{ 0x5e, 0xc0, 0x02, 0x01, 0x00, 0xea, 0x01 },
		{ 0x5e, 0xc0, 0x02, 0xeb, 0x01, 0x40, 0x01 },
	};

	if (type == ASUS_ANIME_G635L || type == ASUS_ANIME_G835L) {
		memcpy(hdr, strix_pfx[min_t(unsigned int, pane, 1)], 7);
		return;
	}

	memcpy(hdr, ga_pfx[min_t(unsigned int, pane, 2)], 7);
}

static int asus_anime_write(struct asus_drvdata *drvdata, u8 *buf, size_t len)
{
	int ret;

	if (!buf || len < 1 || len > ASUS_ANIME_PACKET_SIZE)
		return -EINVAL;

	ret = hid_hw_raw_request(drvdata->hdev, buf[0], buf, len,
				 HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	if (ret < 0)
		return ret;

	return 0;
}

static int asus_anime_clear_pkt(struct asus_drvdata *drvdata)
{
	memset(drvdata->anime_buf, 0, ASUS_ANIME_PACKET_SIZE);
	drvdata->anime_buf[0] = FEATURE_KBD_LED_REPORT_ID2;
	return 0;
}

static int asus_anime_init_device(struct asus_drvdata *drvdata)
{
	static const char *asus_tech = "ASUS Tech.Inc.";
	int ret;

	ret = asus_anime_clear_pkt(drvdata);
	if (ret)
		return ret;

	memcpy(drvdata->anime_buf + 1, asus_tech, strlen(asus_tech));
	ret = asus_anime_write(drvdata, drvdata->anime_buf, ASUS_ANIME_PACKET_SIZE);
	if (ret < 0)
		return ret;

	ret = asus_anime_clear_pkt(drvdata);
	if (ret)
		return ret;

	drvdata->anime_buf[1] = 0xc2;
	return asus_anime_write(drvdata, drvdata->anime_buf, ASUS_ANIME_PACKET_SIZE);
}

static int asus_anime_flush(struct asus_drvdata *drvdata)
{
	int ret;

	ret = asus_anime_clear_pkt(drvdata);
	if (ret)
		return ret;

	drvdata->anime_buf[1] = 0xc0;
	drvdata->anime_buf[2] = 0x03;
	return asus_anime_write(drvdata, drvdata->anime_buf, ASUS_ANIME_PACKET_SIZE);
}

static int asus_anime_set_brightness(struct asus_drvdata *drvdata, u8 brightness)
{
	int ret;

	ret = asus_anime_clear_pkt(drvdata);
	if (ret)
		return ret;

	drvdata->anime_buf[1] = 0xc0;
	drvdata->anime_buf[2] = 0x04;
	drvdata->anime_buf[3] = brightness;
	return asus_anime_write(drvdata, drvdata->anime_buf, ASUS_ANIME_PACKET_SIZE);
}

static int asus_anime_set_display(struct asus_drvdata *drvdata, bool enabled)
{
	int ret;

	ret = asus_anime_clear_pkt(drvdata);
	if (ret)
		return ret;

	drvdata->anime_buf[1] = 0xc3;
	drvdata->anime_buf[2] = 0x01;
	drvdata->anime_buf[3] = enabled ? 0x00 : 0x80;
	return asus_anime_write(drvdata, drvdata->anime_buf, ASUS_ANIME_PACKET_SIZE);
}

static int asus_anime_set_powersave_anim(struct asus_drvdata *drvdata, bool enabled)
{
	int ret;

	ret = asus_anime_clear_pkt(drvdata);
	if (ret)
		return ret;

	drvdata->anime_buf[1] = 0xc4;
	drvdata->anime_buf[2] = 0x01;
	drvdata->anime_buf[3] = enabled ? 0x00 : 0x80;
	return asus_anime_write(drvdata, drvdata->anime_buf, ASUS_ANIME_PACKET_SIZE);
}

static int asus_anime_commit_builtins(struct asus_drvdata *drvdata)
{
	int ret;

	ret = asus_anime_clear_pkt(drvdata);
	if (ret)
		return ret;

	drvdata->anime_buf[1] = 0xc5;
	drvdata->anime_buf[2] = drvdata->anime_awake |
				(drvdata->anime_sleep << 1) |
				(drvdata->anime_shutdown << 2) |
				(drvdata->anime_boot << 3);
	return asus_anime_write(drvdata, drvdata->anime_buf, ASUS_ANIME_PACKET_SIZE);
}

static int asus_anime_write_frame(struct asus_drvdata *drvdata,
				  const u8 *data, size_t len)
{
	unsigned int panes = asus_anime_pane_count(drvdata->anime_type);
	unsigned int pane, offset = 0;
	int ret;

	if (len != asus_anime_led_count(drvdata->anime_type))
		return -EINVAL;

	for (pane = 0; pane < panes; pane++) {
		unsigned int chunk;

		ret = asus_anime_clear_pkt(drvdata);
		if (ret)
			return ret;

		asus_anime_fill_prefix(drvdata->anime_type, pane,
				       drvdata->anime_buf);

		if (drvdata->anime_type == ASUS_ANIME_G635L ||
		    drvdata->anime_type == ASUS_ANIME_G835L) {
			chunk = pane ? (len - ASUS_ANIME_STRIX_PANE1_LEN) :
				       ASUS_ANIME_STRIX_PANE1_LEN;
		} else {
			chunk = min_t(unsigned int, ASUS_ANIME_PANE_LEN, len - offset);
		}

		if (offset + chunk > len)
			return -EINVAL;

		memcpy(drvdata->anime_buf + ASUS_ANIME_BLOCK_START,
		       data + offset, chunk);
		offset += chunk;

		ret = asus_anime_write(drvdata, drvdata->anime_buf,
				       ASUS_ANIME_PACKET_SIZE);
		if (ret < 0)
			return ret;
	}

	return asus_anime_flush(drvdata);
}

static const char * const asus_anime_effect_names[] = {
	"direct",
	"glitch_construction",
	"static_emergence",
	"binary_banner_scroll",
	"rog_logo_glitch",
	"banner_swipe",
	"starfield",
	"glitch_out",
	"see_ya",
};

static int asus_anime_apply_effect_unlocked(struct asus_drvdata *drvdata,
					    unsigned int index)
{
	int ret;

	if (index >= ARRAY_SIZE(asus_anime_effect_names))
		return -EINVAL;

	if (index == 0) {
		drvdata->anime_builtin = false;
		return asus_anime_set_powersave_anim(drvdata, false);
	}

	switch (index) {
	case 1:
		drvdata->anime_boot = 0;
		break;
	case 2:
		drvdata->anime_boot = 1;
		break;
	case 3:
		drvdata->anime_awake = 0;
		break;
	case 4:
		drvdata->anime_awake = 1;
		break;
	case 5:
		drvdata->anime_sleep = 0;
		break;
	case 6:
		drvdata->anime_sleep = 1;
		break;
	case 7:
		drvdata->anime_shutdown = 0;
		break;
	case 8:
		drvdata->anime_shutdown = 1;
		break;
	default:
		return -EINVAL;
	}

	drvdata->anime_builtin = true;
	ret = asus_anime_commit_builtins(drvdata);
	if (ret < 0)
		return ret;

	return asus_anime_set_powersave_anim(drvdata, true);
}

static int asus_anime_set_effect(struct led_classdev_dynamic *ldev,
				 unsigned int index)
{
	struct asus_drvdata *drvdata = ldev->driver_data;

	guard(mutex)(&drvdata->aura_lock);
	return asus_anime_apply_effect_unlocked(drvdata, index);
}

static int asus_anime_set_enabled(struct led_classdev_dynamic *ldev, bool enabled)
{
	struct asus_drvdata *drvdata = ldev->driver_data;

	guard(mutex)(&drvdata->aura_lock);
	return asus_anime_set_display(drvdata, enabled);
}

static int asus_anime_direct_write(struct led_classdev_dynamic *ldev,
				   const u8 *buffer, size_t size)
{
	struct asus_drvdata *drvdata = ldev->driver_data;
	int ret;

	guard(mutex)(&drvdata->aura_lock);

	ret = asus_anime_write_frame(drvdata, buffer, size);
	if (ret < 0)
		return ret;

	drvdata->anime_builtin = false;
	return asus_anime_set_powersave_anim(drvdata, false);
}

static int asus_anime_brightness_set(struct led_classdev *cdev,
				     enum led_brightness brightness)
{
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	struct asus_drvdata *drvdata = ldev->driver_data;

	guard(mutex)(&drvdata->aura_lock);
	return asus_anime_set_brightness(drvdata, brightness);
}

static const struct led_dynamic_ops asus_anime_ops = {
	.set_effect = asus_anime_set_effect,
	.set_enabled = asus_anime_set_enabled,
	.direct_write = asus_anime_direct_write,
};

static int asus_init_anime(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct led_classdev_dynamic *ldev = &drvdata->dldev_anime;
	unsigned int i, led_count;
	int ret;

	if (asus_classify_pid_193b(hdev) != ASUS_PID_193B_ANIME)
		return 0;

	drvdata->anime_buf = devm_kzalloc(&hdev->dev, ASUS_ANIME_PACKET_SIZE,
					  GFP_KERNEL);
	if (!drvdata->anime_buf)
		return -ENOMEM;

	drvdata->anime_type = asus_anime_type_from_dmi();
	if (drvdata->anime_type == ASUS_ANIME_UNKNOWN) {
		drvdata->anime_type = ASUS_ANIME_GA402;
		hid_info(hdev,
			 "AniMe Matrix on unknown board; using GA402-like 3-pane layout\n");
	}

	led_count = asus_anime_led_count(drvdata->anime_type);

	scoped_guard(mutex, &drvdata->aura_lock) {
		ret = asus_anime_init_device(drvdata);
		if (ret < 0) {
			hid_warn(hdev, "AniMe Matrix init failed: %d\n", ret);
			return ret;
		}

		ret = asus_anime_set_display(drvdata, true);
		if (ret < 0)
			return ret;

		ret = asus_anime_set_brightness(drvdata, 2);
		if (ret < 0)
			return ret;
	}

	for (i = 0; i < ARRAY_SIZE(asus_anime_effect_names); i++)
		drvdata->effects_anime[i] = asus_anime_effect_names[i];

	ldev->cdev.name = "asus::anime";
	ldev->cdev.max_brightness = 3;
	ldev->cdev.brightness = 2;
	ldev->cdev.brightness_set_blocking = asus_anime_brightness_set;
	ldev->ops = &asus_anime_ops;
	ldev->driver_data = drvdata;
	ldev->zone_type = "matrix";
	ldev->led_count = led_count;
	ldev->direct_sample_size = 1;
	ldev->effects = drvdata->effects_anime;
	ldev->num_effects = ARRAY_SIZE(asus_anime_effect_names);
	ldev->current_effect = 0;
	ldev->enabled = true;

	ret = devm_led_classdev_dynamic_register(&hdev->dev, ldev);
	if (ret < 0) {
		hid_warn(hdev, "Failed to register AniMe Dynamic Lighting: %d\n",
			 ret);
		return ret;
	}

	drvdata->has_dldev_anime = true;
	hid_info(hdev,
		 "Registered AniMe Matrix Dynamic Lighting (%u LEDs, type %u)\n",
		 led_count, drvdata->anime_type);
	return 0;
}

#else /* !IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC) */

static inline int asus_init_dynamic_lighting(struct hid_device *hdev)
{
	return 0;
}

static inline int asus_init_slash(struct hid_device *hdev)
{
	return 0;
}

static inline int asus_init_anime(struct hid_device *hdev)
{
	return 0;
}

static inline void asus_aura_capture_boot_effect(struct hid_device *hdev)
{
}

#endif /* IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC) */

/*
 * [0]       REPORT_ID (same value defined in report descriptor)
 * [1]	     rest battery level. range [0..255]
 * [2]..[7]  Bluetooth hardware address (MAC address)
 * [8]       charging status
 *            = 0 : AC offline / discharging
 *            = 1 : AC online  / charging
 *            = 2 : AC online  / fully charged
 */
static int asus_parse_battery(struct asus_drvdata *drvdata, u8 *data, int size)
{
	u8 sts;
	u8 lvl;
	int val;

	lvl = data[1];
	sts = data[8];

	drvdata->battery_capacity = ((int)lvl * 100) / (int)BATTERY_LEVEL_MAX;

	switch (sts) {
	case BATTERY_STAT_CHARGING:
		val = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case BATTERY_STAT_FULL:
		val = POWER_SUPPLY_STATUS_FULL;
		break;
	case BATTERY_STAT_DISCONNECT:
	default:
		val = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	}
	drvdata->battery_stat = val;

	return 0;
}

static int asus_report_battery(struct asus_drvdata *drvdata, u8 *data, int size)
{
	/* notify only the autonomous event by device */
	if ((drvdata->battery_in_query == false) &&
			 (size == BATTERY_REPORT_SIZE))
		power_supply_changed(drvdata->battery);

	return 0;
}

static int asus_battery_query(struct asus_drvdata *drvdata)
{
	u8 *buf;
	int ret = 0;

	buf = kmalloc(BATTERY_REPORT_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	drvdata->battery_in_query = true;
	ret = hid_hw_raw_request(drvdata->hdev, BATTERY_REPORT_ID,
				buf, BATTERY_REPORT_SIZE,
				HID_INPUT_REPORT, HID_REQ_GET_REPORT);
	drvdata->battery_in_query = false;
	if (ret == BATTERY_REPORT_SIZE)
		ret = asus_parse_battery(drvdata, buf, BATTERY_REPORT_SIZE);
	else
		ret = -ENODATA;

	kfree(buf);

	return ret;
}

static enum power_supply_property asus_battery_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};

#define	QUERY_MIN_INTERVAL	(60 * HZ)	/* 60[sec] */

static int asus_battery_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	struct asus_drvdata *drvdata = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_CAPACITY:
		if (time_before(drvdata->battery_next_query, jiffies)) {
			drvdata->battery_next_query =
					 jiffies + QUERY_MIN_INTERVAL;
			ret = asus_battery_query(drvdata);
			if (ret)
				return ret;
		}
		if (psp == POWER_SUPPLY_PROP_STATUS)
			val->intval = drvdata->battery_stat;
		else
			val->intval = drvdata->battery_capacity;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_DEVICE;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = drvdata->hdev->name;
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int asus_battery_probe(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);
	struct power_supply_config pscfg = { .drv_data = drvdata };
	int ret = 0;

	drvdata->battery_capacity = 0;
	drvdata->battery_stat = POWER_SUPPLY_STATUS_UNKNOWN;
	drvdata->battery_in_query = false;

	drvdata->battery_desc.properties = asus_battery_props;
	drvdata->battery_desc.num_properties = ARRAY_SIZE(asus_battery_props);
	drvdata->battery_desc.get_property = asus_battery_get_property;
	drvdata->battery_desc.type = POWER_SUPPLY_TYPE_BATTERY;
	drvdata->battery_desc.use_for_apm = 0;
	drvdata->battery_desc.name = devm_kasprintf(&hdev->dev, GFP_KERNEL,
					"asus-keyboard-%s-battery",
					strlen(hdev->uniq) ?
					hdev->uniq : dev_name(&hdev->dev));
	if (!drvdata->battery_desc.name)
		return -ENOMEM;

	drvdata->battery_next_query = jiffies;

	drvdata->battery = devm_power_supply_register(&hdev->dev,
				&(drvdata->battery_desc), &pscfg);
	if (IS_ERR(drvdata->battery)) {
		ret = PTR_ERR(drvdata->battery);
		drvdata->battery = NULL;
		hid_err(hdev, "Unable to register battery device\n");
		return ret;
	}

	power_supply_powers(drvdata->battery, &hdev->dev);

	return ret;
}

static int asus_input_configured(struct hid_device *hdev, struct hid_input *hi)
{
	struct input_dev *input = hi->input;
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	/* T100CHI uses MULTI_INPUT, bind the touchpad to the mouse hid_input */
	if (drvdata->quirks & QUIRK_T100CHI &&
	    hi->report->id != T100CHI_MOUSE_REPORT_ID)
		return 0;

	/* Handle MULTI_INPUT on E1239T mouse/touchpad USB interface */
	if (drvdata->tp && (drvdata->quirks & QUIRK_MEDION_E1239T)) {
		switch (hi->report->id) {
		case E1239T_TP_TOGGLE_REPORT_ID:
			input_set_capability(input, EV_KEY, KEY_F21);
			input->name = "Asus Touchpad Keys";
			drvdata->tp_kbd_input = input;
			return 0;
		case INPUT_REPORT_ID:
			break; /* Touchpad report, handled below */
		default:
			return 0; /* Ignore other reports */
		}
	}

	if (drvdata->tp) {
		int ret;

		input_set_abs_params(input, ABS_MT_POSITION_X, 0,
				     drvdata->tp->max_x, 0, 0);
		input_set_abs_params(input, ABS_MT_POSITION_Y, 0,
				     drvdata->tp->max_y, 0, 0);
		input_abs_set_res(input, ABS_MT_POSITION_X, drvdata->tp->res_x);
		input_abs_set_res(input, ABS_MT_POSITION_Y, drvdata->tp->res_y);

		if (drvdata->tp->contact_size >= 5) {
			input_set_abs_params(input, ABS_TOOL_WIDTH, 0,
					     MAX_TOUCH_MAJOR, 0, 0);
			input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0,
					     MAX_TOUCH_MAJOR, 0, 0);
			input_set_abs_params(input, ABS_MT_PRESSURE, 0,
					      MAX_PRESSURE, 0, 0);
		}

		__set_bit(BTN_LEFT, input->keybit);
		__set_bit(INPUT_PROP_BUTTONPAD, input->propbit);

		ret = input_mt_init_slots(input, drvdata->tp->max_contacts,
					  INPUT_MT_POINTER);

		if (ret) {
			hid_err(hdev, "Asus input mt init slots failed: %d\n", ret);
			return ret;
		}
	}

	drvdata->input = input;

	if ((drvdata->quirks & QUIRK_HID_FN_LOCK) &&
	    (asus_kbd_fn_lock_set(drvdata, true)))
		hid_warn(hdev, "Error while setting FN lock to ON\n");

	return 0;
}

#define asus_map_key_clear(c)	hid_map_usage_clear(hi, usage, bit, \
						    max, EV_KEY, (c))
static int asus_input_mapping(struct hid_device *hdev,
		struct hid_input *hi, struct hid_field *field,
		struct hid_usage *usage, unsigned long **bit,
		int *max)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->quirks & QUIRK_SKIP_INPUT_MAPPING) {
		/* Don't map anything from the HID report.
		 * We do it all manually in asus_input_configured
		 */
		return -1;
	}

	/*
	 * Ignore a bunch of bogus collections in the T100CHI descriptor.
	 * This avoids a bunch of non-functional hid_input devices getting
	 * created because of the T100CHI using HID_QUIRK_MULTI_INPUT.
	 */
	if ((drvdata->quirks & (QUIRK_T100CHI | QUIRK_T90CHI)) &&
	    (field->application == (HID_UP_GENDESK | 0x0080) ||
	     field->application == HID_GD_MOUSE ||
	     usage->hid == (HID_UP_GENDEVCTRLS | 0x0024) ||
	     usage->hid == (HID_UP_GENDEVCTRLS | 0x0025) ||
	     usage->hid == (HID_UP_GENDEVCTRLS | 0x0026)))
		return -1;

	/* ASUS-specific keyboard hotkeys and led backlight */
	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_ASUSVENDOR) {
		switch (usage->hid & HID_USAGE) {
		case 0x10: asus_map_key_clear(KEY_BRIGHTNESSDOWN);	break;
		case 0x20: asus_map_key_clear(KEY_BRIGHTNESSUP);		break;
		case 0x35: asus_map_key_clear(KEY_DISPLAY_OFF);		break;
		case 0x6c: asus_map_key_clear(KEY_SLEEP);		break;
		case 0x7c: asus_map_key_clear(KEY_MICMUTE);		break;
		case 0x82: asus_map_key_clear(KEY_CAMERA);		break;
		case 0x85: asus_map_key_clear(KEY_CAMERA);		break;
		case 0x86: asus_map_key_clear(KEY_PROG1);	break; /* MyASUS key */
		case 0x88: asus_map_key_clear(KEY_RFKILL);			break;
		case 0xb5: asus_map_key_clear(KEY_CALC);			break;
		case 0xc4: asus_map_key_clear(KEY_KBDILLUMUP);		break;
		case 0xc5: asus_map_key_clear(KEY_KBDILLUMDOWN);		break;
		case 0xc7: asus_map_key_clear(KEY_KBDILLUMTOGGLE);	break;
		case 0x4e: asus_map_key_clear(KEY_FN_ESC);		break;
		case 0x7e: asus_map_key_clear(KEY_EMOJI_PICKER);	break;

		case 0x8b: asus_map_key_clear(KEY_PROG1);	break; /* ProArt Creator Hub key */
		case 0x5f: asus_map_key_clear(KEY_PROG2);	break; /* S-shaped programmable key */
		case 0x6b: asus_map_key_clear(KEY_F21);		break; /* ASUS touchpad toggle */
		case 0x38: asus_map_key_clear(KEY_PROG1);	break; /* ROG key */
		case 0xba: asus_map_key_clear(KEY_PROG2);	break; /* Fn+C ASUS Splendid */
		case 0x5c: asus_map_key_clear(KEY_PROG3);	break; /* Fn+Space Power4Gear */
		case 0x99: asus_map_key_clear(KEY_PROG4);	break; /* Fn+F5 "fan" symbol */
		case 0xae: asus_map_key_clear(KEY_PROG4);	break; /* Fn+F5 "fan" symbol */
		case 0x92: asus_map_key_clear(KEY_CALC);	break; /* Fn+Ret "Calc" symbol */
		case 0xb2: asus_map_key_clear(KEY_PROG2);	break; /* Fn+Left previous aura */
		case 0xb3: asus_map_key_clear(KEY_PROG3);	break; /* Fn+Left next aura */
		case 0x6a: asus_map_key_clear(KEY_F13);		break; /* Screenpad toggle */
		case 0x4b: asus_map_key_clear(KEY_F14);		break; /* Arrows/Pg-Up/Dn toggle */
		case 0xa5: asus_map_key_clear(KEY_F15);		break; /* ROG Ally left back */
		case 0xa6: asus_map_key_clear(KEY_F16);		break; /* ROG Ally QAM button */
		case 0xa7: asus_map_key_clear(KEY_F17);		break; /* ROG Ally ROG long-press */
		case 0xa8: asus_map_key_clear(KEY_F18);		break; /* ROG Ally ROG long-press-release */
		case 0x9c: asus_map_key_clear(KEY_F19);		break; /* Zephyrus Duo tent mode */

		default:
			/* ASUS lazily declares 256 usages, ignore the rest,
			 * as some make the keyboard appear as a pointer device. */
			return -1;
		}

		set_bit(EV_REP, hi->input->evbit);
		return 1;
	}

	if ((usage->hid & HID_USAGE_PAGE) == HID_UP_MSVENDOR) {
		switch (usage->hid & HID_USAGE) {
		case 0xff01: asus_map_key_clear(BTN_1);	break;
		case 0xff02: asus_map_key_clear(BTN_2);	break;
		case 0xff03: asus_map_key_clear(BTN_3);	break;
		case 0xff04: asus_map_key_clear(BTN_4);	break;
		case 0xff05: asus_map_key_clear(BTN_5);	break;
		case 0xff06: asus_map_key_clear(BTN_6);	break;
		case 0xff07: asus_map_key_clear(BTN_7);	break;
		case 0xff08: asus_map_key_clear(BTN_8);	break;
		case 0xff09: asus_map_key_clear(BTN_9);	break;
		case 0xff0a: asus_map_key_clear(BTN_A);	break;
		case 0xff0b: asus_map_key_clear(BTN_B);	break;
		case 0x00f1: asus_map_key_clear(KEY_WLAN);	break;
		case 0x00f2: asus_map_key_clear(KEY_BRIGHTNESSDOWN);	break;
		case 0x00f3: asus_map_key_clear(KEY_BRIGHTNESSUP);	break;
		case 0x00f4: asus_map_key_clear(KEY_DISPLAY_OFF);	break;
		case 0x00f7: asus_map_key_clear(KEY_CAMERA);	break;
		case 0x00f8: asus_map_key_clear(KEY_PROG1);	break;
		default:
			return 0;
		}

		set_bit(EV_REP, hi->input->evbit);
		return 1;
	}

	if (drvdata->quirks & QUIRK_NO_CONSUMER_USAGES &&
		(usage->hid & HID_USAGE_PAGE) == HID_UP_CONSUMER) {
		switch (usage->hid & HID_USAGE) {
		case 0xe2: /* Mute */
		case 0xe9: /* Volume up */
		case 0xea: /* Volume down */
			return 0;
		default:
			/* Ignore dummy Consumer usages which make the
			 * keyboard incorrectly appear as a pointer device.
			 */
			return -1;
		}
	}

	/*
	 * The mute button is broken and only sends press events, we
	 * deal with this in our raw_event handler, so do not map it.
	 */
	if ((drvdata->quirks & QUIRK_MEDION_E1239T) &&
	    usage->hid == (HID_UP_CONSUMER | 0xe2)) {
		input_set_capability(hi->input, EV_KEY, KEY_MUTE);
		return -1;
	}

	return 0;
}

static int asus_start_multitouch(struct hid_device *hdev)
{
	int ret;
	static const unsigned char buf[] = {
		FEATURE_REPORT_ID, 0x00, 0x03, 0x01, 0x00
	};
	unsigned char *dmabuf = kmemdup(buf, sizeof(buf), GFP_KERNEL);

	if (!dmabuf) {
		ret = -ENOMEM;
		hid_err(hdev, "Asus failed to alloc dma buf: %d\n", ret);
		return ret;
	}

	ret = hid_hw_raw_request(hdev, dmabuf[0], dmabuf, sizeof(buf),
					HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	kfree(dmabuf);

	if (ret != sizeof(buf)) {
		hid_err(hdev, "Asus failed to start multitouch: %d\n", ret);
		return ret;
	}

	return 0;
}

static int __maybe_unused asus_resume(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)
	if (drvdata->has_dldev_kbd || drvdata->has_dldev_lightbar)
		asus_aura_restore(drvdata);
#endif

	/*
	 * If we have a backlight listener registered, restore the previous state,
	 * in case of error do not fail: most models restore the backlight
	 * automatically, and the error is non-fatal.
	 */
	if (drvdata->listener.brightness_set)
		asus_kbd_backlight_set(&drvdata->listener, drvdata->kbd_backlight_brightness);

	return 0;
}

static int __maybe_unused asus_reset_resume(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)
	if (drvdata->has_dldev_kbd || drvdata->has_dldev_lightbar)
		asus_aura_restore(drvdata);
#endif

	if (drvdata->tp)
		return asus_start_multitouch(hdev);

	return 0;
}

static int asus_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct hid_report_enum *rep_enum;
	struct asus_drvdata *drvdata;
	struct hid_report *rep;
	bool is_vendor = false;
	int ret;

	drvdata = devm_kzalloc(&hdev->dev, sizeof(*drvdata), GFP_KERNEL);
	if (drvdata == NULL)
		return -ENOMEM;

	hid_set_drvdata(hdev, drvdata);

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)
	ret = devm_mutex_init(&hdev->dev, &drvdata->aura_lock);
	if (ret)
		return ret;

	drvdata->aura_buf = devm_kzalloc(&hdev->dev, AURA_FEATURE_REPORT_SIZE,
					 GFP_KERNEL);
	if (!drvdata->aura_buf)
		return -ENOMEM;
#endif

	drvdata->quirks = id->driver_data;

	/*
	 * T90CHI's keyboard dock returns same ID values as T100CHI's dock.
	 * Thus, identify T90CHI dock with product name string.
	 */
	if (strstr(hdev->name, "T90CHI")) {
		drvdata->quirks &= ~QUIRK_T100CHI;
		drvdata->quirks |= QUIRK_T90CHI;
	}

	if (drvdata->quirks & QUIRK_IS_MULTITOUCH)
		drvdata->tp = &asus_i2c_tp;

	if ((drvdata->quirks & QUIRK_T100_KEYBOARD) && hid_is_usb(hdev)) {
		struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

		if (intf->altsetting->desc.bInterfaceNumber == T100_TPAD_INTF) {
			drvdata->quirks = QUIRK_SKIP_INPUT_MAPPING;
			/*
			 * The T100HA uses the same USB-ids as the T100TAF and
			 * the T200TA uses the same USB-ids as the T100TA, while
			 * both have different max x/y values as the T100TA[F].
			 */
			if (dmi_match(DMI_PRODUCT_NAME, "T100HAN"))
				drvdata->tp = &asus_t100ha_tp;
			else if (dmi_match(DMI_PRODUCT_NAME, "T200TA"))
				drvdata->tp = &asus_t200ta_tp;
			else
				drvdata->tp = &asus_t100ta_tp;
		}
	}

	if (drvdata->quirks & QUIRK_T100CHI) {
		/*
		 * All functionality is on a single HID interface and for
		 * userspace the touchpad must be a separate input_dev.
		 */
		hdev->quirks |= HID_QUIRK_MULTI_INPUT;
		drvdata->tp = &asus_t100chi_tp;
	}

	if ((drvdata->quirks & QUIRK_MEDION_E1239T) && hid_is_usb(hdev)) {
		struct usb_host_interface *alt =
			to_usb_interface(hdev->dev.parent)->altsetting;

		if (alt->desc.bInterfaceNumber == MEDION_E1239T_TPAD_INTF) {
			/* For separate input-devs for tp and tp toggle key */
			hdev->quirks |= HID_QUIRK_MULTI_INPUT;
			drvdata->quirks |= QUIRK_SKIP_INPUT_MAPPING;
			drvdata->tp = &medion_e1239t_tp;
		}
	}

	if (drvdata->quirks & QUIRK_NO_INIT_REPORTS)
		hdev->quirks |= HID_QUIRK_NO_INIT_REPORTS;

	drvdata->hdev = hdev;

	if (drvdata->quirks & (QUIRK_T100CHI | QUIRK_T90CHI)) {
		ret = asus_battery_probe(hdev);
		if (ret) {
			hid_err(hdev,
			    "Asus hid battery_probe failed: %d\n", ret);
			return ret;
		}
	}

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "Asus hid parse failed: %d\n", ret);
		return ret;
	}

	/*
	 * USB 0x193b is shared by Slash and AniMe Matrix. LampArray siblings
	 * bind as usual; otherwise require a positive Slash/AniMe classification.
	 */
	if (hdev->product == USB_DEVICE_ID_ASUSTEK_ROG_SLASH &&
	    !asus_is_lamparray_interface(hdev)) {
		enum asus_pid_193b_kind kind = asus_classify_pid_193b(hdev);

		if (kind == ASUS_PID_193B_NONE) {
			hid_dbg(hdev,
				"Skipping ambiguous 0x193b (not Slash/AniMe)\n");
			return -ENODEV;
		}
	}

	/* Check for vendor for RGB init and handle generic devices properly. */
	rep_enum = &hdev->report_enum[HID_INPUT_REPORT];
	list_for_each_entry(rep, &rep_enum->report_list, list) {
		if ((rep->application & HID_USAGE_PAGE) == HID_UP_ASUSVENDOR)
			is_vendor = true;
	}

	/*
	 * A vendor collection may be the only application collection on the
	 * interface, which hidinput_connect() otherwise skips, leaving the
	 * hotkey usages unmapped. Unpopulated inputs are dropped later.
	 */
	if (is_vendor && (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD))
		hdev->quirks |= HID_QUIRK_HIDINPUT_FORCE;

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)
	/*
	 * LampArray is a Dynamic Lighting backend owned in-kernel: do not
	 * export hidraw for that interface. Aura 0xBC remains the fallback
	 * when LampArray is absent.
	 */
	if (asus_is_lamparray_interface(hdev)) {
		ret = hid_hw_start(hdev, 0);
		if (ret) {
			hid_err(hdev, "Asus LampArray hw start failed: %d\n", ret);
			return ret;
		}
		hid_info(hdev, "Bound ASUS LampArray interface (no hidraw)\n");
		return 0;
	}
#endif

	ret = asus_worker_create(hdev, drvdata);
	if (ret) {
		hid_warn(hdev, "Failed to initialize worker: %d\n", ret);
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		asus_worker_stop(drvdata->worker);
		hid_err(hdev, "Asus hw start failed: %d\n", ret);
		return ret;
	}

	if (asus_classify_pid_193b(hdev) == ASUS_PID_193B_ANIME) {
		ret = asus_init_anime(hdev);
		if (ret < 0)
			hid_warn(hdev, "Failed to initialize AniMe Matrix: %d\n",
				 ret);
	} else {
		/*
		 * Read any committed Aura effect before the keyboard handshake
		 * overwrites the feature report. Skip on AniMe Matrix.
		 */
		if (asus_has_report_id(hdev, FEATURE_KBD_LED_REPORT_ID1))
			asus_aura_capture_boot_effect(hdev);

		if (!drvdata->tp) {
			for (int r = 0; r < ARRAY_SIZE(asus_report_id_init); r++) {
				if (asus_has_report_id(hdev, asus_report_id_init[r])) {
					ret = asus_kbd_init(hdev, asus_report_id_init[r]);
					if (ret < 0)
						hid_warn(hdev,
							 "Failed to initialize 0x%x: %d.\n",
							 asus_report_id_init[r], ret);
				}
			}
		}

		/* Laptops keyboard backlight is always at 0x5a */
		if (is_vendor && (drvdata->quirks & QUIRK_USE_KBD_BACKLIGHT) &&
		    (asus_has_report_id(hdev, FEATURE_KBD_REPORT_ID)) &&
		    (asus_kbd_register_leds(hdev)))
			hid_warn(hdev, "Failed to initialize backlight.\n");

		if (asus_has_report_id(hdev, FEATURE_KBD_LED_REPORT_ID1) ||
		    asus_has_report_id(hdev, FEATURE_KBD_LED_REPORT_ID2)) {
			ret = asus_init_dynamic_lighting(hdev);
			if (ret < 0)
				hid_warn(hdev,
					 "Failed to initialize dynamic lighting: %d\n",
					 ret);

			ret = asus_init_slash(hdev);
			if (ret < 0)
				hid_warn(hdev,
					 "Failed to initialize Slash lighting: %d\n",
					 ret);
		}
	}

	/*
	 * For ROG keyboards, skip rename for consistency and ->input check as
	 * some devices do not have inputs.
	 */
	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD)
		return 0;

	/*
	 * Check that input registration succeeded. Checking that
	 * HID_CLAIMED_INPUT is set prevents a UAF when all input devices
	 * were freed during registration due to no usages being mapped,
	 * leaving drvdata->input pointing to freed memory.
	 */
	if (drvdata->input && (hdev->claimed & HID_CLAIMED_INPUT)) {
		if (drvdata->tp)
			drvdata->input->name = "Asus TouchPad";
		else
			drvdata->input->name = "Asus Keyboard";

		if (drvdata->tp) {
			ret = asus_start_multitouch(hdev);
			if (ret)
				goto err_stop_hw;
		}
	}

	return 0;
err_stop_hw:
	if (drvdata->listener.brightness_set)
		asus_hid_unregister_listener(&drvdata->listener);

	asus_worker_stop(drvdata->worker);
	hid_hw_stop(hdev);
	return ret;
}

static void asus_remove(struct hid_device *hdev)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->listener.brightness_set)
		asus_hid_unregister_listener(&drvdata->listener);

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)
	if (asus_is_lamparray_interface(hdev)) {
		asus_lamparray_unbind_from_owners(hdev);
		hid_hw_stop(hdev);
		return;
	}
	if (drvdata->lamparray_hdev) {
		struct hid_device *la = NULL;

		scoped_guard(mutex, &drvdata->aura_lock) {
			asus_lamparray_release_unlocked(drvdata);
			la = drvdata->lamparray_hdev;
			drvdata->lamparray_hdev = NULL;
			drvdata->lamparray_count = 0;
		}
		if (la)
			put_device(&la->dev);
	}
#endif

	asus_worker_stop(drvdata->worker);
	hid_hw_stop(hdev);
}

static const __u8 asus_g752_fixed_rdesc[] = {
        0x19, 0x00,			/*   Usage Minimum (0x00)       */
        0x2A, 0xFF, 0x00,		/*   Usage Maximum (0xFF)       */
};

static const __u8 *asus_report_fixup(struct hid_device *hdev, __u8 *rdesc,
		unsigned int *rsize)
{
	struct asus_drvdata *drvdata = hid_get_drvdata(hdev);

	if (drvdata->quirks & QUIRK_FIX_NOTEBOOK_REPORT &&
			*rsize >= 56 && rdesc[54] == 0x25 && rdesc[55] == 0x65) {
		hid_info(hdev, "Fixing up Asus notebook report descriptor\n");
		rdesc[55] = 0xdd;
	}
	/* For the T100TA/T200TA keyboard dock */
	if (drvdata->quirks & QUIRK_T100_KEYBOARD &&
		 (*rsize == 76 || *rsize == 101) &&
		 rdesc[73] == 0x81 && rdesc[74] == 0x01) {
		hid_info(hdev, "Fixing up Asus T100 keyb report descriptor\n");
		rdesc[74] &= ~HID_MAIN_ITEM_CONSTANT;
	}
	/* For the T100CHI/T90CHI keyboard dock */
	if (drvdata->quirks & (QUIRK_T100CHI | QUIRK_T90CHI)) {
		int rsize_orig;
		int offs;

		if (drvdata->quirks & QUIRK_T100CHI) {
			rsize_orig = 403;
			offs = 388;
		} else {
			rsize_orig = 306;
			offs = 291;
		}

		/*
		 * Change Usage (76h) to Usage Minimum (00h), Usage Maximum
		 * (FFh) and clear the flags in the Input() byte.
		 * Note the descriptor has a bogus 0 byte at the end so we
		 * only need 1 extra byte.
		 */
		if (*rsize == rsize_orig &&
			rdesc[offs] == 0x09 && rdesc[offs + 1] == 0x76) {
			__u8 *new_rdesc;

			new_rdesc = devm_kzalloc(&hdev->dev, rsize_orig + 1,
						 GFP_KERNEL);
			if (!new_rdesc)
				return rdesc;

			hid_info(hdev, "Fixing up %s keyb report descriptor\n",
				drvdata->quirks & QUIRK_T100CHI ?
				"T100CHI" : "T90CHI");

			memcpy(new_rdesc, rdesc, rsize_orig);
			*rsize = rsize_orig + 1;
			rdesc = new_rdesc;

			memmove(rdesc + offs + 4, rdesc + offs + 2, 12);
			rdesc[offs] = 0x19;
			rdesc[offs + 1] = 0x00;
			rdesc[offs + 2] = 0x29;
			rdesc[offs + 3] = 0xff;
			rdesc[offs + 14] = 0x00;
		}
	}

	if (drvdata->quirks & QUIRK_G752_KEYBOARD &&
		 *rsize == 75 && rdesc[61] == 0x15 && rdesc[62] == 0x00) {
		/* report is missing usage minimum and maximum */
		__u8 *new_rdesc;
		size_t new_size = *rsize + sizeof(asus_g752_fixed_rdesc);

		new_rdesc = devm_kzalloc(&hdev->dev, new_size, GFP_KERNEL);
		if (new_rdesc == NULL)
			return rdesc;

		hid_info(hdev, "Fixing up Asus G752 keyb report descriptor\n");
		/* copy the valid part */
		memcpy(new_rdesc, rdesc, 61);
		/* insert missing part */
		memcpy(new_rdesc + 61, asus_g752_fixed_rdesc, sizeof(asus_g752_fixed_rdesc));
		/* copy remaining data */
		memcpy(new_rdesc + 61 + sizeof(asus_g752_fixed_rdesc), rdesc + 61, *rsize - 61);

		*rsize = new_size;
		rdesc = new_rdesc;
	}

	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD &&
			*rsize == 331 && rdesc[190] == 0x85 && rdesc[191] == 0x5a &&
			rdesc[204] == 0x95 && rdesc[205] == 0x05) {
		hid_info(hdev, "Fixing up Asus N-KEY keyb report descriptor\n");
		rdesc[205] = 0x01;
	}

	/* match many more n-key devices */
	if (drvdata->quirks & QUIRK_ROG_NKEY_KEYBOARD && *rsize > 15) {
		for (int i = 0; i < *rsize - 15; i++) {
			/* offset to the count from 0x5a report part always 14 */
			if (rdesc[i] == 0x85 && rdesc[i + 1] == 0x5a &&
			    rdesc[i + 14] == 0x95 && rdesc[i + 15] == 0x05) {
				hid_info(hdev, "Fixing up Asus N-Key report descriptor\n");
				rdesc[i + 15] = 0x01;
				break;
			}
		}
	}

	return rdesc;
}

static const struct hid_device_id asus_devices[] = {
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_I2C_KEYBOARD), I2C_KEYBOARD_QUIRKS},
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_I2C_ZENBOOK_KEYBOARD),
	  I2C_KEYBOARD_QUIRKS | QUIRK_FILTER_CAMERA_COMPANION },
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_I2C_TOUCHPAD), I2C_TOUCHPAD_QUIRKS },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_ROG_KEYBOARD1), QUIRK_USE_KBD_BACKLIGHT },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_ROG_KEYBOARD2), QUIRK_USE_KBD_BACKLIGHT },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_ROG_KEYBOARD3), QUIRK_G752_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_FX503VD_KEYBOARD),
	  QUIRK_USE_KBD_BACKLIGHT },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_SLASH),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_HID_FN_LOCK },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD2),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_HID_FN_LOCK },
	{ HID_I2C_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD2),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_HID_FN_LOCK },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD3),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_Z13_LIGHTBAR),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_ROG_ALLY_XPAD},
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_NKEY_ALLY_X),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD | QUIRK_ROG_ALLY_XPAD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_XGM_2022),
	},
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_XGM_2023),
	},
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
	    USB_DEVICE_ID_ASUSTEK_ROG_CLAYMORE_II_KEYBOARD),
	  QUIRK_ROG_CLAYMORE_II_KEYBOARD },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_T100TA_KEYBOARD),
	  QUIRK_T100_KEYBOARD | QUIRK_NO_CONSUMER_USAGES },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_T100TAF_KEYBOARD),
	  QUIRK_T100_KEYBOARD | QUIRK_NO_CONSUMER_USAGES },
	{ HID_USB_DEVICE(USB_VENDOR_ID_CHICONY, USB_DEVICE_ID_ASUS_AK1D) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_TURBOX, USB_DEVICE_ID_ASUS_MD_5110) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_JESS, USB_DEVICE_ID_ASUS_MD_5112) },
	{ HID_BLUETOOTH_DEVICE(USB_VENDOR_ID_ASUSTEK,
		USB_DEVICE_ID_ASUSTEK_T100CHI_KEYBOARD), QUIRK_T100CHI },
	{ HID_USB_DEVICE(USB_VENDOR_ID_ITE, USB_DEVICE_ID_ITE_MEDION_E1239T),
		QUIRK_MEDION_E1239T },
	/*
	 * Note bind to the HID_GROUP_GENERIC group, so that we only bind to the keyboard
	 * part, while letting hid-multitouch.c handle the touchpad.
	 */
	{ HID_DEVICE(BUS_USB, HID_GROUP_GENERIC,
		USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_ROG_Z13_FOLIO),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_DEVICE(BUS_BLUETOOTH, HID_GROUP_GENERIC,
		USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_ROG_NKEY_KEYBOARD3_BT),
	  QUIRK_USE_KBD_BACKLIGHT | QUIRK_ROG_NKEY_KEYBOARD },
	{ HID_DEVICE(BUS_USB, HID_GROUP_GENERIC,
		USB_VENDOR_ID_ASUSTEK, USB_DEVICE_ID_ASUSTEK_T101HA_KEYBOARD) },
	{ }
};
MODULE_DEVICE_TABLE(hid, asus_devices);

static struct hid_driver asus_driver = {
	.name			= "asus",
	.id_table		= asus_devices,
	.report_fixup		= asus_report_fixup,
	.probe                  = asus_probe,
	.remove			= asus_remove,
	.input_mapping          = asus_input_mapping,
	.input_configured       = asus_input_configured,
	.reset_resume           = pm_ptr(asus_reset_resume),
	.resume			= pm_ptr(asus_resume),
	.event			= asus_event,
	.raw_event		= asus_raw_event
};
module_hid_driver(asus_driver);

MODULE_IMPORT_NS("ASUS_WMI");
MODULE_LICENSE("GPL");
