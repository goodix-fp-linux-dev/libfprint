/*
 * Elan SPI driver for libfprint
 *
 * Copyright (C) 2021 Matthew Mirvish <matthew@mm12.xyz>
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

#define FP_COMPONENT "elanspi"

#include "drivers_api.h"
#include "elanspi.h"

#include <linux/hidraw.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/types.h>
#include <errno.h>

struct _FpiDeviceElanSpi
{
  FpImageDevice parent;

  /* sensor info */
  guint8   sensor_width, sensor_height, sensor_ic_version, sensor_id;
  gboolean sensor_otp;
  guint8   sensor_vcm_mode;

  /* processed frame info */
  guint8 frame_width, frame_height;

  /* init info */
  guint8 sensor_raw_version, sensor_reg_17;
  guint8 sensor_reg_vref1, sensor_reg_28, sensor_reg_27, sensor_reg_dac2;

  /* calibration info */
  union
  {
    struct
    {
      guint8 dac_value;
      guint8 line_ptr;
      guint8 dacfine_retry;
      gint64 otp_timeout;
    } old_data;
    struct
    {
      guint16 gdac_value;
      guint16 gdac_step;
      guint16 best_gdac;
      guint16 best_meandiff;
    } hv_data;
  };

  /* generic temp info for async reading */
  guint8 sensor_status;
  gint64 capture_timeout;

  /* background / calibration parameters */
  guint16 *bg_image;
  guint16 *last_image;
  guint16 *prev_frame_image;

  gint     fp_empty_counter;
  GSList  *fp_frame_list;

  /* wait ctx */
  gint     finger_wait_debounce;

  gboolean deactivating, capturing;

  /* active SPI status info */
  int spi_fd;
};

G_DECLARE_FINAL_TYPE (FpiDeviceElanSpi, fpi_device_elanspi, FPI, DEVICE_ELANSPI, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceElanSpi, fpi_device_elanspi, FP_TYPE_IMAGE_DEVICE);

static void
elanspi_do_hwreset (FpiDeviceElanSpi *self, GError **err)
{
  /* Skip in emulation mode, since we don't mock hid devices */
  if (g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") == 0)
    return;

  /*
   * TODO: Make this also work with the non-HID cases
   */

  int fd = open ((char *) fpi_device_get_udev_data (FP_DEVICE (self), FPI_DEVICE_UDEV_SUBTYPE_HIDRAW), O_RDWR);

  if (fd < 0)
    {
      g_set_error (err, G_IO_ERROR, g_io_error_from_errno (errno), "unable to open hid");
      return;
    }

  guint8 buf[5] = {
    0xe, 0, 0, 0, 0
  };

  if (ioctl (fd, HIDIOCSFEATURE (5), &buf) != 5)
    {
      g_set_error (err, G_IO_ERROR, g_io_error_from_errno (errno), "unable to reset via hid");
      goto out;
    }

out:
  close (fd);
}

/*
 * Three main processes involved in driving these sensors:
 *   - initialization (device type detection)
 *   - calibration
 *      - image capture (single)
 *   - image capture (stitched)
 */

enum elanspi_init_state {
  ELANSPI_INIT_READ_STATUS1,
  ELANSPI_INIT_HWSWRESET,       /* fused b.c. hw reset is currently sync */
  ELANSPI_INIT_SWRESETDELAY1,
  ELANSPI_INIT_READ_HEIGHT,
  ELANSPI_INIT_READ_WIDTH,
  ELANSPI_INIT_READ_REG17,         /* both of these states finish setting up sensor settings */
  ELANSPI_INIT_READ_VERSION,       /* can jump straight to calibrate */
  ELANSPI_INIT_SWRESET2,
  ELANSPI_INIT_SWRESETDELAY2,
  ELANSPI_INIT_OTP_READ_VREF1,
  ELANSPI_INIT_OTP_WRITE_VREF1,
  ELANSPI_INIT_OTP_WRITE_0x28,
  ELANSPI_INIT_OTP_LOOP_READ_0x28,       /* may loop */
  ELANSPI_INIT_OTP_LOOP_READ_0x27,
  ELANSPI_INIT_OTP_LOOP_UPDATEDAC_READ_DAC2,
  ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_DAC2,
  ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_10,
  /* exit loop */
  ELANSPI_INIT_OTP_WRITE_0xb,
  ELANSPI_INIT_OTP_WRITE_0xc,
  /* do calibration (mutexc) */
  ELANSPI_INIT_CALIBRATE,
  ELANSPI_INIT_BG_CAPTURE,
  ELANSPI_INIT_BG_SAVE,
  ELANSPI_INIT_NSTATES
};

enum elanspi_calibrate_old_state {
  ELANSPI_CALIBOLD_UNPROTECT,
  ELANSPI_CALIBOLD_WRITE_STARTCALIB,
  ELANSPI_CALIBOLD_STARTCALIBDELAY,
  ELANSPI_CALIBOLD_SEND_REGTABLE,
  /* calibrate dac base value */
  ELANSPI_CALIBOLD_DACBASE_CAPTURE,
  ELANSPI_CALIBOLD_DACBASE_WRITE_DAC1,
  /* check for finger */
  ELANSPI_CALIBOLD_CHECKFIN_CAPTURE,
  /* increase gain */
  ELANSPI_CALIBOLD_WRITE_GAIN,
  /* calibrate dac stage2 */
  ELANSPI_CALIBOLD_DACFINE_CAPTURE,
  ELANSPI_CALIBOLD_DACFINE_WRITE_DAC1,
  ELANSPI_CALIBOLD_DACFINE_LOOP,
  /* exit ok (cleanup by protecting) */
  ELANSPI_CALIBOLD_PROTECT,
  ELANSPI_CALIBOLD_NSTATES
};

enum elanspi_capture_old_state {
  ELANSPI_CAPTOLD_WRITE_CAPTURE,
  ELANSPI_CAPTOLD_CHECK_LINEREADY,
  ELANSPI_CAPTOLD_RECV_LINE,

  ELANSPI_CAPTOLD_NSTATES
};

enum elanspi_calibrate_hv_state {
  ELANSPI_CALIBHV_SELECT_PAGE0_0,
  ELANSPI_CALIBHV_WRITE_STARTCALIB,
  ELANSPI_CALIBHV_UNPROTECT,
  ELANSPI_CALIBHV_SEND_REGTABLE0,
  ELANSPI_CALIBHV_SELECT_PAGE1,
  ELANSPI_CALIBHV_SEND_REGTABLE1,
  ELANSPI_CALIBHV_SELECT_PAGE0_1,
  ELANSPI_CALIBHV_WRITE_GDAC_H,
  ELANSPI_CALIBHV_WRITE_GDAC_L,
  ELANSPI_CALIBHV_CAPTURE,
  ELANSPI_CALIBHV_PROCESS,
  ELANSPI_CALIBHV_WRITE_BEST_GDAC_H,
  ELANSPI_CALIBHV_WRITE_BEST_GDAC_L,
  /* cleanup by protecting */
  ELANSPI_CALIBHV_PROTECT,
  ELANSPI_CALIBHV_NSTATES
};

enum elanspi_capture_hv_state {
  ELANSPI_CAPTHV_WRITE_CAPTURE,
  ELANSPI_CAPTHV_CHECK_READY,
  ELANSPI_CAPTHV_RECV_IMAGE,
  ELANSPI_CAPTHV_NSTATES
};

enum elanspi_write_regtable_state {
  ELANSPI_WRTABLE_WRITE,
  ELANSPI_WRTABLE_ITERATE,
  ELANSPI_WRTABLE_NSTATES
};

enum elanspi_fp_capture_state {
  ELANSPI_FPCAPT_INIT,
  /* wait for finger */
  ELANSPI_FPCAPT_WAITDOWN_CAPTURE,
  ELANSPI_FPCAPT_WAITDOWN_PROCESS,
  /* capture full image */
  ELANSPI_FPCAPT_FP_CAPTURE,
  ELANSPI_FPCAPT_FP_PROCESS,
  /* wait for no finger */
  ELANSPI_FPCAPT_WAITUP_CAPTURE,
  ELANSPI_FPCAPT_WAITUP_PROCESS,
  ELANSPI_FPCAPT_NSTATES
};

/* helpers */

static FpiSpiTransfer *
elanspi_do_swreset (FpiDeviceElanSpi *self)
{
  FpiSpiTransfer * xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 1);
  xfer->buffer_wr[0] = 0x31;
  return xfer;
}
static FpiSpiTransfer *
elanspi_do_startcalib (FpiDeviceElanSpi *self)
{
  FpiSpiTransfer * xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 1);
  xfer->buffer_wr[0] = 0x4;
  return xfer;
}
static FpiSpiTransfer *
elanspi_do_capture (FpiDeviceElanSpi *self)
{
  FpiSpiTransfer * xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 1);
  xfer->buffer_wr[0] = 0x1;
  return xfer;
}
static FpiSpiTransfer *
elanspi_do_selectpage (FpiDeviceElanSpi *self, guint8 page)
{
  FpiSpiTransfer * xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 2);
  xfer->buffer_wr[0] = 0x7;
  xfer->buffer_wr[1] = page;
  return xfer;
}

