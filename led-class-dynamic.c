// SPDX-License-Identifier: GPL-2.0-only
/*
 * LED Dynamic Lighting Class Interface
 *
 * Copyright (C) 2026 Open Gaming Collective
 * Author: Marco Scardovi <scardracs@disroot.org>
 * Author: Denis Benato <denis.benato@linux.dev>
 */

#include <linux/cleanup.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/hex.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/led-dynamic-lighting.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/list.h>
#include <linux/spinlock.h>

/*
 * OUT-OF-TREE SHIM: upstream stores the back-pointer in
 * led_classdev::led_dynamic. The kernel's struct cannot be extended from a
 * module, so keep a small cdev -> ldev table. Registration is rare and the
 * table short, so a list under a spinlock is enough.
 */
struct dl_backptr {
	struct list_head node;
	struct led_classdev *cdev;
	struct led_classdev_dynamic *ldev;
};

static LIST_HEAD(dl_backptrs);
static DEFINE_SPINLOCK(dl_backptr_lock);

struct led_classdev_dynamic *led_dynamic_lookup(struct led_classdev *cdev)
{
	struct dl_backptr *bp;
	struct led_classdev_dynamic *ldev = NULL;

	if (!cdev)
		return NULL;

	spin_lock(&dl_backptr_lock);
	list_for_each_entry(bp, &dl_backptrs, node) {
		if (bp->cdev == cdev) {
			ldev = bp->ldev;
			break;
		}
	}
	spin_unlock(&dl_backptr_lock);

	return ldev;
}
EXPORT_SYMBOL_GPL(led_dynamic_lookup);

/* Upstream: cdev->led_dynamic = ldev (NULL clears). */
static int dl_backptr_set(struct led_classdev *cdev,
			  struct led_classdev_dynamic *ldev)
{
	struct dl_backptr *bp, *tmp, *fresh = NULL;

	if (ldev) {
		fresh = kzalloc(sizeof(*fresh), GFP_KERNEL);
		if (!fresh)
			return -ENOMEM;
		fresh->cdev = cdev;
		fresh->ldev = ldev;
	}

	spin_lock(&dl_backptr_lock);
	list_for_each_entry_safe(bp, tmp, &dl_backptrs, node) {
		if (bp->cdev == cdev) {
			list_del(&bp->node);
			kfree(bp);
		}
	}
	if (fresh)
		list_add(&fresh->node, &dl_backptrs);
	spin_unlock(&dl_backptr_lock);

	return 0;
}


static const char * const dl_direction_names[] = {
	[DL_DIRECTION_LEFT]			= "left",
	[DL_DIRECTION_RIGHT]			= "right",
	[DL_DIRECTION_UP]			= "up",
	[DL_DIRECTION_DOWN]			= "down",
};

static const char * const dl_power_state_names[] = {
	"boot",
	"awake",
	"sleep",
	"shutdown",
};

static const char * const dl_enabled_names[] = {
	"false",
	"true",
};

static ssize_t dl_emit_indexed_names(char *buf, const char * const *names,
				     unsigned int count, u32 mask, bool filter)
{
	ssize_t len = 0;
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (!names[i])
			continue;
		if (filter && !(mask & BIT(i)))
			continue;

		len += sysfs_emit_at(buf, len, "%s ", names[i]);
	}

	if (len > 0)
		buf[len - 1] = '\n';
	else
		len = sysfs_emit(buf, "\n");

	return len;
}

static ssize_t zone_type_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	if (!ldev->zone_type || !*ldev->zone_type)
		return sysfs_emit(buf, "unknown\n");

	return sysfs_emit(buf, "%s\n", ldev->zone_type);
}
static DEVICE_ATTR_RO(zone_type);

static ssize_t led_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return sysfs_emit(buf, "%u\n", ldev->led_count);
}
static DEVICE_ATTR_RO(led_count);

