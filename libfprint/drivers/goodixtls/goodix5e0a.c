// Goodix Tls driver for libfprint — 5e0a device
//
// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Natasha England-Elbro <ashenglandelbro@protonmail.com>
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

#include "fp-device.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-context.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"

#define FP_COMPONENT "goodixtls5e0a"

#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/evp.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix5e0a.h"

// ---- PSK FILE PATH ----

#define GOODIX_5E0A_PSK_FILE "/etc/libfprint/goodix-5e0a.psk"

typedef unsigned short Goodix5e0aPix;

struct _FpiDeviceGoodixTls5e0a
{
  FpiDeviceGoodixTls parent;

  guint8 image_psk[32];
  gboolean has_image_psk;

  Goodix5e0aPix *calibration_img;  // baseline frame (no finger) for subtraction

  guint8 *prev_scan_data;   // previous scan raw data for diversity check
  guint16 prev_scan_size;

  guint8 fdt_down_payload[35];  // dynamically computed FDT_DOWN payload
  guint8 fdt_up_payload[35];    // dynamically computed FDT_UP payload
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls5e0a, fpi_device_goodixtls5e0a, FPI,
                      DEVICE_GOODIXTLS5E0A, FpiDeviceGoodixTls);

G_DEFINE_TYPE (FpiDeviceGoodixTls5e0a, fpi_device_goodixtls5e0a,
               FPI_TYPE_DEVICE_GOODIXTLS);

// Forward declarations
static void goodix_5e0a_decode_frame (Goodix5e0aPix *frame, guint32 raw_size, const guint8 *raw_frame);
static void on_calibration_image (FpDevice *dev, guint8 *data, guint16 len, gpointer user_data, GError *err) G_GNUC_UNUSED;

// ---- CALIBRATION ----

static void
on_calibration_image (FpDevice *dev, guint8 *data, guint16 len,
                      gpointer user_data, GError *err)
{
  FpiSsm *ssm = user_data;

  if (err)
    {
      fp_warn ("Calibration capture failed (non-fatal): %s", err->message);
      g_error_free (err);
      fpi_ssm_next_state (ssm);
      return;
    }

  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  if (len < GOODIX_5E0A_RAW_FRAME_SIZE)
    {
      fp_warn ("Calibration data too small: %d", len);
      fpi_ssm_next_state (ssm);
      return;
    }

  if (!self->calibration_img)
    self->calibration_img = calloc (GOODIX_5E0A_FRAME_SIZE,
                                    sizeof (Goodix5e0aPix));

  goodix_5e0a_decode_frame (self->calibration_img,
                            GOODIX_5E0A_RAW_FRAME_SIZE, data);
  fp_dbg ("Calibration frame captured (%d pixels)", GOODIX_5E0A_FRAME_SIZE);
  fpi_ssm_next_state (ssm);
}

G_GNUC_UNUSED static void
linear_subtract_5e0a (Goodix5e0aPix *src, const Goodix5e0aPix *baseline,
                      guint16 len)
{
  // Subtract baseline from scan: result = clamp(scan - baseline, 0, max)
  // This removes fixed pattern noise
  for (guint16 i = 0; i < len; i++)
    {
      if (src[i] > baseline[i])
        src[i] = src[i] - baseline[i];
      else
        src[i] = 0;
    }
}

// ---- HELPERS ----

static void
check_none_5e0a (FpDevice *dev, gpointer user_data, GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (user_data, error);
      return;
    }
  fpi_ssm_next_state (user_data);
}

static void
check_none_cmd_5e0a (FpDevice *dev, guint8 *data, guint16 len,
                     gpointer ssm, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  fpi_ssm_next_state (ssm);
}