static FpiSpiTransfer *
elanspi_single_read_cmd (FpiDeviceElanSpi *self, guint8 cmd_id, guint8 *data_out)
{
  FpiSpiTransfer * xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 2);
  xfer->buffer_wr[0] = cmd_id;
  xfer->buffer_wr[1] = 0xff;
  fpi_spi_transfer_read_full (xfer, data_out, 1, NULL);
  return xfer;
}

static FpiSpiTransfer *
elanspi_read_status (FpiDeviceElanSpi *self, guint8 *data_out)
{
  return elanspi_single_read_cmd (self, 0x3, data_out);
}
static FpiSpiTransfer *
elanspi_read_width (FpiDeviceElanSpi *self, guint8 *data_out)
{
  return elanspi_single_read_cmd (self, 0x9, data_out);
}
static FpiSpiTransfer *
elanspi_read_height (FpiDeviceElanSpi *self, guint8 *data_out)
{
  return elanspi_single_read_cmd (self, 0x8, data_out);
}
static FpiSpiTransfer *
elanspi_read_version (FpiDeviceElanSpi *self, guint8 *data_out)
{
  return elanspi_single_read_cmd (self, 0xa, data_out);
}

static FpiSpiTransfer *
elanspi_read_register (FpiDeviceElanSpi *self, guint8 reg_id, guint8 *data_out)
{
  FpiSpiTransfer * xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 1);
  xfer->buffer_wr[0] = reg_id | 0x40;
  fpi_spi_transfer_read_full (xfer, data_out, 1, NULL);
  return xfer;
}

static FpiSpiTransfer *
elanspi_write_register (FpiDeviceElanSpi *self, guint8 reg_id, guint8 data_in)
{
  FpiSpiTransfer * xfer = fpi_spi_transfer_new (FP_DEVICE (self), self->spi_fd);

  fpi_spi_transfer_write (xfer, 2);
  xfer->buffer_wr[0] = reg_id | 0x80;
  xfer->buffer_wr[1] = data_in;
  return xfer;
}

static void
elanspi_determine_sensor (FpiDeviceElanSpi *self, GError **err)
{
  guint8 raw_height = self->sensor_height;
  guint8 raw_width = self->sensor_width;

  if (((raw_height == 0xa1) && (raw_width == 0xa1)) ||
      ((raw_height == 0xd1) && (raw_width == 0x51)) ||
      ((raw_height == 0xc1) && (raw_width == 0x39)))
    {
      self->sensor_ic_version = 0;           /* Version 0 */
      self->sensor_width = raw_width - 1;
      self->sensor_height = raw_height - 1;
    }
  else
    {
      /* If the sensor is exactly 96x96 (0x60 x 0x60), the version is the high bit of register 17 */
      if (raw_width == 0x60 && raw_height == 0x60)
        {
          self->sensor_ic_version = (self->sensor_reg_17 & 0x80) ? 1 : 0;
        }
      else
        {
          if (((raw_height != 0xa0) || (raw_width != 0x50)) &&
              ((raw_height != 0x90) || (raw_width != 0x40)) &&
              ((raw_height != 0x78) || (raw_width != 0x78)))
            {
              if (((raw_height != 0x40) || (raw_width != 0x58)) &&
                  ((raw_height != 0x50) || (raw_width != 0x50)))
                {
                  /* Old sensor hack?? */
                  self->sensor_width = 0x78;
                  self->sensor_height = 0x78;
                  self->sensor_ic_version = 0;
                }
              else
                {
                  /* Otherwise, read the version 'normally' */
                  self->sensor_ic_version = (self->sensor_raw_version & 0x70) >> 4;
                }
            }
          else
            {
              self->sensor_ic_version = 1;
            }
        }
    }

  fp_dbg ("<init/detect> after hardcoded lookup; %dx%d, version %d", self->sensor_width, self->sensor_height, self->sensor_ic_version);

  for (const struct elanspi_sensor_entry *entry = elanspi_sensor_table; entry->name; entry += 1)
    {
      if (entry->ic_version == self->sensor_ic_version && entry->width == self->sensor_width && entry->height == self->sensor_height)
        {
          self->sensor_id = entry->sensor_id;
          self->sensor_otp = entry->is_otp_model;

          fp_dbg ("<init/detect> found sensor ID %d => [%s] (%d x %d)", self->sensor_id, entry->name, self->sensor_width, self->sensor_height);
          break;
        }
    }

  if (self->sensor_id == 0xff)
    {
      *err = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "unknown sensor (%dx%d, v%d)", self->sensor_width, self->sensor_height, self->sensor_ic_version);
      return;
    }

  /* setup frame size */
  if (fpi_device_get_driver_data (FP_DEVICE (self)) & ELANSPI_HV_FLIPPED)
    {
      self->frame_width = self->sensor_height;
      self->frame_height = self->sensor_width > ELANSPI_MAX_FRAME_HEIGHT ? ELANSPI_MAX_FRAME_HEIGHT : self->sensor_width;
    }
  else
    {
      self->frame_width = self->sensor_width;
      self->frame_height = self->sensor_height > ELANSPI_MAX_FRAME_HEIGHT ? ELANSPI_MAX_FRAME_HEIGHT : self->sensor_height;
    }
}

