/*
 * Goodix driver for USB devices 27c6:5e0a
 *
 * Copyright (C) 2026 The libfprint Goodix 5e0a contributors
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

#include "drivers/goodixtls/goodix5xx.h"
#include "fp-device.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-assembling.h"
#include "fpi-context.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"
#include "glibconfig.h"
#include "gusb/gusb-device.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define FP_COMPONENT "goodixtls5e0a"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#include "nbis/include/lfs.h"
#pragma GCC diagnostic pop
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix5e0a.h"

struct _FpiDeviceGoodixTls5e0a
{
  FpiDeviceGoodixTls5xx parent;

  gboolean              session_started;
  FpiSsm               *scan_ssm;
  guint                 scan_gen;
  guint                 scan_timeout_gen;
  GSource              *down_timeout;

  /* TLS session parking state across deactivate/activate cycles */
  gboolean              tls_parked;
  gint64                tls_parked_at;
  guint                 tls_parked_gen;

  /* Warm activation state to avoid redundant sensor reset/configuration */
  gboolean              warm_ok;
  gint64                last_clean_mono;
  guint                 warm_boot_seq;
  const char           *warm_down_reason;
  gboolean              warm_attempted;
  gboolean              warm_retried;

  /* Multi-frame burst capture and best-frame selection */
  guint                 frame_count;
  FpImage              *best_img;
  guint                 best_minutiae;
  guint                 best_frame_no;

  /* Verify retry guard against rapid retry burn on continuous touch */
  gboolean              retry_guard;
  gint64                retry_guard_mono;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls5e0a, fpi_device_goodixtls5e0a, FPI,
                      DEVICE_GOODIXTLS5E0A, FpiDeviceGoodixTls5xx);

G_DEFINE_TYPE (FpiDeviceGoodixTls5e0a, fpi_device_goodixtls5e0a,
               FPI_TYPE_DEVICE_GOODIXTLS5XX);

/* Session command used during activation. */
#define GOODIX_CMD_SESSION_D6 (0xd6)

/* Pre-shared key for TLS_PSK_WITH_AES_128_CBC_SHA256. */
static const guint8 goodix_5e0a_psk[32] = {
  0xd8, 0x53, 0xad, 0x19, 0x41, 0xb2, 0xdc, 0x53,
  0x50, 0xc7, 0x66, 0xcd, 0x72, 0x6e, 0xf7, 0xa5,
  0xdf, 0x7d, 0x5f, 0xa3, 0x90, 0x53, 0xbf, 0xac,
  0x26, 0x9c, 0xe7, 0x52, 0xd7, 0xa8, 0xb2, 0xab
};

/* Sensor configuration blob uploaded during activation. */
static const guint8 goodix_5e0a_config[256] = {
  0xb0, 0x11, 0x60, 0x71, 0x2c, 0x9d, 0x2c, 0xc9, 0x1c, 0xe5, 0x18, 0xfd, 0x00, 0xfd, 0x00, 0xfd,
  0x03, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x04, 0x00, 0x84, 0x00, 0x15, 0xb3, 0x86, 0x00, 0x00,
  0xc4, 0x88, 0x00, 0x00, 0xba, 0x8a, 0x00, 0x00, 0xb2, 0x8c, 0x00, 0x00, 0xaa, 0x8e, 0x00, 0x00,
  0xc1, 0x90, 0x00, 0xbb, 0xbb, 0x92, 0x00, 0xb1, 0xb1, 0x94, 0x00, 0x00, 0xa8, 0x96, 0x00, 0x00,
  0xb6, 0x98, 0x00, 0x00, 0x00, 0x9a, 0x00, 0x00, 0x00, 0xd2, 0x00, 0x00, 0x00, 0xd4, 0x00, 0x00,
  0x00, 0xd6, 0x00, 0x00, 0x00, 0xd8, 0x00, 0x00, 0x00, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00,
  0x00, 0x70, 0x00, 0x00, 0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x20, 0x00, 0x10,
  0x40, 0x2a, 0x01, 0x02, 0x04, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x32, 0x00, 0x80, 0x00, 0x01,
  0x00, 0x5c, 0x00, 0x80, 0x00, 0x56, 0x00, 0x24, 0x20, 0x58, 0x00, 0x03, 0x02, 0x32, 0x00, 0x0c,
  0x02, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80, 0x15, 0x2a, 0x01, 0x82,
  0x03, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14, 0x00, 0x80, 0x00, 0x01, 0x00, 0x5c, 0x00, 0x00,
  0x01, 0x56, 0x00, 0x04, 0x20, 0x58, 0x00, 0x03, 0x02, 0x32, 0x00, 0x0c, 0x02, 0x66, 0x00, 0x03,
  0x00, 0x7c, 0x00, 0x00, 0x58, 0x82, 0x00, 0x80, 0x15, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x80,
  0x00, 0x54, 0x00, 0x10, 0x01, 0x62, 0x00, 0x04, 0x03, 0x64, 0x00, 0x19, 0x00, 0x66, 0x00, 0x03,
  0x00, 0x7c, 0x00, 0x01, 0x58, 0x2a, 0x01, 0x08, 0x00, 0x5c, 0x00, 0x00, 0x01, 0x52, 0x00, 0x08,
  0x00, 0x54, 0x00, 0x00, 0x01, 0x66, 0x00, 0x03, 0x00, 0x7c, 0x00, 0x01, 0x58, 0x00, 0x53, 0x0e
};

/* Session initialization commands */
static const guint8 goodix_5e0a_query_ae[3]    = {0x00, 0x01, 0x00};
static const guint8 goodix_5e0a_session_d6[2]  = {0x00, 0x00};

/* Image capture payload, also published via capture_payload. */
static const guint8 goodix_5e0a_img_payload[10] = {
  0x05, 0x00, 0xb0, 0x00, 0xb2, 0x00, 0xb0, 0x00, 0xb1, 0x00
};

/* Finger-detect down table. */
static const guint8 goodix_5e0a_down_s12[35] = {
  0x1c, 0x01, 0xb0, 0x00, 0xb2, 0x00, 0xb0, 0x00, 0xb1, 0x00,
  0x80, 0xb7, 0x80, 0xce, 0x80, 0xaa, 0x80, 0xbe, 0x80, 0xb1, 0x80, 0xc2,
  0x00, 0x00, 0x00, 0x00,
  0xb0, 0x00, 0xb2, 0x00, 0xb0, 0x00, 0xb1, 0x00, 0x00
};

