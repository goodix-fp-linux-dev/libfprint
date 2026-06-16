// Goodix Tls driver for libfprint
//
// Driver for the Goodix GF3206 ("MilanG") capacitive fingerprint sensor,
// USB id 27c6:5f10 (e.g. Honor MagicBook X16 Pro power-button reader).
//
// The sensor streams a 56x176 raw frame over TLS-PSK. Each scan row is 84
// bytes; the useful 54 pixels per row are 12-bit packed (thirteen 6-byte
// groups yielding four pixels each, plus a trailing 4-byte group yielding two
// pixels). The decoded rows are transposed to produce the final 176x54 image.
// Because the small, beaded ridges of this capacitive sensor yield unstable
// NBIS minutiae, matching uses the SIGFM (SIFT keypoint) algorithm instead.
//
// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Natasha England-Elbro <natasha@natashaee.me>
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "drivers/goodixtls/goodix5xx.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"

#define FP_COMPONENT "goodixtls5f10"

#include <glib.h>
#include <math.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix5f10.h"

// Image geometry. The sensor reports 56 columns per row but the two trailing
// "dummy" columns are dropped during unpacking, leaving 54 useful pixels.
#define GOODIX5F10_WIDTH 176
#define GOODIX5F10_HEIGHT 54
#define GOODIX5F10_ROW_STRIDE 84  // raw bytes per scan row
#define GOODIX5F10_ROW_PIXELS 54  // decoded pixels per scan row

struct _FpiDeviceGoodixTls5f10
{
  FpiDeviceGoodixTls parent;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls5f10, fpi_device_goodixtls5f10, FPI,
                      DEVICE_GOODIXTLS5F10, FpiDeviceGoodixTls5xx);

G_DEFINE_TYPE (FpiDeviceGoodixTls5f10, fpi_device_goodixtls5f10,
               FPI_TYPE_DEVICE_GOODIXTLS5XX);

// ---- ACTIVE SECTION START ----

static GoodixTls5xxMcuConfig get_mcu_config (void);
static GoodixTls5xxMcuConfig get_mcu_config_fdt_mode (void);

enum activate_states {
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_RESET,
  ACTIVATE_SET_MCU_IDLE,
  /* The Windows driver issues a second reset right before the config upload
   * (RESET, read_otp, RESET, upload_config). Mirroring that double reset is
   * what reliably clears an MCU left mid-FDT by a previously crashed TLS
   * session (e.g. an interrupted login-screen verify): a single reset/rebind
   * does not revive such a hung MCU, but the vendor's reset-before-upload
   * does. Harmless on a clean (warm) activation. */
  ACTIVATE_RESET2,
  ACTIVATE_UPLOAD_MCU_CONFIG,
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY,
  /* Warm up the finger-detect engine with a couple of throwaway fdt-mode/fdt-up
   * rounds (no finger needed; they return immediately). The very first fdt-down
   * after a cold activation is otherwise sluggish (~1.5s to register a press),
   * which makes the first touch of a session feel like it triggers on release.
   * Windows keeps the engine warm via the always-on power-button scan; we don't,
   * so we converge the per-cell baseline tracker here before the first capture. */
  ACTIVATE_FDT_WARM_MODE_1,
  ACTIVATE_FDT_WARM_UP_1,
  ACTIVATE_FDT_WARM_MODE_2,
  ACTIVATE_FDT_WARM_UP_2,
  ACTIVATE_NUM_STATES,
};