static void
elanspi_capture_old_line_handler (FpiSpiTransfer *transfer, FpDevice *dev, gpointer unused_data, GError *error)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* copy buffer from line into last_image */
  for (int col = 0; col < self->sensor_width; col += 1)
    {
      guint8 low =  transfer->buffer_rd[col * 2 + 1];
      guint8 high = transfer->buffer_rd[col * 2];

      self->last_image[self->sensor_width * self->old_data.line_ptr + col] = low + high * 0x100;
    }

  /* increment line ptr */
  self->old_data.line_ptr += 1;
  /* if there is still data, continue from check lineready */
  if (self->old_data.line_ptr < self->sensor_height)
    {
      fpi_ssm_jump_to_state (transfer->ssm, ELANSPI_CAPTOLD_CHECK_LINEREADY);
    }
  else
    {
      /* check for termination */
      if (fpi_device_get_current_action (dev) == FPI_DEVICE_ACTION_NONE)
        {
          fpi_ssm_mark_completed (transfer->ssm);
          return;
        }
      /* check for cancellation */
      if (fpi_device_action_is_cancelled (dev))
        {
          g_cancellable_set_error_if_cancelled (fpi_device_get_cancellable (dev), &error);
          fpi_ssm_mark_failed (transfer->ssm, error);
          return;
        }
      /* otherwise finish succesfully */
      fpi_ssm_mark_completed (transfer->ssm);
    }
}

static void
elanspi_capture_old_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);
  FpiSpiTransfer *xfer = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELANSPI_CAPTOLD_WRITE_CAPTURE:
      /* reset capture state */
      self->old_data.line_ptr = 0;
      self->capture_timeout = g_get_monotonic_time () + ELANSPI_OLD_CAPTURE_TIMEOUT_USEC;
      xfer = elanspi_do_capture (self);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, NULL, fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CAPTOLD_CHECK_LINEREADY:
      xfer = elanspi_read_status (self, &self->sensor_status);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, NULL, fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CAPTOLD_RECV_LINE:
      /* is the sensor ready? */
      if (!(self->sensor_status & 4))
        {
          /* has the timeout expired? -- disabled in testing since valgrind is very slow */
          if (g_get_monotonic_time () > self->capture_timeout && g_strcmp0 (g_getenv ("FP_DEVICE_EMULATION"), "1") != 0)
            {
              /* end with a timeout */
              fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "timed out waiting for new line"));
              return;
            }
          /* check again */
          fpi_ssm_jump_to_state (ssm, ELANSPI_CAPTOLD_CHECK_LINEREADY);
          return;
        }
      /* otherwise, perform a read */
      xfer = fpi_spi_transfer_new (dev, self->spi_fd);
      xfer->ssm = ssm;
      fpi_spi_transfer_write (xfer, 2);
      xfer->buffer_wr[0] = 0x10;                   /* receieve line */
      fpi_spi_transfer_read (xfer, self->sensor_width * 2);
      fpi_spi_transfer_submit (xfer, NULL, elanspi_capture_old_line_handler, NULL);
      return;
    }
}

static void
elanspi_send_regtable_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanSpi *self  = FPI_DEVICE_ELANSPI (dev);
  FpiSpiTransfer *xfer  = NULL;
  const struct elanspi_reg_entry *entry = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELANSPI_WRTABLE_WRITE:
      xfer = elanspi_write_register (self, entry->addr, entry->value);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_WRTABLE_ITERATE:
      entry += 1;
      if (entry->addr != 0xff)
        {
          fpi_ssm_set_data (ssm, (gpointer) entry, NULL);
          fpi_ssm_jump_to_state (ssm, ELANSPI_WRTABLE_WRITE);
          return;
        }
      fpi_ssm_mark_completed (ssm);
      return;
    }
}

static FpiSsm *
elanspi_write_regtable (FpiDeviceElanSpi *self, const struct elanspi_regtable * table)
{
  /* find regtable pointer */
  const struct elanspi_reg_entry * starting_entry = table->other;

  for (int i = 0; table->entries[i].table; i += 1)
    {
      if (table->entries[i].sid == self->sensor_id)
        {
          starting_entry = table->entries[i].table;
          break;
        }
    }
  if (starting_entry == NULL)
    {
      fp_err ("<regtable> unknown regtable for sensor %d", self->sensor_id);
      return NULL;
    }

  FpiSsm * ssm = fpi_ssm_new (FP_DEVICE (self), elanspi_send_regtable_handler, ELANSPI_WRTABLE_NSTATES);

  fpi_ssm_set_data (ssm, (gpointer) starting_entry, NULL);
  return ssm;
}

static int
elanspi_mean_image (FpiDeviceElanSpi *self, const guint16 *img)
{
  int total = 0;

  for (int i = 0; i < self->sensor_width * self->sensor_height; i += 1)
    total += img[i];
  return total / (self->sensor_width * self->sensor_height);
}

