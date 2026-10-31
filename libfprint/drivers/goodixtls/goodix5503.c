// Goodix Tls driver for libfprint - 27c6:5503
//
// Based on goodix511.c by Alexander Meiler, Matthieu CHARETTE and
// Natasha England-Elbro, and on the 5503 protocol work in goodix-fp-dump.

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

#define FP_COMPONENT "goodixtls5503"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "drivers/goodixtls/goodix5xx.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix5503.h"

#define GOODIX5503_WIDTH 64
#define GOODIX5503_HEIGHT 80

struct _FpiDeviceGoodixTls5503
{
  FpiDeviceGoodixTls5xx parent;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls5503, fpi_device_goodixtls5503, FPI,
                      DEVICE_GOODIXTLS5503, FpiDeviceGoodixTls5xx);

G_DEFINE_TYPE (FpiDeviceGoodixTls5503, fpi_device_goodixtls5503,
               FPI_TYPE_DEVICE_GOODIXTLS5XX);

// ---- ACTIVATE SECTION START ----

// Mirrors driver_5503.py run_driver(): these states run before the TLS
// handshake, post_tls_states run after it (the 5503 rejects image requests
// when the MCU config is uploaded before TLS / TLS_SUCCESSFULLY_ESTABLISHED
// is sent).
enum activate_states {
  ACTIVATE_FLUSH,
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_RESET,
  ACTIVATE_NOP,
  ACTIVATE_READ_OTP,
  ACTIVATE_POV_IMAGE_CHECK,
  ACTIVATE_NUM_STATES,
};

enum post_tls_states {
  POST_TLS_UPLOAD_MCU_CONFIG,
  POST_TLS_SET_DRV_STATE_1,
  POST_TLS_SET_DRV_STATE_2,
  POST_TLS_GET_POV_IMAGE,
  POST_TLS_NUM_STATES,
};

// reset_sensor | reset_sensor << 2 (as sent by goodix-fp-dump), sleep 20 ms
static const guint8 reset_payload[] = {0x05, 0x14};
static const guint8 zero_payload[] = {0x00, 0x00};
static const guint8 drv_state_payload[] = {0x01, 0x00};

static void
check_reply (FpDevice *dev, guint8 *data, guint16 len, gpointer ssm,
             GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  if (data && len > 0)
    {
      g_autofree gchar *str = data_to_str (data, MIN (len, 16));
      fp_dbg ("state %d reply (%u bytes): %s%s", fpi_ssm_get_cur_state (ssm),
              len, str, len > 16 ? "..." : "");
    }
  fpi_ssm_next_state (ssm);
}

// The sensor reports GF3208_... with the factory key and GF3258_... once it
// has been re-keyed; goodix-fp-dump accepts both ("GF32[0,5]8_RTSEC_APP_10062").
static void
check_firmware_version (FpDevice *dev, gchar *firmware, gpointer ssm,
                        GError *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("Device firmware: \"%s\"", firmware);

  for (guint i = 0; i < G_N_ELEMENTS (goodix_5503_firmware_versions); i++)
    if (g_strcmp0 (firmware, goodix_5503_firmware_versions[i]) == 0)
      {
        fpi_ssm_next_state (ssm);
        return;
      }

  fpi_ssm_mark_failed (ssm,
                       g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Unsupported device firmware: \"%s\"",
                                    firmware));
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_FLUSH:
      // Like goodix-fp-dump's empty_buffer(): the sensor may still hold an
      // unsolicited reply (e.g. FDT_UP 0x34) from the previous session. Start
      // reading and let it arrive (it gets dropped as unexpected) before the
      // first real command.
      goodix_start_read_loop (dev);
      fpi_ssm_next_state_delayed (ssm, 100);
      break;

    case ACTIVATE_READ_AND_NOP:
      goodix_send_nop (dev, goodixtls5xx_check_none, ssm);
      break;

    case ACTIVATE_CHECK_FW_VER:
      goodix_send_query_firmware_version (dev, check_firmware_version, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read (dev, GOODIX_5503_PSK_FLAGS, 0,
                                   goodixtls5xx_check_preset_psk_read, ssm);
      break;

    case ACTIVATE_RESET:
      goodix_send_raw_cmd (dev, GOODIX_CMD_RESET, reset_payload,
                           sizeof (reset_payload), TRUE, check_reply, ssm);
      break;

    case ACTIVATE_NOP:
      goodix_send_nop (dev, goodixtls5xx_check_none, ssm);
      break;

    case ACTIVATE_READ_OTP:
      goodix_send_read_otp (dev, check_reply, ssm);
      break;

    case ACTIVATE_POV_IMAGE_CHECK:
      goodix_send_raw_cmd (dev, GOODIX_CMD_POV_IMAGE_CHECK, zero_payload,
                           sizeof (zero_payload), TRUE, check_reply, ssm);
      break;
    }
}