static ssize_t effect_index_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return dl_emit_indexed_names(buf, ldev->effects, ldev->num_effects, 0, false);
}
static DEVICE_ATTR_RO(effect_index);

static ssize_t effect_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	const char *name;

	guard(mutex)(&ldev->lock);

	name = led_dynamic_effect_name(ldev);
	if (!name)
		return sysfs_emit(buf, "unknown\n");

	return sysfs_emit(buf, "%s\n", name);
}

static ssize_t effect_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	int match, ret;

	if (!ldev->ops->set_effect)
		return -EOPNOTSUPP;

	match = __sysfs_match_string(ldev->effects, ldev->num_effects, buf);
	if (match < 0)
		return -EINVAL;

	guard(mutex)(&cdev->led_access);
	if (led_sysfs_is_disabled(cdev))
		return -EBUSY;
	led_trigger_remove(cdev);
	guard(mutex)(&ldev->lock);

	ret = ldev->ops->set_effect(ldev, match);
	if (ret < 0)
		return ret;

	ldev->current_effect = match;
	return count;
}
static DEVICE_ATTR_RW(effect);

static ssize_t enabled_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return sysfs_emit(buf, "%s\n", ldev->enabled ? "true" : "false");
}

static ssize_t enabled_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	int match, ret;

	if (!ldev->ops->set_enabled)
		return -EOPNOTSUPP;

	match = sysfs_match_string(dl_enabled_names, buf);
	if (match < 0)
		return -EINVAL;

	guard(mutex)(&cdev->led_access);
	if (led_sysfs_is_disabled(cdev))
		return -EBUSY;
	led_trigger_remove(cdev);
	guard(mutex)(&ldev->lock);

	ret = ldev->ops->set_enabled(ldev, match);
	if (ret < 0)
		return ret;

	ldev->enabled = !!match;
	return count;
}
static DEVICE_ATTR_RW(enabled);

static ssize_t enabled_index_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "false true\n");
}
static DEVICE_ATTR_RO(enabled_index);

static ssize_t speed_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return sysfs_emit(buf, "%u\n", ldev->speed);
}

static ssize_t speed_range_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return sysfs_emit(buf, "%u-%u\n", ldev->speed_min, ldev->speed_max);
}
static DEVICE_ATTR_RO(speed_range);

static ssize_t speed_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	unsigned int speed;
	int ret;

	if (!ldev->ops->set_speed)
		return -EOPNOTSUPP;

	ret = kstrtouint(buf, 10, &speed);
	if (ret)
		return ret;

	if (speed < ldev->speed_min || speed > ldev->speed_max)
		return -EINVAL;

	guard(mutex)(&cdev->led_access);
	if (led_sysfs_is_disabled(cdev))
		return -EBUSY;
	led_trigger_remove(cdev);
	guard(mutex)(&ldev->lock);

	ret = ldev->ops->set_speed(ldev, speed);
	if (ret < 0)
		return ret;

	ldev->speed = speed;
	return count;
}
static DEVICE_ATTR_RW(speed);

static ssize_t direction_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	if (ldev->direction >= ARRAY_SIZE(dl_direction_names) ||
	    !dl_direction_names[ldev->direction])
		return sysfs_emit(buf, "unknown\n");

	return sysfs_emit(buf, "%s\n", dl_direction_names[ldev->direction]);
}

static ssize_t direction_index_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return dl_emit_indexed_names(buf, dl_direction_names,
				     ARRAY_SIZE(dl_direction_names),
				     ldev->supported_directions, true);
}
static DEVICE_ATTR_RO(direction_index);

static ssize_t direction_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	int match, ret;

	if (!ldev->ops->set_direction || !ldev->supported_directions)
		return -EOPNOTSUPP;

	match = sysfs_match_string(dl_direction_names, buf);
	if (match < 0 || !(ldev->supported_directions & BIT(match)))
		return -EINVAL;

	guard(mutex)(&cdev->led_access);
	if (led_sysfs_is_disabled(cdev))
		return -EBUSY;
	led_trigger_remove(cdev);
	guard(mutex)(&ldev->lock);

	ret = ldev->ops->set_direction(ldev, match);
	if (ret < 0)
		return ret;

	ldev->direction = match;
	return count;
}
static DEVICE_ATTR_RW(direction);

