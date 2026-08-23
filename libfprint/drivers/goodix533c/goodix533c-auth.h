/*
 * Goodix 27c6:533c native driver for libfprint — Verify/identify flow
 *
 * Copyright (C) 2026 libfprint contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "goodix533c-private.h"

/* Reset verify/identify action state and start the top-level auth flow. The
 * shared SSM dispatches on fpi_device_get_current_action() internally.
 * Implements both FpDeviceClass::verify and FpDeviceClass::identify. */
void goodix533c_auth_start (FpDevice *dev);

/* Drop any match result queued while waiting for finger-up. Used by
 * goodix533c_auth_start() and by close()/cancel() to discard stale
 * results. */
void goodix533c_clear_pending_result_report (FpiDeviceGoodix533c *self);
