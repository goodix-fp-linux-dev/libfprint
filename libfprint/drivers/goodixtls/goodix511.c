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

#define FP_COMPONENT "goodixtls511"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodix511.h"

#include <math.h>

#define GOODIX511_WIDTH 64
#define GOODIX511_HEIGHT 80
#define GOODIX511_SCAN_WIDTH 88
#define GOODIX511_FRAME_SIZE (GOODIX511_WIDTH * GOODIX511_HEIGHT)
// For every 4 pixels there are 6 bytes and there are 8 extra start bytes and 5
// extra end
#define GOODIX511_RAW_FRAME_SIZE \
  8 + (GOODIX511_HEIGHT * GOODIX511_SCAN_WIDTH) / 4 * 6 + 5
#define GOODIX511_CAP_FRAMES 1 // Number of frames we capture per swipe

typedef unsigned short Goodix511Pix;

struct _FpiDeviceGoodixTls511
{
  FpiDeviceGoodixTls parent;

  guint8            *otp;
};

G_DECLARE_FINAL_TYPE (FpiDeviceGoodixTls511, fpi_device_goodixtls511, FPI,
                      DEVICE_GOODIXTLS511, FpiDeviceGoodixTls5xx);

G_DEFINE_TYPE (FpiDeviceGoodixTls511, fpi_device_goodixtls511,
               FPI_TYPE_DEVICE_GOODIXTLS5XX);

// ---- ACTIVE SECTION START ----

enum activate_states {
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_RESET,
  ACTIVATE_SET_MCU_IDLE,
  ACTIVATE_READ_ODP,
  ACTIVATE_UPLOAD_MCU_CONFIG,
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY,
  ACTIVATE_NUM_STATES,
};

enum otp_write_states {
  OTP_WRITE_1,
  OTP_WRITE_2,
  OTP_WRITE_3,
  OTP_WRITE_4,

  OTP_WRITE_NUM,
};

static guint16 otp_write_addrs[] = {0x0220, 0x0236, 0x0238, 0x023a};

static void
otp_write_run (FpiSsm *ssm, FpDevice *dev)
{
  guint16 data;
  FpiDeviceGoodixTls511 *self = FPI_DEVICE_GOODIXTLS511 (dev);
  guint8 *otp = self->otp;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OTP_WRITE_1:
      data = otp[46] << 4 | 8;
      break;

    case OTP_WRITE_2:
      data = otp[47];
      break;

    case OTP_WRITE_3:
      data = otp[48];
      break;

    case OTP_WRITE_4:
      data = otp[49];
      break;
    }

  goodix_send_write_sensor_register (
    dev, otp_write_addrs[fpi_ssm_get_cur_state (ssm)], data, goodixtls5xx_check_none,
    ssm);
  if (fpi_ssm_get_cur_state (ssm) == OTP_WRITE_NUM - 1)
    free (self->otp);
}

static void
read_otp_callback (FpDevice *dev, guint8 *data, guint16 len,
                   gpointer ssm, GError *err)
{
  if (err)
    {
      fpi_ssm_mark_failed (ssm, err);
      return;
    }
  if (len < 64)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (FP_DEVICE_ERROR,
                                             FP_DEVICE_ERROR_DATA_INVALID,
                                             "OTP is invalid (len: %d)", 64));
      return;
    }
  FpiDeviceGoodixTls511 *self = FPI_DEVICE_GOODIXTLS511 (dev);
  self->otp = malloc (len);
  memcpy (self->otp, data, len);
  FpiSsm *otp_ssm = fpi_ssm_new (dev, otp_write_run, OTP_WRITE_NUM);
  fpi_ssm_start_subsm (ssm, otp_ssm);
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_READ_AND_NOP:
      // Nop seems to clear the previous command buffer. But we are
      // unable to do so.
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
      goodix_send_query_firmware_version (dev, goodixtls5xx_check_firmware_version, ssm);
      break;

    case ACTIVATE_CHECK_PSK:
      goodix_send_preset_psk_read (dev, GOODIX_511_PSK_FLAGS, 0,
                                   goodixtls5xx_check_preset_psk_read, ssm);
      break;

    case ACTIVATE_RESET:
      goodix_send_reset (dev, TRUE, 20, goodixtls5xx_check_reset, ssm);
      break;

    case ACTIVATE_SET_MCU_IDLE:
      goodix_send_mcu_switch_to_idle_mode (dev, 20, goodixtls5xx_check_idle, ssm);
      break;

    case ACTIVATE_READ_ODP:
      goodix_send_read_otp (dev, read_otp_callback, ssm);
      break;

    case ACTIVATE_UPLOAD_MCU_CONFIG:
      goodix_send_upload_config_mcu (dev, goodix_511_config,
                                     sizeof (goodix_511_config), NULL,
                                     goodixtls5xx_check_config_upload, ssm);
      break;

    case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
      goodix_send_set_powerdown_scan_frequency (
        dev, 100, goodixtls5xx_check_powerdown_scan_freq, ssm);
      break;
    }
}

