/* Goodix TLS driver for libfprint
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Natasha England-Elbro <natasha@natashaee.me>
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
#include "fp-image-device.h"
#include "fpi-image-device.h"
#include "fpi-ssm.h"
#define FP_COMPONENT "goodixtls5xx"

#include "drivers/goodixtls/goodix5xx.h"
#include "drivers_api.h"
#include "goodix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct
{
  GoodixTls5xxPix * calibration_img;
} FpiDeviceGoodixTls5xxPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (FpiDeviceGoodixTls5xx, fpi_device_goodixtls5xx, FPI_TYPE_DEVICE_GOODIXTLS)

enum CALIBRATION_STAGES {
  CALIBRATION_STAGE_FDT_UP,
  CALIBRATION_STAGE_NAV0,
  CALIBRATION_STAGE_GET_IMG,

  CALIBRATION_STAGE_NUM,

};

enum SCAN_STAGES {
  SCAN_STAGE_QUERY_MCU,
  SCAN_STAGE_SWITCH_TO_FDT_MODE,
  SCAN_STAGE_CALIBRATE,
  SCAN_STAGE_SWITCH_TO_FDT_DOWN_ARM,
  SCAN_STAGE_SWITCH_TO_FDT_DOWN,
  SCAN_STAGE_GET_IMG,
  SCAN_STAGE_SWITCH_TO_FTD_UP,
  SCAN_STAGE_SWITCH_TO_FTD_DONE,

  SCAN_STAGE_NUM,
};


/* Base class provides no default MCU config; 5e0a overrides change_state
 * so this guard only covers direct base-class scans. */
static gboolean
get_mcu_cfg_or_skip (FpDevice *dev, FpiSsm *ssm, GoodixTls5xxMcuConfig *cfg)
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  if (!cls->get_mcu_cfg)
    {
      fpi_ssm_next_state (ssm);
      return FALSE;
    }
  *cfg = cls->get_mcu_cfg ();
  return TRUE;
}

static void
send_switch_mode (FpDevice * dev, FpiSsm * ssm, void (*mode_switch)(FpDevice *,
                                                                    const guint8 *,
                                                                    guint16,
                                                                    GDestroyNotify,
                                                                    GoodixDefaultCallback,
                                                                    gpointer))
{
  GoodixTls5xxMcuConfig cfg;
  if (!get_mcu_cfg_or_skip (dev, ssm, &cfg))
    return;

  mode_switch (dev, cfg.data, cfg.data_len, cfg.free_fn, goodixtls5xx_check_none_cmd, ssm);
}
static void
on_calibrate_scan (FpDevice * dev, guint8 * data, guint16 len, gpointer ssm, GError * err)
{
  FpiDeviceGoodixTls5xx * self = FPI_DEVICE_GOODIXTLS5XX (dev);
  FpiDeviceGoodixTls5xxPrivate * priv = fpi_device_goodixtls5xx_get_instance_private (self);
  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (self);

  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  if (!priv->calibration_img)
    priv->calibration_img = g_try_new0 (GoodixTls5xxPix, cls->scan_height * cls->scan_width);
  if (!priv->calibration_img)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }
  goodixtls5xx_decode_frame (priv->calibration_img, cls->scan_height * cls->scan_width, len, data);

  fpi_ssm_next_state (ssm);
}
static void
calibrate_run (FpiSsm * ssm, FpDevice * dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CALIBRATION_STAGE_FDT_UP:
      {
        FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
        if (cls->get_fdt_up_cfg)
          {
            GoodixTls5xxMcuConfig cfg = cls->get_fdt_up_cfg ();
            goodix_send_mcu_switch_to_fdt_up (dev, cfg.data, cfg.data_len, cfg.free_fn, goodixtls5xx_check_none_cmd, ssm);
          }
        else
          {
            send_switch_mode (dev, ssm, goodix_send_mcu_switch_to_fdt_up);
          }
      }
      break;

    case CALIBRATION_STAGE_NAV0:
      goodix_send_nav_0 (dev, goodixtls5xx_check_none_cmd, ssm);
      break;

    case CALIBRATION_STAGE_GET_IMG:
      goodix_tls_read_image (dev, on_calibrate_scan, ssm);
    }
}

static void
do_calibration (FpDevice * dev, FpiSsm * parent)
{
  fpi_ssm_start_subsm (parent, fpi_ssm_new (dev, calibrate_run, CALIBRATION_STAGE_NUM));
}


void
goodixtls5xx_check_none (FpDevice *dev, gpointer user_data, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fpi_ssm_next_state (user_data);
}

