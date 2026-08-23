/*
 * Goodix 27c6:533c native driver for libfprint — Enrollment flow
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

/* Reset enrollment action state and run the full enroll flow (capture
 * reference -> wait finger -> capture -> extract/quality-gate/store ->
 * wait finger up, repeated GOODIX533C_ENROLL_SAMPLES times); reports
 * completion through fpi_device_enroll_*. Implements
 * FpDeviceClass::enroll. */
void goodix533c_enroll_start (FpDevice *dev);
