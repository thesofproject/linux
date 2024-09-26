// SPDX-License-Identifier: GPL-2.0
//
// Register cache access API - maple tree based cache
//
// Copyright 2023 Arm, Ltd
//
// Author: Mark Brown <broonie@kernel.org>

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/maple_tree.h>
#include <linux/slab.h>

#include "internal.h"

#if IS_ENABLED(CONFIG_64BIT)
/*
 * On 64 bit systems uintptr_t will be 64 bit but unsigned long 32 bit
 * so we can store the register values directly in the maple tree.  We
 * need to always set an out of bounds bit due to maple tree's
 * handling of NULL.
 */
#define REGCACHE_MAPLE_NONZERO (1UL << 32)

static unsigned long regcache_maple_entry_to_value(void *entry)
{
	return (uintptr_t)entry & ~REGCACHE_MAPLE_NONZERO;
}

static int regcache_maple_write(struct regmap *map, unsigned int reg,
				unsigned int val)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, reg, reg);
	uintptr_t entry = val | REGCACHE_MAPLE_NONZERO;
	int ret;

	/*
	 * This is safe because the regmap lock means the Maple lock
	 * is redundant, but we need to take it due to lockdep asserts
	 * in the maple tree code.
	 */
	mas_lock(&mas);

	mas_set_range(&mas, reg, reg);
	ret = mas_store_gfp(&mas, (void *)entry, map->alloc_flags);

	mas_unlock(&mas);

	return ret;
}

#else

/*
 * On 32 bit systems we can't distingush between NULL and a valid 0
 * value in a 32 bit register so kmalloc() extra storage for the
 * values.
 */
static unsigned long regcache_maple_entry_to_value(unsigned long *entry)
{
	return *entry;
}

static int regcache_maple_write(struct regmap *map, unsigned int reg,
				unsigned int val)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, reg, reg);
	unsigned long *entry;
	int ret;

	rcu_read_lock();

	entry = mas_walk(&mas);
	if (entry) {
		*entry = val;
		rcu_read_unlock();
		return 0;
	}

	rcu_read_unlock();

	entry = kmalloc(sizeof(unsigned long), map->alloc_flags);
	if (!entry)
		return -ENOMEM;
	*entry = val;

	/*
	 * This is safe because the regmap lock means the Maple lock
	 * is redundant, but we need to take it due to lockdep asserts
	 * in the maple tree code.
	 */
	mas_lock(&mas);

	mas_set_range(&mas, reg, reg);
	ret = mas_store_gfp(&mas, entry, map->alloc_flags);

	mas_unlock(&mas);

	if (ret != 0)
		kfree(entry);
	
	return ret;
}
#endif

static int regcache_maple_read(struct regmap *map,
			       unsigned int reg, unsigned int *value)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, reg, reg);
	void *entry;

	rcu_read_lock();

	entry = mas_walk(&mas);
	if (!entry) {
		rcu_read_unlock();
		return -ENOENT;
	}

	*value = regcache_maple_entry_to_value(entry);

	rcu_read_unlock();

	return 0;
}

static int regcache_maple_drop(struct regmap *map, unsigned int min,
			       unsigned int max)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, min, max);
	unsigned long *entry;
	int ret = 0;

	mas_lock(&mas);

	mas_for_each(&mas, entry, max) {
		if (WARN_ON_ONCE(mas.index != mas.last)) {
			ret = -EFAULT;
			goto out;
		}

		if (mas.index < min || mas.last > max)
			continue;

		/*
		 * This is safe because the regmap lock means the
		 * Maple lock is redundant, but we need to take it due
		 * to lockdep asserts in the maple tree code.
		 */
		if (!IS_ENABLED(CONFIG_64BIT)) {
			mas_unlock(&mas);
			kfree(entry);
			mas_lock(&mas);
		}

		mas_erase(&mas);
	}

out:
	mas_unlock(&mas);

	return ret;
}