/* Finger-detect up table. */
static const guint8 goodix_5e0a_up_u01[35] = {
  0x0e, 0x01, 0xb0, 0x00, 0xb2, 0x00, 0xb0, 0x00, 0xb1, 0x00,
  0x80, 0x94, 0x80, 0xc2, 0x80, 0x97, 0x80, 0xb1, 0x80, 0xa5, 0x80, 0x21,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const FpIdEntry goodix_5e0a_id_table[] = {
  {.vid = 0x27c6, .pid = 0x5e0a},
  {.vid = 0, .pid = 0, .driver_data = 0},
};

static void goodix5e0a_reset_touch_frames (FpiDeviceGoodixTls5e0a *self);

#define GOODIX_5E0A_TLS_PARK_TTL_US (G_USEC_PER_SEC * 30)
#define GOODIX_5E0A_TLS_PARK_HEALTH_TIMEOUT_MS 500
#define GOODIX_5E0A_WARM_TTL_US (G_USEC_PER_SEC * 60)

enum activate_states {
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_RESET,
  ACTIVATE_READ_CHIP_ID,
  ACTIVATE_READ_OTP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_UPLOAD_CONFIG,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_NUM_STATES,
};

static void activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error);

/* Read the PSK slot before TLS to latch the MCU crypto state. Best effort:
 * failure falls through to TLS and surfaces there. */
static void
on_psk_hash_read (FpDevice *dev, gboolean success, guint32 flags,
                  guint8 *psk, guint16 length, gpointer user_data,
                  GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fp_warn ("PSK hash read failed: %s", error->message);
      g_error_free (error);
    }
  else
    {
      fp_dbg ("PSK hash read: success=%d, len=%u", success, length);
    }
  fpi_ssm_next_state (ssm);
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_READ_AND_NOP:
      goodix_start_read_loop (dev);
      goodix_send_nop (dev, goodixtls5xx_check_none, ssm);
      break;

    case ACTIVATE_RESET:
      /* The reset command is not part of the activation sequence. */
      fpi_ssm_jump_to_state (ssm, ACTIVATE_CHECK_FW_VER);
      break;

    case ACTIVATE_READ_CHIP_ID:
      if (self->warm_attempted)
        {
          fpi_ssm_jump_to_state (ssm, ACTIVATE_CHECK_FW_VER);
          return;
        }
      goodix_send_read_sensor_register (dev, 0x0000, 4, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_READ_OTP:
      if (self->warm_attempted)
        {
          fpi_ssm_jump_to_state (ssm, ACTIVATE_CHECK_FW_VER);
          return;
        }
      goodix_send_read_otp (dev, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_send_query_firmware_version (dev, goodixtls5xx_check_firmware_version, ssm);
      break;

    case ACTIVATE_UPLOAD_CONFIG:
      if (self->warm_attempted)
        {
          fpi_ssm_jump_to_state (ssm, ACTIVATE_NUM_STATES);
          return;
        }
      /* Config is uploaded after TLS completes; advance to the PSK read. */
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      if (self->warm_attempted)
        {
          fpi_ssm_jump_to_state (ssm, ACTIVATE_NUM_STATES);
          return;
        }
      goodix_send_preset_psk_read_5e0a (dev, GOODIX_5E0A_PSK_FLAGS, 32, 0,
                                        on_psk_hash_read, ssm);
      break;
    }
}

static void
on_chip_enabled (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  if (error)
    {
      self->warm_ok = FALSE;
      self->warm_down_reason = "failed-last";
      self->warm_attempted = FALSE;
      goodix_session_mark_dirty (dev);
      fp_err ("failed to enable chip: %s (code: %d)", error->message, error->code);
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
      return;
    }
  self->warm_ok = TRUE;
  self->last_clean_mono = g_get_monotonic_time ();
  self->warm_boot_seq = goodix_boot_seq_get (dev);
  self->warm_attempted = FALSE;
  fp_dbg ("Chip enabled! Activation complete.");
  fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), NULL);
}

static gboolean
goodix5e0a_warm_fresh (FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  return self->warm_ok
         && self->warm_boot_seq == goodix_boot_seq_get (dev)
         && (g_get_monotonic_time () - self->last_clean_mono) < GOODIX_5E0A_WARM_TTL_US;
}

static void
goodix5e0a_log_warm_taken (FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  fp_dbg ("warm activation: reusing MCU config (age=%.1fs, boot_seq=%u)",
             (g_get_monotonic_time () - self->last_clean_mono) / (gdouble) G_USEC_PER_SEC,
             self->warm_boot_seq);
}

static void
goodix5e0a_start_warm_activation (FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  self->session_started = FALSE;
  self->scan_ssm = NULL;
  self->down_timeout = NULL;
  self->warm_attempted = TRUE;

  fp_dbg ("warm path: skipping RESET + config upload, entry=CHECK_FW_VER");
  fpi_ssm_start (fpi_ssm_new (dev, activate_run_state, ACTIVATE_NUM_STATES),
                 activate_complete);
}

static void
goodix5e0a_start_full_activation (FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  self->session_started = FALSE;
  self->scan_ssm = NULL;
  self->down_timeout = NULL;
  self->warm_attempted = FALSE;

  fpi_ssm_start (fpi_ssm_new (dev, activate_run_state, ACTIVATE_NUM_STATES),
                 activate_complete);
}

static void
on_parked_health_reply (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  guint gen = GPOINTER_TO_UINT (user_data);

  if (gen != goodix_activation_gen_get (dev))
    {
      fp_dbg ("dropping stale parked-TLS health reply");
      if (error)
        g_error_free (error);
      return;
    }

  if (error)
    {
      const char *reason = "tls-error";
      gboolean transport_miss = FALSE;

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        {
          reason = "timeout";
          transport_miss = TRUE;
        }
      g_error_free (error);
      goodix_shutdown_tls (dev, NULL);
      goodix_reset_state (dev);
      if (transport_miss)
        {
          self->warm_ok = FALSE;
          self->warm_down_reason = "transport-miss";
        }
      else if (goodix5e0a_warm_fresh (dev))
        {
          goodix5e0a_log_warm_taken (dev);
          goodix5e0a_start_warm_activation (dev);
          return;
        }
      fp_dbg ("parked TLS session unhealthy (%s), full re-handshake", reason);
      goodix5e0a_start_full_activation (dev);
      return;
    }

  fp_dbg ("TLS session reused (parked %.1fs, gen=%u)",
             (g_get_monotonic_time () - self->tls_parked_at) / (gdouble) G_USEC_PER_SEC,
             gen);
  fp_dbg ("parked TLS session healthy, confirming chip enable");
  goodix_send_enable_chip (dev, TRUE, on_chip_enabled, NULL);
}

static void
on_post_tls_config_uploaded (FpDevice *dev, gboolean success,
                             gpointer user_data, GError *error)
{
  if (error)
    {
      fp_err ("failed to upload config after TLS: %s", error->message);
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
      return;
    }
  if (!success)
    {
      fp_err ("MCU rejected config upload after TLS");
      fpi_image_device_activate_complete (
        FP_IMAGE_DEVICE (dev),
        g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                     "failed to upload mcu config after TLS"));
      return;
    }
  goodix_send_enable_chip (dev, TRUE, on_chip_enabled, NULL);
}