static ssize_t effects_palette_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	int len = 0;
	unsigned int i;

	guard(mutex)(&ldev->lock);

	for (i = 0; i < ldev->num_palette_entries; i++) {
		len += sysfs_emit_at(buf, len, "#%02x%02x%02x%c",
				     ldev->palette[i].r,
				     ldev->palette[i].g,
				     ldev->palette[i].b,
				     (i == ldev->num_palette_entries - 1) ? '\n' : ' ');
	}

	if (!len)
		len = sysfs_emit(buf, "\n");

	return len;
}

static ssize_t max_palette_entries_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return sysfs_emit(buf, "%u\n", ldev->max_palette_entries);
}
static DEVICE_ATTR_RO(max_palette_entries);

static ssize_t effects_palette_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	const char *cur = buf;
	unsigned int num_parsed = 0;
	int ret;

	if (!ldev->ops->set_palette || !ldev->max_palette_entries)
		return -EOPNOTSUPP;

	struct dl_rgb *temp_palette __free(kfree) = kmalloc_array(ldev->max_palette_entries,
								  sizeof(*temp_palette),
								  GFP_KERNEL);
	if (!temp_palette)
		return -ENOMEM;

	while (*cur) {
		cur = skip_spaces(cur);
		if (!*cur)
			break;

		if (num_parsed >= ldev->max_palette_entries)
			return -EINVAL;

		if (*cur != '#')
			return -EINVAL;
		cur++;

		if (strnlen(cur, 6) < 6)
			return -EINVAL;

		if (hex2bin((u8 *)&temp_palette[num_parsed], cur, 3) < 0)
			return -EINVAL;
		cur += 6;
		if (*cur && !isspace(*cur))
			return -EINVAL;
		num_parsed++;
	}

	if (!num_parsed)
		return -EINVAL;

	guard(mutex)(&cdev->led_access);
	if (led_sysfs_is_disabled(cdev))
		return -EBUSY;
	led_trigger_remove(cdev);
	guard(mutex)(&ldev->lock);

	ret = ldev->ops->set_palette(ldev, temp_palette, num_parsed);
	if (ret < 0)
		return ret;

	memcpy(ldev->palette, temp_palette, num_parsed * sizeof(*temp_palette));
	ldev->num_palette_entries = num_parsed;

	return count;
}
static DEVICE_ATTR_RW(effects_palette);

static ssize_t power_states_index_show(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return dl_emit_indexed_names(buf, dl_power_state_names,
				     ARRAY_SIZE(dl_power_state_names),
				     ldev->supported_power_states, true);
}
static DEVICE_ATTR_RO(power_states_index);

static ssize_t power_states_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	guard(mutex)(&ldev->lock);

	return dl_emit_indexed_names(buf, dl_power_state_names,
				     ARRAY_SIZE(dl_power_state_names),
				     ldev->active_power_states, true);
}

static ssize_t power_states_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	char state_name[16];
	const char *cur = buf;
	u32 target_states = 0;
	int ret, match;
	size_t tok_len;

	if (!ldev->ops->set_power_states || !ldev->supported_power_states)
		return -EOPNOTSUPP;

	while (*cur) {
		cur = skip_spaces(cur);
		if (!*cur || *cur == '\n')
			break;

		tok_len = strcspn(cur, " \t\n");
		if (!tok_len || tok_len >= sizeof(state_name))
			return -EINVAL;

		memcpy(state_name, cur, tok_len);
		state_name[tok_len] = '\0';
		cur += tok_len;

		match = sysfs_match_string(dl_power_state_names, state_name);
		if (match < 0 || !(ldev->supported_power_states & BIT(match)))
			return -EINVAL;

		target_states |= BIT(match);
	}

	guard(mutex)(&cdev->led_access);
	if (led_sysfs_is_disabled(cdev))
		return -EBUSY;
	led_trigger_remove(cdev);
	guard(mutex)(&ldev->lock);

	ret = ldev->ops->set_power_states(ldev, target_states);
	if (ret < 0)
		return ret;

	ldev->active_power_states = target_states;
	return count;
}
static DEVICE_ATTR_RW(power_states);