static void
post_tls_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case POST_TLS_UPLOAD_MCU_CONFIG:
      goodix_send_upload_config_mcu (dev, goodix_5503_config,
                                     sizeof (goodix_5503_config), NULL,
                                     goodixtls5xx_check_config_upload, ssm);
      break;

    case POST_TLS_SET_DRV_STATE_1:
    case POST_TLS_SET_DRV_STATE_2: // the Windows driver sends it twice
      goodix_send_raw_cmd (dev, GOODIX_CMD_SET_DRV_STATE, drv_state_payload,
                           sizeof (drv_state_payload), FALSE, check_reply, ssm);
      break;

    case POST_TLS_GET_POV_IMAGE:
      goodix_send_raw_cmd (dev, GOODIX_CMD_MCU_GET_POV_IMAGE, zero_payload,
                           sizeof (zero_payload), TRUE, check_reply, ssm);
      break;
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  if (!error)
    {
      goodixtls5xx_init_tls (dev);
    }
  else
    {
      fp_err ("failed during activation: %s (code: %d)", error->message,
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

// ---- ACTIVATE SECTION END ----

static GoodixTls5xxMcuConfig
get_fdt_config (GoodixTls5xxFdtKind kind)
{
  GoodixTls5xxMcuConfig cfg = {.free_fn = NULL};

  switch (kind)
    {
    case GOODIXTLS5XX_FDT_MODE:
      cfg.data = goodix_5503_fdt_mode;
      cfg.data_len = sizeof (goodix_5503_fdt_mode);
      break;

    case GOODIXTLS5XX_FDT_DOWN:
      cfg.data = goodix_5503_fdt_down;
      cfg.data_len = sizeof (goodix_5503_fdt_down);
      break;

    case GOODIXTLS5XX_FDT_UP:
    default:
      cfg.data = goodix_5503_fdt_up;
      cfg.data_len = sizeof (goodix_5503_fdt_up);
      break;
    }
  return cfg;
}

static GoodixTls5xxMcuConfig
get_mcu_config (void)
{
  return get_fdt_config (GOODIXTLS5XX_FDT_DOWN);
}

static FpImage *
process_frame (guint8 *frame)
{
  FpImage *img = fp_image_new (GOODIX5503_WIDTH, GOODIX5503_HEIGHT);

  img->flags |= FPI_IMAGE_PARTIAL;
  memcpy (img->data, frame, GOODIX5503_WIDTH * GOODIX5503_HEIGHT);
  return img;
}

static void
fpi_device_goodixtls5503_init (FpiDeviceGoodixTls5503 *self)
{
}

static void
fpi_device_goodixtls5503_class_init (FpiDeviceGoodixTls5503Class *class)
{
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS (class);
  FpiDeviceGoodixTls5xxClass *xx_cls = FPI_DEVICE_GOODIXTLS5XX_CLASS (class);

  xx_cls->get_mcu_cfg = get_mcu_config;
  xx_cls->get_fdt_cfg = get_fdt_config;
  xx_cls->process_frame = process_frame;
  xx_cls->scan_height = GOODIX5503_HEIGHT;
  xx_cls->scan_width = GOODIX5503_WIDTH;
  xx_cls->psk = goodix_5503_pmk_hash;
  xx_cls->psk_flags = GOODIX_5503_PSK_FLAGS;
  xx_cls->psk_len = sizeof (goodix_5503_pmk_hash);
  xx_cls->firmware_version = goodix_5503_firmware_versions[0];
  xx_cls->get_image_payload = goodix_5503_get_image;
  xx_cls->get_image_payload_len = sizeof (goodix_5503_get_image);
  xx_cls->frame_header = 0;
  xx_cls->frame_trailer = 4;
  xx_cls->skip_nav_calibration = TRUE;
  xx_cls->post_tls_run = post_tls_run_state;
  xx_cls->post_tls_num_states = POST_TLS_NUM_STATES;
  xx_cls->skip_query_mcu = TRUE;
  xx_cls->verify_fdt_up = TRUE;

  gx_class->skip_tls_established = TRUE;
  gx_class->interface = GOODIX_5503_INTERFACE;
  gx_class->ep_in = GOODIX_5503_EP_IN;
  gx_class->ep_out = GOODIX_5503_EP_OUT;

  dev_class->id = "goodixtls5503";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 5503";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 20;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  // Capacitive sensor that does not heat up; the default thermal model locks
  // the device ("Device disabled to prevent overheating") after a few long
  // enroll sessions.
  dev_class->temp_hot_seconds = -1;

  // The 64x80 (~3x4 mm) image has almost no minutiae, so NBIS cannot work;
  // use SIGFM (SIFT based). bz3_threshold is the SIGFM score threshold here.
  // TODO: tune with real enroll/verify data ("sigfm score X/Y" debug output)
  img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM;
  img_dev_class->bz3_threshold = 12;
  img_dev_class->img_width = GOODIX5503_WIDTH;
  img_dev_class->img_height = GOODIX5503_HEIGHT;

  img_dev_class->activate = dev_activate;

  fpi_device_class_auto_initialize_features (dev_class);
}
