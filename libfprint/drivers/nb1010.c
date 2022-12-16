/*
 * Next Biometrics driver for libfprint
 *
 * Copyright (C) 2021 Huan Wang <fredwanghuan@gmail.com>
 * Copyright (C) 2011-2012 Andrej Krutak <dev@andree.sk>
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

#define FP_COMPONENT "nb1010"
#include "fpi-log.h"

#include "drivers_api.h"

#define FRAME_HEIGHT 180
#define FRAME_WIDTH 256

#define NB1010_EP_OUT 0x02 | FPI_USB_ENDPOINT_OUT
#define NB1010_EP_IN 0x03 | FPI_USB_ENDPOINT_IN

#define NB1010_SENSITIVITY_BIT 12

#define NB1010_CMD_RECV_LEN 16
#define NB1010_CAPTURE_RECV_LEN 540
#define NB1010_CAPTURE_HEADER_LEN 25

#define NB1010_LINE_PER_PARTIAL 2
#define NB1010_N_PARTIAL (FRAME_HEIGHT / NB1010_LINE_PER_PARTIAL)

#define NB1010_DEFAULT_TIMEOUT 500
#define NB1010_TRANSITION_DELAY 50

/* Loop ssm states */
enum {
  M_WAIT_PRINT,
  M_REQUEST_PRINT,
  M_CHECK_PRINT,
  M_READ_PRINT_PRESTART,
  M_READ_PRINT_START,
  M_READ_PRINT_POLL,
  M_SUBMIT_PRINT,

  /* Number of states */
  M_LOOP_NUM_STATES,
};

/*
 * The Follow Commands are obtained by decoding the usbcap, so it does not expose all the command available to the device.
 * Known:
 * 1. every command starts with 0x80
 * 2. second byte is the comand, third byte is the seqence nubmer, init with rand, gets incremented
 *    everytime a new instruction is sent to the device. However device does not care or check the sequence, just echo back
 *    whatever chosen by the host.
 * 3. cmd: 0x07 check, expect [0x80, 0x29...] as response
 * 4. cmd: 0x16 ???, expect [0x80, 0x20...] as response. Happens during device init.
 * 5. cmd: 0x13 print device, expect [0x80, 0x23...] as response. Response contains the device string
 * 6. cmd: 0x38 check finger, expect [0x80, 0x37...] as response. The 14th byte indicate whether finger present [0-255]
 * 7. cmd: 0x0d ???, expect [0x80, 0x20...] as response. Happens before capture.
 * 8. cmd: 0x12 capture, expect [0x80, 0x20...] as response. After capture read 90 times in sequence to get all the frame.
 */

static guint8 nb1010_cmd_check_finger[] = {
  0x80, 0x38, 0x01, 0x00, 0x08, 0x00, 0x00, 0x00,
};

/* pre capture, dont know what does it do, but appears everytime a capture begins */
static guint8 nb1010_cmd_precapture[] = {
  0x80, 0x0d, 0x03, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
};

static guint8 nb1010_cmd_capture[] = {
  0x80, 0x12, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00,
  0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
};

struct _FpiDeviceNb1010
{
  FpImageDevice parent;
  FpiSsm       *ssm;
  guint8       *scanline_buf;
  gboolean      deactivating;
  int           partial_received;
};
G_DECLARE_FINAL_TYPE (FpiDeviceNb1010, fpi_device_nb1010, FPI, DEVICE_NB1010, FpImageDevice);
G_DEFINE_TYPE (FpiDeviceNb1010, fpi_device_nb1010, FP_TYPE_IMAGE_DEVICE);

static void
nb1010_dev_init (FpImageDevice *dev)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);
  GError *error = NULL;

  g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);

  self->scanline_buf = g_malloc0 (FRAME_WIDTH * FRAME_HEIGHT);

  fpi_image_device_open_complete (dev, error);
  fp_dbg ("nb1010 Initialized");
}

static void
nb1010_dev_deinit (FpImageDevice *dev)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);
  GError *error = NULL;

  g_clear_pointer (&self->scanline_buf, g_free);

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error);
  fpi_image_device_close_complete (dev, error);
  fp_dbg ("nb1010 Deinitialized");
}

