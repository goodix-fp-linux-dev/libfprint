// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Natasha England-Elbro <natasha@natashaee.me>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
//
#include "fp-image-device.h"
#include "fpi-image-device.h"
#include "fpi-ssm.h"
#define FP_COMPONENT "goodixtls5xx"

#include "drivers/goodixtls/goodix5xx.h"
#include "drivers_api.h"
#include "goodix.h"
#include <errno.h>
#include <stdio.h>


#define GOODIX5XX_MAX_FDT_CELLS 32

typedef struct
{
  guint8 * otp; // TODO: Remove
  GoodixTls5xxPix* calibration_img;
  guint8   fdt_cells[GOODIX5XX_MAX_FDT_CELLS];
  guint    n_fdt_cells;
  gboolean fdt_have;
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
  SCAN_STAGE_SWITCH_TO_FDT_DOWN,
  SCAN_STAGE_GET_IMG,
  SCAN_STAGE_SWITCH_TO_FTD_UP,
  SCAN_STAGE_SWITCH_TO_FTD_DONE,

  SCAN_STAGE_NUM,
};


// Copy an fdt template, replacing the value byte after each 0x80 flag with the
// next derived cell. The template is [selector...][0x80 v][0x80 v]...; the scan
// skips the selector header (bytes before the first 0x80) then steps in pairs,
// stopping if the [0x80, value] pattern breaks. Returns NULL on allocation
// failure; the caller falls back to the unmodified template.
static guint8 *
apply_adaptive_fdt (const guint8 *tmpl, guint16 len,
                    const guint8 *cells, guint n_cells)
{
  guint8 *out = malloc (len);
  guint start = 0, k = 0;

  if (!out)
    return NULL;
  memcpy (out, tmpl, len);
  while (start < len && tmpl[start] != 0x80)
    start++;
  for (guint i = start; i + 1 < len && k < n_cells; i += 2)
    {
      if (tmpl[i] != 0x80)
        break;
      out[i + 1] = cells[k++];
    }
  return out;
}

// fdt-mode reply body: 00 00 01 00 <Nx LE uint16 baseline>. Each fdt threshold
// cell is derived as baseline >> 1 (the 0x80 flag is supplied by the template).
static void
on_fdt_mode_baseline (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
                      GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  FpiDeviceGoodixTls5xxPrivate *priv = fpi_device_goodixtls5xx_get_instance_private (
    FPI_DEVICE_GOODIXTLS5XX (dev));

  if (data && len >= 6)
    {
      guint n = 0;
      for (guint i = 4; i + 1 < len && n < GOODIX5XX_MAX_FDT_CELLS; i += 2)
        {
          guint16 b = data[i] | (data[i + 1] << 8);
          priv->fdt_cells[n++] = (b >> 1) & 0xff;
        }
      priv->n_fdt_cells = n;
      priv->fdt_have = n > 0;
    }
  else
    {
      fp_dbg ("short fdt-mode reply (%u bytes), keeping previous thresholds", len);
    }

  fpi_ssm_next_state (ssm);
}

static void
send_switch_mode_cfg (FpDevice * dev, gpointer ssm, void (*mode_switch)(FpDevice *, const guint8 *, guint16, GDestroyNotify, GoodixDefaultCallback, gpointer), GoodixTls5xxGetMcuFn cfg_fn, GoodixDefaultCallback cb)
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  GoodixTls5xxMcuConfig cfg = cfg_fn ();

  if (cls->fdt_adaptive)
    {
      FpiDeviceGoodixTls5xxPrivate *priv = fpi_device_goodixtls5xx_get_instance_private (
        FPI_DEVICE_GOODIXTLS5XX (dev));
      guint8 *adapt = priv->fdt_have
                      ? apply_adaptive_fdt (cfg.data, cfg.data_len,
                                            priv->fdt_cells, priv->n_fdt_cells)
                      : NULL;

      if (adapt)
        {
          if (cfg.free_fn)
            cfg.free_fn ((void *) cfg.data);
          mode_switch (dev, adapt, cfg.data_len, free, cb, ssm);
          return;
        }
    }

  mode_switch (dev, cfg.data, cfg.data_len, cfg.free_fn, cb, ssm);
}