static void
elanspi_calibrate_old_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);
  FpiSpiTransfer *xfer = NULL;
  GError *err  = NULL;
  FpiSsm *chld = NULL;
  int mean_value = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELANSPI_CALIBOLD_UNPROTECT:
      xfer = elanspi_write_register (self, 0x00, 0x5a);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBOLD_WRITE_STARTCALIB:
      xfer = elanspi_do_startcalib (self);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBOLD_STARTCALIBDELAY:
      fpi_ssm_next_state_delayed (ssm, 1);
      return;

    case ELANSPI_CALIBOLD_SEND_REGTABLE:
      chld = elanspi_write_regtable (self, &elanspi_calibration_table_old);
      if (chld == NULL)
        {
          err = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "unknown calibration table for sensor");
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_CALIBOLD_DACBASE_CAPTURE:
    case ELANSPI_CALIBOLD_CHECKFIN_CAPTURE:
    case ELANSPI_CALIBOLD_DACFINE_CAPTURE:
      chld = fpi_ssm_new (dev, elanspi_capture_old_handler, ELANSPI_CAPTOLD_NSTATES);
      fpi_ssm_silence_debug (chld);
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_CALIBOLD_DACBASE_WRITE_DAC1:
      /* compute dac */
      self->old_data.dac_value = ((elanspi_mean_image (self, self->last_image) & 0xffff) + 0x80) >> 8;
      if (0x3f < self->old_data.dac_value)
        self->old_data.dac_value = 0x3f;
      fp_dbg ("<calibold> dac init is 0x%02x", self->old_data.dac_value);
      /* write it */
      xfer = elanspi_write_register (self, 0x6, self->old_data.dac_value - 0x40);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBOLD_WRITE_GAIN:
      /* check if finger was present */
      if (elanspi_mean_image (self, self->last_image) >= ELANSPI_MAX_OLD_STAGE1_CALIBRATION_MEAN)
        {
          err = fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER, "finger on sensor during calibration");
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      /* if ok, increase gain */
      xfer = elanspi_write_register (self, 0x5, 0x6f);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      /* initialize retry counter */
      self->old_data.dacfine_retry = 0;
      return;

    case ELANSPI_CALIBOLD_DACFINE_WRITE_DAC1:
      mean_value = elanspi_mean_image (self, self->last_image);
      if (mean_value >= ELANSPI_MIN_OLD_STAGE2_CALBIRATION_MEAN && mean_value <= ELANSPI_MAX_OLD_STAGE2_CALBIRATION_MEAN)
        {
          /* finished calibration, goto bg */
          fpi_ssm_jump_to_state (ssm, ELANSPI_CALIBOLD_PROTECT);
          return;
        }

      if (mean_value < (ELANSPI_MIN_OLD_STAGE2_CALBIRATION_MEAN + (ELANSPI_MAX_OLD_STAGE2_CALBIRATION_MEAN - ELANSPI_MIN_OLD_STAGE2_CALBIRATION_MEAN) / 2))
        self->old_data.dac_value -= 1;
      else
        self->old_data.dac_value += 1;

      /* write it */
      xfer = elanspi_write_register (self, 0x6, self->old_data.dac_value - 0x40);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBOLD_DACFINE_LOOP:
      /* check the retry counter */
      self->old_data.dacfine_retry += 1;
      if (self->old_data.dacfine_retry >= 2)
        {
          /* bail with calibration error */
          err = fpi_device_retry_new_msg (FP_DEVICE_RETRY_REMOVE_FINGER, "finger on sensor during calibration");
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fp_dbg ("<calibold> repeating calibration for the %dth time", self->old_data.dacfine_retry);
      /* otherwise, take another image */
      fpi_ssm_jump_to_state (ssm, ELANSPI_CALIBOLD_DACFINE_CAPTURE);
      return;

    case ELANSPI_CALIBOLD_PROTECT:
      fp_dbg ("<calibold> calibration ok, saving bg image");
      xfer = elanspi_write_register (self, 0x00, 0x00);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;
    }
}

static void
elanspi_capture_hv_image_handler (FpiSpiTransfer *transfer, FpDevice *dev, gpointer unused_data, GError *error)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  int i, outptr;
  guint16 value = 0;

  for (i = 0, outptr = 0; i < transfer->length_rd && outptr < (self->sensor_height * self->sensor_width * 2); i += 1)
    {
      if (transfer->buffer_rd[i] != 0xff)
        {
          if (outptr % 2)
            {
              value <<= 8;
              value |= transfer->buffer_rd[i];
              self->last_image[outptr / 2] = value;
            }
          else
            {
              value = transfer->buffer_rd[i];
            }
          outptr += 1;
        }
    }

  if (outptr != (self->sensor_height * self->sensor_width * 2))
    {
      fp_warn ("<capture/hv> did not receive full image");
      /* mark ssm failed */
      error = fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO, "hv image receieve did not fill buffer");
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  fpi_ssm_mark_completed (transfer->ssm);
}


static void
elanspi_capture_hv_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);
  FpiSpiTransfer *xfer = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELANSPI_CAPTHV_WRITE_CAPTURE:
      /* reset capture state */
      self->old_data.line_ptr = 0;
      self->capture_timeout = g_get_monotonic_time () + ELANSPI_HV_CAPTURE_TIMEOUT_USEC;
      xfer = elanspi_do_capture (self);
      xfer->ssm = ssm;
      /* these are specifically cancellable because they don't leave the device at some aribtrary line offset, since
       * these devices only send entire images */
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CAPTHV_CHECK_READY:
      xfer = elanspi_read_status (self, &self->sensor_status);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CAPTHV_RECV_IMAGE:
      /* is the sensor ready? */
      if (!(self->sensor_status & 4))
        {
          /* has the timeout expired? */
          if (g_get_monotonic_time () > self->capture_timeout)
            {
              /* end with a timeout */
              fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "timed out waiting for image"));
              return;
            }
          /* check again */
          fpi_ssm_jump_to_state (ssm, ELANSPI_CAPTHV_CHECK_READY);
          return;
        }
      /* otherwise, read the image
       * the hv sensors seem to  use 128 bytes of padding(?) this is only tested on the 0xe sensors */
      xfer = fpi_spi_transfer_new (dev, self->spi_fd);
      xfer->ssm = ssm;
      fpi_spi_transfer_write (xfer, 2);
      xfer->buffer_wr[0] = 0x10;                   /* receieve line */
      fpi_spi_transfer_read (xfer, self->sensor_height * (self->sensor_width * 2 + 48));
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), elanspi_capture_hv_image_handler, NULL);
      return;
    }
}

static void
elanspi_calibrate_hv_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);
  FpiSpiTransfer *xfer = NULL;
  GError *err  = NULL;
  FpiSsm *chld = NULL;
  int mean_diff = 0;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELANSPI_CALIBHV_SELECT_PAGE0_0:
      /* initialize gdac */
      self->hv_data.gdac_value = 0x100;
      self->hv_data.gdac_step  = 0x100;
      self->hv_data.best_gdac  = 0x0;
      self->hv_data.best_meandiff = 0xffff;

    case ELANSPI_CALIBHV_SELECT_PAGE0_1:
      xfer = elanspi_do_selectpage (self, 0);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBHV_WRITE_STARTCALIB:
      xfer = elanspi_do_startcalib (self);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBHV_UNPROTECT:
      xfer = elanspi_write_register (self, 0x00, 0x5a);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBHV_SEND_REGTABLE0:
      chld = elanspi_write_regtable (self, &elanspi_calibration_table_new_page0);
      if (chld == NULL)
        {
          err = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "unknown calibration table for sensor");
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_CALIBHV_SELECT_PAGE1:
      xfer = elanspi_do_selectpage (self, 1);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBHV_SEND_REGTABLE1:
      chld = elanspi_write_regtable (self, &elanspi_calibration_table_new_page1);
      if (chld == NULL)
        {
          err = fpi_device_error_new_msg (FP_DEVICE_ERROR_NOT_SUPPORTED, "unknown calibration table for sensor");
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_CALIBHV_WRITE_GDAC_H:
    case ELANSPI_CALIBHV_WRITE_BEST_GDAC_H:
      if (fpi_ssm_get_cur_state (ssm) == ELANSPI_CALIBHV_WRITE_BEST_GDAC_H)
        self->hv_data.gdac_value = self->hv_data.best_gdac;
      xfer = elanspi_write_register (self, 0x06, (self->hv_data.gdac_value >> 2) & 0xff);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBHV_WRITE_GDAC_L:
    case ELANSPI_CALIBHV_WRITE_BEST_GDAC_L:
      xfer = elanspi_write_register (self, 0x07, self->hv_data.gdac_value & 3);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_CALIBHV_CAPTURE:
      chld = fpi_ssm_new (dev, elanspi_capture_hv_handler, ELANSPI_CAPTHV_NSTATES);
      fpi_ssm_silence_debug (chld);
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_CALIBHV_PROCESS:
      /* compute mean */
      mean_diff = abs (elanspi_mean_image (self, self->last_image) - ELANSPI_HV_CALIBRATION_TARGET_MEAN);
      if (mean_diff < 100)
        {
          fp_dbg ("<calibhv> calibration ok (mdiff < 100 w/ gdac=%04x)", self->hv_data.gdac_value);
          /* exit early, jump right to protect */
          fpi_ssm_jump_to_state (ssm, ELANSPI_CALIBHV_PROTECT);
          return;
        }
      if (mean_diff < self->hv_data.best_meandiff)
        {
          self->hv_data.best_meandiff = mean_diff;
          self->hv_data.best_gdac = self->hv_data.gdac_value;
        }
      /* shrink step */
      self->hv_data.gdac_step /= 2;
      if (self->hv_data.gdac_step == 0)
        {
          fp_dbg ("<calibhv> calibration ok (step = 0 w/ best_gdac=%04x)", self->hv_data.best_gdac);
          /* exit, using best value */
          fpi_ssm_jump_to_state (ssm, ELANSPI_CALIBHV_WRITE_BEST_GDAC_H);
          return;
        }
      /* update gdac */
      if (elanspi_mean_image (self, self->last_image) < ELANSPI_HV_CALIBRATION_TARGET_MEAN)
        self->hv_data.gdac_value -= self->hv_data.gdac_step;
      else
        self->hv_data.gdac_value += self->hv_data.gdac_step;
      /* advance back to capture */
      fpi_ssm_jump_to_state (ssm, ELANSPI_CALIBHV_WRITE_GDAC_H);
      return;

    case ELANSPI_CALIBHV_PROTECT:
      fp_dbg ("<calibhv> calibration ok, saving bg image");
      xfer = elanspi_write_register (self, 0x00, 0x00);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    }
}