// Compute SHA-256 of data into out (must be 32 bytes)
static gboolean
compute_sha256 (const guint8 *data, gsize len, guint8 *out)
{
  EVP_MD_CTX *mdctx = EVP_MD_CTX_new ();
  if (!mdctx) return FALSE;

  unsigned int md_len = 32;
  gboolean ok = EVP_DigestInit_ex (mdctx, EVP_sha256 (), NULL) &&
                EVP_DigestUpdate (mdctx, data, len) &&
                EVP_DigestFinal_ex (mdctx, out, &md_len);
  EVP_MD_CTX_free (mdctx);
  return ok;
}

// ---- FRAME DECODE ----

// Decode the 4/6 byte packing used by Goodix sensors.
// The 5e0a raw frame has NO 8-byte header / 5-byte footer (unlike 5110).
static void
goodix_5e0a_decode_frame (Goodix5e0aPix *frame, guint32 raw_size,
                          const guint8 *raw_frame)
{
  Goodix5e0aPix *pix = frame;

  for (guint32 i = 0; i < raw_size; i += 6)
    {
      const guint8 *chunk = raw_frame + i;
      *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
      *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
      *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
      *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
    }
}

// Compare function for qsort of Goodix5e0aPix values
static int
cmp_pix (const void *a, const void *b)
{
  Goodix5e0aPix pa = *(const Goodix5e0aPix *) a;
  Goodix5e0aPix pb = *(const Goodix5e0aPix *) b;
  return (pa > pb) - (pa < pb);
}

static void
goodix_5e0a_squash_frame (Goodix5e0aPix *frame, guint8 *squashed,
                          guint16 frame_size)
{
  // Elan-style "thirds" normalization for small sensors.
  // Instead of simple linear min/max scaling, we divide pixels into 3 groups
  // by intensity percentile and map each group to a different output range.
  // This preserves more contrast in the ridge/valley boundary region where
  // most fingerprint detail lives.
  //
  // First, collect only active (non-dead-zone) pixels for percentile calc.
  Goodix5e0aPix *active = g_malloc (frame_size * sizeof (Goodix5e0aPix));
  int n_active = 0;

  for (int i = 0; i < frame_size; i++)
    {
      if (frame[i] != 0)
        active[n_active++] = frame[i];
    }

  if (n_active < 4)
    {
      // Degenerate case: nearly all dead pixels
      memset (squashed, 0xff, frame_size);
      g_free (active);
      return;
    }

  qsort (active, n_active, sizeof (Goodix5e0aPix), cmp_pix);

  // Percentile boundaries (same as elan driver):
  //   lvl0 = minimum,  lvl1 = 30th percentile,
  //   lvl2 = 65th percentile,  lvl3 = maximum
  Goodix5e0aPix lvl0 = active[0];
  Goodix5e0aPix lvl1 = active[n_active * 3 / 10];
  Goodix5e0aPix lvl2 = active[n_active * 65 / 100];
  Goodix5e0aPix lvl3 = active[n_active - 1];

  g_free (active);

  // Avoid division by zero on flat regions
  if (lvl1 == lvl0) lvl1 = lvl0 + 1;
  if (lvl2 == lvl1) lvl2 = lvl1 + 1;
  if (lvl3 == lvl2) lvl3 = lvl2 + 1;

  for (int i = 0; i < frame_size; i++)
    {
      if (frame[i] == 0)
        {
          // Dead zone sentinel — NBIS treats white as background
          squashed[i] = 0xff;
          continue;
        }

      Goodix5e0aPix px = frame[i];
      guint32 val;

      // Map to 3 output ranges: [0-99], [99-155], [155-254]
      // The middle range (ridges/valleys boundary) gets compressed,
      // giving more contrast to the extremes where detail matters.
      if (px < lvl1)
        val = (px - lvl0) * 99 / (lvl1 - lvl0);
      else if (px < lvl2)
        val = 99 + (px - lvl1) * 56 / (lvl2 - lvl1);
      else
        val = 155 + (px - lvl2) * 99 / (lvl3 - lvl2);

      if (val > 254)
        val = 254;
      squashed[i] = (guint8) val;
    }
}