static void
send_switch_mode (FpDevice * dev, gpointer ssm, void (*mode_switch)(FpDevice *, const guint8 *, guint16, GDestroyNotify, GoodixDefaultCallback, gpointer))
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  send_switch_mode_cfg (dev, ssm, mode_switch, cls->get_mcu_cfg,
                        goodixtls5xx_check_none_cmd);
}
static void on_calibrate_scan(FpDevice* dev, guint8* data, guint16 len, gpointer ssm, GError* err) {
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }
  FpiDeviceGoodixTls5xx* self = FPI_DEVICE_GOODIXTLS5XX(dev);
  FpiDeviceGoodixTls5xxPrivate* priv = fpi_device_goodixtls5xx_get_instance_private(self);
  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (self);
  if (!priv->calibration_img) {
    priv->calibration_img = calloc(cls->scan_height * cls->scan_width, sizeof(GoodixTls5xxPix));
  }
  if (cls->decode_frame)
    cls->decode_frame(priv->calibration_img, len, data);
  else
    goodixtls5xx_decode_frame(priv->calibration_img, len, data);

  fpi_ssm_next_state(ssm);
}
static void calibrate_run(FpiSsm* ssm, FpDevice* dev) {
  switch (fpi_ssm_get_cur_state(ssm)) {
    case CALIBRATION_STAGE_FDT_UP:
      send_switch_mode(dev, ssm, goodix_send_mcu_switch_to_fdt_up);
      break;
    case CALIBRATION_STAGE_NAV0:
      goodix_send_nav_0(dev, goodixtls5xx_check_none_cmd, ssm);
      break;
    case CALIBRATION_STAGE_GET_IMG:
      goodix_tls_read_image(dev, on_calibrate_scan, ssm);
  }
}

static void do_calibration(FpDevice* dev, FpiSsm* parent) {
  fpi_ssm_start_subsm(parent, fpi_ssm_new(dev, calibrate_run, CALIBRATION_STAGE_NUM));
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
  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
      return;
    }

  fp_dbg ("Device firmware: \"%s\"", firmware);
  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (FPI_DEVICE_GOODIXTLS5XX (dev));

  // Prefix match so a driver can accept a whole firmware family
  // (e.g. GF_ST411SEC_APP_ covers both the 5110 and the 5f10/GF3206).
  if (!g_str_has_prefix (firmware, cls->firmware_version))
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

  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

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

  FpiDeviceGoodixTls5xxClass * cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
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
static void linear_subtract_inplace(GoodixTls5xxPix* src, GoodixTls5xxPix* by, guint16 len) {
  const guint16 max = -1;
  for (guint16 n = 0; n != len; ++n) {
    src[n] = MAX(0, max - ((max - src[n]) - (max - by[n])));
  }
}

static void
scan_on_read_img (FpDevice *dev, guint8 *data, guint16 len,
                  gpointer ssm, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  FpImageDevice * img_dev = FP_IMAGE_DEVICE (dev);

  FpiDeviceGoodixTls5xx* self = FPI_DEVICE_GOODIXTLS5XX(dev);
  FpiDeviceGoodixTls5xxPrivate* priv = fpi_device_goodixtls5xx_get_instance_private(self);
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  const guint frame_px = cls->scan_width * cls->scan_height;
  GoodixTls5xxPix * raw_frame = calloc (frame_px, sizeof (GoodixTls5xxPix));
  if (cls->decode_frame)
    cls->decode_frame (raw_frame, len, data);
  else
    goodixtls5xx_decode_frame (raw_frame, len, data);

  FpImage * img;
  if (cls->process_raw_frame)
    {
      // Driver-specific flat-field / enhancement straight from raw+calibration.
      img = cls->process_raw_frame (raw_frame, priv->calibration_img,
                                    cls->scan_width, cls->scan_height);
      free (raw_frame);
    }
  else
    {
      linear_subtract_inplace(raw_frame, priv->calibration_img, cls->scan_width * cls->scan_height);
      guint8 * squashed = calloc (cls->scan_height * cls->scan_width, 1);
      goodixtls5xx_squash_frame_linear (raw_frame, squashed, cls->scan_height * cls->scan_width);
      free (raw_frame);
      img = cls->process_frame (squashed);
      free(squashed);
    }

  fpi_image_device_image_captured (img_dev, img);

  fpi_ssm_next_state (ssm);
}