static void
on_tls_activation_complete (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  /* Drop this completion if a deactivate bumped the generation while the TLS handshake was in flight. */
  if (GPOINTER_TO_UINT (user_data) != goodix_activation_gen_get (dev))
    {
      fp_dbg ("dropping stale TLS activation completion");
      if (error)
        g_error_free (error);
      return;
    }

  if (error)
    {
      goodix_session_mark_dirty (dev);
      if (self->warm_attempted && !self->warm_retried)
        {
          self->warm_ok = FALSE;
          self->warm_down_reason = "failed-last";
          self->warm_attempted = FALSE;
          self->warm_retried = TRUE;
          fp_dbg ("warm attempt failed (%s), retrying full ladder", error->message);
          g_error_free (error);
          goodix_shutdown_tls (dev, NULL);
          goodix_reset_state (dev);
          goodix5e0a_start_full_activation (dev);
          return;
        }
      self->warm_ok = FALSE;
      self->warm_down_reason = "failed-last";
      self->warm_attempted = FALSE;
      fp_err ("failed during TLS activation: %s (code: %d)", error->message, error->code);
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
      return;
    }

  fp_dbg ("TLS connection ready!");

  /* Upload config after TLS on the cold path; warm reuse keeps its config. */
  if (self->warm_attempted)
    {
      goodix_send_enable_chip (dev, TRUE, on_chip_enabled, NULL);
    }
  else
    {
      goodix_send_upload_config_mcu (dev, (guint8 *) goodix_5e0a_config,
                                     sizeof (goodix_5e0a_config), NULL,
                                     on_post_tls_config_uploaded, NULL);
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  G_DEBUG_HERE ();
  if (!error)
    {
      /* Capture the activation generation for the staleness guard. */
      goodix_tls_init (dev, on_tls_activation_complete,
                       GUINT_TO_POINTER (goodix_activation_gen_get (dev)));
    }
  else
    {
      goodix_session_mark_dirty (dev);
      if (self->warm_attempted && !self->warm_retried)
        {
          self->warm_ok = FALSE;
          self->warm_down_reason = "failed-last";
          self->warm_attempted = FALSE;
          self->warm_retried = TRUE;
          fp_dbg ("warm attempt failed (%s), retrying full ladder", error->message);
          g_error_free (error);
          goodix5e0a_start_full_activation (dev);
          return;
        }
      self->warm_ok = FALSE;
      self->warm_down_reason = "failed-last";
      self->warm_attempted = FALSE;
      fp_err ("failed during activation: %s (code: %d)", error->message, error->code);
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
    }
}

static void
dev_activate (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  guint pre_gen = goodix_activation_gen_get (dev);
  guint new_gen = goodix_activation_gen_bump (dev);

  goodix5e0a_reset_touch_frames (self);
  self->warm_retried = FALSE;

  if (self->tls_parked && self->tls_parked_gen == pre_gen
      && goodix_tls_is_alive (dev)
      && (g_get_monotonic_time () - self->tls_parked_at) < GOODIX_5E0A_TLS_PARK_TTL_US)
    {
      GoodixCallbackInfo *cb_info;
      GoodixQueryMcuState payload;

      self->tls_parked = FALSE;
      self->scan_ssm = NULL;
      self->down_timeout = NULL;
      fp_dbg ("parked TLS session candidate fresh, health-checking (gen=%u)", new_gen);
      goodix_start_read_loop (dev);

      cb_info = g_new0 (GoodixCallbackInfo, 1);
      cb_info->callback = G_CALLBACK (on_parked_health_reply);
      cb_info->user_data = GUINT_TO_POINTER (new_gen);
      payload.unused_flags = 0x55;
      goodix_send_protocol (dev, GOODIX_CMD_QUERY_MCU_STATE,
                            (guint8 *) &payload, sizeof (payload),
                            NULL, TRUE,
                            GOODIX_5E0A_TLS_PARK_HEALTH_TIMEOUT_MS,
                            FALSE, goodix_receive_none, cb_info);
      return;
    }

  if (self->tls_parked)
    {
      const char *reason;

      if (self->tls_parked_gen != pre_gen)
        reason = "gen-mismatch";
      else if (!goodix_tls_is_alive (dev))
        reason = "tls-error";
      else
        reason = "expired";
      self->tls_parked = FALSE;
      fp_dbg ("parked TLS session unhealthy (%s), full re-handshake", reason);
      goodix_shutdown_tls (dev, NULL);
    }

  if (goodix5e0a_warm_fresh (dev))
    {
      goodix5e0a_log_warm_taken (dev);
      goodix5e0a_start_warm_activation (dev);
      return;
    }

  {
    const char *reason;

    if (self->warm_ok && self->warm_boot_seq == goodix_boot_seq_get (dev))
      {
        reason = "ttl-expired";
        self->warm_ok = FALSE;
        self->warm_down_reason = "ttl-expired";
      }
    else if (self->warm_ok)
      {
        reason = "cold-start";
        self->warm_ok = FALSE;
        self->warm_down_reason = "cold-start";
      }
    else
      {
        reason = self->warm_down_reason ? self->warm_down_reason : "cold-start";
      }
    self->warm_attempted = FALSE;
    fp_dbg ("warm expired: reason=%s", reason);
    goodix5e0a_start_full_activation (dev);
  }
}

enum goodix5e0a_scan_states {
  SCAN_5E0A_SESSION_AE,
  SCAN_5E0A_SESSION_D6,
  SCAN_5E0A_FDT_DOWN,
  SCAN_5E0A_GET_IMAGE,
  SCAN_5E0A_FDT_UP_1,
  SCAN_5E0A_UP_AE,
  SCAN_5E0A_FDT_UP_2,
  SCAN_5E0A_NUM_STATES,
};

static void
send_cmd_noreply (FpDevice *dev, guint8 cmd, const guint8 *payload, guint16 len,
                  GoodixNoneCallback cb, gpointer user_data)
{
  GoodixCallbackInfo *cb_info = NULL;
  GoodixCmdCallback callback = NULL;

  if (cb)
    {
      cb_info = g_new0 (GoodixCallbackInfo, 1);
      cb_info->callback = G_CALLBACK (cb);
      cb_info->user_data = user_data;
      callback = goodix_receive_none;
    }

  goodix_send_protocol (dev, cmd, payload, len, NULL, TRUE, GOODIX_TIMEOUT,
                        FALSE, callback, cb_info);
}

static void
send_cmd_reply (FpDevice *dev, guint8 cmd, const guint8 *payload, guint16 len,
                guint timeout_ms, GoodixDefaultCallback cb, gpointer user_data)
{
  GoodixCallbackInfo *cb_info = NULL;
  GoodixCmdCallback callback = NULL;

  if (cb)
    {
      cb_info = g_new0 (GoodixCallbackInfo, 1);
      cb_info->callback = G_CALLBACK (cb);
      cb_info->user_data = user_data;
      callback = goodix_receive_default;
    }

  goodix_send_protocol (dev, cmd, payload, len, NULL, TRUE, timeout_ms,
                        TRUE, callback, cb_info);
}

static void
goodix5e0a_step_cb (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fp_dbg ("ignoring expected command error: %s", error->message);
      g_error_free (error);
    }
  fpi_ssm_next_state (ssm);
}