static void
nb1010_dev_activate (FpImageDevice *dev)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);

  self->deactivating = FALSE;

  fpi_image_device_activate_complete (dev, NULL);
  fp_dbg ("nb1010 Activated");
}

static void
nb1010_dev_deactivated (FpImageDevice *dev, GError * err)
{
  fpi_image_device_deactivate_complete (dev, err);
  fp_dbg ("nb1010 Deactivated");
}

static void
nb1010_dev_deactivate (FpImageDevice *dev)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);

  self->deactivating = TRUE;
  if (self->ssm == NULL)
    nb1010_dev_deactivated (dev, NULL);
}

static void
nb1010_request_fingerprint (FpiDeviceNb1010 *dev)
{
  FpiUsbTransfer *transfer = NULL;

  transfer = fpi_usb_transfer_new (FP_DEVICE ( dev));
  transfer->short_is_error = TRUE;
  transfer->ssm = dev->ssm;

  fpi_usb_transfer_fill_bulk_full (transfer, NB1010_EP_OUT,
                                   nb1010_cmd_check_finger, G_N_ELEMENTS (nb1010_cmd_check_finger),
                                   NULL);
  fpi_usb_transfer_submit (transfer, NB1010_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
nb1010_check_fingerprint_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                             gpointer unused_data, GError *error)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  if (transfer->buffer[NB1010_SENSITIVITY_BIT] > 0x30)
    fpi_ssm_next_state (transfer->ssm);
  else
    fpi_ssm_jump_to_state (transfer->ssm, M_WAIT_PRINT);
}

static void
nb1010_cmd_check_fingerprint (FpiDeviceNb1010 *dev)
{
  FpiUsbTransfer *transfer = NULL;

  transfer = fpi_usb_transfer_new (FP_DEVICE ( dev));
  transfer->short_is_error = TRUE;
  transfer->ssm = dev->ssm;

  fpi_usb_transfer_fill_bulk (transfer, NB1010_EP_IN, NB1010_CMD_RECV_LEN);
  fpi_usb_transfer_submit (transfer, NB1010_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           nb1010_check_fingerprint_cb, NULL);
}

static void
nb1010_read_ignore_data_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                            gpointer unused_data, GError *error)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);
  FpiUsbTransfer *new_transfer = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  new_transfer = fpi_usb_transfer_new ( dev );
  new_transfer->short_is_error = TRUE;
  new_transfer->ssm = transfer->ssm;

  fpi_usb_transfer_fill_bulk (new_transfer, NB1010_EP_IN, NB1010_CMD_RECV_LEN);
  fpi_usb_transfer_submit (new_transfer, NB1010_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           fpi_ssm_usb_transfer_cb, NULL);
}

static void
nb1010_write_ignore_read (FpiDeviceNb1010 *dev, guint8 *buf, gsize len)
{
  FpiUsbTransfer *transfer = NULL;

  transfer = fpi_usb_transfer_new (FP_DEVICE ( dev));
  transfer->short_is_error = TRUE;
  transfer->ssm = dev->ssm;

  fpi_usb_transfer_fill_bulk_full (transfer, NB1010_EP_OUT, buf, len, NULL);
  fpi_usb_transfer_submit (transfer, NB1010_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           nb1010_read_ignore_data_cb, NULL);
}


static void
nb1010_read_capture_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                        gpointer unused_data, GError *error)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }
  if (self->deactivating)
    {
      fpi_ssm_mark_completed (transfer->ssm);
      return;
    }

  g_assert (transfer->actual_length == NB1010_CAPTURE_RECV_LEN);

  size_t offset = self->partial_received * NB1010_LINE_PER_PARTIAL * FRAME_WIDTH;

  memcpy (self->scanline_buf + offset,
          transfer->buffer + NB1010_CAPTURE_HEADER_LEN, NB1010_LINE_PER_PARTIAL * FRAME_WIDTH);

  self->partial_received++;
  if (self->partial_received == NB1010_N_PARTIAL)
    {
      fpi_ssm_next_state (transfer->ssm);
      return;
    }

  fpi_usb_transfer_submit (fpi_usb_transfer_ref (transfer), NB1010_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           nb1010_read_capture_cb, NULL);
}