static void
query_mcu_state_cb (FpDevice * dev, guchar * mcu_state, guint16 len,
                    gpointer ssm, GError * error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
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

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_STAGE_QUERY_MCU:
      {
        // Re-prime the baseline each scan: clear it so the fdt-mode prime sends
        // the raw template and its reply repopulates the thresholds for this
        // scan's fdt-down/up.
        FpiDeviceGoodixTls5xxPrivate *priv = fpi_device_goodixtls5xx_get_instance_private (
          FPI_DEVICE_GOODIXTLS5XX (dev));
        priv->fdt_have = FALSE;
        goodix_send_query_mcu_state (dev, query_mcu_state_cb, ssm);
      }
      break;
    case SCAN_STAGE_SWITCH_TO_FDT_MODE:
      {
        FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
        GoodixTls5xxGetMcuFn cfg_fn = cls->get_mcu_cfg_fdt_mode
                                      ? cls->get_mcu_cfg_fdt_mode
                                      : cls->get_mcu_cfg;
        /* goodix_send_mcu_switch_to_fdt_mode sends its payload verbatim (no
         * selector byte is prepended, unlike fdt-down/fdt-up). Sensors whose
         * firmware requires the leading fdt-mode selector provide it via
         * get_mcu_cfg_fdt_mode; others fall back to get_mcu_cfg unchanged.
         * In adaptive mode the reply carries the live baseline, so the callback
         * captures it (on_fdt_mode_baseline) to derive the fdt-down/up thresholds. */
        send_switch_mode_cfg (dev, ssm, goodix_send_mcu_switch_to_fdt_mode, cfg_fn,
                              cls->fdt_adaptive ? on_fdt_mode_baseline
                                                : goodixtls5xx_check_none_cmd);
      }
      break;

    case SCAN_STAGE_CALIBRATE:
      do_calibration(dev, ssm);
      break;
    case SCAN_STAGE_SWITCH_TO_FDT_DOWN:
      {
        FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
        /* fdt-down replies (and so this command completes) only when a finger
         * is on the sensor, which requires the proper fdt-down thresholds.
         * Drivers needing distinct thresholds provide get_mcu_cfg_fdt_down. */
        send_switch_mode_cfg (dev, ssm, goodix_send_mcu_switch_to_fdt_down,
                              cls->get_mcu_cfg_fdt_down ? cls->get_mcu_cfg_fdt_down
                                                        : cls->get_mcu_cfg,
                              goodixtls5xx_check_none_cmd);
      }
      break;

    case SCAN_STAGE_GET_IMG:
      fpi_image_device_report_finger_status (img_dev, TRUE);
      scan_get_img (dev, ssm);
      break;

    case SCAN_STAGE_SWITCH_TO_FTD_UP:
      send_switch_mode (dev, ssm, goodix_send_mcu_switch_to_fdt_up );
      break;

    case SCAN_STAGE_SWITCH_TO_FTD_DONE:
      fpi_image_device_report_finger_status (img_dev, FALSE);
      fpi_ssm_next_state(ssm);
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
goodixtls5xx_decode_frame (GoodixTls5xxPix * frame, guint32 frame_size, const guint8 *raw_frame)
{
  GoodixTls5xxPix *pix = frame;

  for (int i = 8; i != frame_size - 5; i += 6)
    {
      const guint8 *chunk = raw_frame + i;
      *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
      *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
      *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
      *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
    }
}

gboolean
goodixtls5xx_save_image_to_pgm (FpImage *img, const char *path)
{
  FILE *fd = fopen (path, "w");
  size_t write_size;
  const guchar *data = fp_image_get_data (img, &write_size);
  int r;

  if (!fd)
    {
      g_warning ("could not open '%s' for writing: %d", path, errno);
      return FALSE;
    }

  r = fprintf (fd, "P5 %d %d 255\n", fp_image_get_width (img),
               fp_image_get_height (img));
  if (r < 0)
    {
      fclose (fd);
      g_critical ("pgm header write failed, error %d", r);
      return FALSE;
    }

  r = fwrite (data, 1, write_size, fd);
  if (r < write_size)
    {
      fclose (fd);
      g_critical ("short write (%d)", r);
      return FALSE;
    }

  fclose (fd);
  g_debug ("written to '%s'", path);

  return TRUE;
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

  if (goodix_dev_deinit (dev, &error))
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

  if (goodix_dev_init (dev, &error))
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

  goodix_reset_state (dev);
  GError *error = NULL;

  goodix_shutdown_tls (dev, &error);

  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  goodixtls5xx_cleanup(FPI_DEVICE_GOODIXTLS5XX(dev));

  if (cls->reset_state)
    cls->reset_state (dev);
  fpi_image_device_deactivate_complete (img_dev, error);
}

static void
tls_activation_complete (FpDevice *dev, gpointer user_data,
                         GError *error)
{
  if (error)
    {
      fp_err ("failed to complete tls activation: %s", error->message);
      return;
    }
  FpImageDevice *image_dev = FP_IMAGE_DEVICE (dev);

  fpi_image_device_activate_complete (image_dev, error);
}

void
goodixtls5xx_init_tls (FpDevice * dev)
{
  goodix_tls_init (dev, tls_activation_complete, NULL);
}

void
fpi_device_goodixtls5xx_class_init (FpiDeviceGoodixTls5xxClass * self)
{
  self->get_mcu_cfg = NULL;
  self->process_frame = NULL;
  self->scan_height = 0;
  self->scan_width = 0;
  self->reset_state = NULL;

  FpImageDeviceClass *img_cls = FP_IMAGE_DEVICE_CLASS (self);

  img_cls->change_state = dev_change_state;
  img_cls->deactivate = dev_deactivate;
  img_cls->img_close = dev_deinit;
  img_cls->img_open = dev_init;
}

void
fpi_device_goodixtls5xx_init (FpiDeviceGoodixTls5xx * self)
{
  FpiDeviceGoodixTls5xxPrivate* priv = fpi_device_goodixtls5xx_get_instance_private(self);
  priv->calibration_img = NULL;
  priv->otp = NULL;
}

void goodixtls5xx_cleanup(FpiDeviceGoodixTls5xx* dev) {
  FpiDeviceGoodixTls5xxPrivate* priv = fpi_device_goodixtls5xx_get_instance_private(dev);
  g_free(priv->calibration_img);
  priv->calibration_img = NULL;
}