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
#include <stdio.h>


typedef struct
{
  guint8 * otp; // TODO: Remove
  GoodixTls5xxPix* calibration_img;
  guint fdt_down_mean;    // mean FDT value while the finger was detected
  gboolean have_fdt_down;
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


typedef void (*GoodixTls5xxModeSwitchFn)(FpDevice *, const guint8 *, guint16, GDestroyNotify, GoodixDefaultCallback, gpointer);

static void
send_switch_mode_cb (FpDevice * dev, gpointer ssm, GoodixTls5xxFdtKind kind,
                     GoodixTls5xxModeSwitchFn mode_switch,
                     GoodixDefaultCallback callback)
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  GoodixTls5xxMcuConfig cfg = cls->get_fdt_cfg ? cls->get_fdt_cfg (kind) : cls->get_mcu_cfg ();

  mode_switch (dev, cfg.data, cfg.data_len, cfg.free_fn, callback, ssm);
}

static void
send_switch_mode (FpDevice * dev, gpointer ssm, GoodixTls5xxFdtKind kind, GoodixTls5xxModeSwitchFn mode_switch)
{
  send_switch_mode_cb (dev, ssm, kind, mode_switch, goodixtls5xx_check_none_cmd);
}

/* FDT replies: 4 byte header followed by 6 little endian 16 bit values, one
 * per detection zone. A finger lowers the values (e.g. ~0x100 vs ~0x175 at
 * rest on the 5503). */
#define GOODIX_FDT_ZONES 6
#define GOODIX_FDT_REPLY_LEN (4 + 2 * GOODIX_FDT_ZONES)
/* A real finger-up must raise the mean by at least this much compared to the
 * finger-down reading; the 5503 firmware sometimes reports finger-up while its
 * own values still show the finger (light touch). */
#define GOODIX_FDT_UP_MIN_RISE 40
#define GOODIX_FDT_UP_RETRY_MS 50

static gboolean
fdt_reply_mean (const guint8 *data, guint16 len, guint *mean)
{
  guint sum = 0;

  if (!data || len < GOODIX_FDT_REPLY_LEN)
    return FALSE;
  for (int i = 0; i < GOODIX_FDT_ZONES; i++)
    sum += data[4 + 2 * i] | (data[5 + 2 * i] << 8);
  *mean = sum / GOODIX_FDT_ZONES;
  return TRUE;
}

static void
on_fdt_down_reply (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
                   GError *err)
{
  FpiDeviceGoodixTls5xxPrivate *priv =
    fpi_device_goodixtls5xx_get_instance_private (FPI_DEVICE_GOODIXTLS5XX (dev));

  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  priv->have_fdt_down = fdt_reply_mean (data, len, &priv->fdt_down_mean);
  if (priv->have_fdt_down)
    fp_dbg ("finger down, fdt mean 0x%03x", priv->fdt_down_mean);
  fpi_ssm_next_state (ssm);
}

static void on_fdt_up_reply (FpDevice *dev, guint8 *data, guint16 len,
                             gpointer ssm, GError *err);

static void
resend_fdt_up (FpDevice *dev, gpointer ssm)
{
  send_switch_mode_cb (dev, ssm, GOODIXTLS5XX_FDT_UP,
                       goodix_send_mcu_switch_to_fdt_up, on_fdt_up_reply);
}

static void
on_fdt_up_reply (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
                 GError *err)
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  FpiDeviceGoodixTls5xxPrivate *priv =
    fpi_device_goodixtls5xx_get_instance_private (FPI_DEVICE_GOODIXTLS5XX (dev));
  guint up_mean;

  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  if (cls->verify_fdt_up && priv->have_fdt_down &&
      fdt_reply_mean (data, len, &up_mean) &&
      up_mean < priv->fdt_down_mean + GOODIX_FDT_UP_MIN_RISE)
    {
      fp_dbg ("ignoring spurious finger-up (fdt mean 0x%03x, down 0x%03x)",
              up_mean, priv->fdt_down_mean);
      fpi_device_add_timeout (dev, GOODIX_FDT_UP_RETRY_MS, resend_fdt_up,
                              ssm, NULL);
      return;
    }

  priv->have_fdt_down = FALSE;
  fpi_ssm_next_state (ssm);
}

