#include <assert.h>
#include <stdio.h>
#include "p487-re-parser.h"
int main(void)
{
	u32 out;
	const char padded[12] = "7.53";
	const char re_l[12] = "06.02", re_r[12] = "06.26";
	const char temp_l[12] = "30", temp_r[12] = "30";
	assert(!p487_tas_parse_re("7.53", 4, &out) && out == 3947888U);
	assert(!p487_tas_parse_re("07.53\n", 6, &out) && out == 3947888U);
	assert(!p487_tas_parse_re(padded, sizeof(padded), &out) && out == 3947888U);
	assert(p487_tas_parse_re("7.531", 5, &out) < 0);
	assert(p487_tas_parse_re("7.53\0x", 6, &out) < 0);
	assert(p487_tas_parse_re("0.00", 4, &out) < 0);
	assert(!p487_tas_parse_re(re_l, sizeof(re_l), &out) && out == 3156213U);
	assert(p487_tas_re_hundredths(out) == 602);
	assert(!p487_tas_parse_re(re_r, sizeof(re_r), &out) && out == 3282042U);
	assert(p487_tas_re_hundredths(out) == 626);
	assert(!p487_tas_parse_temp(temp_l, sizeof(temp_l), &out) && out == 30);
	assert(!p487_tas_parse_temp(temp_r, sizeof(temp_r), &out) && out == 30);
	assert(p487_tas_parse_temp("30\0x", 4, &out) < 0);
	assert(p487_tas_parse_temp("4294967296", 10, &out) < 0);
	puts("P487 TAS exact TG 12-byte Re/TCAL fixtures PASS");
	puts("P487 TAS shared Re parser PASS");
	return 0;
}
