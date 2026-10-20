/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * LED Dynamic Lighting Class Interface
 *
 * Copyright (C) 2026 Open Gaming Collective
 * Author: Marco Scardovi <scardracs@disroot.org>
 * Author: Denis Benato <denis.benato@linux.dev>
 */

#ifndef _LINUX_LED_DYNAMIC_LIGHTING_H
#define _LINUX_LED_DYNAMIC_LIGHTING_H

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kconfig.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>

/**
 * enum dl_direction - Effect animation propagation direction
 * @DL_DIRECTION_LEFT: Animation moves toward the left
 * @DL_DIRECTION_RIGHT: Animation moves toward the right
 * @DL_DIRECTION_UP: Animation moves upward
 * @DL_DIRECTION_DOWN: Animation moves downward
 * @DL_DIRECTION_MAX: Number of directions
 */
enum dl_direction {
	DL_DIRECTION_LEFT = 0,
	DL_DIRECTION_RIGHT,
	DL_DIRECTION_UP,
	DL_DIRECTION_DOWN,
	DL_DIRECTION_MAX,
};

/* Power state bitmask flags */
#define DL_POWER_STATE_BOOT		BIT(0)
#define DL_POWER_STATE_AWAKE		BIT(1)
#define DL_POWER_STATE_SLEEP		BIT(2)
#define DL_POWER_STATE_SHUTDOWN		BIT(3)
#define DL_POWER_STATE_ALL		(DL_POWER_STATE_BOOT | \
					 DL_POWER_STATE_AWAKE | \
					 DL_POWER_STATE_SLEEP | \
					 DL_POWER_STATE_SHUTDOWN)

/**
 * struct dl_rgb - 24-bit RGB color representation
 * @r: Red component (0-255)
 * @g: Green component (0-255)
 * @b: Blue component (0-255)
 */
struct dl_rgb {
	u8 r;
	u8 g;
	u8 b;
};

struct led_classdev_dynamic;

/**
 * struct led_dynamic_ops - Hardware driver callback vector
 * @set_effect: Select the effect at @index in ldev->effects
 * @set_speed: Configure effect speed (within speed_min..speed_max)
 * @set_direction: Configure effect propagation direction
 * @set_palette: Apply multi-color stacked palette
 * @set_enabled: Turn the lighting engine on or off without changing effect
 * @direct_write: Stream packed RGB buffer (size must equal led_count * 3)
 * @set_power_states: Update active power state persistence bitmask
 *
 * Every callback is optional. Sysfs files are created only for implemented
 * ops (and the matching capability fields). Existing LED drivers can keep
 * their current brightness / multi_intensity path and implement only the
 * ops they already have hardware for.
 */
struct led_dynamic_ops {
	int (*set_effect)(struct led_classdev_dynamic *ldev,
			  unsigned int index);
	int (*set_speed)(struct led_classdev_dynamic *ldev,
			 unsigned int speed);
	int (*set_direction)(struct led_classdev_dynamic *ldev,
			     enum dl_direction direction);
	int (*set_palette)(struct led_classdev_dynamic *ldev,
			   const struct dl_rgb *palette,
			   unsigned int num_entries);
	int (*set_enabled)(struct led_classdev_dynamic *ldev,
			   bool enabled);
	int (*direct_write)(struct led_classdev_dynamic *ldev,
			    const u8 *buffer, size_t size);
	int (*set_power_states)(struct led_classdev_dynamic *ldev,
				u32 active_states);
};