static void
goodix5e0a_on_d6_reply (FpDevice *dev, guint8 *data, guint16 len,
                        gpointer ssm, GError *err)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  if (err)
    {
      fp_warn ("session d6 reply error: %s", err->message);
      g_error_free (err);
    }
  else
    {
      fp_dbg ("session d6 replied successfully (len=%u)", len);
    }
  self->session_started = TRUE;
  if (self->retry_guard)
    fpi_ssm_jump_to_state (ssm, SCAN_5E0A_FDT_UP_1);
  else
    fpi_ssm_next_state (ssm);
}

static void goodix5e0a_on_fdt_down_reply (FpDevice *dev,
                                          guint8   *data,
                                          guint16   len,
                                          gpointer  ssm,
                                          GError   *err);

static void
goodix5e0a_on_down_poll_timeout (FpDevice *dev, gpointer user_data)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  FpiSsm *ssm = user_data;

  self->down_timeout = NULL;

  if (self->scan_ssm != ssm)
    return;
  if (self->scan_timeout_gen != self->scan_gen)
    return;

  send_cmd_reply (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN,
                  goodix_5e0a_down_s12, sizeof (goodix_5e0a_down_s12),
                  0, goodix5e0a_on_fdt_down_reply, ssm);
}

static void
goodix5e0a_on_fdt_down_reply (FpDevice *dev, guint8 *data, guint16 len,
                              gpointer ssm, GError *err)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  guint8 status;
  guint32 channel_energy = 0;
  gboolean touch;

  if (err)
    {
      if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  status = (len > 0) ? data[0] : 0x00;
  fp_dbg ("finger-detect reply: status=0x%02x len=%u", status, len);

  if (len >= 4)
    for (guint16 i = 4; i + 1 < len; i += 2)
      channel_energy += (guint32) data[i] | ((guint32) data[i + 1] << 8);

  /* Gating rule: touch = channel-byte energy (data[2] != 0xff and channel_energy > 0), never byte0 */
  touch = (len >= 4 && data[2] != 0xff && channel_energy > 0);

  if (touch)
    {
      if (self->down_timeout)
        {
          g_source_destroy (self->down_timeout);
          self->down_timeout = NULL;
        }
      fp_dbg ("touch confirmed: mask=0x%02x energy=%u",
              (data && len >= 3) ? data[2] : 0, channel_energy);
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), TRUE);
      fpi_ssm_next_state (ssm);
      return;
    }

  /* No touch (empty air or poor contact): pace re-sampling silently after 50ms */
  if (self->down_timeout)
    {
      g_source_destroy (self->down_timeout);
      self->down_timeout = NULL;
    }
  self->scan_timeout_gen = self->scan_gen;
  self->down_timeout = fpi_device_add_timeout (dev, 50, goodix5e0a_on_down_poll_timeout, ssm, NULL);
}

static FpImage * process_raw_frame (GoodixTls5xxPix * pix);
static guint goodix5e0a_count_minutiae (FpImage *img);

static guint32
goodix5e0a_decode_frame (GoodixTls5xxPix *out_row_major, const guint8 *data, guint16 len)
{
  g_autofree guint8 *packed = NULL;
  guint32 packed_len = 0;
  guint32 pixel_idx = 0;

  if (!out_row_major || !data)
    return 0;

  packed = g_new0 (guint8, GOODIX_5E0A_ACT_BYTES);

  /* Each frame is 80 blocks of 132 bytes followed by a four-byte footer.
   * Each block carries 96 packed pixel bytes and 36 padding bytes. */
  for (guint32 block = 0; block < GOODIX_5E0A_FRAME_BLOCKS; block++)
    {
      guint32 src = block * GOODIX_5E0A_BLOCK_BYTES;
      if (src + GOODIX_5E0A_BLOCK_ACTIVE_BYTES > len)
        break;

      memcpy (packed + packed_len, data + src, GOODIX_5E0A_BLOCK_ACTIVE_BYTES);
      packed_len += GOODIX_5E0A_BLOCK_ACTIVE_BYTES;
    }

  for (guint32 i = 0; i + 6 <= packed_len && pixel_idx + 4 <= GOODIX_5E0A_FRAME_SIZE; i += 6)
    {
      const guint8 *c = packed + i;
      out_row_major[pixel_idx++] = ((c[0] & 0x0f) << 8) | c[1];
      out_row_major[pixel_idx++] = (c[3] << 4) | (c[0] >> 4);
      out_row_major[pixel_idx++] = ((c[5] & 0x0f) << 8) | c[2];
      out_row_major[pixel_idx++] = (c[4] << 4) | (c[5] >> 4);
    }

  return pixel_idx;
}

static void
goodix5e0a_reset_touch_frames (FpiDeviceGoodixTls5e0a *self)
{
  if (self->best_img != NULL)
    {
      g_object_unref (self->best_img);
      self->best_img = NULL;
    }
  self->frame_count = 0;
  self->best_minutiae = 0;
  self->best_frame_no = 0;
}

static FpImage *
goodix5e0a_claim_best_frame (FpiDeviceGoodixTls5e0a *self)
{
  FpImage *best;

  if (self->best_img == NULL)
    return NULL;
  best = self->best_img;
  fp_dbg ("best frame %u/%u: minutiae=%u (submitting)",
             self->best_frame_no, (guint) GOODIX_5E0A_FRAMES_PER_TOUCH,
             self->best_minutiae);
  self->best_img = NULL;
  self->best_minutiae = 0;
  self->best_frame_no = 0;
  return best;
}

static gboolean
goodix5e0a_keep_best_frame (FpDevice *dev, gpointer ssm, FpImage *img,
                            guint16 declen, guint active, guint range);