/*
 * kernfs delivers bin-attribute writes in at most PAGE_SIZE chunks. Stage
 * partial writes and invoke the driver only when the full direct_buffer
 * payload (led_count * direct_sample_size, default 3) has arrived. A short
 * write that cannot grow into that size is rejected. Any other non-zero
 * offset is accepted only as the next contiguous chunk.
 */
static ssize_t led_dynamic_stage_bin_write(struct led_classdev *cdev,
					   struct led_classdev_dynamic *ldev,
					   char *buf, loff_t off, size_t count,
					   size_t size,
					   int (*commit)(struct led_classdev_dynamic *ldev,
							 const u8 *data, size_t len))
{
	size_t end;
	int ret;

	u8 *staging __free(kfree) = NULL;

	if (!size || !count || off < 0)
		return -EINVAL;

	if (check_add_overflow((size_t)off, count, &end) || end > size)
		return -EINVAL;

	if (off == 0 && count < PAGE_SIZE && count != size)
		return -EINVAL;

	/* Allocate before the LED locks. The buffer is adopted only if needed. */
	if (off == 0) {
		staging = kmalloc(size, GFP_KERNEL);
		if (!staging)
			return -ENOMEM;
	}

	guard(mutex)(&cdev->led_access);
	if (led_sysfs_is_disabled(cdev))
		return -EBUSY;
	guard(mutex)(&ldev->lock);

	if (off == 0) {
		ldev->write_filled = 0;
		if (!ldev->write_staging || ldev->write_staging_size != size) {
			kfree(ldev->write_staging);
			ldev->write_staging = no_free_ptr(staging);
			ldev->write_staging_size = size;
		}
	} else if (!ldev->write_staging || ldev->write_staging_size != size ||
		   (size_t)off != ldev->write_filled) {
		return -EINVAL;
	}

	memcpy(ldev->write_staging + off, buf, count);
	ldev->write_filled = end;

	if (end != size)
		return count;

	led_trigger_remove(cdev);
	ret = commit(ldev, ldev->write_staging, size);
	ldev->write_filled = 0;
	if (ret < 0)
		return ret;

	return count;
}

static int led_dynamic_commit_direct(struct led_classdev_dynamic *ldev,
				     const u8 *data, size_t len)
{
	int ret, direct_idx;

	ret = ldev->ops->direct_write(ldev, data, len);
	if (ret < 0)
		return ret;

	direct_idx = led_dynamic_effect_index(ldev, "direct");
	if (direct_idx >= 0)
		ldev->current_effect = direct_idx;

	return 0;
}

static ssize_t direct_buffer_write(struct file *filp, struct kobject *kobj,
				   const struct bin_attribute *bin_attr,
				   char *buf, loff_t off, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);
	size_t expected_size;

	if (!ldev->ops->direct_write)
		return -EOPNOTSUPP;

	if (check_mul_overflow((size_t)ldev->led_count,
			       (size_t)(ldev->direct_sample_size ?: 3),
			       &expected_size))
		return -EOVERFLOW;

	return led_dynamic_stage_bin_write(cdev, ldev, buf, off, count,
					   expected_size,
					   led_dynamic_commit_direct);
}