// Unsharp mask filter to enhance ridge contrast for NBIS minutiae detection.
// The sensor produces low-contrast images; sharpening makes ridges and valleys
// more distinct so NBIS can reliably detect minutiae.
static void
goodix_5e0a_unsharp_mask (guint8 *out, const guint8 *in,
                          int width, int height,
                          int radius, double strength)
{
  for (int y = 0; y < height; y++)
    {
      for (int x = 0; x < width; x++)
        {
          int idx = y * width + x;
          if (in[idx] == 0xff)
            {
              out[idx] = 0xff;
              continue;
            }

          // Box blur in the neighborhood, excluding dead zone pixels
          int sum = 0, cnt = 0;
          for (int dy = -radius; dy <= radius; dy++)
            {
              int ny = y + dy;
              if (ny < 0 || ny >= height)
                continue;
              for (int dx = -radius; dx <= radius; dx++)
                {
                  int nx = x + dx;
                  if (nx < 0 || nx >= width)
                    continue;
                  guint8 px = in[ny * width + nx];
                  if (px != 0xff)
                    {
                      sum += px;
                      cnt++;
                    }
                }
            }

          if (cnt == 0)
            {
              out[idx] = in[idx];
              continue;
            }

          double blur = (double) sum / cnt;
          double sharp = in[idx] + strength * (in[idx] - blur);
          if (sharp < 0.0)
            sharp = 0.0;
          if (sharp > 254.0)
            sharp = 254.0;
          out[idx] = (guint8) sharp;
        }
    }
}

// ---- ACTIVATION STATE MACHINE ----

enum activate_5e0a_states {
  ACTIVATE_READ_AND_NOP1,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP2,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_CMD_TLS,
  ACTIVATE_POV_IMAGE_CHECK,
  ACTIVATE_IMG_TLS,
  ACTIVATE_CALIBRATE,
  ACTIVATE_DONE,

  ACTIVATE_5E0A_NUM_STATES,
};

static void
on_psk_read_5e0a (FpDevice *dev, gboolean success, guint32 flags,
                  guint8 *device_hash, guint16 length, gpointer user_data,
                  GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  if (error)
    {
      fp_warn ("PSK read error: %s", error->message);
      g_error_free (error);
      // Continue anyway — image TLS may still work or fail gracefully
      fpi_ssm_next_state (ssm);
      return;
    }

  if (!success || length < 32)
    {
      fp_warn ("PSK read returned failure or short hash (len=%d)", length);
      fpi_ssm_next_state (ssm);
      return;
    }

  g_autofree gchar *hash_str = data_to_str (device_hash, length);
  fp_dbg ("Device PSK hash: 0x%s (flags: 0x%08x)", hash_str, flags);

  // If we have a PSK loaded from file, verify it matches the device hash
  if (self->has_image_psk)
    {
      guint8 expected_hash[32];
      if (compute_sha256 (self->image_psk, 32, expected_hash) &&
          memcmp (device_hash, expected_hash, 32) == 0)
        {
          fp_info ("PSK verified successfully");
          fpi_ssm_next_state (ssm);
          return;
        }

      fp_warn ("PSK file does NOT match device hash — will not erase device");
      fp_warn ("Run extract_psk.py or whitebox_encrypt.py to set up PSK manually");
      fp_warn ("See https://github.com/carminezac/libfprint-goodix for instructions");
    }
  else
    {
      fp_warn ("No valid PSK — run extract_psk.py or whitebox_encrypt.py to set up PSK");
      fp_warn ("See https://github.com/carminezac/libfprint-goodix for instructions");
    }

  fpi_ssm_next_state (ssm);
}

