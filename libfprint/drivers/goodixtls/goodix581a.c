// Goodix 27c6:581a (Xiaomi GM168) driver for libfprint

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

#define FP_COMPONENT "goodixtls581a"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix581a.h"

struct _FpiDeviceGoodixTls581a
{
  FpiDeviceGoodixTls5xx parent;

  GoodixTls5xxPix *calibration_img;
  guint32 *calibration_sum;
  guint calibration_frame_count;
  FpiSsm *scan_ssm;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls581a, fpi_device_goodixtls581a, FPI,
                      DEVICE_GOODIXTLS581A, FpiDeviceGoodixTls5xx);

G_DEFINE_TYPE (FpiDeviceGoodixTls581a, fpi_device_goodixtls581a,
               FPI_TYPE_DEVICE_GOODIXTLS5XX);

enum activate_states {
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_READ_FIRMWARE,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_RESET,
  ACTIVATE_READ_REG,
  ACTIVATE_READ_OTP,
  ACTIVATE_CMD_D6,
  ACTIVATE_INIT_TLS,
  ACTIVATE_UPLOAD_MCU_CONFIG,
  ACTIVATE_CMD_C4_1,
  ACTIVATE_CMD_C4_2,
  ACTIVATE_CMD_D2,
  ACTIVATE_CMD_36,
  ACTIVATE_NUM_STATES,
};

/* GM168 firmware exposes SHA256(PSK) under preset-PSK tag 0xbb020001.
 * Compare the hash for the configured all-zero TLS key without logging the
 * value returned by the sensor. 0xbb020003 is not this hash field; probing
 * it returned BUSY on GM168SEC firmware and provided no key diagnostic.
 */
static const guint8 goodix_581a_tls_psk_hash[32] = {
  0x66, 0x68, 0x7a, 0xad, 0xf8, 0x62, 0xbd, 0x77,
  0x6c, 0x8f, 0xc1, 0x8b, 0x8e, 0x9f, 0x8e, 0x20,
  0x08, 0x97, 0x14, 0x85, 0x6e, 0xe2, 0x33, 0xb3,
  0x90, 0x2a, 0x59, 0x1d, 0x0d, 0x5f, 0x29, 0x25,
};

static void
on_581a_firmware_version (FpDevice *dev, gchar *firmware,
                          gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fp_warn ("581a firmware version query failed: %s", error->message);
      g_clear_error (&error);
      fpi_ssm_next_state (ssm);
      return;
    }

  fp_warn ("581a firmware version: %s", firmware ? firmware : "(empty)");
  fpi_ssm_next_state (ssm);
}

