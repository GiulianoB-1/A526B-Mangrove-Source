#include <assert.h>
#include <stdio.h>
#include "p487-re-parser.h"
int main(void)
{
	u32 out;
	const char padded[12] = "7.53";
	assert(!p487_tas_parse_re("7.53", 4, &out) && out == 3947888U);
	assert(!p487_tas_parse_re("07.53\n", 6, &out) && out == 3947888U);
	assert(!p487_tas_parse_re(padded, sizeof(padded), &out) && out == 3947888U);
	assert(p487_tas_parse_re("7.531", 5, &out) < 0);
	assert(p487_tas_parse_re("7.53\0x", 6, &out) < 0);
	assert(p487_tas_parse_re("0.00", 4, &out) < 0);
	puts("P487 TAS shared Re parser PASS");
	return 0;
}