static void
nb1010_read_capture (FpiDeviceNb1010 *dev)
{
  FpiUsbTransfer *transfer = NULL;

  transfer = fpi_usb_transfer_new ( FP_DEVICE ( dev));
  transfer->short_is_error = TRUE;
  transfer->ssm = dev->ssm;

  fpi_usb_transfer_fill_bulk (transfer, NB1010_EP_IN, NB1010_CAPTURE_RECV_LEN);
  fpi_usb_transfer_submit (transfer, NB1010_DEFAULT_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (dev)),
                           nb1010_read_capture_cb, NULL);
}

static int
submit_image (FpiSsm        *ssm,
              FpImageDevice *dev)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);
  FpImage *img;

  img = fp_image_new (FRAME_WIDTH, FRAME_HEIGHT);
  if (img == NULL)
    return 0;

  memcpy (img->data, self->scanline_buf, FRAME_WIDTH * FRAME_HEIGHT);
  fpi_image_device_image_captured (dev, img);

  return 1;
}

static void
m_loop_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  fp_dbg ("nb1010 ssm complete cb");

  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (_dev);

  self->ssm = NULL;

  if (self->deactivating)
    nb1010_dev_deactivated (dev, error);
  else if (error != NULL)
    fpi_image_device_session_error (dev, error);
}

static void
m_loop_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (_dev);

  if (self->deactivating)
    {
      fp_dbg ("deactivating, marking completed");
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case M_WAIT_PRINT:
      /* Wait fingerprint scanning */
      fpi_ssm_next_state_delayed (ssm, NB1010_TRANSITION_DELAY);
      break;

    case M_REQUEST_PRINT:
      nb1010_request_fingerprint (self);
      break;

    case M_CHECK_PRINT:
      nb1010_cmd_check_fingerprint (self);
      break;

    case M_READ_PRINT_PRESTART:
      fpi_image_device_report_finger_status (dev, TRUE);
      nb1010_write_ignore_read (self, nb1010_cmd_precapture, G_N_ELEMENTS (nb1010_cmd_precapture));
      break;

    case M_READ_PRINT_START:
      self->partial_received = 0;
      nb1010_write_ignore_read (self, nb1010_cmd_capture, G_N_ELEMENTS (nb1010_cmd_capture));
      break;

    case M_READ_PRINT_POLL:
      nb1010_read_capture (self);
      break;

    case M_SUBMIT_PRINT:
      if (submit_image (ssm, dev))
        {
          fpi_ssm_mark_completed (ssm);
          fpi_image_device_report_finger_status (dev, FALSE);
        }
      else
        {
          fpi_ssm_jump_to_state (ssm, M_WAIT_PRINT);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
nb1010_dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpiDeviceNb1010 *self = FPI_DEVICE_NB1010 (dev);
  FpiSsm *ssm_loop;

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    {
      ssm_loop = fpi_ssm_new (FP_DEVICE (dev), m_loop_state, M_LOOP_NUM_STATES);
      self->ssm = ssm_loop;
      fpi_ssm_start (ssm_loop, m_loop_complete);
    }
}


static const FpIdEntry id_table[] = {
  { .vid = 0x298d,  .pid = 0x1010, },
  { .vid = 0,  .pid = 0,  .driver_data = 0 },
};

static void
fpi_device_nb1010_init (FpiDeviceNb1010 *self)
{
}

static void
fpi_device_nb1010_class_init (FpiDeviceNb1010Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "NextBiometrics NB-1010-U";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_height = FRAME_HEIGHT;
  img_class->img_width = FRAME_WIDTH;

  img_class->score_threshold = 24;

  img_class->img_open = nb1010_dev_init;
  img_class->img_close = nb1010_dev_deinit;
  img_class->activate = nb1010_dev_activate;
  img_class->deactivate = nb1010_dev_deactivate;
  img_class->change_state = nb1010_dev_change_state;
}