static void
goodix5e0a_on_read_img (FpDevice *dev, guint8 *data, guint16 len,
                        gpointer ssm, GError *err)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  g_autofree GoodixTls5xxPix *raw_frame = NULL;
  FpImage *img;
  guint frame_active = 0;
  guint16 frame_min = 65535, frame_max = 0;
  guint frame_range = 0;

  if (self->scan_ssm != ssm)
    {
      if (err)
        g_error_free (err);
      return;
    }

  /* Fall back to the best frame captured so far if a mid-burst read fails. */
  if (err)
    {
      if (action != FPI_DEVICE_ACTION_ENROLL && self->best_img != NULL)
        {
          g_error_free (err);
          img = goodix5e0a_claim_best_frame (self);
          goto deliver;
        }
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  /* Submit the best frame captured so far if a mid-burst read is truncated. */
  if (action != FPI_DEVICE_ACTION_ENROLL && self->best_img != NULL
      && (data == NULL || len < GOODIX_5E0A_FRAME_WIRE_BYTES))
    {
      fp_dbg ("frame %u/%u: short declen=%u, submitting best-so-far %u/%u",
                 self->frame_count + 1, (guint) GOODIX_5E0A_FRAMES_PER_TOUCH,
                 len, self->best_frame_no,
                 (guint) GOODIX_5E0A_FRAMES_PER_TOUCH);
      img = goodix5e0a_claim_best_frame (self);
      goto deliver;
    }

  raw_frame = g_new0 (GoodixTls5xxPix, GOODIX_5E0A_FRAME_SIZE);
  goodix5e0a_decode_frame (raw_frame, data, len);

  for (guint32 i = 0; i < GOODIX_5E0A_FRAME_SIZE; i++)
    {
      if (raw_frame[i] > 30)
        {
          frame_active++;
          if (raw_frame[i] < frame_min)
            frame_min = raw_frame[i];
          if (raw_frame[i] > frame_max)
            frame_max = raw_frame[i];
        }
    }
  frame_range = (frame_min != 65535 && frame_max > frame_min)
                ? (guint) (frame_max - frame_min) : 0;

  img = process_raw_frame (raw_frame);

  if (action == FPI_DEVICE_ACTION_ENROLL)
    {
      guint minutiae_count;

      if (img == NULL)
        {
          fp_dbg ("enrollment touch rejected: poor frame quality (press firmer)");
          fpi_image_device_retry_scan (FP_IMAGE_DEVICE (dev), FP_DEVICE_RETRY_TOO_SHORT);
          fpi_ssm_next_state (ssm);
          return;
        }
      minutiae_count = goodix5e0a_count_minutiae (img);
      fp_dbg ("enrollment quality check: minutiae_count=%u (floor=%d)",
              minutiae_count, GOODIX_5E0A_ENROLL_MIN_MINUTIAE);
      if (minutiae_count < GOODIX_5E0A_ENROLL_MIN_MINUTIAE)
        {
          fp_dbg ("enrollment touch rejected: minutiae_count=%u < %d (press firmer)",
                  minutiae_count, GOODIX_5E0A_ENROLL_MIN_MINUTIAE);
          g_object_unref (img);
          fpi_image_device_retry_scan (FP_IMAGE_DEVICE (dev), FP_DEVICE_RETRY_TOO_SHORT);
          fpi_ssm_next_state (ssm);
          return;
        }
    }

  if (action != FPI_DEVICE_ACTION_ENROLL)
    {
      if (goodix5e0a_keep_best_frame (dev, ssm, img, len, frame_active, frame_range))
        return;
      img = goodix5e0a_claim_best_frame (self);
      if (img == NULL)
        {
          fpi_image_device_retry_scan (FP_IMAGE_DEVICE (dev), FP_DEVICE_RETRY_TOO_SHORT);
          goto deliver_done;
        }
    }

deliver:
  fpi_image_device_image_captured (FP_IMAGE_DEVICE (dev), img);

deliver_done:
  if (action != FPI_DEVICE_ACTION_ENROLL)
    {
      self->scan_ssm = NULL;
      self->retry_guard = TRUE;
      self->retry_guard_mono = g_get_monotonic_time ();
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), FALSE);
      fpi_ssm_mark_completed (ssm);
    }
  else
    {
      fpi_ssm_next_state (ssm);
    }
}

static gboolean
goodix5e0a_keep_best_frame (FpDevice *dev, gpointer ssm, FpImage *img,
                            guint16 declen G_GNUC_UNUSED, guint active, guint range)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  guint minutiae = img ? goodix5e0a_count_minutiae (img) : 0;

  self->frame_count++;
  fp_dbg ("frame %u/%u: active=%u range=%u minutiae=%u",
             self->frame_count, (guint) GOODIX_5E0A_FRAMES_PER_TOUCH,
             active, range, minutiae);

  if (img != NULL)
    {
      if (self->best_img == NULL || minutiae > self->best_minutiae)
        {
          if (self->best_img != NULL)
            g_object_unref (self->best_img);
          self->best_img = img;
          self->best_minutiae = minutiae;
          self->best_frame_no = self->frame_count;
        }
      else
        {
          g_object_unref (img);
        }
    }

  if (self->frame_count < GOODIX_5E0A_FRAMES_PER_TOUCH)
    {
      goodix_tls_read_image (dev, goodix5e0a_on_read_img, ssm);
      return TRUE;
    }
  return FALSE;
}

static void
goodix5e0a_on_fdt_up_reply (FpDevice *dev, guint8 *data, guint16 len,
                            gpointer ssm, GError *err)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  if (self->scan_ssm != ssm)
    {
      if (err)
        g_error_free (err);
      return;
    }

  if (err)
    {
      if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fp_dbg ("finger-detect command failed: %s", err->message);
      if (self->retry_guard)
        {
          /* A timeout here means the finger is still down, not released.
           * Re-issue while the guard is held; stop after 30s. */
          if (g_get_monotonic_time () - self->retry_guard_mono > 30 * G_USEC_PER_SEC)
            {
              self->retry_guard = FALSE;
              fp_dbg ("retry guard: orphaned hold past 30s, failing claim");
              fpi_ssm_mark_failed (ssm, err);
              return;
            }
          g_error_free (err);
          fp_dbg ("retry guard: finger still present, re-issuing FDT UP");
          send_cmd_reply (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP,
                          goodix_5e0a_up_u01, sizeof (goodix_5e0a_up_u01),
                          2000, goodix5e0a_on_fdt_up_reply, ssm);
          return;
        }
      g_error_free (err);
    }
  else
    {
      fp_dbg ("finger release detected");
    }

  if (self->retry_guard)
    {
      self->retry_guard = FALSE;
      fp_dbg ("retry guard: release ok, arming FDT DOWN");
      fpi_ssm_jump_to_state (ssm, SCAN_5E0A_FDT_DOWN);
      return;
    }

  /* Mark current scan SSM completed before notifying libfprint,
   * so that when libfprint synchronously requests AWAIT_FINGER_ON,
   * the concurrency guard does not block the new scan SSM. */
  self->scan_ssm = NULL;
  fpi_ssm_next_state (ssm);
  fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), FALSE);
}