static void
on_581a_psk_read (FpDevice *dev, gboolean success, guint32 flags,
                  guint8 *psk, guint16 length, gpointer user_data,
                  GError *error)
{
  FpiSsm *ssm = user_data;
  const gboolean matches_tls_key =
    success && flags == 0xbb020001 &&
    length == sizeof (goodix_581a_tls_psk_hash) && psk &&
    memcmp (psk, goodix_581a_tls_psk_hash,
            sizeof (goodix_581a_tls_psk_hash)) == 0;

  if (error)
    {
      fp_err ("581a PSK diagnostic read failed: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_warn ("581a PSK diagnostic: readable=%d flags=0x%08x length=%u matches_tls_key=%d",
           success, flags, length, matches_tls_key);

  if (!success)
    {
      /* Some firmware rejects PSK reads in application mode. This is only a
       * diagnostic probe, so do not prevent the real TLS handshake from
       * determining whether the configured key works.
       */
      fp_warn ("581a PSK could not be read; continuing to TLS handshake");
    }

  fpi_ssm_next_state (ssm);
}

enum scan_581a_states {
  SCAN_581A_CALIBRATE,
  SCAN_581A_ARM_TOUCH,
  SCAN_581A_CAPTURE,
  SCAN_581A_AWAIT_UP,
  SCAN_581A_NUM_STATES,
};

/* The 5xx decoder unpacks samples in stream order. The 581a sends 64 samples
 * for each of 80 columns, so transpose that stream into physical row-major
 * image order before baseline subtraction or image processing.
 */
static void
goodix_581a_decode_frame (GoodixTls5xxPix *frame, guint32 frame_size,
                          const guint8 *raw_frame, guint16 header_len,
                          guint16 footer_len)
{
  GoodixTls5xxPix stream[GOODIX_581A_FRAME_SIZE];

  goodixtls5xx_decode_frame (stream, frame_size, raw_frame, header_len,
                             footer_len);

  for (guint i = 0; i < GOODIX_581A_FRAME_SIZE; i++)
    frame[goodix_581a_stream_to_image_index (i)] = stream[i];
}

/* One scan action captures a Mode 1 ambient reference, waits for FDT touch,
 * captures Mode 5, then waits for finger-up. Keep the baseline across touches
 * in an enrollment action so samples share the same reference.
 */
static void
on_tls_init_ready (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  if (error)
    {
      fp_err ("581a TLS handshake failed: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fp_dbg ("581a TLS Handshake successfully established!");
  fpi_ssm_next_state (ssm);
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  static const guint8 reset_payload[] = {0x05, 0x14};
  static const guint8 reg_payload[] = {0x00, 0x00, 0x00, 0x04, 0x00};
  static const guint8 otp_payload[] = {0x00, 0x00};
  static const guint8 d6_payload[] = {0x00, 0x00};
  static const guint8 c4_payload[] = {0x01, 0x00};
  static const guint8 d2_payload[] = {0x00, 0x00};

  /* Firmware requires this command order; TLS must be established before
   * uploading the sensor-specific MCU configuration.
   */
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_READ_AND_NOP:
      goodix_start_read_loop (dev);
      goodix_send_nop (dev, goodixtls5xx_check_none, ssm);
      break;

    case ACTIVATE_READ_FIRMWARE:
      goodix_send_query_firmware_version (dev, on_581a_firmware_version, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_gm168_preset_psk_read (
        dev, 0xbb020001, sizeof (goodix_581a_tls_psk_hash),
        on_581a_psk_read, ssm);
      break;

    case ACTIVATE_RESET:
      goodix_send_protocol (dev, GOODIX_CMD_RESET, reset_payload, sizeof (reset_payload),
                            NULL, TRUE, 500, TRUE, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_READ_REG:
      goodix_send_protocol (dev, GOODIX_CMD_READ_SENSOR_REGISTER, reg_payload, sizeof (reg_payload),
                            NULL, TRUE, 500, TRUE, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_READ_OTP:
      goodix_send_protocol (dev, GOODIX_CMD_READ_OTP, otp_payload, sizeof (otp_payload),
                            NULL, TRUE, 500, TRUE, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_CMD_D6:
      goodix_send_protocol (dev, 0xd6, d6_payload, sizeof (d6_payload),
                            NULL, TRUE, 500, TRUE, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_INIT_TLS:
      goodix_tls_init (dev, on_tls_init_ready, ssm);
      break;

    case ACTIVATE_UPLOAD_MCU_CONFIG:
      goodix_send_upload_config_mcu (dev, (guint8 *) goodix_581a_config,
                                     sizeof (goodix_581a_config), NULL,
                                     goodixtls5xx_check_config_upload, ssm);
      break;

    case ACTIVATE_CMD_C4_1:
      goodix_send_protocol (dev, 0xc4, c4_payload, sizeof (c4_payload),
                            NULL, TRUE, 500, FALSE, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_CMD_C4_2:
      goodix_send_protocol (dev, 0xc4, c4_payload, sizeof (c4_payload),
                            NULL, TRUE, 500, FALSE, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_CMD_D2:
      goodix_send_protocol (dev, 0xd2, d2_payload, sizeof (d2_payload),
                            NULL, TRUE, 500, TRUE, goodixtls5xx_check_none_cmd, ssm);
      break;

    case ACTIVATE_CMD_36:
      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE,
                            goodix_581a_payload_36, sizeof (goodix_581a_payload_36),
                            NULL, TRUE, 500, TRUE, goodixtls5xx_check_none_cmd, ssm);
      break;
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (!error)
    {
      fp_dbg ("581a activation successfully completed!");
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), NULL);
    }
  else
    {
      fp_err ("581a failed during activation: %s (code: %d)", error->message,
              error->code);
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
    }
}

static void
dev_activate (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);

  fpi_ssm_start (fpi_ssm_new (dev, activate_run_state, ACTIVATE_NUM_STATES),
                 activate_complete);
}

static void
on_581a_calib_read (FpDevice *dev, guint8 *data, guint16 length,
                    gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (dev);

  if (!self->scan_ssm || self->scan_ssm != ssm)
    {
      if (error)
        g_error_free (error);
      return;
    }

  if (error)
    {
      g_clear_pointer (&self->calibration_sum, g_free);
      self->calibration_frame_count = 0;
      fp_err ("581a: Failed to capture baseline image: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (!data || length != GOODIX_581A_RAW_FRAME_SIZE)
    {
      g_clear_pointer (&self->calibration_sum, g_free);
      self->calibration_frame_count = 0;
      fpi_ssm_mark_failed (ssm,
                           g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "581a baseline frame has invalid length %u (expected %u)",
                                        length, GOODIX_581A_RAW_FRAME_SIZE));
      return;
    }

  GoodixTls5xxPix frame[GOODIX_581A_FRAME_SIZE];
  goodix_581a_decode_frame (frame, length, data, 0, 4);

  if (!self->calibration_sum)
    self->calibration_sum = g_new0 (guint32, GOODIX_581A_FRAME_SIZE);
  goodix_581a_accumulate_frame (self->calibration_sum, frame);
  self->calibration_frame_count++;

  if (self->calibration_frame_count < GOODIX_581A_AVERAGE_FRAMES)
    {
      fp_dbg ("581a: Ambient baseline frame %u/%u",
              self->calibration_frame_count, GOODIX_581A_AVERAGE_FRAMES);
      goodix_tls_read_image (dev, goodix_581a_get_image_mode1,
                             sizeof (goodix_581a_get_image_mode1),
                             on_581a_calib_read, ssm);
      return;
    }

  g_clear_pointer (&self->calibration_img, g_free);
  self->calibration_img = g_new (GoodixTls5xxPix, GOODIX_581A_FRAME_SIZE);
  goodix_581a_finish_average (self->calibration_img, self->calibration_sum,
                              GOODIX_581A_AVERAGE_FRAMES);
  g_clear_pointer (&self->calibration_sum, g_free);
  self->calibration_frame_count = 0;

  fp_dbg ("581a: Ambient baseline averaged from %u frames",
          GOODIX_581A_AVERAGE_FRAMES);
  fpi_ssm_next_state (ssm);
}

static void
on_581a_touch_detected (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (dev);

  if (!self->scan_ssm || self->scan_ssm != ssm)
    {
      if (error)
        g_error_free (error);
      return;
    }

  if (error)
    {
      fp_err ("581a: Touch interrupt error: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("581a: Hardware capacitive touch detected on sensor!");
  fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), TRUE);
  fpi_ssm_next_state (ssm);
}

static int
compare_ints (const void *a, const void *b)
{
  int arg1 = *(const int *) a;
  int arg2 = *(const int *) b;
  return (arg1 > arg2) - (arg1 < arg2);
}

static void
on_581a_mode5_read (FpDevice *dev, guint8 *data, guint16 length,
                    gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (dev);

  if (!self->scan_ssm || self->scan_ssm != ssm)
    {
      if (error)
        g_error_free (error);
      return;
    }

  if (error)
    {
      fp_err ("581a: Failed to capture Mode 5 scan: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (!data || length != GOODIX_581A_RAW_FRAME_SIZE)
    {
      fpi_ssm_mark_failed (ssm,
                           g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "581a scan frame has invalid length %u (expected %u)",
                                        length, GOODIX_581A_RAW_FRAME_SIZE));
      return;
    }

  GoodixTls5xxPix raw_frame[GOODIX_581A_FRAME_SIZE];
  goodix_581a_decode_frame (raw_frame, length, data, 0, 4);

  /* Fit a gain and offset from the untouched baseline to this scan. Keeping
   * the residual signed preserves both ridge polarities and accommodates
   * sensor gain drift. Ignore pixels which are zero in both frames (inactive
   * columns at some sensor tcodes) when fitting the flat field.
   */
  double sum_baseline = 0.0;
  double sum_scan = 0.0;
  double sum_baseline_sq = 0.0;
  double sum_baseline_scan = 0.0;
  guint n_fit = 0;
  for (guint i = 0; i < GOODIX_581A_FRAME_SIZE; i++)
    {
      const double baseline = self->calibration_img ? self->calibration_img[i] : 0;
      const double scan = raw_frame[i];

      if (baseline == 0.0 && scan == 0.0)
        continue;

      sum_baseline += baseline;
      sum_scan += scan;
      sum_baseline_sq += baseline * baseline;
      sum_baseline_scan += baseline * scan;
      n_fit++;
    }

  double gain = 1.0;
  double offset = 0.0;
  if (n_fit > 0)
    {
      const double denominator = n_fit * sum_baseline_sq - sum_baseline * sum_baseline;
      if (denominator > 0.0)
        gain = (n_fit * sum_baseline_scan - sum_baseline * sum_scan) / denominator;
      offset = (sum_scan - gain * sum_baseline) / n_fit;
    }

  gint32 residual[GOODIX_581A_FRAME_SIZE];
  gint32 active_vals[GOODIX_581A_FRAME_SIZE];
  int n_active = 0;

  for (int y = 0; y < GOODIX_581A_HEIGHT; y++)
    {
      for (int x = 0; x < GOODIX_581A_WIDTH; x++)
        {
          int idx = y * GOODIX_581A_WIDTH + x;
          const double baseline = self->calibration_img ? self->calibration_img[idx] : 0;
          residual[idx] = raw_frame[idx] - (gint32) (gain * baseline + offset);

          if (x >= 2 && x < GOODIX_581A_WIDTH - 2 &&
              y >= 2 && y < GOODIX_581A_HEIGHT - 2)
            {
              active_vals[n_active++] = residual[idx];
            }
        }
    }

  /* Use the signed, flat-fielded difference before any high-pass operation.
   * 20 counts is a provisional separation point from early 581a captures;
   * validate it across more devices and repeated air/finger frames.
   */
  gint32 p5 = 0, p95 = 0;
  gboolean has_usable_contrast = FALSE;
  if (n_active > 50)
    {
      qsort (active_vals, n_active, sizeof (gint32), compare_ints);
      p5 = active_vals[n_active * 5 / 100];
      p95 = active_vals[n_active * 95 / 100];
      has_usable_contrast = p95 - p5 >= 20;
    }

  fp_dbg ("581a: Ridge contrast range: p5=%d, p95=%d, active=%d pixels, usable=%d",
          p5, p95, n_active, has_usable_contrast);

  FpImage *img = fp_image_new (GOODIX_581A_WIDTH, GOODIX_581A_HEIGHT);
  img->flags |= FPI_IMAGE_PARTIAL;
  img->ppmm = 500.0 / 25.4;

  for (int y = 0; y < GOODIX_581A_HEIGHT; y++)
    {
      for (int x = 0; x < GOODIX_581A_WIDTH; x++)
        {
          int idx = y * GOODIX_581A_WIDTH + x;
          if (!has_usable_contrast || x < 2 || x >= GOODIX_581A_WIDTH - 2 ||
              y < 2 || y >= GOODIX_581A_HEIGHT - 2)
            {
              img->data[idx] = 255;
            }
          else
            {
              gint32 val = (residual[idx] - p5) * 255 / (p95 - p5);
              if (val < 0) val = 0;
              if (val > 255) val = 255;
              img->data[idx] = (guint8) (255 - val);
            }
        }
    }

  fp_dbg ("581a: Frame processed, reporting native 80x64 image captured for SIGFM!");
  img->ppmm = 500.0 / 25.4;
  fpi_image_device_image_captured (FP_IMAGE_DEVICE (dev), img);

  if (!self->scan_ssm)
    return;

  /* Advance to AWAIT_UP to wait for user to physically remove their finger */
  fpi_ssm_next_state (ssm);
}

static void
on_581a_touch_up (FpDevice *dev, guint8 *data, guint16 length,
                  gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (dev);

  if (!self->scan_ssm || self->scan_ssm != ssm)
    {
      if (error)
        g_error_free (error);
      return;
    }

  if (error)
    {
      fp_err ("581a: Touch up error: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("581a: Finger physically lifted from sensor!");
  /* Keep calibration_img intact across touches to maintain stable ambient air baseline */
  self->scan_ssm = NULL;
  fpi_ssm_mark_completed (ssm);

  fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (dev), FALSE);
}

static void
scan_581a_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (dev);

  /* Each captured image is followed by a physical lift before another scan,
   * allowing libfprint to advance one enrollment stage per touch.
   */
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_581A_CALIBRATE:
      if (self->calibration_img)
        {
          fpi_ssm_next_state (ssm);
          return;
        }
      fp_dbg ("581a: Reading ambient baseline...");
      goodix_tls_read_image (dev, goodix_581a_get_image_mode1,
                             sizeof (goodix_581a_get_image_mode1),
                             on_581a_calib_read, ssm);
      break;

    case SCAN_581A_ARM_TOUCH:
      fp_dbg ("581a: Arming FDT touch detection...");
      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN,
                            goodix_581a_fdt_down, sizeof (goodix_581a_fdt_down),
                            NULL, TRUE, 0, TRUE, on_581a_touch_detected, ssm);
      break;

    case SCAN_581A_CAPTURE:
      fp_dbg ("581a: Capturing Mode 5 image...");
      goodix_tls_read_image (dev, goodix_581a_get_image_mode5,
                             sizeof (goodix_581a_get_image_mode5),
                             on_581a_mode5_read, ssm);
      break;

    case SCAN_581A_AWAIT_UP:
      fp_dbg ("581a: Arming FDT finger release detection...");
      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP,
                            goodix_581a_fdt_up, sizeof (goodix_581a_fdt_up),
                            NULL, TRUE, 0, TRUE, on_581a_touch_up, ssm);
      break;
    }
}

static void
scan_581a_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (dev);
  self->scan_ssm = NULL;

  if (error)
    {
      if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        {
          fp_err ("581a scan failed: %s (code: %d)", error->message, error->code);
          fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
        }
      return;
    }

  fp_dbg ("581a scan cycle successfully finished");
}

static void
dev_581a_change_state (FpImageDevice *img_dev, FpiImageDeviceState state)
{
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (img_dev);
  FpDevice *dev = FP_DEVICE (img_dev);

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      fp_dbg ("581a entering AWAIT_FINGER_ON state");
      self->scan_ssm = fpi_ssm_new (dev, scan_581a_run_state, SCAN_581A_NUM_STATES);
      fpi_ssm_start (self->scan_ssm, scan_581a_complete);
    }
}

static void
dev_581a_deactivate (FpImageDevice *img_dev)
{
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (img_dev);
  FpDevice *dev = FP_DEVICE (img_dev);
  GError *error = NULL;

  /* Cancel active async work before releasing the baseline or shutting down
   * TLS because callbacks may still refer to the device state.
   */
  if (self->scan_ssm)
    {
      FpiSsm *ssm = self->scan_ssm;
      self->scan_ssm = NULL;
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_CANCELLED, "Device deactivated"));
    }

  goodix_reset_state (dev);
  goodix_shutdown_tls (dev, &error);
  g_clear_pointer (&self->calibration_img, g_free);
  g_clear_pointer (&self->calibration_sum, g_free);
  self->calibration_frame_count = 0;

  fpi_image_device_deactivate_complete (img_dev, error);
}

static void
fpi_device_goodixtls581a_init (FpiDeviceGoodixTls581a *self)
{
  self->calibration_img = NULL;
  self->calibration_sum = NULL;
  self->calibration_frame_count = 0;
  self->scan_ssm = NULL;
}

static void
fpi_device_goodixtls581a_finalize (GObject *object)
{
  FpiDeviceGoodixTls581a *self = FPI_DEVICE_GOODIXTLS581A (object);

  g_clear_pointer (&self->calibration_img, g_free);
  g_clear_pointer (&self->calibration_sum, g_free);

  G_OBJECT_CLASS (fpi_device_goodixtls581a_parent_class)->finalize (object);
}

static GoodixTls5xxMcuConfig
get_mcu_config (void)
{
  GoodixTls5xxMcuConfig cfg;

  cfg.free_fn = NULL;
  cfg.data = (guint8 *) goodix_581a_fdt_down;
  cfg.data_len = sizeof (goodix_581a_fdt_down);
  return cfg;
}

static FpImage *
process_frame (guint8 * frame)
{
  FpImage * img = fp_image_new (GOODIX_581A_WIDTH, GOODIX_581A_HEIGHT);

  img->flags |= FPI_IMAGE_PARTIAL;
  for (int y = 0; y != GOODIX_581A_HEIGHT; ++y)
    {
      for (int x = 0; x != GOODIX_581A_WIDTH; ++x)
        {
          const int stream_idx = y + x * GOODIX_581A_STREAM_ROWS;
          const int image_idx = x + y * GOODIX_581A_WIDTH;
          img->data[image_idx] = 255 - frame[stream_idx];
        }
    }
  return img;
}

static void
fpi_device_goodixtls581a_class_init (FpiDeviceGoodixTls581aClass * class)
{
  GObjectClass * gobject_class = G_OBJECT_CLASS (class);
  FpiDeviceGoodixTlsClass * gx_class = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass * dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass * img_dev_class = FP_IMAGE_DEVICE_CLASS (class);
  FpiDeviceGoodixTls5xxClass * xx_cls = FPI_DEVICE_GOODIXTLS5XX_CLASS (class);

  gobject_class->finalize = fpi_device_goodixtls581a_finalize;

  xx_cls->get_mcu_cfg = get_mcu_config;
  xx_cls->process_frame = process_frame;
  xx_cls->scan_height = GOODIX_581A_HEIGHT;
  xx_cls->scan_width = GOODIX_581A_WIDTH;
  xx_cls->raw_header_len = 0;
  xx_cls->raw_footer_len = 4;
  xx_cls->calib_img_payload = goodix_581a_get_image_mode1;
  xx_cls->calib_img_payload_len = sizeof (goodix_581a_get_image_mode1);
  xx_cls->get_img_payload = goodix_581a_get_image_mode5;
  xx_cls->get_img_payload_len = sizeof (goodix_581a_get_image_mode5);

  gx_class->interface = GOODIX_581A_INTERFACE;
  gx_class->ep_in = GOODIX_581A_EP_IN;
  gx_class->ep_out = GOODIX_581A_EP_OUT;

  dev_class->id = "goodixtls581a";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 581a (Xiaomi GM168)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 12;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_dev_class->bz3_threshold = 24;
  img_dev_class->img_width = GOODIX_581A_WIDTH;
  img_dev_class->img_height = GOODIX_581A_HEIGHT;
  img_dev_class->use_sigfm = TRUE;
  img_dev_class->activate = dev_activate;
  img_dev_class->deactivate = dev_581a_deactivate;
  img_dev_class->change_state = dev_581a_change_state;

  fpi_device_class_auto_initialize_features (dev_class);
}
