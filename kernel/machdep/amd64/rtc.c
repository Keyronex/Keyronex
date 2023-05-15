/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon May 15 2023.
 */

#include <kdk/amd64/portio.h>
#include <kdk/kernel.h>
#include <stdbool.h>

enum { cmos_address = 0x70, cmos_data = 0x71 };

/* from http://howardhinnant.github.io/date_algorithms.html */
int64_t
days_from_civil(int64_t y, unsigned m, unsigned d)
{
	y -= m <= 2;
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	const unsigned yoe = (unsigned)(y - era * 400); // [0, 399]
	const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d -
	    1; // [0, 365]
	const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 +
	    doy; // [0, 146096]
	return era * 146097 + (int64_t)(doe)-719468;
}

int
get_update_in_progress_flag()
{
	outb(cmos_address, 0x0A);
	return (inb(cmos_data) & 0x80);
}

unsigned char
read_cmos(int reg)
{
	outb(cmos_address, reg);
	return inb(cmos_data);
}

static int64_t
decode(int64_t value, bool bcd)
{
	if (bcd)
		return (value & 0x0f) + ((value / 16) * 10);
	else
		return value;
}

void
read_rtc()
{
	uint8_t status_b = read_cmos(0x0b);
	bool bcd = !(status_b & 0x04);
	int64_t second, minute, hour, day, month, year;
	uint64_t seconds;

	kassert(status_b & 0x02);

	kprintf("Waiting for RTC.\n");

	while (!(read_cmos(0xa) & 0x80))
		;

	while (read_cmos(0xa) & 0x80)
		asm("pause");

	second = decode(read_cmos(0x0), bcd);
	minute = decode(read_cmos(0x02), bcd);
	hour = decode(read_cmos(0x04), bcd);
	day = decode(read_cmos(0x07), bcd);
	month = decode(read_cmos(0x08), bcd);
	year = decode(read_cmos(0x09), bcd) + 2000;

	seconds = second;
	seconds += minute * 60;
	seconds += hour * 3600;
	seconds += days_from_civil(year, month, day) * 86400;

	ke_datetime_set(seconds * NS_PER_S);

	kprintf("Booting at %ld-%ld-%ld %ld:%ld GMT\n", year, month, day, hour,
	    minute);
}
