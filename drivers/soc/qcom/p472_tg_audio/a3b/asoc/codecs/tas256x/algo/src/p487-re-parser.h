/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared bounded parser: TG's fractional field is hundredths, not a float. */
#ifndef P487_TAS_RE_PARSER_H
#define P487_TAS_RE_PARSER_H
#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/errno.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
typedef uint32_t u32;
typedef uint64_t u64;
#endif
static inline int p487_tas_parse_re(const char *s, size_t n, u32 *out)
{
	size_t i = 0;
	u32 whole = 0, frac = 0, digits = 0;
	u64 q;
	while (i < n && s[i] >= '0' && s[i] <= '9') {
		if (whole > 4095)
			return -ERANGE;
		whole = whole * 10 + s[i++] - '0';
		digits++;
	}
	if (!digits || i == n || s[i++] != '.')
		return -EINVAL;
	digits = 0;
	while (i < n && s[i] >= '0' && s[i] <= '9') {
		if (++digits > 2)
			return -EINVAL;
		frac = frac * 10 + s[i++] - '0';
	}
	if (!digits)
		return -EINVAL;
	for (; i < n; i++)
		if (s[i] != '\0' && s[i] != ' ' && s[i] != '\n' &&
		    s[i] != '\r' && s[i] != '\t')
			return -EINVAL;
	q = ((u64)whole << 19) + (((u64)frac << 19) / 100);
	if (!q || q > 0x7fffffffU)
		return -ERANGE;
	*out = (u32)q;
	return 0;
}
#endif
