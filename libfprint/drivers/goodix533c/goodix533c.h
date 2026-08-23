/*
 * Goodix 27c6:533c native driver for libfprint
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

/*
 * This driver targets a single goal: FpDevice open() succeeding against
 * real 27c6:533c hardware, followed by capture of one raw frame.  It is
 * rooted directly at FP_TYPE_DEVICE (not FpImageDevice, not
 * FpiDeviceGoodixTls) because 533c's finger-detect/calibration sequence is
 * fundamentally session-dynamic (see measure_baseline() in
 * driver_53xc.py / findings/native-driver-architecture.md) and does not
 * fit goodix5xx.c's shared FDT state machine, which assumes a static
 * config blob sourced from a no-argument class vfunc.
 *
 * The wire-level checksum/framing codec (goodix_proto.c/.h) and the
 * embedded TLS-PSK server (goodixtls.c/.h) are reused unmodified from the
 * sibling goodixtls/ driver directory -- both are already device-agnostic.
 * Everything else here is new, ported from the *logic* (not the compiled
 * functions -- those are hard-tied to FpiDeviceGoodixTls) of goodix.c,
 * cross-checked stage for stage against vendor/goodix-fp-dump-nikicat's
 * driver_53xc.py, which is authoritative for this exact silicon.
 */

#pragma once

#include "fpi-device.h"

G_DECLARE_FINAL_TYPE (FpiDeviceGoodix533c, fpi_device_goodix533c, FPI,
                      DEVICE_GOODIX533C, FpDevice)

#define FPI_TYPE_DEVICE_GOODIX533C (fpi_device_goodix533c_get_type ())

#define GOODIX533C_SENSOR_WIDTH  (108)
#define GOODIX533C_SENSOR_HEIGHT (88)

/* Matches driver_53xc.py's wait_for_finger() overall deadline (30s).
 * Public so a test harness can quote the same figure in its prompt
 * instead of duplicating the number. */
#define GOODIX533C_FINGER_WAIT_TIMEOUT_MS (30000)

/**
 * Goodix533cProgressFunc: called once, mid-sequence, right as the driver
 * arms finger detection and starts waiting for a touch -- the harness's
 * cue to prompt the user. No data, just a checkpoint.
 */
typedef void (*Goodix533cProgressFunc)(FpDevice *dev,
                                       gpointer  user_data);

/**
 * Goodix533cCaptureDoneFunc: callback for the test-only capture entry
 * point below. Called exactly once, whether the sequence ran to
 * completion or failed partway through -- any frames already captured
 * before the failure are still handed back (non-NULL), so a harness can
 * keep whatever succeeded instead of discarding it just because a later
 * stage (e.g. finger-wait) failed.
 *
 * @raw_pixels: (nullable): the no-finger reference frame,
 *   GOODIX533C_SENSOR_WIDTH * GOODIX533C_SENSOR_HEIGHT 12-bit-ish samples
 *   (one guint16 per pixel, unpacked straight off the wire -- not
 *   squashed), owned by the callee, valid only for the duration of the
 *   callback. NULL if the reference frame itself was never captured.
 * @squashed: (nullable): the reference frame min-max stretched to 8 bits
 *   per pixel, row-major, GOODIX533C_SENSOR_WIDTH * GOODIX533C_SENSOR_HEIGHT
 *   bytes. NULL under the same condition as @raw_pixels.
 * @live_raw_pixels: (nullable): the live (finger-present) frame, same
 *   shape/units as @raw_pixels. NULL unless a finger was detected and a
 *   live frame was successfully captured.
 * @corrected: (nullable): the live frame flat-fielded against the
 *   reference frame (least-squares scale+offset subtracted, see
 *   flat_field() in driver_53xc.py) and then min-max stretched to 8 bits
 *   per pixel, same shape as @squashed. This is the PGM-ready fingerprint
 *   image. NULL under the same condition as @live_raw_pixels.
 */
typedef void (*Goodix533cCaptureDoneFunc)(FpDevice      *dev,
                                          const guint16 *raw_pixels,
                                          const guint8  *squashed,
                                          const guint16 *live_raw_pixels,
                                          const guint8  *corrected,
                                          gpointer       user_data,
                                          GError        *error);

/**
 * fpi_device_goodix533c_capture_test:
 *
 * Not public libfprint API -- a test-only entry point for driving the
 * reset -> PSK/firmware check (already done by open()) -> TLS handshake ->
 * config upload -> FDT baseline -> reference-frame capture -> sleep/query
 * -> arm finger detection -> wait for touch -> live-frame capture -> flat
 * field sequence, for use by a standalone test harness after
 * fp_device_open() has completed. Must only be called once per open()
 * session.
 *
 * @wait_for_finger_cb: (nullable): invoked once finger detection is armed
 *   and the driver starts waiting for a touch, so the harness can prompt
 *   the user right before the bounded wait begins. May be NULL.
 */
void fpi_device_goodix533c_capture_test (FpDevice                  *dev,
                                         Goodix533cProgressFunc     wait_for_finger_cb,
                                         Goodix533cCaptureDoneFunc  callback,
                                         gpointer                   user_data);