static void
elanspi_init_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);
  FpiSpiTransfer *xfer = NULL;
  GError *err  = NULL;
  FpiSsm *chld = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELANSPI_INIT_READ_STATUS1:
      xfer = elanspi_read_status (self, &self->sensor_status);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_HWSWRESET:
      fp_dbg ("<init> got status %02x", self->sensor_status);
      elanspi_do_hwreset (self, &err);
      fp_dbg ("<init> sync hw reset");
      if (err)
        {
          fp_err ("<init> sync hw reset failed");
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
do_sw_reset:
      xfer = elanspi_do_swreset (self);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_SWRESETDELAY1:
    case ELANSPI_INIT_SWRESETDELAY2:
      fpi_ssm_next_state_delayed (ssm, 4);
      return;

    case ELANSPI_INIT_READ_HEIGHT:
      fp_dbg ("<init> sw reset ok");
      xfer = elanspi_read_height (self, &self->sensor_height);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_READ_WIDTH:
      self->sensor_height += 1;
      fp_dbg ("<init> raw height = %d", self->sensor_height);
      xfer = elanspi_read_width (self, &self->sensor_width);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_READ_REG17:
      self->sensor_width += 1;
      fp_dbg ("<init> raw width = %d", self->sensor_width);
      xfer = elanspi_read_register (self, 0x17, &self->sensor_reg_17);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_READ_VERSION:
      fp_dbg ("<init> raw reg17 = %d", self->sensor_reg_17);
      xfer = elanspi_read_version (self, &self->sensor_raw_version);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_SWRESET2:
      fp_dbg ("<init> raw version = %02x", self->sensor_raw_version);
      elanspi_determine_sensor (self, &err);
      if (err)
        {
          fp_err ("<init> sensor detection error");
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      /* allocate memory */
      g_clear_pointer (&self->bg_image, g_free);
      g_clear_pointer (&self->last_image, g_free);
      g_clear_pointer (&self->prev_frame_image, g_free);
      self->last_image = g_malloc0 (self->sensor_width * self->sensor_height * 2);
      self->bg_image = g_malloc0 (self->sensor_width * self->sensor_height * 2);
      self->prev_frame_image = g_malloc0 (self->sensor_width * self->sensor_height * 2);
      /* reset again */
      goto do_sw_reset;

    case ELANSPI_INIT_OTP_READ_VREF1:
      /* is this sensor otp? */
      if (!self->sensor_otp)
        {
          /* go to calibration */
          fpi_ssm_jump_to_state (ssm, ELANSPI_INIT_CALIBRATE);
          return;
        }
      /* otherwise, begin otp */
      self->old_data.otp_timeout = g_get_monotonic_time () + ELANSPI_OTP_TIMEOUT_USEC;
      xfer = elanspi_read_register (self, 0x3d, &self->sensor_reg_vref1);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_OTP_WRITE_VREF1:
      /* mask out low bits */
      self->sensor_reg_vref1 &= 0x3f;
      xfer = elanspi_write_register (self, 0x3d, self->sensor_reg_vref1);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_OTP_WRITE_0x28:
      xfer = elanspi_write_register (self, 0x28, 0x78);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    /* begin loop */
    case ELANSPI_INIT_OTP_LOOP_READ_0x28:
      /* begin read of 0x28 */
      xfer = elanspi_read_register (self, 0x28, &self->sensor_reg_28);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_OTP_LOOP_READ_0x27:
      if (self->sensor_reg_28 & 0x40)
        {
          /* try again */
          fp_dbg ("<init/otp> looping");
          fpi_ssm_jump_to_state (ssm, ELANSPI_INIT_OTP_LOOP_READ_0x28);
          return;
        }
      /* otherwise, read reg 27 */
      xfer = elanspi_read_register (self, 0x27, &self->sensor_reg_27);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_OTP_LOOP_UPDATEDAC_READ_DAC2:
      /* if high bit set, exit with mode 2 */
      if (self->sensor_reg_27 & 0x80)
        {
          self->sensor_vcm_mode = 2;
          fpi_ssm_jump_to_state (ssm, ELANSPI_INIT_OTP_WRITE_0xb);
          return;
        }
      /* if low two bits are not set, loop */
      if ((self->sensor_reg_27 & 6) != 6)
        {
          /* have we hit the timeout */
          if (g_get_monotonic_time () > self->old_data.otp_timeout)
            {
              fp_warn ("<init/otp> timed out waiting for vcom detection");
              self->sensor_vcm_mode = 2;
              fpi_ssm_jump_to_state (ssm, ELANSPI_INIT_OTP_WRITE_0xb);
              return;
            }
          /* try again */
          fp_dbg ("<init/otp> looping");
          fpi_ssm_jump_to_state (ssm, ELANSPI_INIT_OTP_LOOP_READ_0x28);
          return;
        }
      /* otherwise, set vcm mode from low bit and read dac2 */
      self->sensor_vcm_mode = (self->sensor_reg_27 & 1) + 1;
      xfer = elanspi_read_register (self, 0x7, &self->sensor_reg_dac2);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_DAC2:
      /* set high bit and rewrite */
      self->sensor_reg_dac2 |= 0x80;
      xfer = elanspi_write_register (self, 0x7, self->sensor_reg_dac2);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_OTP_LOOP_UPDATEDAC_WRITE_10:
      xfer = elanspi_write_register (self, 0xa, 0x97);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    /* end loop, joins to here on early exits */
    case ELANSPI_INIT_OTP_WRITE_0xb:
      fp_dbg ("<init/otp> got vcm mode = %d", self->sensor_vcm_mode);
      /* if mode is 0, skip to calibration */
      if (self->sensor_vcm_mode == 0)
        {
          fpi_ssm_jump_to_state (ssm, ELANSPI_INIT_CALIBRATE);
          return;
        }
      xfer = elanspi_write_register (self, 0xb, self->sensor_vcm_mode == 2 ? 0x72 : 0x71);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_OTP_WRITE_0xc:
      xfer = elanspi_write_register (self, 0xc, self->sensor_vcm_mode == 2 ? 0x62 : 0x49);
      xfer->ssm = ssm;
      fpi_spi_transfer_submit (xfer, fpi_device_get_cancellable (dev), fpi_ssm_spi_transfer_cb, NULL);
      return;

    case ELANSPI_INIT_CALIBRATE:
      fp_dbg ("<init/calibrate> starting calibrate");
      /* if sensor is hv */
      if (self->sensor_id == 0xe)
        chld = fpi_ssm_new_full (dev, elanspi_calibrate_hv_handler, ELANSPI_CALIBHV_NSTATES, ELANSPI_CALIBHV_PROTECT, "HV calibrate");
      else
        chld = fpi_ssm_new_full (dev, elanspi_calibrate_old_handler, ELANSPI_CALIBOLD_NSTATES, ELANSPI_CALIBOLD_PROTECT, "old calibrate");
      fpi_ssm_silence_debug (chld);
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_INIT_BG_CAPTURE:
      if (self->sensor_id == 0xe)
        chld = fpi_ssm_new (dev, elanspi_capture_hv_handler, ELANSPI_CAPTHV_NSTATES);
      else
        chld = fpi_ssm_new (dev, elanspi_capture_old_handler, ELANSPI_CAPTOLD_NSTATES);
      fpi_ssm_silence_debug (chld);
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_INIT_BG_SAVE:
      memcpy (self->bg_image, self->last_image, self->sensor_height * self->sensor_width * 2);
      fpi_ssm_mark_completed (ssm);
      return;
    }
}