void
goodixtls5xx_check_none_cmd (FpDevice *dev, guint8 *data, guint16 len,
                             gpointer ssm, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fpi_ssm_next_state (ssm);
}

void
goodixtls5xx_check_firmware_version (FpDevice *dev, gchar *firmware,
                                     gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (FPI_DEVICE_GOODIXTLS5XX (dev));

  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fp_dbg ("Device firmware: \"%s\"", firmware);

  if (strcmp (firmware, cls->firmware_version))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid device firmware: \"%s\"", firmware);
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fpi_ssm_next_state (user_data);
}


void
goodixtls5xx_check_preset_psk_read (FpDevice *dev, gboolean success,
                                    guint32 flags, guint8 *psk, guint16 length,
                                    gpointer user_data, GError *error)
{
  g_autofree gchar *psk_str = data_to_str (psk, length);
  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  if (!success)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to read PSK from device");
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fp_dbg ("Device PSK: 0x%s", psk_str);
  fp_dbg ("Device PSK flags: 0x%08x", flags);

  if (flags != cls->psk_flags)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid device PSK flags: 0x%08x", flags);
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  if (length != cls->psk_len)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid device PSK: 0x%s", psk_str);
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  if (memcmp (psk, cls->psk, cls->psk_len))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid device PSK: 0x%s", psk_str);
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fpi_ssm_next_state (user_data);
}

void
goodixtls5xx_check_idle (FpDevice *dev, gpointer user_data, GError *err)
{

  if (err)
    {
      fpi_ssm_mark_failed (user_data, err);
      return;
    }
  fpi_ssm_next_state (user_data);
}
void
goodixtls5xx_check_config_upload (FpDevice *dev, gboolean success,
                                  gpointer user_data, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
    }
  else if (!success)
    {
      fpi_ssm_mark_failed (user_data,
                           g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                        "failed to upload mcu config"));
    }
  else
    {
      fpi_ssm_next_state (user_data);
    }
}
void
goodixtls5xx_check_reset (FpDevice *dev, gboolean success, guint16 number,
                          gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  if (!success)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to reset device");
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fp_dbg ("Device reset number: %d", number);

  if (number != cls->reset_number)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid device reset number: %d", number);
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fpi_ssm_next_state (user_data);
}

void
goodixtls5xx_check_powerdown_scan_freq (FpDevice *dev, gboolean success,
                                        gpointer user_data, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
    }
  else if (!success)
    {
      fpi_ssm_mark_failed (user_data,
                           g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                        "failed to set powerdown freq"));
    }
  else
    {
      fpi_ssm_next_state (user_data);
    }
}

void
goodixtls5xx_squash_frame_linear (GoodixTls5xxPix *frame, guint8 *squashed, guint16 frame_size)
{
  GoodixTls5xxPix min = 0xffff;
  GoodixTls5xxPix max = 0;

  for (int i = 0; i != frame_size; ++i)
    {
      const GoodixTls5xxPix pix = frame[i];
      if (pix < min)
        min = pix;
      if (pix > max)
        max = pix;
    }

  for (int i = 0; i != frame_size; ++i)
    {
      const GoodixTls5xxPix pix = frame[i];
      if (pix - min == 0 || max - min == 0)
        squashed[i] = 0;
      else
        squashed[i] = (pix - min) * 0xff / (max - min);
    }
}
static void
linear_subtract_inplace (GoodixTls5xxPix * src, GoodixTls5xxPix * by, guint16 len)
{
  for (guint16 n = 0; n != len; ++n)
    src[n] = (src[n] > by[n]) ? (src[n] - by[n]) : 0;
}

static void
scan_on_read_img (FpDevice *dev, guint8 *data, guint16 len,
                  gpointer ssm, GError *err)
{
  FpImageDevice * img_dev = FP_IMAGE_DEVICE (dev);
  FpiDeviceGoodixTls5xx * self = FPI_DEVICE_GOODIXTLS5XX (dev);
  FpiDeviceGoodixTls5xxPrivate * priv = fpi_device_goodixtls5xx_get_instance_private (self);
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  GoodixTls5xxPix * raw_frame;
  FpImage * img = NULL;

  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  raw_frame = g_try_new0 (GoodixTls5xxPix, cls->scan_width * cls->scan_height);
  if (!raw_frame)
    {
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }
  goodixtls5xx_decode_frame (raw_frame, cls->scan_width * cls->scan_height, len, data);
  if (priv->calibration_img)
    linear_subtract_inplace (raw_frame, priv->calibration_img, cls->scan_width * cls->scan_height);

  if (cls->process_raw_frame)
    {
      img = cls->process_raw_frame (raw_frame);
    }
  else if (cls->process_frame)
    {
      guint8 * squashed = g_try_malloc0 (cls->scan_height * cls->scan_width);
      if (!squashed)
        {
          g_free (raw_frame);
          fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
          return;
        }
      goodixtls5xx_squash_frame_linear (raw_frame, squashed, cls->scan_height * cls->scan_width);
      img = cls->process_frame (squashed);
      g_free (squashed);
    }
  g_free (raw_frame);

  fpi_image_device_image_captured (img_dev, img);

  fpi_ssm_next_state (ssm);
}