static void
on_cmd_tls_complete (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fp_err ("Command TLS init failed: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fp_dbg ("Command TLS established");
  fpi_ssm_next_state (ssm);
}

static void
on_pov_image_check_done (FpDevice *dev, guint8 *data, guint16 len,
                         gpointer ssm, GError *err)
{
  if (err)
    {
      fp_warn ("POV image check failed: %s — continuing anyway", err->message);
      g_error_free (err);
    }
  else
    {
      fp_dbg ("POV image check OK");
    }
  fpi_ssm_next_state (ssm);
}

static void
on_img_tls_complete (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fp_err ("Image TLS init failed: %s", error->message);
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fp_dbg ("Image TLS established");
  fpi_ssm_next_state (ssm);
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_READ_AND_NOP1:
      goodix_start_read_loop (dev);
      goodix_send_nop (dev, check_none_5e0a, ssm);
      break;

    case ACTIVATE_ENABLE_CHIP:
      goodix_send_enable_chip (dev, TRUE, check_none_5e0a, ssm);
      break;

    case ACTIVATE_NOP2:
      goodix_send_nop (dev, check_none_5e0a, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read (dev, GOODIX_5E0A_PSK_FLAGS, 0,
                                   on_psk_read_5e0a, ssm);
      break;

    case ACTIVATE_CMD_TLS:
      // Command TLS with PSK = 32 zero bytes (default)
      goodix_tls_init (dev, on_cmd_tls_complete, ssm);
      break;

    case ACTIVATE_POV_IMAGE_CHECK:
      goodix_send_pov_image_check (dev, on_pov_image_check_done, ssm);
      break;

    case ACTIVATE_IMG_TLS:
      {
        // Image TLS with the device-specific PSK.
        // After ACTIVATE_CHECK_PSK, self->image_psk should be populated
        // either from file or from auto-enrollment.
        const guint8 *psk = self->image_psk;
        guint psk_len = 32;

        if (!self->has_image_psk)
          {
            fp_warn ("No image PSK available — image TLS will likely fail. "
                     "Run extract_psk.py or whitebox_encrypt.py to set up PSK.");
          }
        else
          {
            fp_info ("Starting image TLS with PSK from file");
          }

        goodix_tls_init_image (dev, psk, psk_len, on_img_tls_complete, ssm);
      }
      break;

    case ACTIVATE_CALIBRATE:
      // Skip calibration during init — GET_IMAGE without finger causes timeout.
      // The 5e0a sensor requires a finger to be present for image capture.
      // Calibration would need to be done after the first FDT cycle.
      fpi_ssm_next_state (ssm);
      break;

    case ACTIVATE_DONE:
      fpi_ssm_next_state (ssm);
      break;
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  G_DEBUG_HERE ();
  if (error)
    {
      fp_err ("Failed during 5e0a activation: %s (code: %d)",
              error->message, error->code);
    }
  fpi_image_device_activate_complete (FP_IMAGE_DEVICE (dev), error);
}

// ---- SCAN STATE MACHINE ----

enum scan_5e0a_states {
  SCAN_QUERY_MCU,
  SCAN_READ_FDT_BASE,
  SCAN_FDT_DOWN,
  SCAN_GET_IMAGE,
  SCAN_FDT_UP,
  SCAN_DONE,

  SCAN_5E0A_NUM_STATES,
};

static void
scan_on_read_img_5e0a (FpDevice *dev, guint8 *data, guint16 len,
                       gpointer ssm, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }

  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);

  fp_dbg ("Got decrypted image data: %d bytes", len);

  // The decrypted data is the raw frame (10560 bytes) followed by a
  // 4-byte footer/checksum. Use only the first RAW_FRAME_SIZE bytes.
  if (len < GOODIX_5E0A_RAW_FRAME_SIZE)
    fp_warn ("Image data too small: %d < %d", len, GOODIX_5E0A_RAW_FRAME_SIZE);

  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);

  Goodix5e0aPix *raw_frame = calloc (GOODIX_5E0A_FRAME_SIZE,
                                     sizeof (Goodix5e0aPix));
  goodix_5e0a_decode_frame (raw_frame, GOODIX_5E0A_RAW_FRAME_SIZE, data);

  // Apply calibration: biased subtraction (same formula as 511 driver)
  if (self->calibration_img)
    {
      for (int i = 0; i < GOODIX_5E0A_FRAME_SIZE; i++)
        {
          guint16 max = 0xFFFF;
          gint32 val = max - ((max - raw_frame[i]) - (max - self->calibration_img[i]));
          raw_frame[i] = (Goodix5e0aPix) CLAMP (val, 0, max);
        }
    }

  guint8 *squashed = calloc (GOODIX_5E0A_FRAME_SIZE, 1);
  goodix_5e0a_squash_frame (raw_frame, squashed, GOODIX_5E0A_FRAME_SIZE);
  free (raw_frame);

  // The sensor frame is GOODIX_5E0A_WIDTH (80) columns x GOODIX_5E0A_HEIGHT
  // (88) rows. The scan line width is 80 pixels, with 88 rows.
  int img_w = GOODIX_5E0A_WIDTH;   // 80
  int img_h = GOODIX_5E0A_HEIGHT;  // 88

  // Apply unsharp mask to enhance ridge/valley contrast.
  // The sensor produces low-contrast images; NBIS needs good ridge definition
  // to detect minutiae. Empirically, radius=3, strength=4.0 yields the most
  // minutiae (8 vs 3 without sharpening on test images).
  guint8 *sharpened = calloc (GOODIX_5E0A_FRAME_SIZE, 1);
  goodix_5e0a_unsharp_mask (sharpened, squashed, img_w, img_h, 3, 4.0);

  // Create the FpImage with the correct orientation.
  // Do NOT invert colors (empirically reduces minutiae to 0).
  // Do NOT fill dead zone with average (reduces minutiae to 0).
  // Keep 0xFF dead zone — NBIS handles white background well.
  FpImage *img = fp_image_new (img_w, img_h);
  img->ppmm = 19.685;  // 500 DPI
  // FPI_IMAGE_PARTIAL is critical for small sensors:
  // 1. Enables remove_perimeter_pts in NBIS — removes false minutiae at
  //    image borders that would otherwise poison matching.
  // 2. Signals to the matching pipeline that this is a partial impression,
  //    so border artifacts should be discounted.
  img->flags |= FPI_IMAGE_PARTIAL;
  memcpy (img->data, sharpened, GOODIX_5E0A_FRAME_SIZE);

  free (squashed);
  free (sharpened);

  // Upscale 2x — critical for small sensors. Makes the image large enough
  // for NBIS to reliably detect minutiae. Used by aes3k, egis0570, elanspi.
  FpImage *resized = fpi_image_resize (img, 2, 2);
  g_object_unref (img);

  // Enrollment diversity check — compare DECODED pixels, not raw TLS data.
  // Raw TLS data is always different (different encryption nonces), so we
  // must compare the squashed pixel data after image processing.
  guint16 img_size = resized->width * resized->height;
  if (self->prev_scan_data && self->prev_scan_size == img_size)
    {
      int same = 0;
      for (guint16 i = 0; i < img_size; i++)
        if (abs ((int)resized->data[i] - (int)self->prev_scan_data[i]) < 10)
          same++;
      int pct = same * 100 / img_size;
      fp_dbg ("Diversity check: %d%% similar to previous scan", pct);
      if (pct > 90)
        {
          fp_dbg ("Scan too similar (%d%%), rejecting — lift and reposition finger", pct);
          g_object_unref (resized);
          // Go to FDT_UP first so user must lift finger before retrying
          fpi_ssm_jump_to_state (ssm, SCAN_FDT_UP);
          return;
        }
    }
  // Save decoded pixels for next comparison
  g_free (self->prev_scan_data);
  self->prev_scan_data = g_memdup2 (resized->data, img_size);
  self->prev_scan_size = img_size;

  fpi_image_device_image_captured (img_dev, resized);
  fpi_ssm_next_state (ssm);
}