// The MCU accepts the config (status 0x01) only when the bytes are correct;
// a wrong/corrupt config is rejected with 0x00. Treat rejection as fatal so a
// bad config surfaces immediately instead of silently leaving the FDT engine
// unconfigured (which manifests later as "MCU has no config" / FDT timeouts).
static void
check_config_upload_5f10 (FpDevice *dev, gboolean success, gpointer ssm,
                          GError *error)
{
  if (error)
    fpi_ssm_mark_failed (ssm, error);
  else if (!success)
    fpi_ssm_mark_failed (ssm,
                         g_error_new (FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                      "MCU config upload rejected by sensor"));
  else
    fpi_ssm_next_state (ssm);
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_READ_AND_NOP:
      goodix_start_read_loop (dev);
      goodix_send_nop (dev, goodixtls5xx_check_none, ssm);
      break;

    case ACTIVATE_ENABLE_CHIP:
      goodix_send_enable_chip (dev, TRUE, goodixtls5xx_check_none, ssm);
      break;

    case ACTIVATE_NOP:
      goodix_send_nop (dev, goodixtls5xx_check_none, ssm);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_send_query_firmware_version (
        dev, goodixtls5xx_check_firmware_version, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read (dev, GOODIX_5F10_PSK_FLAGS, 0,
                                   goodixtls5xx_check_preset_psk_read, ssm);
      break;

    case ACTIVATE_RESET:
      goodix_send_reset (dev, TRUE, 20, goodixtls5xx_check_reset, ssm);
      break;

    case ACTIVATE_SET_MCU_IDLE:
      goodix_send_mcu_switch_to_idle_mode (dev, 20, goodixtls5xx_check_idle,
                                           ssm);
      break;

    case ACTIVATE_RESET2:
      goodix_send_reset (dev, TRUE, 20, goodixtls5xx_check_reset, ssm);
      break;

    case ACTIVATE_UPLOAD_MCU_CONFIG:
      goodix_send_upload_config_mcu (dev, goodix_5f10_config,
                                     sizeof (goodix_5f10_config), NULL,
                                     check_config_upload_5f10, ssm);
      break;

    case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
      goodix_send_set_powerdown_scan_frequency (
        dev, 100, goodixtls5xx_check_powerdown_scan_freq, ssm);
      break;

    case ACTIVATE_FDT_WARM_MODE_1:
    case ACTIVATE_FDT_WARM_MODE_2:
      {
        GoodixTls5xxMcuConfig cfg = get_mcu_config_fdt_mode ();
        goodix_send_mcu_switch_to_fdt_mode (dev, cfg.data, cfg.data_len,
                                            cfg.free_fn,
                                            goodixtls5xx_check_none_cmd, ssm);
      }
      break;

    case ACTIVATE_FDT_WARM_UP_1:
    case ACTIVATE_FDT_WARM_UP_2:
      {
        GoodixTls5xxMcuConfig cfg = get_mcu_config ();
        goodix_send_mcu_switch_to_fdt_up (dev, cfg.data, cfg.data_len,
                                          cfg.free_fn,
                                          goodixtls5xx_check_none_cmd, ssm);
      }
      break;
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  G_DEBUG_HERE ();
  if (!error)
    goodixtls5xx_init_tls (dev);
  else
    {
      fp_err ("failed during activation: %s (code: %d)", error->message,
              error->code);
      fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
    }
}

// ---- ACTIVE SECTION END ----

// ---- SCAN SECTION START ----

// FDT threshold templates: [selector] followed by [0x80, value] cells. With
// fdt_adaptive (see class_init) the value bytes are recomputed each scan from
// the live baseline; only the selector framing and cell count are used here.
// fdt-mode carries the 0x0d selector inline (the transport prepends 0x0c/0x0e
// for fdt-down/fdt-up but nothing for fdt-mode); the seed values are a cold-start
// baseline>>1 estimate, used only until the first reply lands.
static const guint8 fdt_switch_state_mode[] = {
  0x01, 0x80, 0xa0, 0x80, 0x93, 0x80, 0x9b, 0x80, 0x94, 0x80, 0x90,
  0x80, 0x8f, 0x80, 0x94, 0x80, 0x8b, 0x80, 0x8a, 0x80, 0x83,
};

static const guint8 fdt_switch_state_mode_primed[] = {
  0x0d, 0x01, 0x80, 0xa0, 0x80, 0x93, 0x80, 0x9b, 0x80, 0x94, 0x80, 0x90,
  0x80, 0x8f, 0x80, 0x94, 0x80, 0x8b, 0x80, 0x8a, 0x80, 0x83,
};

static const guint8 fdt_switch_state_down[] = {
  0x01, 0x80, 0xb9, 0x80, 0xb4, 0x80, 0xb5, 0x80, 0xaf, 0x80, 0xb4,
  0x80, 0xac, 0x80, 0xb2, 0x80, 0xa7, 0x80, 0xab, 0x80, 0xa5,
};

static GoodixTls5xxMcuConfig
get_mcu_config (void)
{
  GoodixTls5xxMcuConfig cfg;

  cfg.free_fn = NULL;
  cfg.data = fdt_switch_state_mode;
  cfg.data_len = sizeof (fdt_switch_state_mode);
  return cfg;
}

static GoodixTls5xxMcuConfig
get_mcu_config_fdt_down (void)
{
  GoodixTls5xxMcuConfig cfg;

  cfg.free_fn = NULL;
  cfg.data = fdt_switch_state_down;
  cfg.data_len = sizeof (fdt_switch_state_down);
  return cfg;
}

static GoodixTls5xxMcuConfig
get_mcu_config_fdt_mode (void)
{
  GoodixTls5xxMcuConfig cfg;

  cfg.free_fn = NULL;
  cfg.data = fdt_switch_state_mode_primed;
  cfg.data_len = sizeof (fdt_switch_state_mode_primed);
  return cfg;
}

// MilanG / GF3206 frame decode: per 84-byte scan row, 12-bit unpack the first
// 82 bytes into 54 pixels (13 groups of 6 bytes -> 4 px, then 4 bytes -> 2 px),
// writing transposed so the result is a 176-wide x 54-high image.
static void
decode_frame_5f10 (GoodixTls5xxPix *frame, guint32 frame_size,
                   const guint8 *raw_frame)
{
  const guint8 *body = raw_frame + 8;

  for (int r = 0; r < GOODIX5F10_WIDTH; ++r)
    {
      const guint8 *row = body + r * GOODIX5F10_ROW_STRIDE;
      int i = 0;
      int c = 0;

      while (c < GOODIX5F10_ROW_PIXELS)
        {
          if (c == GOODIX5F10_ROW_PIXELS - 2)
            {
              // trailing group: 4 bytes -> 2 pixels
              frame[r + (c + 0) * GOODIX5F10_WIDTH] =
                ((row[i + 0] & 0xf) << 8) + row[i + 1];
              frame[r + (c + 1) * GOODIX5F10_WIDTH] =
                (row[i + 3] << 4) + (row[i + 0] >> 4);
              c += 2;
              i += 4;
            }
          else
            {
              // 6 bytes -> 4 pixels
              frame[r + (c + 0) * GOODIX5F10_WIDTH] =
                ((row[i + 0] & 0xf) << 8) + row[i + 1];
              frame[r + (c + 1) * GOODIX5F10_WIDTH] =
                (row[i + 3] << 4) + (row[i + 0] >> 4);
              frame[r + (c + 2) * GOODIX5F10_WIDTH] =
                ((row[i + 5] & 0xf) << 8) + row[i + 2];
              frame[r + (c + 3) * GOODIX5F10_WIDTH] =
                (row[i + 4] << 4) + (row[i + 5] >> 4);
              c += 4;
              i += 6;
            }
        }
    }
}

// ---- SCAN SECTION END ----

// ---- DEV SECTION START ----

static void
dev_activate (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);

  fpi_ssm_start (fpi_ssm_new (dev, activate_run_state, ACTIVATE_NUM_STATES),
                 activate_complete);
}

