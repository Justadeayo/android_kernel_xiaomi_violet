// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 deu <fawwazzuladhim700@gmail.com>.
 * schedutil_lut.h - Per-cluster util-to-frequency LUT for schedutil governor
 */

#ifndef _SUGOV_DVFS_LUT_H
#define _SUGOV_DVFS_LUT_H

struct sugov_lut_entry {
	unsigned int hroom_util;
	unsigned int freq_khz;
};

/* ── Silver (cpu0-3) ──────────────────────────────────────────────────────── */
static const struct sugov_lut_entry sugov_lut_silver[] = {
	{   0,  1113600 },
	{ 160,  1113600 },
	{ 245,  1305600 },
	{ 281,  1382400 },
	{ 320,  1632000 },
	{ 405,  1785600 },
};

/* ── Gold (cpu4-6) ────────────────────────────────────────────────────────── */
static const struct sugov_lut_entry sugov_lut_gold[] = {
	{   0,   825600 },
	{ 195,   825600 },
	{ 406,  825600 },
	{ 467,  940800 },
	{ 493,  1056000 },
	{ 543,  1171200 },
	{ 597,  1401600 },
	{ 700,  1804800 },
	{ 762,  2016000 },
	{ 826,  2227200 },
	{ 847,  2323200 },
	{ 899,  2419200 },
};

/* ── Prime (cpu7) ─────────────────────────────────────────────────────────── */
static const struct sugov_lut_entry sugov_lut_prime[] = {
	{   0,   825600 },
	{ 195,   825600 },
	{ 406,  1056000 },
	{ 467,  1171200 },
	{ 493,  1401600 },
	{ 543,  1497600 },
	{ 597,  1612800 },
	{ 700,  1804800 },
	{ 792,  2016000 },
	{ 876,  2227200 },
	{ 937,  2323200 },
	{ 985,  2419200 },
	{ 1001, 2534400 },
	{ 1024, 2956800 },
};

#define SUGOV_LUT_SIZE(lut) (ARRAY_SIZE(lut))

/*
 * sugov_lut_lookup - interpolate freq from hroom util using a LUT
 * @lut:  pointer to the cluster's LUT array
 * @size: number of entries (use SUGOV_LUT_SIZE)
 * @util:  util value (0-1024)
 *
 * Returns frequency in kHz.
 */
static inline unsigned int sugov_lut_lookup(const struct sugov_lut_entry *lut,
					    unsigned int size,
					    unsigned long util)
{
	unsigned int i;

	if (util <= lut[0].hroom_util)
		return lut[0].freq_khz;

	for (i = 1; i < size; i++) {
		if (util <= lut[i].hroom_util) {
			unsigned long du = lut[i].hroom_util - lut[i-1].hroom_util;
			unsigned long df = lut[i].freq_khz   - lut[i-1].freq_khz;
			return lut[i-1].freq_khz +
			       (unsigned int)(df * (util - lut[i-1].hroom_util) / du);
		}
	}

	return lut[size - 1].freq_khz;
}

#endif /* _SUGOV_DVFS_LUT_H */