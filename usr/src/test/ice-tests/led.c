/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2026 Edgecast Cloud LLC.
 */

/* Run the MAC LED callback, reset replay and detach restore. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define	B_TRUE		1
#define	B_FALSE		0
#define	ICE_SUCCESS	0
#define	ASSERT(x)	assert(x)
#define	MUTEX_HELD(m)	(*(m) != 0)

typedef int boolean_t;
typedef unsigned int uint_t;
typedef int kmutex_t;
typedef enum {
	MAC_LED_DEFAULT = 1 << 0,
	MAC_LED_OFF = 1 << 1,
	MAC_LED_IDENT = 1 << 2,
	MAC_LED_ON = 1 << 3
} mac_led_mode_t;

struct ice_port_info {
	int unused;
};

#define	CE_WARN		2

typedef struct ice {
	void *ice_dip;
	kmutex_t ice_rebuild_lock;
	boolean_t ice_led_ident;
	struct {
		struct ice_port_info *port_info;
	} ice_hw;
} ice_t;

static struct ice_port_info port;
static unsigned commands, blinks, errors;
static int fail;

static void
mutex_enter(kmutex_t *m)
{
	assert(*m == 0);
	*m = 1;
}

static void
mutex_exit(kmutex_t *m)
{
	assert(*m == 1);
	*m = 0;
}

static void
ice_error(ice_t *ice, const char *fmt, ...)
{
	(void) ice;
	(void) fmt;
	errors++;
}

static unsigned warnings;

static void
dev_err(void *dip, int level, const char *fmt, ...)
{
	(void) dip;
	assert(level == CE_WARN && fmt[0] == '!');
	warnings++;
}

static ice_t *current;

static int
ice_aq_set_port_id_led(struct ice_port_info *pi, bool is_orig_mode,
    void *cd)
{
	assert(pi == &port && cd == NULL);
	/* The admin queue command runs under the lifecycle lock. */
	assert(current->ice_rebuild_lock == 1);
	commands++;
	if (!is_orig_mode)
		blinks++;
	return (fail);
}

#include "ice_led_body.h"

static ice_t
fresh(void)
{
	ice_t ice;

	(void) memset(&ice, 0, sizeof (ice));
	ice.ice_hw.port_info = &port;
	commands = blinks = errors = warnings = 0;
	fail = 0;
	return (ice);
}

int
main(void)
{
	ice_t ice = fresh();

	current = &ice;
	/* MAC never passes flags; a caller that does is rejected. */
	assert(ice_led_set(&ice, MAC_LED_IDENT, 1) == EINVAL);
	/* The command cannot hold the LED on or off. */
	assert(ice_led_set(&ice, MAC_LED_ON, 0) == ENOTSUP);
	assert(ice_led_set(&ice, MAC_LED_OFF, 0) == ENOTSUP);
	assert(commands == 0);

	assert(ice_led_set(&ice, MAC_LED_IDENT, 0) == 0);
	assert(commands == 1 && blinks == 1 && ice.ice_led_ident);
	assert(ice_led_set(&ice, MAC_LED_DEFAULT, 0) == 0);
	assert(commands == 2 && blinks == 1 && !ice.ice_led_ident);

	/* A failed command reports EIO and keeps the accepted mode. */
	fail = -1;
	assert(ice_led_set(&ice, MAC_LED_IDENT, 0) == EIO);
	assert(!ice.ice_led_ident && errors == 1);
	assert(ice.ice_rebuild_lock == 0);

	/* A rebuild blinks again only when MAC holds IDENT. */
	ice = fresh();
	ice.ice_rebuild_lock = 1;
	ice_led_replay(&ice);
	assert(commands == 0);
	ice.ice_led_ident = B_TRUE;
	ice_led_replay(&ice);
	assert(commands == 1 && blinks == 1 && ice.ice_led_ident);
	fail = -1;
	ice_led_replay(&ice);
	assert(errors == 1 && ice.ice_led_ident);
	ice.ice_rebuild_lock = 0;

	/* Detach hands a blinking LED back to firmware, once. */
	ice = fresh();
	ice_led_fini(&ice);
	assert(commands == 0);
	ice.ice_led_ident = B_TRUE;
	ice_led_fini(&ice);
	assert(commands == 1 && blinks == 0 && !ice.ice_led_ident);
	ice_led_fini(&ice);
	assert(commands == 1 && ice.ice_rebuild_lock == 0 && warnings == 0);

	/* A failed restore is logged and the LED is still recorded as IDENT. */
	ice = fresh();
	ice.ice_led_ident = B_TRUE;
	fail = -1;
	ice_led_fini(&ice);
	assert(commands == 1 && warnings == 1 && ice.ice_led_ident);
	assert(ice.ice_rebuild_lock == 0);

	(void) puts("PASS: LED identify, replay and detach restore");
	return (0);
}