static void
activate_complete (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  G_DEBUG_HERE ();
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

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- SCAN SECTION START ----

const guint8 fdt_switch_state_mode[] = {
  0x01,
  0x80,
  0xaf,
  0x80,
  0xbf,
  0x80,
  0xa4,
  0x80,
  0xb8,
  0x80,
  0xa8,
  0x80,
  0xb7,
};


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

static void
fpi_device_goodixtls511_init (FpiDeviceGoodixTls511 *self)
{
}
static GoodixTls5xxMcuConfig
get_mcu_config ()
{
  GoodixTls5xxMcuConfig cfg;

  cfg.free_fn = NULL;
  cfg.data = fdt_switch_state_mode;
  cfg.data_len = sizeof (fdt_switch_state_mode);
  return cfg;
}

static FpImage *
crop_frame (guint8 * frame)
{
  FpImage * img = fp_image_new (GOODIX511_WIDTH, GOODIX511_HEIGHT);

  img->flags |= FPI_IMAGE_PARTIAL;
  for (int y = 0; y != GOODIX511_HEIGHT; ++y)
    {
      for (int x = 0; x != GOODIX511_WIDTH; ++x)
        {
          const int idx = x + y * GOODIX511_SCAN_WIDTH;
          img->data[x + y * GOODIX511_WIDTH] = frame[idx];
        }
    }
  return img;
}

static void
fpi_device_goodixtls511_class_init (FpiDeviceGoodixTls511Class * class)
{
  FpiDeviceGoodixTlsClass * gx_class = FPI_DEVICE_GOODIXTLS_CLASS (class);
  FpDeviceClass * dev_class = FP_DEVICE_CLASS (class);
  FpImageDeviceClass * img_dev_class = FP_IMAGE_DEVICE_CLASS (class);
  FpiDeviceGoodixTls5xxClass * xx_cls = FPI_DEVICE_GOODIXTLS5XX_CLASS (class);

  xx_cls->get_mcu_cfg = get_mcu_config;
  xx_cls->process_frame = crop_frame;
  xx_cls->scan_height = GOODIX511_HEIGHT;
  xx_cls->scan_width = GOODIX511_SCAN_WIDTH;
  xx_cls->psk = goodix_511_psk_0;
  xx_cls->psk_flags = GOODIX_511_PSK_FLAGS;
  xx_cls->psk_len = sizeof (goodix_511_psk_0);
  xx_cls->firmware_version = GOODIX_511_FIRMWARE_VERSION;
  xx_cls->reset_number = GOODIX_511_RESET_NUMBER;

  gx_class->interface = GOODIX_511_INTERFACE;
  gx_class->ep_in = GOODIX_511_EP_IN;
  gx_class->ep_out = GOODIX_511_EP_OUT;

  dev_class->id = "goodixtls511";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 511";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 20;

  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  // TODO
  img_dev_class->bz3_threshold = 24;
  img_dev_class->img_width = GOODIX511_WIDTH;
  img_dev_class->img_height = GOODIX511_HEIGHT;

  img_dev_class->activate = dev_activate;

  fpi_device_class_auto_initialize_features (dev_class);
}