enum elanspi_guess_result {
  ELANSPI_GUESS_FINGERPRINT,
  ELANSPI_GUESS_EMPTY,
  ELANSPI_GUESS_UNKNOWN
};

/* in place correct image, returning number of invalid pixels */
static gint
elanspi_correct_with_bg (FpiDeviceElanSpi *self, guint16 *raw_image)
{
  gint count = 0;

  for (int i = 0; i < self->sensor_width * self->sensor_height; i += 1)
    {
      if (raw_image[i] < self->bg_image[i])
        {
          count += 1;
          raw_image[i] = 0;
        }
      else
        {
          raw_image[i] -= self->bg_image[i];
        }
    }

  return count;
}

static guint16
elanspi_lookup_pixel_with_rotation (FpiDeviceElanSpi *self, const guint16 *data_in, int y, int x)
{
  int rotation = fpi_device_get_driver_data (FP_DEVICE (self)) & 3;
  gint x1 = x, y1 = y;

  if (rotation == ELANSPI_180_ROTATE)
    {
      x1 = (self->sensor_width - x - 1);
      y1 = (self->sensor_height - y - 1);
    }
  else if (rotation == ELANSPI_90LEFT_ROTATE)
    {
      x1 = y;
      y1 = (self->sensor_width - x - 1);
    }
  else if (rotation == ELANSPI_90RIGHT_ROTATE)
    {
      x1 = (self->sensor_height - y - 1);
      y1 = x;
    }
  return data_in[y1 * self->sensor_width + x1];
}

static enum elanspi_guess_result
elanspi_guess_image (FpiDeviceElanSpi *self, guint16 *raw_image)
{
  g_autofree guint16 * image_copy = g_malloc0 (self->sensor_height * self->sensor_width * 2);
  guint8 frame_width, frame_height;

  /* make clang happy about div0 */
  frame_width = self->frame_width;
  frame_height = self->frame_height;
  g_assert (frame_width && frame_height);

  memcpy (image_copy, raw_image, self->sensor_height * self->sensor_width * 2);

  gint invalid_percent = (100 * elanspi_correct_with_bg (self, image_copy)) / (self->sensor_height * self->sensor_width);
  gint is_fp = 0, is_empty = 0;

  gint64 mean = 0;
  gint64 sq_stddev = 0;

  for (int j = 0; j < frame_height; j += 1)
    for (int i = 0; i < frame_width; i += 1)
      mean += (gint64) elanspi_lookup_pixel_with_rotation (self, image_copy, j, i);

  mean /= (frame_width * frame_height);

  for (int j = 0; j < frame_height; j += 1)
    for (int i = 0; i < frame_width; i += 1)
      {
        gint64 k = (gint64) elanspi_lookup_pixel_with_rotation (self, image_copy, j, i) - mean;
        sq_stddev += k * k;
      }

  sq_stddev /= (frame_width * frame_height);

  if (invalid_percent < ELANSPI_MAX_REAL_INVALID_PERCENT)
    is_fp += 1;
  if (invalid_percent > ELANSPI_MIN_EMPTY_INVALID_PERCENT)
    is_empty += 1;

  if (sq_stddev > ELANSPI_MIN_REAL_STDDEV)
    is_fp += 1;
  if (sq_stddev < ELANSPI_MAX_EMPTY_STDDEV)
    is_empty += 1;

  fp_dbg ("<guess> stddev=%" G_GUINT64_FORMAT "d, ip=%d, is_fp=%d, is_empty=%d", sq_stddev, invalid_percent, is_fp, is_empty);

  if (is_fp > is_empty)
    return ELANSPI_GUESS_FINGERPRINT;
  else if (is_empty > is_fp)
    return ELANSPI_GUESS_EMPTY;
  else
    return ELANSPI_GUESS_UNKNOWN;
}

/* returns TRUE when the waiting is complete */
static gboolean
elanspi_check_waitupdown_done (FpiDeviceElanSpi *self, enum elanspi_guess_result target)
{
  enum elanspi_guess_result guess = elanspi_guess_image (self, self->last_image);

  if (guess == ELANSPI_GUESS_UNKNOWN)
    return FALSE;
  if (guess == target)
    {
      self->finger_wait_debounce += 1;
      return self->finger_wait_debounce == ELANSPI_MIN_FRAMES_DEBOUNCE;
    }
  else
    {
      self->finger_wait_debounce = 0;
      return FALSE;
    }
}

static int
cmp_u16 (const void *a, const void *b)
{
  return (int) (*(guint16 *) a - *(guint16 *) b);
}