static void
goodix5e0a_scan_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_5E0A_SESSION_AE:
      send_cmd_noreply (dev, GOODIX_CMD_QUERY_MCU_STATE,
                        goodix_5e0a_query_ae, sizeof (goodix_5e0a_query_ae),
                        goodix5e0a_step_cb, ssm);
      break;

    case SCAN_5E0A_SESSION_D6:
      if (self->session_started)
        {
          if (self->retry_guard)
            fpi_ssm_jump_to_state (ssm, SCAN_5E0A_FDT_UP_1);
          else
            fpi_ssm_jump_to_state (ssm, SCAN_5E0A_FDT_DOWN);
          return;
        }
      send_cmd_reply (dev, GOODIX_CMD_SESSION_D6,
                      goodix_5e0a_session_d6, sizeof (goodix_5e0a_session_d6),
                      GOODIX_TIMEOUT, goodix5e0a_on_d6_reply, ssm);
      break;

    case SCAN_5E0A_FDT_DOWN:
      /* Blocking wait for the MCU capacitive touch interrupt: timeout 0
       * installs no libfprint timer, so the await survives until a real
       * touch, client cancel, or deactivate. Any finite timeout turns an
       * idle wait into "Command timed out: 0x32". */
      send_cmd_reply (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN,
                      goodix_5e0a_down_s12, sizeof (goodix_5e0a_down_s12),
                      0, goodix5e0a_on_fdt_down_reply, ssm);
      break;

    case SCAN_5E0A_GET_IMAGE:
      goodix_tls_read_image (dev, goodix5e0a_on_read_img, ssm);
      break;

    case SCAN_5E0A_FDT_UP_1:
      send_cmd_noreply (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP,
                        goodix_5e0a_up_u01, sizeof (goodix_5e0a_up_u01),
                        goodix5e0a_step_cb, ssm);
      break;

    case SCAN_5E0A_UP_AE:
      send_cmd_noreply (dev, GOODIX_CMD_QUERY_MCU_STATE,
                        goodix_5e0a_query_ae, sizeof (goodix_5e0a_query_ae),
                        goodix5e0a_step_cb, ssm);
      break;

    case SCAN_5E0A_FDT_UP_2:
      send_cmd_reply (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP,
                        goodix_5e0a_up_u01, sizeof (goodix_5e0a_up_u01),
                        self->retry_guard ? 2000 : 5000, goodix5e0a_on_fdt_up_reply, ssm);
      break;
    }
}

static void
goodix5e0a_scan_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  self->scan_gen++;
  self->scan_ssm = NULL;
  goodix5e0a_reset_touch_frames (self);
  if (self->down_timeout)
    {
      g_source_destroy (self->down_timeout);
      self->down_timeout = NULL;
    }

  if (error)
    {
      goodix_session_mark_dirty (dev);
      self->warm_ok = FALSE;
      self->retry_guard = FALSE;
      fp_err ("failed to scan: %s (code: %d)", error->message, error->code);
      fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
      return;
    }
  fp_dbg ("finished scan stage");
}

static void
goodix5e0a_scan_start (FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  if (self->scan_ssm != NULL)
    {
      fp_dbg ("scan SSM already active, ignoring start request");
      return;
    }

  if (self->retry_guard)
    {
      gint64 delta_us = g_get_monotonic_time () - self->retry_guard_mono;
      if (delta_us > 2 * G_USEC_PER_SEC)
        {
          fp_dbg ("retry guard expired (delta=%ld ms), clearing", (long) (delta_us / 1000));
          self->retry_guard = FALSE;
        }
      else
        {
          fp_dbg ("retry guard active (delta=%ld ms): awaiting finger release", (long) (delta_us / 1000));
        }
    }

  goodix5e0a_reset_touch_frames (self);

  self->scan_gen++;
  self->scan_ssm = fpi_ssm_new (dev, goodix5e0a_scan_run_state, SCAN_5E0A_NUM_STATES);
  fpi_ssm_start (self->scan_ssm, goodix5e0a_scan_complete);
}

static void
goodix5e0a_change_state (FpImageDevice *img_dev, FpiImageDeviceState state)
{
  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    goodix5e0a_scan_start (FP_DEVICE (img_dev));
}

static void
goodix5e0a_deactivate (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  gboolean scan_was_active;
  g_autoptr(GError) tls_err = NULL;

  goodix5e0a_reset_touch_frames (self);
  goodix_activation_gen_bump (dev);

  self->session_started = FALSE;
  self->scan_gen++;
  if (self->down_timeout)
    {
      g_source_destroy (self->down_timeout);
      self->down_timeout = NULL;
    }

  goodix_reset_state (dev);
  scan_was_active = (self->scan_ssm != NULL);
  if (self->scan_ssm != NULL)
    {
      fpi_ssm_free (self->scan_ssm);
      self->scan_ssm = NULL;
    }

  /* Only park when deactivation arrived idle; a scan torn down mid-flight
   * can leave dangling replies that poison the next reuse. */
  if (scan_was_active && goodix_tls_is_alive (dev) && self->warm_ok)
    fp_dbg ("park invalidated: scan active at deactivate");
  if (goodix_tls_is_alive (dev) && self->warm_ok && !scan_was_active)
    {
      goodix_stop_read_loop (dev);
      self->tls_parked = TRUE;
      self->tls_parked_at = g_get_monotonic_time ();
      self->tls_parked_gen = goodix_activation_gen_get (dev);
      goodix_session_mark_clean (dev);
      fp_dbg ("parking live TLS session (gen=%u)", self->tls_parked_gen);
      fpi_image_device_deactivate_complete (img_dev, NULL);
      return;
    }

  self->tls_parked = FALSE;
  self->retry_guard = FALSE;
  self->retry_guard_mono = 0;
  goodix_session_mark_dirty (dev);
  goodix_shutdown_tls (dev, &tls_err);
  goodix_stop_read_loop (dev);
  fpi_image_device_deactivate_complete (img_dev, g_steal_pointer (&tls_err));
}

static void
fpi_device_goodixtls5e0a_init (FpiDeviceGoodixTls5e0a *self)
{
  self->session_started = FALSE;
  self->scan_ssm = NULL;
  self->scan_gen = 0;
  self->scan_timeout_gen = 0;
  self->down_timeout = NULL;
  self->tls_parked = FALSE;
  self->tls_parked_at = 0;
  self->tls_parked_gen = 0;
  self->warm_ok = FALSE;
  self->last_clean_mono = 0;
  self->warm_boot_seq = 0;
  self->warm_down_reason = "cold-start";
  self->warm_attempted = FALSE;
  self->warm_retried = FALSE;
  self->frame_count = 0;
  self->best_img = NULL;
  self->best_minutiae = 0;
  self->best_frame_no = 0;
  self->retry_guard = FALSE;
  self->retry_guard_mono = 0;
}

