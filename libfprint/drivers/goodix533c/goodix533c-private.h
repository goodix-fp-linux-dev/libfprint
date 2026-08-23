/*
 * Goodix 27c6:533c native driver for libfprint — Private device state
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
 * Shared private state and sub-SSM entry points for the goodix533c driver.
 * goodix533c.c owns the transport/protocol/capture internals and defines
 * everything declared here; goodix533c-match.c, goodix533c-enroll.c, and
 * goodix533c-auth.c only see this header (plus goodix533c-match.h) so they
 * stay decoupled from the wire protocol.
 */

#pragma once

#include "fpi-device.h"
#include "fpi-ssm.h"

/* goodixtls.h embeds SSL_CTX / SSL fields but does not include openssl
 * itself (it relies on its one existing includer, goodix533c.c, having
 * already done so) -- since this header now also embeds a GoodixTlsServer
 * by value in the struct below, include openssl first here too. */
#include <openssl/ssl.h>

#include "../goodixtls/goodixtls.h"

#include "goodix533c.h"

/* Enrollment sample count. Starting point taken from the sibling goodix53x5
 * driver (same SIGFM approach, same 108x88 sensor resolution) — not yet
 * independently tuned against 533c's own capture characteristics. */
#define GOODIX533C_ENROLL_SAMPLES 8

/* SIGFM (SIFT-based) matching parameters — same starting points as
 * goodix53x5-private.h; see the report for why these were kept as-is. */
#define GOODIX533C_SIGFM_BEST_MIN 150
#define GOODIX533C_MIN_CAPTURE_KEYPOINTS 20

/* decode_frame() in goodix533c.c uses the same 12-bit packing as
 * goodix53x5-image.c's goodix_device_decode_image() (bit-identical chunk
 * layout), and NOTES.md's gain sweep confirms this device's raw samples
 * span the same 0-4095 range ("clipped_px=.../9504", "0-4095 range"). Reused
 * as-is; see the report for why this gate is likely inert at the gain this
 * driver already uses. */
#define GOODIX533C_RAW12_CLIP 4095

/* Enrollment stages with more than this fraction of non-contact (clipped)
 * pixels are rejected with a retry. See GOODIX533C_RAW12_CLIP's comment —
 * this gate is expected to rarely (if ever) fire on this device at its
 * current headroom-safe gain, but it is cheap and correct to keep. */
#define GOODIX533C_ENROLL_MAX_CLIPPED_FRACTION 0.10

/* Generic single-in-flight command callback shape. Declared here (not just
 * in goodix533c.c) because it is the type of the callback/user_data fields
 * below. */
typedef void (*Goodix533cCmdCallback)(FpDevice *dev,
                                      guint8   *data,
                                      guint16   length,
                                      gpointer  user_data,
                                      GError   *error);

/* --- Device struct --- */
struct _FpiDeviceGoodix533c
{
  FpDevice      parent_instance;

  GCancellable *transfer_cancel_tkn;
  gboolean      interface_claimed;
  gboolean      read_loop_started;

  /* reassembly buffer for the current incoming pack */
  guint8       *rx_buf;
  guint32       rx_len;

  /* in-flight command state -- single command at a time */
  guint8                cmd;
  gboolean              ack_pending;
  gboolean              reply_pending;
  GSource              *timeout_src;
  Goodix533cCmdCallback callback;
  gpointer              user_data;

  /* embedded TLS-PSK server -- goodixtls.c, unmodified */
  GoodixTlsServer tls;
  gboolean        tls_active;

  /* per-session FDT baseline, read fresh every open */
  guint8   fdt_template[24];
  gboolean have_fdt_template;

  /* most recent no-finger reference frame (raw12), used to flat-field the
   * next live capture against. Re-captured at the start of every
   * enroll/verify/identify attempt, not just once per open() session. */
  guint16  *reference_pixels;
  gboolean  have_reference;

  /* most recent live (finger-present) capture */
  guint16  *live_raw_pixels;           /* raw12, transient */
  guint8   *captured_image;            /* flat-fielded + squashed 8-bit frame,
                                         * GOODIX533C_SENSOR_WIDTH *
                                         * GOODIX533C_SENSOR_HEIGHT bytes --
                                         * this is what SIGFM matches against */
  double    captured_clipped_fraction; /* non-contact pixel fraction, quality gate */

  /* Top-level enroll/verify/identify SSM currently running, if any. */
  FpiSsm *task_ssm;

  /* Enrollment tracking */
  GPtrArray *enroll_features; /* array of GBytes* serialized SIGFM templates */
  gint       enroll_stage;

  /* Failed verify/identify attempts wait for lift-off before completing so
   * one held invalid finger cannot be re-read as the next attempt. */
  gboolean verify_wait_finger_up;

  /* Verify/identify result queued until post-match cleanup (finger-up wait)
   * has completed -- see goodix533c-auth.c. */
  gboolean        pending_result_report;
  FpiDeviceAction pending_result_action;
  FpiMatchResult  pending_verify_result;
  FpPrint        *pending_identify_match;
  GError         *pending_result_error;
  GError         *pending_action_error;
};

/* ===========================================================================
 * Sub-SSM entry points, implemented in goodix533c.c, shared by the
 * capture-test harness and the enroll/auth SSMs below.
 * ======================================================================= */

/* Capture the TX-off no-finger reference frame into self->reference_pixels.
 * Must run before goodix533c_start_live_capture_subsm(). */
void goodix533c_start_ref_capture_subsm (FpiSsm   *parent_ssm,
                                         FpDevice *dev);

/* Arm finger-down detection and block (within the SSM) until the device's
 * asynchronous touch notification arrives. @wait_for_finger_cb is invoked
 * once detection is armed and the wait begins; may be NULL (used by
 * enroll/verify/identify, which use fpi_device_report_finger_status_changes()
 * instead). */
void goodix533c_start_finger_wait_subsm (FpiSsm                 *parent_ssm,
                                         FpDevice               *dev,
                                         Goodix533cProgressFunc  wait_for_finger_cb,
                                         gpointer                wait_for_finger_data);

/* Capture a live finger frame, decrypt/decode it, flat-field it against
 * self->reference_pixels, and store the processed 8-bit frame into
 * self->captured_image plus the quality metric into
 * self->captured_clipped_fraction. */
void goodix533c_start_live_capture_subsm (FpiSsm   *parent_ssm,
                                          FpDevice *dev);

/* Block (within the SSM) until finger lift-off is detected. */
void goodix533c_start_finger_up_subsm (FpiSsm   *parent_ssm,
                                       FpDevice *dev);

/* Force-fail whatever command is currently in flight (ack/reply wait) with
 * G_IO_ERROR_CANCELLED. Used by FpDeviceClass::cancel to unblock a long
 * finger-wait immediately instead of waiting out its timeout. */
void goodix533c_cancel_pending_command (FpDevice *dev);