static void
on_query_mcu_ack_5e0a (FpDevice *dev, gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fp_warn ("query_mcu_state ACK error (non-fatal): %s", error->message);
      g_error_free (error);
    }
  fpi_ssm_next_state (ssm);
}

// DAC base values from OTP (used in FDT payloads)
static const guint8 dac_base[] = {
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00
};

// FDT_MANUAL payload to read current sensor baseline
static const guint8 fdt_manual_payload[] = {
  0x0D, 0x01,
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00,
  0x00, 0x00, 0x00, 0x00
};

static void
build_fdt_payload (guint8 *payload, guint8 prefix, const guint16 *thresholds)
{
  payload[0] = prefix;
  payload[1] = 0x01;  // has_base flag
  // bytes [2..9]: DAC base from OTP
  memcpy (payload + 2, dac_base, 8);
  // bytes [10..21]: 6 threshold words (LE)
  for (int i = 0; i < 6; i++)
    {
      payload[10 + i * 2]     = thresholds[i] & 0xFF;
      payload[10 + i * 2 + 1] = (thresholds[i] >> 8) & 0xFF;
    }
  // bytes [22..25]: padding
  memset (payload + 22, 0, 4);
  // bytes [26..33]: DAC base copy
  memcpy (payload + 26, dac_base, 8);
  // byte [34]: trailing zero
  payload[34] = 0x00;
}