static double
goodix5e0a_axis_correlation (const GoodixTls5xxPix *pix,
                             int                    width,
                             int                    height,
                             int                    dx,
                             int                    dy)
{
  double sum_a = 0.0, sum_b = 0.0;
  guint count = 0;
  double mean_a, mean_b;
  double covariance = 0.0, variance_a = 0.0, variance_b = 0.0;
  double denominator;

  for (int y = 0; y + dy < height; y++)
    for (int x = 0; x + dx < width; x++)
      {
        sum_a += pix[y * width + x];
        sum_b += pix[(y + dy) * width + x + dx];
        count++;
      }

  if (count == 0)
    return 0.0;

  mean_a = sum_a / count;
  mean_b = sum_b / count;

  for (int y = 0; y + dy < height; y++)
    for (int x = 0; x + dx < width; x++)
      {
        double a = pix[y * width + x] - mean_a;
        double b = pix[(y + dy) * width + x + dx] - mean_b;
        covariance += a * b;
        variance_a += a * a;
        variance_b += b * b;
      }

  denominator = sqrt (variance_a * variance_b);
  return denominator > 1e-6 ? covariance / denominator : 0.0;
}

static FpImage *
process_raw_frame (GoodixTls5xxPix * pix)
{
  const int W = GOODIX_5E0A_WIDTH;
  const int H = GOODIX_5E0A_HEIGHT;
  const int dst_w = GOODIX_5E0A_SCALED_WIDTH;
  const int dst_h = GOODIX_5E0A_SCALED_HEIGHT;

  guint16 min_v = 65535, max_v = 0;
  guint active = 0;
  guint16 range = 0;
  double horizontal_corr = 0.0, vertical_corr = 0.0, horizontal_lag4_corr = 0.0;
  GString *active_cols = NULL;
  g_autofree float *residual = NULL;
  float residual_min = G_MAXFLOAT;
  float residual_max = -G_MAXFLOAT;
  float residual_range = 0.0f;
  g_autofree guint8 *normalized = NULL;
  FpImage *scaled = NULL;

  for (int r = 0; r < H; ++r)
    {
      for (int c = 0; c < W; ++c)
        {
          guint16 v = pix[r * W + c];
          if (v > 30)
            {
              active++;
              if (v < min_v)
                min_v = v;
              if (v > max_v)
                max_v = v;
            }
        }
    }

  if (min_v == 65535)
    min_v = 0;
  range = (max_v > min_v) ? (max_v - min_v) : 1;

  horizontal_corr = goodix5e0a_axis_correlation (pix, W, H, 1, 0);
  vertical_corr = goodix5e0a_axis_correlation (pix, W, H, 0, 1);
  horizontal_lag4_corr = goodix5e0a_axis_correlation (pix, W, H, 4, 0);

  active_cols = g_string_new ("");
  for (int c = 0; c < W; ++c)
    {
      guint32 c_sum = 0;
      for (int r = 0; r < H; ++r)
        c_sum += pix[r * W + c];
      if (c_sum > 0)
        g_string_append_printf (active_cols, "%d ", c);
    }
  if (active_cols->len > 0)
    fp_dbg ("active cols: %s", active_cols->str);
  else
    fp_dbg ("active cols: NONE (all 0)");
  g_string_free (active_cols, TRUE);

  fp_dbg ("frame stats: active=%u, min_v=%u, max_v=%u, range=%u, h_corr=%.3f, v_corr=%.3f, h_lag4_corr=%.3f (native %dx%d WxH)",
          active, min_v, max_v, range,
          horizontal_corr, vertical_corr, horizontal_lag4_corr, W, H);

  if (active < 64 || range < 8)
    return NULL;

  /* Remove the slowly varying pressure/offset field before global scaling.
   * A 3x3 local mean is the smallest window that removes this field without
   * averaging across a full ridge period. */
  residual = g_new (float, GOODIX_5E0A_FRAME_SIZE);
  for (int y = 0; y < H; y++)
    {
      for (int x = 0; x < W; x++)
        {
          guint32 local_sum = 0;
          guint local_count = 0;
          float value;
          for (int yy = MAX (0, y - 1); yy <= MIN (H - 1, y + 1); yy++)
            for (int xx = MAX (0, x - 1); xx <= MIN (W - 1, x + 1); xx++)
              {
                local_sum += pix[yy * W + xx];
                local_count++;
              }

          value = pix[y * W + x] - (float) local_sum / local_count;
          residual[y * W + x] = value;
          residual_min = MIN (residual_min, value);
          residual_max = MAX (residual_max, value);
        }
    }

  residual_range = residual_max - residual_min;
  fp_dbg ("local contrast: min=%.2f max=%.2f range=%.2f window=3x3 gain=%.2f",
          residual_min, residual_max, residual_range, GOODIX_5E0A_CONTRAST_GAIN);
  if (residual_range < 1.0f)
    return NULL;

  normalized = g_new (guint8, GOODIX_5E0A_FRAME_SIZE);
  for (guint i = 0; i < GOODIX_5E0A_FRAME_SIZE; i++)
    {
      int value = (int) roundf (128.0f + residual[i] * GOODIX_5E0A_CONTRAST_GAIN);
      normalized[i] = (guint8) CLAMP (value, 0, 255);
    }

  /* Create the scaled 128x160 image directly via bilinear upscaling.
   * Use FPI_IMAGE_COLORS_INVERTED for capacitive ridges (high ADC = black).
   * Omit FPI_IMAGE_PARTIAL so remove_perimeter_pts=0 retains edge minutiae. */
  scaled = fp_image_new (dst_w, dst_h);
  scaled->flags = FPI_IMAGE_COLORS_INVERTED;
  scaled->ppmm = 500.0 / 25.4;

  for (int y = 0; y < dst_h; y++)
    {
      float src_y = (y + 0.5f) * 0.5f - 0.5f;
      int y0, y1;
      float y_frac;

      if (src_y < 0.0f)
        src_y = 0.0f;
      y0 = (int) src_y;
      y1 = (y0 + 1 < H) ? y0 + 1 : y0;
      y_frac = src_y - (float) y0;

      for (int x = 0; x < dst_w; x++)
        {
          float src_x = (x + 0.5f) * 0.5f - 0.5f;
          int x0, x1;
          float x_frac, top, bot, val;
          int norm;

          if (src_x < 0.0f)
            src_x = 0.0f;
          x0 = (int) src_x;
          x1 = (x0 + 1 < W) ? x0 + 1 : x0;
          x_frac = src_x - (float) x0;

          top = (float) normalized[y0 * W + x0] * (1.0f - x_frac) + (float) normalized[y0 * W + x1] * x_frac;
          bot = (float) normalized[y1 * W + x0] * (1.0f - x_frac) + (float) normalized[y1 * W + x1] * x_frac;
          val = top * (1.0f - y_frac) + bot * y_frac;
          norm = (int) roundf (val);
          scaled->data[y * dst_w + x] = (guint8) CLAMP (norm, 0, 255);
        }
    }

  fp_dbg ("scaled image: %dx%d (WxH) flags=0x%02x active=%u range=%u ppmm=%.3f",
          scaled->width, scaled->height, scaled->flags, active, range, scaled->ppmm);
  return scaled;
}