static void
scan_get_img (FpDevice * dev, FpiSsm * ssm)
{
  goodix_tls_read_image (dev, scan_on_read_img, ssm);
}

static void
scan_run_state (FpiSsm * ssm, FpDevice * dev)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_STAGE_QUERY_MCU:
      goodix_send_query_mcu_state (dev, goodixtls5xx_check_none, ssm);
      break;

    case SCAN_STAGE_SWITCH_TO_FDT_MODE:
      {
        GoodixTls5xxMcuConfig cfg;
        if (!get_mcu_cfg_or_skip (dev, ssm, &cfg))
          break;
        goodix_send_mcu_switch_to_fdt_mode (dev, cfg.data, cfg.data_len, cfg.free_fn, goodixtls5xx_check_none, ssm);
      }
      break;

    case SCAN_STAGE_CALIBRATE:
      {
        if (cls->has_calibration)
          do_calibration (dev, ssm);
        else
          fpi_ssm_next_state (ssm);
      }
      break;

    case SCAN_STAGE_SWITCH_TO_FDT_DOWN_ARM:
      fpi_ssm_next_state (ssm);
      break;

    case SCAN_STAGE_SWITCH_TO_FDT_DOWN:
      {
        if (cls->get_fdt_down_cfg)
          {
            GoodixTls5xxMcuConfig cfg = cls->get_fdt_down_cfg ();
            goodix_send_mcu_switch_to_fdt_down (dev, cfg.data, cfg.data_len, cfg.free_fn, goodixtls5xx_check_none_cmd, ssm);
          }
        else
          {
            send_switch_mode (dev, ssm, goodix_send_mcu_switch_to_fdt_down);
          }
      }
      break;

    case SCAN_STAGE_GET_IMG:
      fpi_image_device_report_finger_status (img_dev, TRUE);
      scan_get_img (dev, ssm);
      break;

    case SCAN_STAGE_SWITCH_TO_FTD_UP:
      {
        /* 27-byte merge layout is scoped to the 511-era base-class devices
         * (mode header + bytes 10-25 from the FDT-DOWN table). The 5e0a
         * bypasses this path entirely via its own scan SSM in goodix5e0a.c
         * and never sets get_mcu_cfg/get_fdt_down_cfg, so no virtual
         * merge hook is needed unless a future 5xx device conflicts. */
        if (cls->get_mcu_cfg && cls->get_fdt_down_cfg)
          {
            GoodixTls5xxMcuConfig mode = cls->get_mcu_cfg ();
            GoodixTls5xxMcuConfig down = cls->get_fdt_down_cfg ();
            if (mode.data && down.data && mode.data_len == 27 && down.data_len >= 27)
              {
                guint8 post[27];
                memcpy (post, mode.data, sizeof (post));
                memcpy (post + 10, down.data + 10, 16);
                post[26] = 0x00;
                goodix_send_mcu_switch_to_fdt_mode (dev, post, sizeof (post), NULL, goodixtls5xx_check_none, ssm);
                break;
              }
          }
        if (cls->get_fdt_up_cfg)
          {
            GoodixTls5xxMcuConfig cfg = cls->get_fdt_up_cfg ();
            goodix_send_mcu_switch_to_fdt_up (dev, cfg.data, cfg.data_len, cfg.free_fn, goodixtls5xx_check_none_cmd, ssm);
          }
        else
          {
            send_switch_mode (dev, ssm, goodix_send_mcu_switch_to_fdt_up);
          }
      }
      break;

    case SCAN_STAGE_SWITCH_TO_FTD_DONE:
      fpi_image_device_report_finger_status (img_dev, FALSE);
      fpi_ssm_next_state (ssm);
      break;
    }
}

static void
scan_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (error)
    {
      fp_err ("failed to scan: %s (code: %d)", error->message, error->code);
      fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
      return;
    }
  fp_dbg ("finished scan");
}


void
goodixtls5xx_scan_start (FpiDeviceGoodixTls5xx * dev)
{
  fpi_ssm_start (fpi_ssm_new (FP_DEVICE (dev), scan_run_state, SCAN_STAGE_NUM), scan_complete);
}