static int led_dynamic_validate(struct led_classdev_dynamic *ldev, size_t *direct_buffer_size)
{
	unsigned int i;
	unsigned int stride = ldev->direct_sample_size ?: 3;

	if (check_mul_overflow((size_t)ldev->led_count, (size_t)stride,
			       direct_buffer_size))
		return -EOVERFLOW;

	if (ldev->ops->set_effect) {
		if (!ldev->effects || !ldev->num_effects)
			return -EINVAL;

		for (i = 0; i < ldev->num_effects; i++) {
			if (!ldev->effects[i] || !*ldev->effects[i])
				return -EINVAL;
		}

		if (ldev->current_effect >= ldev->num_effects)
			return -EINVAL;
	}

	if (ldev->speed_min > ldev->speed_max)
		return -EINVAL;

	if (ldev->ops->set_speed &&
	    (ldev->speed < ldev->speed_min || ldev->speed > ldev->speed_max))
		return -EINVAL;

	if (ldev->direction >= DL_DIRECTION_MAX)
		return -EINVAL;

	if (ldev->supported_directions &&
	    !(ldev->supported_directions & BIT(ldev->direction)))
		return -EINVAL;

	if (ldev->num_palette_entries > ldev->max_palette_entries)
		return -EINVAL;

	if (ldev->num_palette_entries && !ldev->palette)
		return -EINVAL;

	if (ldev->active_power_states & ~ldev->supported_power_states)
		return -EINVAL;

	if (ldev->host) {
		if (!ldev->host->dev)
			return -EINVAL;
		if (led_dynamic_lookup(ldev->host) ||
		    (ldev->host->flags & LED_DYNAMIC_LIGHTING))
			return -EBUSY;
	}

	return 0;
}

static umode_t dl_attr_is_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	if (attr == &dev_attr_power_states_index.attr ||
	    attr == &dev_attr_power_states.attr) {
		if (!ldev->supported_power_states || !ldev->ops->set_power_states)
			return 0;
	}

	if (attr == &dev_attr_speed_range.attr ||
	    attr == &dev_attr_speed.attr) {
		if (!ldev->ops->set_speed)
			return 0;
	}

	if (attr == &dev_attr_direction_index.attr ||
	    attr == &dev_attr_direction.attr) {
		if (!ldev->supported_directions || !ldev->ops->set_direction)
			return 0;
	}

	if (attr == &dev_attr_max_palette_entries.attr ||
	    attr == &dev_attr_effects_palette.attr) {
		if (!ldev->max_palette_entries || !ldev->ops->set_palette)
			return 0;
	}

	if (attr == &dev_attr_effect.attr || attr == &dev_attr_effect_index.attr) {
		if (!ldev->num_effects || !ldev->ops->set_effect)
			return 0;
	}

	if (attr == &dev_attr_enabled.attr ||
	    attr == &dev_attr_enabled_index.attr) {
		if (!ldev->ops->set_enabled)
			return 0;
	}

	return attr->mode;
}

static umode_t dl_bin_attr_is_visible(struct kobject *kobj,
				      const struct bin_attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct led_classdev *cdev = dev_get_drvdata(dev);
	struct led_classdev_dynamic *ldev = lcdev_to_dldev(cdev);

	if (attr == &ldev->bin_attr_direct) {
		if (!ldev->ops->direct_write || !ldev->led_count)
			return 0;
	}

	return attr->attr.mode;
}

static struct attribute *led_dynamic_attrs[] = {
	&dev_attr_zone_type.attr,
	&dev_attr_led_count.attr,
	&dev_attr_effect_index.attr,
	&dev_attr_effect.attr,
	&dev_attr_enabled.attr,
	&dev_attr_enabled_index.attr,
	&dev_attr_speed_range.attr,
	&dev_attr_speed.attr,
	&dev_attr_direction_index.attr,
	&dev_attr_direction.attr,
	&dev_attr_max_palette_entries.attr,
	&dev_attr_effects_palette.attr,
	&dev_attr_power_states_index.attr,
	&dev_attr_power_states.attr,
	NULL,
};