static guint
goodix5e0a_count_minutiae (FpImage *img)
{
  int w, h;
  unsigned char *buf;
  LFSPARMS parms = g_lfsparms_V2;
  double ppmm;
  MINUTIAE *minutiae = NULL;
  int *qmap = NULL, *dmap = NULL, *lcmap = NULL, *lfmap = NULL, *hcmap = NULL;
  int mw, mh, bw, bh, bd;
  unsigned char *bdata = NULL;
  int ret;
  guint count;

  if (!img || !img->data)
    return 0;

  w = img->width;
  h = img->height;
  buf = g_memdup2 (img->data, w * h);

  if (img->flags & FPI_IMAGE_COLORS_INVERTED)
    for (int i = 0; i < w * h; i++)
      buf[i] = 255 - buf[i];

  parms.remove_perimeter_pts = 0;
  ppmm = img->ppmm > 0 ? img->ppmm : (500.0 / 25.4);

  ret = get_minutiae (&minutiae, &qmap, &dmap, &lcmap, &lfmap, &hcmap,
                      &mw, &mh, &bdata, &bw, &bh, &bd,
                      buf, w, h, 8, ppmm, &parms);
  count = (ret == 0 && minutiae) ? minutiae->num : 0;

  g_free (buf);
  if (minutiae)
    free_minutiae (minutiae);
  if (qmap)
    g_free (qmap);
  if (dmap)
    g_free (dmap);
  if (lcmap)
    g_free (lcmap);
  if (lfmap)
    g_free (lfmap);
  if (hcmap)
    g_free (hcmap);
  if (bdata)
    g_free (bdata);

  return count;
}

static void
goodix5e0a_suspend (FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  FpiDeviceAction action = fpi_device_get_current_action (dev);
  g_autoptr(GError) tls_err = NULL;

  fp_dbg ("suspend requested during action: %d", action);

  /* Orphan any in-flight TLS handshake/activation; its completion will drop. */
  goodix_activation_gen_bump (dev);

  self->tls_parked = FALSE;
  goodix_session_mark_dirty (dev);
  self->warm_ok = FALSE;
  self->warm_down_reason = "suspended";
  self->warm_attempted = FALSE;
  goodix5e0a_reset_touch_frames (self);
  self->retry_guard = FALSE;
  self->retry_guard_mono = 0;
  self->session_started = FALSE;
  self->scan_gen++;
  if (self->down_timeout)
    {
      g_source_destroy (self->down_timeout);
      self->down_timeout = NULL;
    }

  /* Reset in-flight protocol commands and timeout */
  goodix_reset_state (dev);

  /* Free in-flight scan state machine */
  if (self->scan_ssm != NULL)
    {
      fpi_ssm_free (self->scan_ssm);
      self->scan_ssm = NULL;
    }

  /* Terminate background read loop and cancel transfers */
  goodix_stop_read_loop (dev);

  /* Tear down TLS context. Synchronous (joins the serve thread); log but
   * do not propagate failures so the NOT_SUPPORTED suspend completion below
   * still triggers clean core deactivation. */
  if (!goodix_shutdown_tls (dev, &tls_err))
    {
      fp_warn ("suspend: TLS shutdown failed: %s",
               tls_err ? tls_err->message : "unknown error");
      g_clear_error (&tls_err);
    }

  /* Complete suspend with NOT_SUPPORTED to trigger clean core deactivation
   * of the active task before sleep. */
  fpi_device_suspend_complete (dev, fpi_device_error_new (FP_DEVICE_ERROR_NOT_SUPPORTED));
}

static void
goodix5e0a_resume (FpDevice *dev)
{
  fp_dbg ("resume requested");

  /* Device state was cleaned up during suspend; complete resume immediately.
   * Subsequent user claims will trigger clean open/activate and hardware re-priming. */
  fpi_device_resume_complete (dev, NULL);
}

static void
fpi_device_goodixtls5e0a_class_init (FpiDeviceGoodixTls5e0aClass * class)
{
  FpiDeviceGoodixTlsClass * gx_class = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass * dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass * img_dev_class = FP_IMAGE_DEVICE_CLASS (class);
  FpiDeviceGoodixTls5xxClass * xx_cls = FPI_DEVICE_GOODIXTLS5XX_CLASS (class);

  xx_cls->process_raw_frame = process_raw_frame;
  xx_cls->scan_height = GOODIX_5E0A_HEIGHT;
  xx_cls->scan_width = GOODIX_5E0A_WIDTH;
  xx_cls->psk = goodix_5e0a_psk;
  xx_cls->psk_flags = GOODIX_5E0A_PSK_FLAGS;
  xx_cls->psk_len = sizeof (goodix_5e0a_psk);
  xx_cls->firmware_version = GOODIX_5E0A_FIRMWARE_VERSION;
  xx_cls->reset_number = GOODIX_5E0A_RESET_NUMBER;
  xx_cls->has_calibration = FALSE;

  gx_class->interface = GOODIX_5E0A_INTERFACE;
  gx_class->ep_in = GOODIX_5E0A_EP_IN;
  gx_class->ep_out = GOODIX_5E0A_EP_OUT;
  gx_class->capture_payload = goodix_5e0a_img_payload;
  gx_class->capture_payload_len = sizeof (goodix_5e0a_img_payload);

  dev_class->id = "goodixtls5e0a";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 5e0a";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = goodix_5e0a_id_table;
  dev_class->nr_enroll_stages = 5;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->temp_hot_seconds = -1; /* Disable thermal watchdog */
  dev_class->suspend = goodix5e0a_suspend;
  dev_class->resume = goodix5e0a_resume;

  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = goodix5e0a_change_state;
  img_dev_class->deactivate = goodix5e0a_deactivate;
  img_dev_class->bz3_threshold = 14;
  img_dev_class->img_width = GOODIX_5E0A_SCALED_WIDTH;
  img_dev_class->img_height = GOODIX_5E0A_SCALED_HEIGHT;

  fpi_device_class_auto_initialize_features (dev_class);
}