static void
on_fdt_manual_response (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fp_warn ("FDT_MANUAL failed: %s — using static thresholds", error->message);
      g_error_free (error);
      // Fall back to static payload
      FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
      memcpy (self->fdt_down_payload, goodix_5e0a_fdt_down_mode,
              sizeof (goodix_5e0a_fdt_down_mode));
      memcpy (self->fdt_up_payload, goodix_5e0a_fdt_up_mode,
              sizeof (goodix_5e0a_fdt_up_mode));
      fpi_ssm_next_state (ssm);
      return;
    }

  // Response inner data format: [4 bytes header] [12 bytes raw base]
  // Header: [status, irq_type, touch_flag_lo, touch_flag_hi]
  // Raw base: 6 zones x uint16_t LE
  if (length < 16)
    {
      fp_warn ("FDT_MANUAL response too short (%d bytes) — using static thresholds",
               length);
      FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
      memcpy (self->fdt_down_payload, goodix_5e0a_fdt_down_mode,
              sizeof (goodix_5e0a_fdt_down_mode));
      memcpy (self->fdt_up_payload, goodix_5e0a_fdt_up_mode,
              sizeof (goodix_5e0a_fdt_up_mode));
      fpi_ssm_next_state (ssm);
      return;
    }

  // Parse the 6 raw base values from bytes [4..15]
  guint16 raw_base[6];
  for (int i = 0; i < 6; i++)
    raw_base[i] = data[4 + i * 2] | (data[4 + i * 2 + 1] << 8);

  // Compute thresholds: threshold[i] = ((raw_base[i] >> 1) << 8) | 0x80
  guint16 thresholds[6];
  fp_dbg ("FDT base readings:");
  for (int i = 0; i < 6; i++)
    {
      thresholds[i] = ((raw_base[i] >> 1) << 8) | 0x80;
      fp_dbg ("  zone %d: raw=0x%04x threshold=0x%04x", i, raw_base[i], thresholds[i]);
    }

  // Build dynamic FDT_DOWN and FDT_UP payloads
  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  build_fdt_payload (self->fdt_down_payload, 0x1c, thresholds);
  build_fdt_payload (self->fdt_up_payload, 0x0e, thresholds);

  fp_dbg ("Built dynamic FDT payloads from sensor baseline");
  fpi_ssm_next_state (ssm);
}

