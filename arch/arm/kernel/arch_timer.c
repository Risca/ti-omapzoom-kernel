/*
 *  linux/arch/arm/kernel/arch_timer.c
 *
 *  Copyright (C) 2011 ARM Ltd.
 *  All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched_clock.h>
#include <linux/clocksource.h>

#include <asm/delay.h>

#include <clocksource/arm_arch_timer.h>

static unsigned int mult;
static unsigned int shift;

static inline u64 notrace cyc_to_ns(u64 cyc, u32 mult, u32 shift)
{
	return (cyc * mult) >> shift;
}

static unsigned long arch_timer_read_counter_long(void)
{
	return arch_timer_read_counter();
}

static unsigned long long notrace arch_timer_sched_clock(void)
{
	u64 cyc = arch_timer_read_counter();
	return cyc_to_ns(cyc, mult, shift);
}

static struct delay_timer arch_delay_timer;

static void __init arch_timer_delay_timer_register(void)
{
	/* Use the architected timer for the delay loop. */
	arch_delay_timer.read_current_timer = arch_timer_read_counter_long;
	arch_delay_timer.freq = arch_timer_get_rate();
	register_current_timer_delay(&arch_delay_timer);
}

int __init arch_timer_arch_init(void)
{
        u32 arch_timer_rate = arch_timer_get_rate();
	u64 res;

	if (arch_timer_rate == 0)
		return -ENXIO;

	arch_timer_delay_timer_register();

	 /* calculate the mult/shift to convert counter ticks to ns. */
	clocks_calc_mult_shift(&mult, &shift, arch_timer_rate, NSEC_PER_SEC, 0);

	/* calculate the ns resolution of this counter */
	res = cyc_to_ns(1ULL, mult, shift);

	/* Cache the sched_clock multiplier to save a divide in the hot path. */
	sched_clock_func = arch_timer_sched_clock;
	pr_info("sched_clock: ARM arch timer >56 bits at %ukHz, resolution %lluns\n",
		arch_timer_rate / 1000, res);

	return 0;
}
