/*
Copyright 2022 Huckies <https://github.com/Huckies>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "timer.h"

/* RTC ticks since timer_clear(). The RTC count itself wraps at RTC_TIMER_MAX_VALUE (one RTC day, 24.6 h
 * with the 32000 Hz internal clock), so the ticks are summed up from the steps between two reads; any
 * read at least once per RTC day keeps it right. Not safe against a second caller in an interrupt: no
 * interrupt handler reads the timer. */
static uint64_t ticks_total = 0;
static uint32_t ticks_last  = 0;

// Generate out-of-line copies for inline functions defined in timer.h.
extern inline fast_timer_t timer_read_fast(void);
extern inline fast_timer_t timer_elapsed_fast(fast_timer_t last);

inline void timer_init(void)
{
    timer_clear();
}

__HIGH_CODE inline uint16_t timer_read(void)
{
    return (uint16_t)(timer_read32() & 0xFFFF);
}

/* Integer only: RTC_TO_MS() is double arithmetic, which is software floating point on this chip, and
 * this is called many times per main loop pass. Wraps after 49.7 days like any 32-bit ms timer. */
__HIGH_CODE uint32_t timer_read32(void)
{
    uint32_t raw = timer_readraw();

    ticks_total += raw >= ticks_last ? raw - ticks_last : raw + (RTC_TIMER_MAX_VALUE - ticks_last);
    ticks_last = raw;
#if FREQ_RTC == 32000
    return (uint32_t)(ticks_total >> 5); /* 32 ticks per ms */
#elif FREQ_RTC == 32768
    return (uint32_t)((ticks_total * 125) >> 12); /* 1000 / 32768 */
#else
#error "timer_read32(): unknown FREQ_RTC"
#endif
}

__HIGH_CODE inline uint64_t timer_read64(void)
{
    return (uint64_t)timer_read32();
}

__HIGH_CODE inline uint16_t timer_elapsed(uint16_t tlast)
{
    return TIMER_DIFF_16(timer_read(), tlast);
}

__HIGH_CODE inline uint32_t timer_elapsed32(uint32_t tlast)
{
    return TIMER_DIFF_32(timer_read32(), tlast);
}

__HIGH_CODE void timer_clear(void)
{
    ticks_last  = timer_readraw();
    ticks_total = 0;
}

__HIGH_CODE uint32_t timer_readraw(void)
{
    uint32_t i;

    do {
        i = R32_RTC_CNT_32K;
    } while (i != R32_RTC_CNT_32K);

    return (i);
}