// ---- DEV SECTION END ----

static FpImage *
process_frame (guint8 *squashed)
{
  FpImage *img = fp_image_new (GOODIX5F10_WIDTH, GOODIX5F10_HEIGHT);

  img->flags |= FPI_IMAGE_PARTIAL;
  memcpy (img->data, squashed, GOODIX5F10_WIDTH * GOODIX5F10_HEIGHT);
  return img;
}

// Box blur with radius r (edge-clamped), separable. Approximates a Gaussian.
static void
box_blur (const gfloat *src, gfloat *dst, guint w, guint h, int r)
{
  gfloat *tmp = g_malloc (w * h * sizeof (gfloat));

  for (guint y = 0; y < h; ++y)
    for (guint x = 0; x < w; ++x)
      {
        gfloat acc = 0; int cnt = 0;
        for (int dx = -r; dx <= r; ++dx)
          {
            int xx = (int) x + dx;
            if (xx < 0) xx = 0; else if (xx >= (int) w) xx = w - 1;
            acc += src[y * w + xx]; cnt++;
          }
        tmp[y * w + x] = acc / cnt;
      }
  for (guint y = 0; y < h; ++y)
    for (guint x = 0; x < w; ++x)
      {
        gfloat acc = 0; int cnt = 0;
        for (int dy = -r; dy <= r; ++dy)
          {
            int yy = (int) y + dy;
            if (yy < 0) yy = 0; else if (yy >= (int) h) yy = h - 1;
            acc += tmp[yy * w + x]; cnt++;
          }
        dst[y * w + x] = acc / cnt;
      }
  g_free (tmp);
}