/**
 * struct led_classdev_dynamic - Dynamic Lighting LED class device
 * @cdev: Embedded standard LED classdev (unused when @host is set)
 * @host: Optional already-registered LED to attach to (drop-in)
 * @ops: Hardware callback dispatch table
 * @lock: Internal mutex protecting ldev state and serialization
 * @zone_type: Driver-defined physical topology string for the lighting zone.
 *	When NULL or empty, sysfs reports ``unknown``
 * @led_count: Total individual LEDs in this zone
 * @direct_sample_size: Bytes per LED in @direct_buffer (0 means 3 / RGB).
 *	Drivers may set 1 for single-channel intensity streams
 * @effects: Driver-owned table of effect names (not a global enum)
 * @num_effects: Number of entries in @effects
 * @current_effect: Index into @effects of the currently selected effect
 * @speed: Current effect animation speed
 * @speed_min: Minimum supported speed level
 * @speed_max: Maximum supported speed level
 * @direction: Current effect animation direction
 * @supported_directions: Bitmask of supported enum dl_direction values
 * @enabled: Lighting engine on/off; independent from @current_effect. The
 *	driver sets the initial value before registration; the class does not
 *	override it
 * @palette: Stacked palette color entries. Drivers may point this at their
 *	own storage of @max_palette_entries entries before registration so
 *	the initial palette is in place when sysfs appears; otherwise the
 *	class allocates it
 * @num_palette_entries: Current number of valid palette entries
 * @max_palette_entries: Maximum allowable palette entries
 * @palette_allocated: True if @palette was allocated by the Dynamic Lighting core
 * @supported_power_states: Bitmask of DL_POWER_STATE_* supported by hardware
 * @active_power_states: Bitmask of currently active DL_POWER_STATE_* states
 * @driver_data: Private driver reference pointer
 * @bin_attr_direct: Per-instance direct RGB binary attribute
 * @bin_attrs: Array of binary attribute pointers for group
 * @write_staging: Scratch buffer for kernfs PAGE_SIZE-chunked bin writes
 * @write_staging_size: Allocated size of @write_staging
 * @write_filled: Bytes accepted so far in the current chunked bin write
 * @group: Per-instance sysfs attribute group
 * @groups: Inline sysfs attribute groups pointer array for cdev
 * @driver_groups: Original driver-provided sysfs groups saved during registration
 * @merged_groups: Optional dynamically allocated merge of driver and Dynamic Lighting groups
 * @attached: True when sysfs was added to @host instead of registering @cdev
 *
 * Drop-in on an existing LED class device
 * ---------------------------------------
 * Drivers that already register a LED (including LED_MULTI_COLOR) can attach
 * Dynamic Lighting without replacing that registration. brightness stays the
 * LED intensity; a driver may map it to a hardware dimmer. effects_palette is
 * the multi-color stack. brightness 0 must not clear a persisted effect:
 * removing a trigger sets brightness to LED_OFF. Lighting off is enabled=false.
 * This class adds optional effect, speed, enabled, palette, and direct RGB nodes:
 *
 *   struct led_classdev_dynamic ldev = {
 *       .host = existing_led_cdev,
 *       .ops = &my_ops,
 *       .effects = my_effects,
 *       .num_effects = ARRAY_SIZE(my_effects),
 *   };
 *   led_classdev_dynamic_register(dev, &ldev);
 *
 * Effect names are defined by the driver. Userspace must read effect_index.
 * Lighting off is enabled=false, not a dedicated off effect.
 */
struct led_classdev_dynamic {
	struct led_classdev cdev;
	struct led_classdev *host;
	const struct led_dynamic_ops *ops;
	struct mutex lock; /* Protects ldev state serialization */

	const char *zone_type;
	unsigned int led_count;
	unsigned int direct_sample_size;

	const char * const *effects;
	unsigned int num_effects;
	unsigned int current_effect;

	unsigned int speed;
	unsigned int speed_min;
	unsigned int speed_max;

	enum dl_direction direction;
	unsigned int supported_directions;

	bool enabled;

	struct dl_rgb *palette;
	unsigned int num_palette_entries;
	unsigned int max_palette_entries;
	bool palette_allocated;

	u32 supported_power_states;
	u32 active_power_states;

	void *driver_data;

	struct bin_attribute bin_attr_direct __aligned(__alignof__(const struct bin_attribute));
	const struct bin_attribute *bin_attrs[2];
	u8 *write_staging;
	size_t write_staging_size;
	size_t write_filled;
	struct attribute_group group;
	const struct attribute_group *groups[2];
	const struct attribute_group **driver_groups;
	const struct attribute_group **merged_groups;
	bool attached;
};

/**
 * led_dynamic_cdev - LED classdev backing a Dynamic Lighting instance
 *
 * Returns @host when this instance was attached to an existing LED,
 * otherwise the embedded @cdev.
 */
static inline struct led_classdev *led_dynamic_cdev(struct led_classdev_dynamic *ldev)
{
	return ldev->host ? ldev->host : &ldev->cdev;
}

/*
 * OUT-OF-TREE SHIM: upstream keeps a led_dynamic back-pointer inside
 * struct led_classdev. This build cannot grow the kernel's struct, so the
 * class keeps a cdev -> ldev table instead (led_dynamic_lookup()).
 * Registration always sets it before sysfs appears.
 */
