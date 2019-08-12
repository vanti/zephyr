/*
 * Copyright (c) 2018 Intel Corporation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <kernel.h>
#include "pm_policy.h"

#include <ti/devices/cc13x2_cc26x2/driverlib/sys_ctrl.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>

#include <ti/drivers/dpl/ClockP.h>
#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/dpl/SwiP.h>

#define LOG_LEVEL CONFIG_SYS_PM_LOG_LEVEL /* From power module Kconfig */
#include <logging/log.h>
LOG_MODULE_DECLARE(power);

#define SECS_TO_TICKS		CONFIG_SYS_CLOCK_TICKS_PER_SEC

/* Wakeup delay from standby in microseconds */
#define WAKEDELAYSTANDBY    240

extern PowerCC26X2_ModuleState PowerCC26X2_module;

/* PM Policy based on SoC/Platform residency requirements */
static const unsigned int pm_min_residency[] = {
#ifdef CONFIG_SYS_POWER_SLEEP_STATES
#ifdef CONFIG_HAS_SYS_POWER_STATE_SLEEP_1
	CONFIG_SYS_PM_MIN_RESIDENCY_SLEEP_1 * SECS_TO_TICKS / MSEC_PER_SEC,
#endif

#endif /* CONFIG_SYS_POWER_SLEEP_STATES */

#ifdef CONFIG_SYS_POWER_DEEP_SLEEP_STATES
#ifdef CONFIG_HAS_SYS_POWER_STATE_DEEP_SLEEP_1
	CONFIG_SYS_PM_MIN_RESIDENCY_DEEP_SLEEP_1 * SECS_TO_TICKS / MSEC_PER_SEC,
#endif

#endif /* CONFIG_SYS_POWER_DEEP_SLEEP_STATES */
};

enum power_states sys_pm_policy_next_state(s32_t ticks)
{
	u32_t constraints;
	bool disallowed = false;
	int i;

	/* check operating conditions, optimally choose DCDC versus GLDO */
	SysCtrl_DCDC_VoltageConditionalControl();

	/* query the declared constraints */
	constraints = Power_getConstraintMask();

	if ((ticks != K_FOREVER) && (ticks < pm_min_residency[0])) {
		LOG_DBG("Not enough time for PM operations: %d", ticks);
		return SYS_POWER_STATE_ACTIVE;
	}

	for (i = ARRAY_SIZE(pm_min_residency) - 1; i >= 0; i--) {
#ifdef CONFIG_SYS_PM_STATE_LOCK
		if (!sys_pm_ctrl_is_state_enabled((enum power_states)(i))) {
			continue;
		}
#endif
		if ((ticks == K_FOREVER) ||
		    (ticks >= pm_min_residency[i])) {
			/* Verify if Power module has constraints set to
			 * disallow a state
			 */
			switch (i) {
			case 0: /* Idle mode */
				if ((constraints & (1 <<
				    PowerCC26XX_DISALLOW_IDLE)) != 0) {
					disallowed = true;
				}
				break;
			case 1: /* Standby mode */
				if ((constraints & (1 <<
				    PowerCC26XX_DISALLOW_STANDBY)) != 0) {
					disallowed = true;
				}
				/* Set timeout for wakeup event */
				if (ticks == K_FOREVER) {
					/* TODO: using a very large timeout to
					 * support 'sleeping forever'. Is there
					 * a etter way?
					 */
					ticks = 0x7FFFFFFF;
				}
				/* TBD: Ideally we'd like to set a timer to
				 * wake up just a little earlier to take care
				 * of the wakeup sequence, ie. by
				 * WAKEDELAYSTANDBY microsecs. However, given
				 * k_timer_start (called later by
				 * ClockP_start) does not have sub-millisecond
				 * accuracy, wakeup can occur up to
				 * WAKEDELAYSTANDBY + 1 msec ahead of the next
				 * timeout.
				 * This also has the implication that
				 * CONFIG_SYS_PM_MIN_RESIDENCY_DEEP_SLEEP_1
				 * must be greater than 1.
				 */
				ticks -= WAKEDELAYSTANDBY *
				    CONFIG_SYS_CLOCK_TICKS_PER_SEC / 1000000;
				ClockP_setTimeout(ClockP_handle(
					(ClockP_Struct *)
					&PowerCC26X2_module.clockObj),
					ticks);
				break;
			default:
				/* This should never be reached */
				LOG_ERR("Invalid sleep state detected\n");
			}

			if (disallowed) {
				disallowed = false;
				continue;
			}

			LOG_DBG("Selected power state %d "
				"(ticks: %d, min_residency: %u)",
				i, ticks, pm_min_residency[i]);
			return (enum power_states)(i);
		}
	}

	LOG_DBG("No suitable power state found!");
	return SYS_POWER_STATE_ACTIVE;
}