static void
scan_run_state (FpiSsm *ssm, FpDevice *dev)
{
  FpImageDevice *img_dev = FP_IMAGE_DEVICE (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case SCAN_QUERY_MCU:
      {
        // 5e0a: send query_mcu_state with reply=FALSE (fire-and-forget).
        // The Python driver sends b"\x00\x01\x00" but the device only
        // needs the ACK, not a data reply.
        GoodixCallbackInfo *cb_info = malloc (sizeof (GoodixCallbackInfo));
        cb_info->callback = G_CALLBACK (on_query_mcu_ack_5e0a);
        cb_info->user_data = ssm;
        guint8 payload[] = { 0x55 };
        goodix_send_protocol (dev, GOODIX_CMD_QUERY_MCU_STATE, payload,
                              sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT,
                              FALSE, goodix_receive_none, cb_info);
      }
      break;

    case SCAN_READ_FDT_BASE:
      fp_dbg ("Reading FDT baseline (FDT_MANUAL cmd 0x36)...");
      {
        GoodixCallbackInfo *cb_info = malloc (sizeof (GoodixCallbackInfo));
        cb_info->callback = G_CALLBACK (on_fdt_manual_response);
        cb_info->user_data = ssm;
        goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE,
                              fdt_manual_payload,
                              sizeof (fdt_manual_payload),
                              NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                              goodix_receive_default, cb_info);
      }
      break;

    case SCAN_FDT_DOWN:
      fp_dbg ("Waiting for finger (FDT_DOWN) with dynamic thresholds...");
      {
        FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
        GoodixCallbackInfo *cb_info = malloc (sizeof (GoodixCallbackInfo));
        cb_info->callback = G_CALLBACK (check_none_cmd_5e0a);
        cb_info->user_data = ssm;
        goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN,
                              self->fdt_down_payload,
                              sizeof (self->fdt_down_payload),
                              NULL, TRUE, 0, TRUE,
                              goodix_receive_default, cb_info);
      }
      break;

    case SCAN_GET_IMAGE:
      fpi_image_device_report_finger_status (img_dev, TRUE);
      goodix_tls_read_image_5e0a_with_payload (
        dev,
        goodix_5e0a_get_image_payload,
        sizeof (goodix_5e0a_get_image_payload),
        scan_on_read_img_5e0a, ssm);
      break;

    case SCAN_FDT_UP:
      // Wait for finger lift using FDT_UP (0x34) with dynamic thresholds.
      // The device responds when the finger is lifted (interrupt=0x200).
      fp_dbg ("Waiting for finger lift (FDT_UP cmd 0x34) with dynamic thresholds...");
      {
        FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
        GoodixCallbackInfo *cb_info = malloc (sizeof (GoodixCallbackInfo));
        cb_info->callback = G_CALLBACK (check_none_cmd_5e0a);
        cb_info->user_data = ssm;
        goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP,
                              self->fdt_up_payload,
                              sizeof (self->fdt_up_payload),
                              NULL, TRUE, 0, TRUE,
                              goodix_receive_default, cb_info);
      }
      break;

    case SCAN_DONE:
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
      fp_err ("Failed to scan: %s (code: %d)", error->message, error->code);
      fpi_image_device_session_error (FP_IMAGE_DEVICE (dev), error);
      return;
    }
  fp_dbg ("5e0a scan finished");
}

// ---- DEVICE CALLBACKS ----

static void
dev_activate (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);

  fpi_ssm_start (fpi_ssm_new (dev, activate_run_state,
                               ACTIVATE_5E0A_NUM_STATES),
                 activate_complete);
}

static void
dev_change_state (FpImageDevice *img_dev, FpiImageDeviceState state)
{
  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      fpi_ssm_start (fpi_ssm_new (FP_DEVICE (img_dev), scan_run_state,
                                   SCAN_5E0A_NUM_STATES),
                     scan_complete);
    }
}