// Turn the raw frame and the no-finger calibration frame into an 8-bit image
// for SIGFM (SIFT) matching.
static FpImage *
process_raw_5f10 (const GoodixTls5xxPix *raw, const GoodixTls5xxPix *calib,
                  guint w, guint h)
{
  const guint n = w * h;
  gfloat *flat = g_malloc (n * sizeof (gfloat));
  gfloat *lp = g_malloc (n * sizeof (gfloat));

  // 1) Flat-field by division: cancels the per-pixel multiplicative fixed
  //    pattern (vertical stripes) of the bare sensor, which would otherwise
  //    produce repeatable SIFT keypoints unrelated to the finger.
  for (guint i = 0; i < n; ++i)
    flat[i] = (gfloat) raw[i] / MAX (calib[i], 1);

  // 2) High-pass: subtract a low-pass (local background) to remove the slow
  //    pressure/vignette gradient and isolate the ridge-scale signal.
  box_blur (flat, lp, w, h, 3);

  gdouble sum = 0, sum2 = 0;
  for (guint i = 0; i < n; ++i)
    {
      flat[i] -= lp[i];
      sum += flat[i];
      sum2 += (gdouble) flat[i] * flat[i];
    }
  g_free (lp);

  // 3) Contrast stretch around the mean (±2 sigma). A finger ridge presses
  //    the sensor and lowers the signal, so low values map to dark ridges.
  gdouble mean = sum / n;
  gdouble var = sum2 / n - mean * mean;
  gdouble sd = var > 0 ? sqrt (var) : 1.0;
  gdouble lo = mean - 2.0 * sd, hi = mean + 2.0 * sd;
  if (hi <= lo)
    hi = lo + 1.0;

  FpImage *img = fp_image_new (w, h);
  img->flags |= FPI_IMAGE_PARTIAL;
  for (guint i = 0; i < n; ++i)
    {
      gdouble t = (flat[i] - lo) * 255.0 / (hi - lo);
      img->data[i] = t < 0 ? 0 : (t > 255 ? 255 : (guint8) t);
    }
  g_free (flat);
  return img;
}

static void
fpi_device_goodixtls5f10_init (FpiDeviceGoodixTls5f10 *self)
{
}

static void
fpi_device_goodixtls5f10_class_init (FpiDeviceGoodixTls5f10Class *class)
{
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS (class);
  FpiDeviceGoodixTls5xxClass *xx_cls = FPI_DEVICE_GOODIXTLS5XX_CLASS (class);

  xx_cls->get_mcu_cfg = get_mcu_config;
  xx_cls->get_mcu_cfg_fdt_down = get_mcu_config_fdt_down;
  xx_cls->get_mcu_cfg_fdt_mode = get_mcu_config_fdt_mode;
  // The GF3206 baseline drifts, so fixed thresholds mis-arm finger detection;
  // derive them live from the fdt-mode reply instead.
  xx_cls->fdt_adaptive = TRUE;
  xx_cls->process_frame = process_frame;
  xx_cls->decode_frame = decode_frame_5f10;
  xx_cls->process_raw_frame = process_raw_5f10;
  xx_cls->scan_height = GOODIX5F10_HEIGHT;
  xx_cls->scan_width = GOODIX5F10_WIDTH;
  xx_cls->psk = goodix_5f10_psk_0;
  xx_cls->psk_flags = GOODIX_5F10_PSK_FLAGS;
  xx_cls->psk_len = sizeof (goodix_5f10_psk_0);
  xx_cls->firmware_version = GOODIX_5F10_FIRMWARE_VERSION;
  xx_cls->reset_number = GOODIX_5F10_RESET_NUMBER;

  gx_class->interface = GOODIX_5F10_INTERFACE;
  gx_class->ep_in = GOODIX_5F10_EP_IN;
  gx_class->ep_out = GOODIX_5F10_EP_OUT;

  dev_class->id = "goodixtls5f10";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 5f10 (GF3206)";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table_5f10;
  dev_class->nr_enroll_stages = 20;

  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  /* This sensor produces small, beaded ridges on which NBIS minutiae are
   * unreliable; use SIGFM (SIFT keypoint) matching instead. bz3_threshold is
   * reused as the SIGFM matched-pair threshold. */
  img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM;
  img_dev_class->bz3_threshold = 24;
  img_dev_class->img_width = GOODIX5F10_WIDTH;
  img_dev_class->img_height = GOODIX5F10_HEIGHT;

  img_dev_class->activate = dev_activate;

  fpi_device_class_auto_initialize_features (dev_class);
}