static int regcache_maple_sync_block(struct regmap *map, unsigned long *entry,
				     struct ma_state *mas,
				     unsigned int min, unsigned int max)
{
	void *buf;
	unsigned int v;
	unsigned long r;
	size_t val_bytes = map->format.val_bytes;
	int ret = 0;

	mas_pause(mas);
	rcu_read_unlock();

	/*
	 * Use a raw write if writing more than one register to a
	 * device that supports raw writes to reduce transaction
	 * overheads.
	 */
	if (max - min > 1 && regmap_can_raw_write(map)) {
		buf = kmalloc(val_bytes * (max - min), map->alloc_flags);
		if (!buf) {
			ret = -ENOMEM;
			goto out;
		}

		/* Render the data for a raw write */
		for (r = min; r < max + 1; r++) {
			ret = regcache_maple_read(map, r, &v);
			if (ret != 0) {
				kfree(buf);
				goto out;
			}
			regcache_set_val(map, buf, r - min, v);
		}

		ret = _regmap_raw_write(map, min, buf, (max - min + 1) * val_bytes,
					false);

		kfree(buf);
	} else {
		for (r = min; r < max + 1; r++) {
			ret = regcache_maple_read(map, r, &v);
			if (ret != 0)
				goto out;
			ret = _regmap_write(map, r, v);
			if (ret != 0)
				goto out;
		}
	}

out:
	rcu_read_lock();

	return ret;
}

static int regcache_maple_sync(struct regmap *map, unsigned int min,
			       unsigned int max)
{
	struct maple_tree *mt = map->cache;
	unsigned long *entry;
	MA_STATE(mas, mt, min, max);
	unsigned int v, last, sync_start;
	int ret = 0;
	bool sync_needed = false;

	map->cache_bypass = true;

	rcu_read_lock();

	mas_for_each(&mas, entry, max) {
		/* Flush if we hit a gap in the cache */
		if (sync_needed && mas.index != last + 1) {
			ret = regcache_maple_sync_block(map, entry, &mas,
							sync_start, last);
			if (ret != 0)
				goto out;
			sync_needed = false;
		}

		v = regcache_maple_entry_to_value(entry);

		if (regcache_reg_needs_sync(map, mas.index, v)) {
			if (!sync_needed) {
				sync_start = mas.index;
				sync_needed = true;
			}
			last = mas.index;
			continue;
		}

		if (!sync_needed)
			continue;

		ret = regcache_maple_sync_block(map, entry, &mas,
						sync_start, last);
		if (ret != 0)
			goto out;
		sync_needed = false;
	}

	if (sync_needed) {
		ret = regcache_maple_sync_block(map, entry, &mas,
						sync_start, last);
	}

out:
	rcu_read_unlock();

	map->cache_bypass = false;

	return ret;
}

static int regcache_maple_exit(struct regmap *map)
{
	struct maple_tree *mt = map->cache;
	MA_STATE(mas, mt, 0, UINT_MAX);
	unsigned int *entry;

	/* if we've already been called then just return */
	if (!mt)
		return 0;

	mas_lock(&mas);
	if (!IS_ENABLED(CONFIG_64BIT)) {
		mas_for_each(&mas, entry, UINT_MAX)
			kfree(entry);
	}
	__mt_destroy(mt);
	mas_unlock(&mas);

	kfree(mt);
	map->cache = NULL;

	return 0;
}

static int regcache_maple_init(struct regmap *map)
{
	struct maple_tree *mt;
	int i;
	int ret;

	mt = kmalloc(sizeof(*mt), map->alloc_flags);
	if (!mt)
		return -ENOMEM;
	map->cache = mt;

	mt_init(mt);

	for (i = 0; i < map->num_reg_defaults; i++) {
		ret = regcache_maple_write(map,
					   map->reg_defaults[i].reg,
					   map->reg_defaults[i].def);
		if (ret != 0)
			goto err;
	}

	return 0;

err:
	regcache_maple_exit(map);
	return ret;
}

struct regcache_ops regcache_maple_ops = {
	.type = REGCACHE_MAPLE,
	.name = "maple",
	.init = regcache_maple_init,
	.exit = regcache_maple_exit,
	.read = regcache_maple_read,
	.write = regcache_maple_write,
	.drop = regcache_maple_drop,
	.sync = regcache_maple_sync,
};