struct led_classdev_dynamic *led_dynamic_lookup(struct led_classdev *lcdev);

static inline struct led_classdev_dynamic *lcdev_to_dldev(struct led_classdev *lcdev)
{
	return led_dynamic_lookup(lcdev);
}

static inline bool is_dynamic_lighting_led(struct led_classdev *lcdev)
{
	return !!(lcdev->flags & LED_DYNAMIC_LIGHTING);
}

static inline const char *
led_dynamic_effect_name(const struct led_classdev_dynamic *ldev)
{
	if (!ldev->effects || ldev->current_effect >= ldev->num_effects)
		return NULL;

	return ldev->effects[ldev->current_effect];
}

static inline bool led_dynamic_effect_is(const struct led_classdev_dynamic *ldev,
					 const char *name)
{
	const char *cur = led_dynamic_effect_name(ldev);

	return cur && name && !strcmp(cur, name);
}

static inline int led_dynamic_effect_index(const struct led_classdev_dynamic *ldev,
					   const char *name)
{
	if (!ldev->effects || !name)
		return -EINVAL;

	return __sysfs_match_string(ldev->effects, ldev->num_effects, name);
}

/**
 * led_dynamic_fill_effects - compact a name pool through a bitmask
 * @dst: caller-owned array with at least hweight32(@mask) slots
 * @dst_n: number of slots in @dst
 * @pool: driver-owned names indexed by bit number
 * @pool_n: number of entries in @pool
 * @mask: bits selecting which names to include
 *
 * Helper for drivers that already track effects as capability bits and
 * want to publish a Dynamic Lighting table without rewriting that logic.
 * Returns -EINVAL if the selected names do not fit in @dst (no silent
 * truncation).
 *
 * Return: number of names written, or -EINVAL.
 */
static inline int led_dynamic_fill_effects(const char **dst, unsigned int dst_n,
					   const char * const *pool,
					   unsigned int pool_n, u32 mask)
{
	unsigned int i, n = 0, need = 0;

	if (!dst || !pool)
		return -EINVAL;

	/* mask is a u32, and BIT(i) is undefined for i >= 32 on 32-bit. */
	for (i = 0; i < pool_n && i < 32; i++) {
		if ((mask & BIT(i)) && pool[i])
			need++;
	}

	if (need > dst_n)
		return -EINVAL;

	for (i = 0; i < pool_n && i < 32; i++) {
		if (!(mask & BIT(i)) || !pool[i])
			continue;
		dst[n++] = pool[i];
	}

	return n;
}

#if IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC)

int led_classdev_dynamic_register_ext(struct device *parent,
				      struct led_classdev_dynamic *ldev,
				      struct led_init_data *init_data);
void led_classdev_dynamic_unregister(struct led_classdev_dynamic *ldev);
int devm_led_classdev_dynamic_register_ext(struct device *parent,
					   struct led_classdev_dynamic *ldev,
					   struct led_init_data *init_data);
void devm_led_classdev_dynamic_unregister(struct device *parent,
					  struct led_classdev_dynamic *ldev);

#else

static inline int led_classdev_dynamic_register_ext(struct device *parent,
						    struct led_classdev_dynamic *ldev,
						    struct led_init_data *init_data)
{
	return -EOPNOTSUPP;
}

static inline void led_classdev_dynamic_unregister(struct led_classdev_dynamic *ldev) {}

static inline int devm_led_classdev_dynamic_register_ext(struct device *parent,
							 struct led_classdev_dynamic *ldev,
							 struct led_init_data *init_data)
{
	return -EOPNOTSUPP;
}

static inline void devm_led_classdev_dynamic_unregister(struct device *parent,
							struct led_classdev_dynamic *ldev) {}

#endif /* IS_REACHABLE(CONFIG_LEDS_CLASS_DYNAMIC) */

static inline int devm_led_classdev_dynamic_register(struct device *parent,
						     struct led_classdev_dynamic *ldev)
{
	return devm_led_classdev_dynamic_register_ext(parent, ldev, NULL);
}

static inline int led_classdev_dynamic_register(struct device *parent,
						struct led_classdev_dynamic *ldev)
{
	return led_classdev_dynamic_register_ext(parent, ldev, NULL);
}

#endif /* _LINUX_LED_DYNAMIC_LIGHTING_H */