static void
elanspi_process_frame (FpiDeviceElanSpi *self, const guint16 *data_in, guint8 *data_out)
{
  size_t frame_size = self->frame_width * self->frame_height;
  guint16 data_in_sorted[frame_size];

  for (int i = 0, offset = 0; i < self->frame_height; i += 1)
    for (int j = 0; j < self->frame_width; j += 1)
      data_in_sorted[offset++] = elanspi_lookup_pixel_with_rotation (self, data_in, i, j);

  qsort (data_in_sorted, frame_size, 2, cmp_u16);
  guint16 lvl0 = data_in_sorted[0];
  guint16 lvl1 = data_in_sorted[frame_size * 3 / 10];
  guint16 lvl2 = data_in_sorted[frame_size * 65 / 100];
  guint16 lvl3 = data_in_sorted[frame_size - 1];

  lvl1 = MAX (lvl1, lvl0 + 1);
  lvl2 = MAX (lvl2, lvl1 + 1);
  lvl3 = MAX (lvl3, lvl2 + 1);

  for (int i = 0; i < self->frame_height; i += 1)
    {
      for (int j = 0; j < self->frame_width; j += 1)
        {
          guint16 px = elanspi_lookup_pixel_with_rotation (self, data_in, i, j);
          if (px < lvl0)
            {
              px = 0;
            }
          else if (px > lvl3)
            {
              px = 255;
            }
          else
            {
              if (lvl0 <= px && px < lvl1)
                px = (px - lvl0) * 99 / (lvl1 - lvl0);
              else if (lvl1 <= px && px < lvl2)
                px = 99 + ((px - lvl1) * 56 / (lvl2 - lvl1));
              else /* (lvl2 <= px && px <= lvl3) */
                px = 155 + ((px - lvl2) * 100 / (lvl3 - lvl2));
            }
          *data_out = px;
          data_out += 1;
        }
    }
}

static unsigned char
elanspi_fp_assembling_get_pixel (struct fpi_frame_asmbl_ctx *ctx, struct fpi_frame *frame, unsigned int x, unsigned int y)
{
  return frame->data[y * ctx->frame_width + x];
}

static void
elanspi_fp_frame_stitch_and_submit (FpiDeviceElanSpi *self)
{
  g_autoptr(FpImage) img = NULL;
  g_autoptr(FpImage) scaled = NULL;
  struct fpi_frame_asmbl_ctx assembling_ctx = {
    .image_width = (self->frame_width * 3) / 2,

    .frame_width = self->frame_width,
    .frame_height = self->frame_height,

    .get_pixel = elanspi_fp_assembling_get_pixel,
  };

  /* stitch image */
  GSList *frame_start = g_slist_nth (self->fp_frame_list, ELANSPI_SWIPE_FRAMES_DISCARD);

  fpi_do_movement_estimation (&assembling_ctx, frame_start);
  img = fpi_assemble_frames (&assembling_ctx, frame_start);
  scaled = fpi_image_resize (img, 2, 2);

  scaled->flags |= FPI_IMAGE_PARTIAL | FPI_IMAGE_COLORS_INVERTED;

  /* submit image */
  fpi_image_device_image_captured (FP_IMAGE_DEVICE (self), g_steal_pointer (&scaled));

  /* clean out frame data */
  g_slist_free_full (g_steal_pointer (&self->fp_frame_list), g_free);
}

static gint64
elanspi_get_frame_diff_stddev_sq (FpiDeviceElanSpi *self, guint16 *frame1, guint16 *frame2)
{
  gint64 mean = 0;
  gint64 sq_stddev = 0;

  for (int j = 0; j < (self->sensor_height * self->sensor_width); j += 1)
    mean += abs ((int) frame1[j] - (int) frame2[j]);

  g_assert (self->sensor_height && self->sensor_width); /* make clang happy about div0 */
  mean /= (self->sensor_height * self->sensor_width);

  for (int j = 0; j < (self->sensor_height * self->sensor_width); j += 1)
    {
      gint64 k = abs ((int) frame1[j] - (int) frame2[j]) - mean;
      sq_stddev += k * k;
    }

  sq_stddev /= (self->sensor_height * self->sensor_width);

  return sq_stddev;
}

static void
elanspi_fp_frame_handler (FpiSsm *ssm, FpiDeviceElanSpi *self)
{
  g_autofree struct fpi_frame *this_frame = NULL;

  switch (elanspi_guess_image (self, self->last_image))
    {
    case ELANSPI_GUESS_UNKNOWN:
      fp_dbg ("<fp_frame> unknown, ignore...");
      break;

    case ELANSPI_GUESS_EMPTY:
      self->fp_empty_counter += 1;
      fp_dbg ("<fp_frame> got empty");
      if (self->fp_empty_counter > 1)
        {
          fp_dbg ("<fp_frame> have enough debounce");
          if (g_slist_length (self->fp_frame_list) >= ELANSPI_MIN_FRAMES_SWIPE)
            {
              fp_dbg ("<fp_frame> have enough frames, submitting");
              elanspi_fp_frame_stitch_and_submit (self);
            }
          else
            {
              fp_dbg ("<fp_frame> not enough frames, reporting short swipe");
              fpi_image_device_retry_scan (FP_IMAGE_DEVICE (self), FP_DEVICE_RETRY_TOO_SHORT);
            }
          goto finish_capture;
        }
      break;

    case ELANSPI_GUESS_FINGERPRINT:
      if (self->fp_empty_counter && self->fp_frame_list)
        {
          if (self->fp_empty_counter < 1)
            {
              fp_dbg ("<fp_frame> possible bounced fp");
              break;
            }
          else
            {
              fp_dbg ("<fp_frame> too many empties, clearing list");
              g_slist_free_full (g_steal_pointer (&self->fp_frame_list), g_free);
              self->fp_empty_counter = 0;
            }
        }

      if (g_slist_length (self->fp_frame_list) > ELANSPI_MAX_FRAMES_SWIPE)
        {
          fp_dbg ("<fp_frame> have enough frames, exiting now");
          elanspi_fp_frame_stitch_and_submit (self);
          goto finish_capture;
        }

      /* append image */
      this_frame = g_malloc0 (self->sensor_height * self->sensor_width + sizeof (struct fpi_frame));
      elanspi_correct_with_bg (self, self->last_image);
      elanspi_process_frame (self, self->last_image, this_frame->data);

      if (self->fp_frame_list)
        {
          gint difference = elanspi_get_frame_diff_stddev_sq (self, self->last_image, self->prev_frame_image);
          fp_dbg ("<fp_frame> diff = %d", difference);
          if (difference < ELANSPI_MIN_FRAME_TO_FRAME_DIFF)
            {
              fp_dbg ("<fp_frame> ignoring b.c. difference is too small");
              break;
            }
        }
      self->fp_frame_list = g_slist_prepend (self->fp_frame_list, g_steal_pointer (&this_frame));
      memcpy (self->prev_frame_image, self->last_image, self->sensor_height * self->sensor_width * 2);
      break;
    }

  if (self->sensor_id == 0xe)
    fpi_ssm_jump_to_state_delayed (ssm, ELANSPI_FPCAPT_FP_CAPTURE, ELANSPI_HV_SENSOR_FRAME_DELAY);
  else
    fpi_ssm_jump_to_state (ssm, ELANSPI_FPCAPT_FP_CAPTURE);

  return;

finish_capture:
  /* prepare for wait up */
  self->finger_wait_debounce = 0;
  fpi_ssm_jump_to_state (ssm, ELANSPI_FPCAPT_WAITUP_CAPTURE);
  return;

}