static void led_dynamic_release_resources(struct led_classdev_dynamic *ldev,
					  struct led_classdev *cdev)
{
	if (ldev->palette_allocated) {
		kfree(ldev->palette);
		ldev->palette = NULL;
		ldev->palette_allocated = false;
	}
	kfree(ldev->write_staging);
	ldev->write_staging = NULL;
	ldev->write_staging_size = 0;
	ldev->write_filled = 0;
	kfree(ldev->merged_groups);
	ldev->merged_groups = NULL;
	if (cdev)
		cdev->groups = ldev->driver_groups;
	mutex_destroy(&ldev->lock);
}

int led_classdev_dynamic_register_ext(struct device *parent,
				      struct led_classdev_dynamic *ldev,
				      struct led_init_data *init_data)
{
	struct led_classdev *cdev;
	size_t direct_buffer_size;
	unsigned int num_driver_groups = 0;
	int ret;

	if (!ldev || !ldev->ops)
		return -EINVAL;

	/*
	 * Host attach adds attributes to an already-registered LED. parent and
	 * init_data apply only to standalone registration of ldev->cdev.
	 */
	if (ldev->host && (parent || init_data))
		return -EINVAL;

	/*
	 * One-shot registration: refuse a second register without unregister.
	 * mutex_init() must not run on a live lock.
	 */
	if (ldev->attached ||
	    led_dynamic_lookup(&ldev->cdev) ||
	    (ldev->cdev.flags & LED_DYNAMIC_LIGHTING) ||
	    (ldev->host && led_dynamic_lookup(ldev->host)))
		return -EBUSY;

	ret = led_dynamic_validate(ldev, &direct_buffer_size);
	if (ret)
		return ret;

	mutex_init(&ldev->lock);
	ldev->attached = false;
	ldev->palette_allocated = false;
	ldev->merged_groups = NULL;
	ldev->write_staging = NULL;
	ldev->write_staging_size = 0;
	ldev->write_filled = 0;

	sysfs_bin_attr_init(&ldev->bin_attr_direct);
	ldev->bin_attr_direct.attr.name = "direct_buffer";
	ldev->bin_attr_direct.attr.mode = 0200;
	ldev->bin_attr_direct.write = direct_buffer_write;
	ldev->bin_attr_direct.size = direct_buffer_size;

	ldev->bin_attrs[0] = &ldev->bin_attr_direct;
	ldev->bin_attrs[1] = NULL;

	ldev->group.attrs = led_dynamic_attrs;
	ldev->group.bin_attrs = ldev->bin_attrs;
	ldev->group.is_visible = dl_attr_is_visible;
	ldev->group.is_bin_visible = dl_bin_attr_is_visible;

	if (ldev->max_palette_entries > 0 && !ldev->palette) {
		ldev->palette =
			kcalloc(ldev->max_palette_entries, sizeof(*ldev->palette),
				GFP_KERNEL);
		if (!ldev->palette) {
			mutex_destroy(&ldev->lock);
			return -ENOMEM;
		}
		ldev->palette_allocated = true;
	}

	if (ldev->host) {
		cdev = ldev->host;
		ret = dl_backptr_set(cdev, ldev);
		if (ret) {
			led_dynamic_release_resources(ldev, NULL);
			return ret;
		}
		cdev->flags |= LED_DYNAMIC_LIGHTING;
		ldev->attached = true;

		ret = device_add_group(cdev->dev, &ldev->group);
		if (ret) {
			dl_backptr_set(cdev, NULL);
			cdev->flags &= ~LED_DYNAMIC_LIGHTING;
			led_dynamic_release_resources(ldev, NULL);
			return ret;
		}

		return 0;
	}

	cdev = &ldev->cdev;
	ret = dl_backptr_set(cdev, ldev);
	if (ret) {
		led_dynamic_release_resources(ldev, cdev);
		return ret;
	}
	cdev->flags |= LED_DYNAMIC_LIGHTING;