static void
read_image (FpDevice * dev, GoodixImageCallback callback, gpointer ssm)
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  if (cls->get_image_payload)
    goodix_tls_read_image_ex (dev, cls->get_image_payload,
                              cls->get_image_payload_len, callback, ssm);
  else
    goodix_tls_read_image (dev, callback, ssm);
}

/* Decode a decrypted frame into out (scan_width * scan_height pixels).
 * Returns FALSE and sets error if the frame is too short. */
static gboolean
decode_checked (FpDevice * dev, const guint8 * data, guint16 len,
                GoodixTls5xxPix * out, GError ** error)
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  const guint32 pixels = cls->scan_width * cls->scan_height;
  const guint32 expected = cls->frame_header + pixels / 4 * 6 + cls->frame_trailer;

  if (len < expected)
    {
      g_set_error (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_INVALID,
                   "Image frame too short: %u bytes, expected %u", len, expected);
      return FALSE;
    }
  if (len != expected)
    fp_dbg ("Image frame is %u bytes, expected %u", len, expected);

  goodixtls5xx_decode_frame_ex (out, pixels, len, data, cls->frame_header,
                                cls->frame_trailer);
  return TRUE;
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
  if (!decode_checked (dev, data, len, priv->calibration_img, &err)) {
    fpi_ssm_mark_failed (ssm, err);
    return;
  }

  fpi_ssm_next_state(ssm);
}
static void calibrate_run(FpiSsm* ssm, FpDevice* dev) {
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);

  switch (fpi_ssm_get_cur_state(ssm)) {
    case CALIBRATION_STAGE_FDT_UP:
      if (cls->skip_nav_calibration)
        fpi_ssm_next_state (ssm);
      else
        send_switch_mode(dev, ssm, GOODIXTLS5XX_FDT_UP, goodix_send_mcu_switch_to_fdt_up);
      break;
    case CALIBRATION_STAGE_NAV0:
      if (cls->skip_nav_calibration)
        fpi_ssm_next_state (ssm);
      else
        goodix_send_nav_0(dev, goodixtls5xx_check_none_cmd, ssm);
      break;
    case CALIBRATION_STAGE_GET_IMG:
      read_image (dev, on_calibrate_scan, ssm);
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
  if (data && len > 0)
    {
      g_autofree gchar *str = data_to_str (data, MIN (len, 32));
      fp_dbg ("reply (%u bytes): %s", len, str);
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

  GoodixTls5xxPix * raw_frame = calloc (cls->scan_width * cls->scan_height, sizeof (GoodixTls5xxPix));
  if (!decode_checked (dev, data, len, raw_frame, &err))
    {
      free (raw_frame);
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  linear_subtract_inplace(raw_frame, priv->calibration_img, cls->scan_width * cls->scan_height);
  guint8 * squashed = calloc (cls->scan_height * cls->scan_width, 1);
  goodixtls5xx_squash_frame_linear (raw_frame, squashed, cls->scan_height * cls->scan_width);
  free (raw_frame);
  FpImage * img = cls->process_frame (squashed);
  free(squashed);

  /* Debug aid: FP_GOODIX_DUMP_DIR=/some/dir saves every processed frame */
  const char *dump_dir = g_getenv ("FP_GOODIX_DUMP_DIR");
  if (dump_dir)
    {
      static guint dump_count = 0;
      g_autofree gchar *name = g_strdup_printf ("goodix-frame-%03u.pgm", dump_count++);
      g_autofree gchar *path = g_build_filename (dump_dir, name, NULL);
      goodixtls5xx_save_image_to_pgm (img, path);
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
  read_image (dev, scan_on_read_img, ssm);
}



static void
scan_run_state (FpiSsm * ssm, FpDevice * dev)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_STAGE_QUERY_MCU:
      if (FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev)->skip_query_mcu)
        fpi_ssm_next_state (ssm);
      else
        goodix_send_query_mcu_state (dev, query_mcu_state_cb, ssm);
      break;
    case SCAN_STAGE_SWITCH_TO_FDT_MODE:
      send_switch_mode (dev, ssm, GOODIXTLS5XX_FDT_MODE, goodix_send_mcu_switch_to_fdt_mode);
      break;

    case SCAN_STAGE_CALIBRATE:
      do_calibration(dev, ssm);
      break;
    case SCAN_STAGE_SWITCH_TO_FDT_DOWN:
      send_switch_mode_cb (dev, ssm, GOODIXTLS5XX_FDT_DOWN, goodix_send_mcu_switch_to_fdt_down, on_fdt_down_reply);
      break;

    case SCAN_STAGE_GET_IMG:
      fpi_image_device_report_finger_status (img_dev, TRUE);
      scan_get_img (dev, ssm);
      break;

    case SCAN_STAGE_SWITCH_TO_FTD_UP:
      send_switch_mode_cb (dev, ssm, GOODIXTLS5XX_FDT_UP, goodix_send_mcu_switch_to_fdt_up, on_fdt_up_reply);
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

guint32
goodixtls5xx_decode_frame_ex (GoodixTls5xxPix * frame, guint32 max_pixels,
                              guint32 frame_size, const guint8 *raw_frame,
                              guint16 header, guint16 trailer)
{
  guint32 n = 0;

  if (frame_size < (guint32) header + trailer)
    return 0;

  const guint32 end = frame_size - trailer;

  for (guint32 i = header; i + 6 <= end && n + 4 <= max_pixels; i += 6)
    {
      const guint8 *chunk = raw_frame + i;
      frame[n++] = ((chunk[0] & 0xf) << 8) + chunk[1];
      frame[n++] = (chunk[3] << 4) + (chunk[0] >> 4);
      frame[n++] = ((chunk[5] & 0xf) << 8) + chunk[2];
      frame[n++] = (chunk[4] << 4) + (chunk[5] >> 4);
    }
  return n;
}

void
goodixtls5xx_decode_frame (GoodixTls5xxPix * frame, guint32 frame_size, const guint8 *raw_frame)
{
  /* Legacy 5110 framing; caller guarantees the buffer is large enough */
  goodixtls5xx_decode_frame_ex (frame, G_MAXUINT32, frame_size, raw_frame, 8, 5);
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
post_tls_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (error)
    fp_err ("failed after tls activation: %s", error->message);
  fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
}

static void
tls_activation_complete (FpDevice *dev, gpointer user_data,
                         GError *error)
{
  FpiDeviceGoodixTls5xxClass *cls = FPI_DEVICE_GOODIXTLS5XX_GET_CLASS (dev);
  FpImageDevice *image_dev = FP_IMAGE_DEVICE (dev);

  if (error)
    {
      fp_err ("failed to complete tls activation: %s", error->message);
      fpi_image_device_activate_complete (image_dev, error);
      return;
    }

  if (cls->post_tls_run)
    {
      fpi_ssm_start (fpi_ssm_new (dev, cls->post_tls_run,
                                  cls->post_tls_num_states),
                     post_tls_complete);
      return;
    }

  fpi_image_device_activate_complete (image_dev, NULL);
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
  self->get_fdt_cfg = NULL;
  self->get_image_payload = NULL;
  self->get_image_payload_len = 0;
  self->frame_header = 8;
  self->frame_trailer = 5;
  self->skip_nav_calibration = FALSE;
  self->post_tls_run = NULL;
  self->post_tls_num_states = 0;
  self->skip_query_mcu = FALSE;
  self->verify_fdt_up = FALSE;

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
  priv->have_fdt_down = FALSE;
  priv->otp = NULL;
}

void goodixtls5xx_cleanup(FpiDeviceGoodixTls5xx* dev) {
  FpiDeviceGoodixTls5xxPrivate* priv = fpi_device_goodixtls5xx_get_instance_private(dev);
  g_free(priv->calibration_img);
  priv->calibration_img = NULL;
  priv->have_fdt_down = FALSE;
}