static void
elanspi_fp_capture_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);
  FpiSsm *chld = NULL;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ELANSPI_FPCAPT_INIT:
      self->finger_wait_debounce = 0;

      fpi_ssm_next_state (ssm);
      return;

    case ELANSPI_FPCAPT_WAITDOWN_CAPTURE:
    case ELANSPI_FPCAPT_WAITUP_CAPTURE:
    case ELANSPI_FPCAPT_FP_CAPTURE:
      /* check if we are deactivating */
      if (self->deactivating)
        {
          fp_dbg ("<capture> got deactivate; exiting");

          self->deactivating = FALSE;
          fpi_ssm_mark_completed (ssm);

          /* mark deactivate done */
          fpi_image_device_deactivate_complete (FP_IMAGE_DEVICE (dev), NULL);

          return;
        }
      /* if sensor is hv */
      if (self->sensor_id == 0xe)
        chld = fpi_ssm_new (dev, elanspi_capture_hv_handler, ELANSPI_CAPTHV_NSTATES);
      else
        chld = fpi_ssm_new (dev, elanspi_capture_old_handler, ELANSPI_CAPTOLD_NSTATES);
      fpi_ssm_silence_debug (chld);
      fpi_ssm_start_subsm (ssm, chld);
      return;

    case ELANSPI_FPCAPT_WAITDOWN_PROCESS:
      if (!elanspi_check_waitupdown_done (self, ELANSPI_GUESS_FINGERPRINT))
        {
          /* take another image */
          fpi_ssm_jump_to_state (ssm, ELANSPI_FPCAPT_WAITDOWN_CAPTURE);
          return;
        }

      /* prepare to take actual image */
      self->finger_wait_debounce = 0;
      g_slist_free_full (g_steal_pointer (&self->fp_frame_list), g_free);
      self->fp_empty_counter = 0;

      /* report finger status */
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (self), TRUE);

      /* jump */
      fpi_ssm_jump_to_state (ssm, ELANSPI_FPCAPT_FP_CAPTURE);
      return;

    case ELANSPI_FPCAPT_FP_PROCESS:
      elanspi_fp_frame_handler (ssm, self);
      return;

    case ELANSPI_FPCAPT_WAITUP_PROCESS:
      if (!elanspi_check_waitupdown_done (self, ELANSPI_GUESS_EMPTY))
        {
          /* take another image */
          fpi_ssm_jump_to_state (ssm, ELANSPI_FPCAPT_WAITUP_CAPTURE);
          return;
        }

      /* Immediately set capturing to FALSE so that when report_finger_status tries to re-start
       * capturing in enroll we don't hit the assert since the old SSM is about to stop. */
      self->capturing = FALSE;
      fpi_image_device_report_finger_status (FP_IMAGE_DEVICE (self), FALSE);

      /* finish */
      fpi_ssm_mark_completed (ssm);
      return;
    }
}

static void
elanspi_open (FpImageDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);
  GError *err = NULL;

  G_DEBUG_HERE ();

  int spi_fd = open (fpi_device_get_udev_data (FP_DEVICE (dev), FPI_DEVICE_UDEV_SUBTYPE_SPIDEV), O_RDWR);

  if (spi_fd < 0)
    {
      g_set_error (&err, G_IO_ERROR, g_io_error_from_errno (errno), "unable to open spi");
      fpi_image_device_open_complete (dev, err);
      return;
    }

  self->spi_fd = spi_fd;

  fpi_image_device_open_complete (dev, NULL);
}

static void
elanspi_close (FpImageDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);

  if (self->spi_fd >= 0)
    {
      close (self->spi_fd);
      self->spi_fd = -1;
    }
  fpi_image_device_close_complete (dev, NULL);
}

static void
elanspi_init_finish (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);

  G_DEBUG_HERE ();
  fpi_image_device_activate_complete (idev, error);
}

static void
elanspi_activate (FpImageDevice *dev)
{
  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (dev), elanspi_init_ssm_handler, ELANSPI_INIT_NSTATES);

  fpi_ssm_start (ssm, elanspi_init_finish);
}

static void
elanspi_deactivate (FpImageDevice *dev)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);

  if (self->capturing)
    {
      self->deactivating = TRUE;
      fp_dbg ("<deactivate> waiting capture to stop");
    }
  else
    {
      fpi_image_device_deactivate_complete (dev, NULL);
    }
}

static void
elanspi_fp_capture_finish (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);

  self->capturing = FALSE;

  if (self->deactivating)
    {
      /* finish deactivate */
      if (error)
        g_error_free (error);
      self->deactivating = FALSE;
      fpi_image_device_deactivate_complete (idev, NULL);
      return;
    }

  /* if there was an error, report it */
  if (error)
    fpi_image_device_session_error (idev, error);
}

static void
elanspi_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (dev);

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      g_assert (self->capturing == FALSE);

      /* start capturer */
      self->capturing = TRUE;
      fpi_ssm_start (fpi_ssm_new (FP_DEVICE (dev),
                                  elanspi_fp_capture_ssm_handler,
                                  ELANSPI_FPCAPT_NSTATES),
                     elanspi_fp_capture_finish);

      fp_dbg ("<change_state> started capturer");
    }
  else
    {
      /* todo: other states? */
    }
}

static void
fpi_device_elanspi_init (FpiDeviceElanSpi *self)
{
  self->spi_fd = -1;
  self->sensor_id = 0xff;
}

static void
fpi_device_elanspi_finalize (GObject *this)
{
  FpiDeviceElanSpi *self = FPI_DEVICE_ELANSPI (this);

  g_clear_pointer (&self->bg_image, g_free);
  g_clear_pointer (&self->last_image, g_free);
  g_clear_pointer (&self->prev_frame_image, g_free);
  g_slist_free_full (g_steal_pointer (&self->fp_frame_list), g_free);

  G_OBJECT_CLASS (fpi_device_elanspi_parent_class)->finalize (this);
}

static void
fpi_device_elanspi_class_init (FpiDeviceElanSpiClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "elanspi";
  dev_class->full_name = "ElanTech Embedded Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_UDEV;
  dev_class->id_table = elanspi_id_table;
  dev_class->scan_type = FP_SCAN_TYPE_SWIPE;
  dev_class->nr_enroll_stages = 7;       /* these sensors are very hit or miss, may as well record a few extras */

  img_class->score_threshold = 24;
  img_class->img_open = elanspi_open;
  img_class->activate = elanspi_activate;
  img_class->deactivate = elanspi_deactivate;
  img_class->change_state = elanspi_change_state;
  img_class->img_close = elanspi_close;

  G_OBJECT_CLASS (klass)->finalize = fpi_device_elanspi_finalize;
}