	ldev->groups[0] = &ldev->group;
	ldev->groups[1] = NULL;
	ldev->driver_groups = cdev->groups;

	while (cdev->groups && cdev->groups[num_driver_groups])
		num_driver_groups++;

	if (num_driver_groups) {
		unsigned int i;

		ldev->merged_groups = kcalloc(num_driver_groups + 2,
					      sizeof(*ldev->merged_groups),
					      GFP_KERNEL);
		if (!ldev->merged_groups) {
			led_dynamic_release_resources(ldev, cdev);
			dl_backptr_set(cdev, NULL);
			cdev->flags &= ~LED_DYNAMIC_LIGHTING;
			return -ENOMEM;
		}

		for (i = 0; i < num_driver_groups; i++)
			ldev->merged_groups[i] = cdev->groups[i];
		ldev->merged_groups[num_driver_groups] = &ldev->group;
		ldev->merged_groups[num_driver_groups + 1] = NULL;
		cdev->groups = ldev->merged_groups;
	} else {
		cdev->groups = ldev->groups;
	}

	ret = led_classdev_register_ext(parent, cdev, init_data);
	if (ret) {
		led_dynamic_release_resources(ldev, cdev);
		dl_backptr_set(cdev, NULL);
		cdev->flags &= ~LED_DYNAMIC_LIGHTING;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(led_classdev_dynamic_register_ext);

void led_classdev_dynamic_unregister(struct led_classdev_dynamic *ldev)
{
	struct led_classdev *cdev;

	if (!ldev)
		return;

	if (ldev->attached) {
		cdev = ldev->host;
		if (cdev && cdev->dev)
			device_remove_group(cdev->dev, &ldev->group);
		if (cdev) {
			dl_backptr_set(cdev, NULL);
			cdev->flags &= ~LED_DYNAMIC_LIGHTING;
		}
		led_dynamic_release_resources(ldev, NULL);
		ldev->attached = false;
		return;
	}

	cdev = &ldev->cdev;
	led_classdev_unregister(cdev);
	dl_backptr_set(cdev, NULL);
	led_dynamic_release_resources(ldev, cdev);
}
EXPORT_SYMBOL_GPL(led_classdev_dynamic_unregister);

static void devm_led_classdev_dynamic_release(struct device *dev, void *res)
{
	led_classdev_dynamic_unregister(*(struct led_classdev_dynamic **)res);
}

int devm_led_classdev_dynamic_register_ext(struct device *parent,
					   struct led_classdev_dynamic *ldev,
					   struct led_init_data *init_data)
{
	struct led_classdev_dynamic **dr;
	int ret;

	dr = devres_alloc(devm_led_classdev_dynamic_release,
			  sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	ret = led_classdev_dynamic_register_ext(parent, ldev, init_data);
	if (ret) {
		devres_free(dr);
		return ret;
	}

	*dr = ldev;
	devres_add(parent, dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_led_classdev_dynamic_register_ext);

static int devm_led_classdev_dynamic_match(struct device *dev,
					   void *res, void *data)
{
	struct led_classdev_dynamic **p = res;

	if (WARN_ON(!p || !*p))
		return 0;

	return *p == data;
}

void devm_led_classdev_dynamic_unregister(struct device *dev,
					  struct led_classdev_dynamic *ldev)
{
	WARN_ON(devres_release(dev,
			       devm_led_classdev_dynamic_release,
			       devm_led_classdev_dynamic_match, ldev));
}
EXPORT_SYMBOL_GPL(devm_led_classdev_dynamic_unregister);

MODULE_AUTHOR("Marco Scardovi <scardracs@disroot.org>");
MODULE_AUTHOR("Denis Benato <denis.benato@linux.dev>");
MODULE_DESCRIPTION("LED Dynamic Lighting Class Interface");
MODULE_LICENSE("GPL");