void
goodixtls5xx_decode_frame (GoodixTls5xxPix * frame, guint32 max_pixels, guint32 frame_size, const guint8 *raw_frame)
{
  GoodixTls5xxPix *pix = frame;
  guint32 start = 0;
  guint32 end = (frame_size >= 4) ? (frame_size - 4) : frame_size;
  guint32 pixel_idx = 0;

  if (!frame || !raw_frame || max_pixels == 0)
    return;

  if (frame_size >= 13 && (frame_size - 13) % 6 == 0)
    {
      start = 8;
      end = frame_size - 5;
    }

  for (guint32 i = start; i + 6 <= end && pixel_idx + 4 <= max_pixels; i += 6)
    {
      const guint8 *chunk = raw_frame + i;
      pix[pixel_idx++] = ((chunk[0] & 0xf) << 8) + chunk[1];
      pix[pixel_idx++] = (chunk[3] << 4) + (chunk[0] >> 4);
      pix[pixel_idx++] = ((chunk[5] & 0xf) << 8) + chunk[2];
      pix[pixel_idx++] = (chunk[4] << 4) + (chunk[5] >> 4);
    }
}

static void
dev_change_state (FpImageDevice * img_dev, FpiImageDeviceState state)
{
  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    goodixtls5xx_scan_start (FPI_DEVICE_GOODIXTLS5XX (img_dev));
}
static void
dev_deinit (FpImageDevice * img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);
  GError *error = NULL;

  if (!goodix_dev_deinit (dev, &error))
    {
      fpi_image_device_close_complete (img_dev, error);
      return;
    }

  fpi_image_device_close_complete (img_dev, NULL);
}
static void
dev_init (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);
  GError *error = NULL;

  if (!goodix_dev_init (dev, &error))
    {
      fpi_image_device_open_complete (img_dev, error);
      return;
    }

  fpi_image_device_open_complete (img_dev, NULL);
}

static void
dev_deactivate (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);
  GError *error = NULL;
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  /* Orphan any in-flight TLS activation; its completion will drop. */
  goodix_activation_gen_bump (dev);

  goodix_reset_state (dev);

  goodix_shutdown_tls (dev, &error);

  goodixtls5xx_cleanup (FPI_DEVICE_GOODIXTLS5XX (dev));

  if (cls->reset_state)
    cls->reset_state (dev);
  goodix_stop_read_loop (dev);
  fpi_image_device_deactivate_complete (img_dev, error);
}

static void
tls_activation_complete (FpDevice *dev, gpointer user_data,
                         GError *error)
{
  FpImageDevice *image_dev;

  /* Drop orphaned completions without touching hardware. */
  if (GPOINTER_TO_UINT (user_data) != goodix_activation_gen_get (dev))
    {
      fp_dbg ("dropping stale TLS activation completion");
      if (error)
        g_error_free (error);
      return;
    }

  image_dev = FP_IMAGE_DEVICE (dev);

  if (error)
    {
      fp_err ("failed to complete tls activation: %s", error->message);
      fpi_image_device_activate_complete (image_dev, error);
      return;
    }

  fpi_image_device_activate_complete (image_dev, error);
}

void
goodixtls5xx_init_tls (FpDevice * dev)
{
  /* Capture the activation generation for the staleness guard. */
  goodix_tls_init (dev, tls_activation_complete,
                   GUINT_TO_POINTER (goodix_activation_gen_get (dev)));
}

void
fpi_device_goodixtls5xx_class_init (FpiDeviceGoodixTls5xxClass * self)
{
  FpImageDeviceClass *img_cls = FP_IMAGE_DEVICE_CLASS (self);

  self->get_mcu_cfg = NULL;
  self->get_fdt_down_cfg = NULL;
  self->get_fdt_up_cfg = NULL;
  self->process_frame = NULL;
  self->scan_height = 0;
  self->scan_width = 0;
  self->reset_state = NULL;

  img_cls->change_state = dev_change_state;
  img_cls->deactivate = dev_deactivate;
  img_cls->img_close = dev_deinit;
  img_cls->img_open = dev_init;
}

void
fpi_device_goodixtls5xx_init (FpiDeviceGoodixTls5xx * self)
{
  FpiDeviceGoodixTls5xxPrivate * priv = fpi_device_goodixtls5xx_get_instance_private (self);

  priv->calibration_img = NULL;
}

void
goodixtls5xx_cleanup (FpiDeviceGoodixTls5xx * dev)
{
  FpiDeviceGoodixTls5xxPrivate * priv = fpi_device_goodixtls5xx_get_instance_private (dev);

  g_free (priv->calibration_img);
  priv->calibration_img = NULL;
}