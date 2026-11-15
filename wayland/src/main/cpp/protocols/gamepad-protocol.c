
/*
 * Copyright © 2024 Yassine Koubry
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include "wayland-util.h"

static const struct wl_interface *gamepad_unstable_v1_types[] = {
	NULL,
	NULL,
	NULL,
	NULL,
};

static const struct wl_message zwp_gamepad_manager_v1_requests[] = {
	{ "destroy", "", gamepad_unstable_v1_types + 0 },
};

static const struct wl_message zwp_gamepad_manager_v1_events[] = {
	{ "gamepad_added", "usuu", gamepad_unstable_v1_types + 0 },
	{ "gamepad_removed", "u", gamepad_unstable_v1_types + 0 },
	{ "gamepad_button", "uuu", gamepad_unstable_v1_types + 0 },
	{ "gamepad_axis", "uuf", gamepad_unstable_v1_types + 0 },
	{ "gamepad_frame", "uu", gamepad_unstable_v1_types + 0 },
};

WL_EXPORT const struct wl_interface zwp_gamepad_manager_v1_interface = {
	"zwp_gamepad_manager_v1", 1,
	1, zwp_gamepad_manager_v1_requests,
	5, zwp_gamepad_manager_v1_events,
};