static void
dev_deactivate (FpImageDevice *img_dev)
{
  FpDevice *dev = FP_DEVICE (img_dev);

  goodix_reset_state (dev);

  FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A (dev);
  g_clear_pointer (&self->calibration_img, free);
  g_clear_pointer (&self->prev_scan_data, g_free);
  self->prev_scan_size = 0;

  GError *error = NULL;
  goodix_shutdown_tls (dev, &error);
  goodix_shutdown_image_tls (dev, &error);

  fpi_image_device_deactivate_complete (img_dev, error);
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
dev_deinit (FpImageDevice *img_dev)
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

// ---- TYPE INIT ----

static gboolean
load_psk_from_file (FpiDeviceGoodixTls5e0a *self)
{
  // Try multiple config file locations
  const char *paths[] = {
    "/etc/libfprint/goodix-5e0a.psk",
    NULL,   // filled with $HOME path below
  };

  // Build home-based path
  const char *home = g_get_home_dir ();
  g_autofree char *home_path = NULL;
  if (home)
    {
      home_path = g_strdup_printf ("%s/.config/libfprint/goodix-5e0a.psk", home);
      paths[1] = home_path;
    }

  for (int i = 0; i < 2; i++)
    {
      if (!paths[i])
        continue;

      g_autofree char *contents = NULL;
      gsize len = 0;

      if (!g_file_get_contents (paths[i], &contents, &len, NULL))
        continue;

      // Strip whitespace/newlines
      g_strstrip (contents);
      gsize hex_len = strlen (contents);

      if (hex_len < 64)
        {
          fp_warn ("PSK file %s too short (%zu chars, need 64 hex)", paths[i], hex_len);
          continue;
        }

      // Parse hex string to bytes
      gboolean valid = TRUE;
      for (int j = 0; j < 32 && valid; j++)
        {
          char byte_str[3] = { contents[j*2], contents[j*2+1], 0 };
          char *endp;
          unsigned long val = strtoul (byte_str, &endp, 16);
          if (*endp != 0)
            {
              valid = FALSE;
              break;
            }
          self->image_psk[j] = (guint8) val;
        }

      if (valid)
        {
          fp_info ("Loaded image PSK from %s", paths[i]);
          self->has_image_psk = TRUE;
          return TRUE;
        }
      else
        {
          fp_warn ("Invalid hex in PSK file %s", paths[i]);
        }
    }

  return FALSE;
}

static void
fpi_device_goodixtls5e0a_init (FpiDeviceGoodixTls5e0a *self)
{
  memset (self->image_psk, 0, sizeof (self->image_psk));
  self->has_image_psk = FALSE;

  // Load device-specific PSK from config file.
  // The PSK is unique per device, extracted via extract_psk.py.
  // Store it as 64 hex chars in one of:
  //   /etc/libfprint/goodix-5e0a.psk
  //   ~/.config/libfprint/goodix-5e0a.psk
  if (!load_psk_from_file (self))
    fp_warn ("No PSK file found — run extract_psk.py or whitebox_encrypt.py to set up PSK");
}

static void
fpi_device_goodixtls5e0a_class_init (FpiDeviceGoodixTls5e0aClass *class)
{
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS (class);

  gx_class->interface = GOODIX_5E0A_INTERFACE;
  gx_class->ep_in = GOODIX_5E0A_EP_IN;
  gx_class->ep_out = GOODIX_5E0A_EP_OUT;

  dev_class->id = "goodixtls5e0a";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 5e0a";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = goodix_5e0a_id_table;
  // 8 enrollment stages: enough captures for bozorth3 to build a reliable
  // template from a small sensor, without exhausting user patience.
  // Each stage captures one image; the NBIS matcher compares against all
  // stored prints in the template, so more stages = more reference angles.
  dev_class->nr_enroll_stages = 20;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  // bz3_threshold: match score threshold for bozorth3 matcher.
  // Small sensors (80x88) produce few minutiae (typically 5-12), so the
  // match scores are inherently lower than full-size sensors.
  // Default libfprint value is 40; we use 20 to balance security vs usability.
  // Too high (>25) = constant false rejections with so few minutiae.
  // Too low (<12) = potential false accepts.
  img_dev_class->bz3_threshold = 20;
  img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM;
  // Sensor frame: 80 pixels wide, 88 pixels tall
  img_dev_class->img_width = GOODIX_5E0A_WIDTH;    // 80
  img_dev_class->img_height = GOODIX_5E0A_HEIGHT;  // 88

  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;
  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;

  fpi_device_class_auto_initialize_features (dev_class);
}
