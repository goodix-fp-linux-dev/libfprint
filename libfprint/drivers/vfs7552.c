/*
 * Validity Sensors, Inc. VFS7552 Fingerprint Reader driver for libfprint
 * Copyright (C) 2013 Arseniy Lartsev <arseniy@chalmers.se>
 *                    AceLan Kao <acelan.kao@canonical.com>
 *               2018 Mark Harfouche <mark.harfouche@gmail.com>
 *               2020 Julius Piso <julius@piso.at>
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

#define FP_COMPONENT "vfs7552"

#include "drivers_api.h"
#include "vfs7552_proto.h"

#define VFS7552_CONTROL_PIXELS (8)
#define VFS7552_LINE_SIZE (VFS7552_IMAGE_WIDTH + VFS7552_CONTROL_PIXELS)
#define VFS7552_IMAGE_CHUNKS (3)

#define CAPTURE_VARIANCE_THRESHOLD 1200
#define FINGER_OFF_VARIANCE_THRESHOLD 100
#define NOISE_VARIANCE_THRESHOLD 4000

/* =================== sync/async USB transfer sequence ==================== */

enum {
  ACTION_SEND,
  ACTION_RECEIVE,
};

struct usb_action
{
  int            type;
  const char    *name;
  int            endpoint;
  int            size;
  unsigned char *data;
  int            correct_reply_size;
};

#define SEND(ENDPOINT, COMMAND) \
  {                             \
    .type = ACTION_SEND,        \
    .endpoint = ENDPOINT,       \
    .name = #COMMAND,           \
    .size = sizeof (COMMAND),   \
    .data = COMMAND             \
  },

#define RECV(ENDPOINT, SIZE)  \
  {                           \
    .type = ACTION_RECEIVE,   \
    .endpoint = ENDPOINT,     \
    .size = SIZE,             \
    .data = NULL              \
  },

#define RECV_CHECK(ENDPOINT, SIZE, EXPECTED) \
  {                                          \
    .type = ACTION_RECEIVE,                  \
    .endpoint = ENDPOINT,                    \
    .size = SIZE,                            \
    .data = EXPECTED,                        \
    .correct_reply_size = sizeof (EXPECTED)  \
  },

#define RECV_CHECK_SIZE(ENDPOINT, SIZE, EXPECTED) \
  {                                               \
    .type = ACTION_RECEIVE,                       \
    .endpoint = ENDPOINT,                         \
    .size = SIZE,                                 \
    .data = NULL,                                 \
    .correct_reply_size = sizeof (EXPECTED)       \
  },

struct usbexchange_data
{
  int                stepcount;
  struct usb_action *actions;
  FpiUsbTransfer    *last_recv;
  int                timeout;
};

/* ================== Class Definition =================== */

struct _FpDeviceVfs7552
{
  FpImageDevice           parent;

  FpiImageDeviceState     dev_state;
  FpiImageDeviceState     dev_state_next;
  gboolean                background_captured;
  unsigned char           background[VFS7552_IMAGE_SIZE];
  unsigned char           image[VFS7552_IMAGE_SIZE];
  gint                    lines_captured;

  gboolean                deactivating;
  gboolean                loop_running;
  struct usbexchange_data init_sequence;
  FpiUsbTransfer         *flying_transfer;
};

G_DECLARE_FINAL_TYPE (FpDeviceVfs7552, fpi_device_vfs7552, FPI, DEVICE_VFS7552,
                      FpImageDevice);
G_DEFINE_TYPE (FpDeviceVfs7552, fpi_device_vfs7552, FP_TYPE_IMAGE_DEVICE);

/* ======================= States ======================== */

enum open_states {
  DEV_OPEN_START,
  DEV_OPEN_NUM_STATES
};

enum activate_states {
  ACTIVATE_INIT,
  ACTIVATE_INTERRUPT_QUERY,
  ACTIVATE_INTERRUPT_CHECK,
  ACTIVATE_FINALIZE,
  ACTIVATE_NUM_STATES
};

enum capture_states {
  CAPTURE_QUERY_DATA_READY,
  CAPTURE_CHECK_DATA_READY,
  CAPTURE_REQUEST_CHUNK,
  CAPTURE_READ_CHUNK,
  CAPTURE_COMPLETE,
  CAPTURE_FINALIZE,
  CAPTURE_NUM_STATES
};

enum deactivate_states {
  DEACTIVATE_ENTER,
  DEACTIVATE_DISABLE_SENSOR,
  DEACTIVATE_NUM_STATES
};

/* ============== USB Sequence Definitions =============== */

struct usb_action vfs7552_initialization[] = {
  SEND (VFS7552_OUT_ENDPOINT, vfs7552_cmd_01)
  RECV_CHECK_SIZE (VFS7552_IN_ENDPOINT, 64, vfs7552_cmd_01_recv)

  SEND (VFS7552_OUT_ENDPOINT, vfs7552_cmd_19)
  RECV_CHECK_SIZE (VFS7552_IN_ENDPOINT, 128, vfs7552_cmd_19_recv)

  SEND (VFS7552_OUT_ENDPOINT, vfs7552_init_00)
  RECV_CHECK (VFS7552_IN_ENDPOINT, 64, VFS7552_NORMAL_REPLY)

  SEND (VFS7552_OUT_ENDPOINT, vfs7552_init_01)
  RECV_CHECK (VFS7552_IN_ENDPOINT, 64, VFS7552_NORMAL_REPLY)

  SEND (VFS7552_OUT_ENDPOINT, vfs7552_init_02)
  RECV_CHECK (VFS7552_IN_ENDPOINT, 64, vfs7552_init_02_recv)

  SEND (VFS7552_OUT_ENDPOINT, vfs7552_init_03)
  RECV_CHECK_SIZE (VFS7552_IN_ENDPOINT, 64, vfs7552_init_03_recv)

  SEND (VFS7552_OUT_ENDPOINT, vfs7552_init_04)
  RECV_CHECK (VFS7552_IN_ENDPOINT, 64, VFS7552_NORMAL_REPLY)

  /*
   * Windows driver does this and it works
   * But in this driver this call never returns...
   * RECV(VFS7552_IN_ENDPOINT_CTRL2, 8)
   */
};

struct usb_action vfs7552_stop_capture[] = {
  SEND (VFS7552_OUT_ENDPOINT, vfs7552_cmd_04)
  RECV_CHECK (VFS7552_IN_ENDPOINT, 64, VFS7552_NORMAL_REPLY)

  SEND (VFS7552_OUT_ENDPOINT, vfs7552_cmd_52)
  RECV_CHECK (VFS7552_IN_ENDPOINT, 64, VFS7552_NORMAL_REPLY)
};

struct usb_action vfs7552_initiate_capture[] = {
  SEND (VFS7552_OUT_ENDPOINT, vfs7552_image_start)
  RECV_CHECK_SIZE (VFS7552_IN_ENDPOINT, 2048, vfs7552_image_start_resp)
};

struct usb_action vfs7552_wait_finger_init[] = {
  RECV_CHECK_SIZE (VFS7552_INTERRUPT_ENDPOINT, 8, interrupt_ok)
};

struct usb_action vfs7552_data_ready_query[] = {
  SEND (VFS7552_OUT_ENDPOINT, vfs7552_is_image_ready)
  RECV_CHECK_SIZE (VFS7552_IN_ENDPOINT, 64, vfs7552_is_image_ready_resp_ready)
};

struct usb_action vfs7552_request_chunk[] = {
  SEND (VFS7552_OUT_ENDPOINT, vfs7552_read_image_chunk)
};

/* ================== USB Communication ================== */

static void
async_send_cb (FpiUsbTransfer *transfer, FpDevice *device,
               gpointer user_data, GError *error)
{
  struct usbexchange_data *data = fpi_ssm_get_data (transfer->ssm);
  struct usb_action *action;

  g_assert (!(fpi_ssm_get_cur_state (transfer->ssm) >= data->stepcount));

  action = &data->actions[fpi_ssm_get_cur_state (transfer->ssm)];
  g_assert (!(action->type != ACTION_SEND));

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* success */
  fpi_ssm_next_state (transfer->ssm);
}

static void
async_recv_cb (FpiUsbTransfer *transfer, FpDevice *device,
               gpointer user_data, GError *error)
{
  struct usbexchange_data *data = fpi_ssm_get_data (transfer->ssm);
  struct usb_action *action;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  g_assert (!(fpi_ssm_get_cur_state (transfer->ssm) >= data->stepcount));

  action = &data->actions[fpi_ssm_get_cur_state (transfer->ssm)];
  g_assert (!(action->type != ACTION_RECEIVE));

  if (action->data != NULL)
    {
      if (transfer->actual_length != action->correct_reply_size)
        {
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Got %d bytes instead of %d",
                                                         (gint) transfer->actual_length,
                                                         action->correct_reply_size));
          return;
        }
      if (memcmp (transfer->buffer, action->data,
                  action->correct_reply_size) != 0)
        {
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Received a wrong reply from the driver."));
          return;
        }
    }
  else
    {
      fp_dbg ("Got %d bytes out of %d",
              (gint) transfer->actual_length,
              (gint) transfer->length);
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
usbexchange_loop (FpiSsm *ssm, FpDevice *_dev)
{
  struct usbexchange_data *data = fpi_ssm_get_data (ssm);
  struct usb_action *action = &data->actions[fpi_ssm_get_cur_state (ssm)];
  FpiUsbTransfer *transfer;

  g_assert (fpi_ssm_get_cur_state (ssm) < data->stepcount);

  switch (action->type)
    {
    case ACTION_SEND:
      fp_dbg ("Sending %s", action->name);
      transfer = fpi_usb_transfer_new (_dev);
      fpi_usb_transfer_fill_bulk_full (transfer, action->endpoint,
                                       action->data, action->size,
                                       NULL);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_submit (transfer, data->timeout, NULL,
                               async_send_cb, NULL);
      break;

    case ACTION_RECEIVE:
      fp_dbg ("Receiving %d bytes", action->size);
      transfer = fpi_usb_transfer_new (_dev);
      fpi_usb_transfer_fill_bulk (transfer, action->endpoint,
                                  action->size);
      transfer->ssm = ssm;
      fpi_usb_transfer_submit (transfer, data->timeout, NULL,
                               async_recv_cb, NULL);

      g_clear_pointer (&data->last_recv, fpi_usb_transfer_unref);
      data->last_recv = fpi_usb_transfer_ref (transfer);
      break;

    default:
      g_assert_not_reached ();
      fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_GENERAL));
      return;
    }
}

static void
usb_exchange_async (FpiSsm                  *ssm,
                    struct usbexchange_data *data,
                    const char              *exchange_name)
{
  FpiSsm *subsm = fpi_ssm_new_full (fpi_ssm_get_device (ssm),
                                    usbexchange_loop,
                                    data->stepcount,
                                    data->stepcount,
                                    exchange_name);

  fpi_ssm_set_data (subsm, data, NULL);
  fpi_ssm_start_subsm (ssm, subsm);
}

/* =========== Image Capturing and Processing ============ */

enum {
  CHUNK_READ_FINISHED,
  CHUNK_READ_NEED_MORE,
  CHUNK_READ_ERROR
};

static int
clean_image (FpDeviceVfs7552 *self)
{
  fp_dbg ("Cleaning image");
  unsigned int sum = 0;

  for (int i = 0; i < VFS7552_IMAGE_SIZE; i++)
    {
      if (self->background[i] < self->image[i])
        self->image[i] = 0;
      else
        self->image[i] = self->background[i] - self->image[i];
      if ((int) (self->image[i]) * 4 > 255)
        self->image[i] = 255;
      else
        self->image[i] *= 4;

      sum += self->image[i];
    }

  if (sum == 0)
    {
      fp_dbg ("frame darker than background; finger present during calibration?");
      // Retake an image of the background at the next opportunity.
      self->background_captured = FALSE;
      return -1;
    }
  return 0;
}

static int
process_chunk (FpDeviceVfs7552 *self, FpiUsbTransfer *transfer)
{
  unsigned char *ptr;
  int n_bytes_in_chunk;
  int n_lines;
  int i;

  if (transfer->actual_length < 6)
    return CHUNK_READ_ERROR;

  ptr = transfer->buffer;
  n_bytes_in_chunk = ptr[2] + ptr[3] * 256;

  if (transfer->actual_length < 6 + n_bytes_in_chunk)
    return CHUNK_READ_ERROR;

  ptr = ptr + 6;
  n_lines = n_bytes_in_chunk / VFS7552_LINE_SIZE;
  if (n_lines + self->lines_captured > VFS7552_IMAGE_HEIGHT)
    {
      g_warning ("Device sent more lines that were expected! Aborting.");
      return CHUNK_READ_ERROR;
    }

  for (i = 0; i < n_lines; i++)
    {
      ptr = ptr + VFS7552_CONTROL_PIXELS;
      memcpy (&self->image[self->lines_captured * VFS7552_IMAGE_WIDTH], ptr, VFS7552_IMAGE_WIDTH);
      ptr = ptr + VFS7552_IMAGE_WIDTH;
      self->lines_captured += 1;
    }

  if (self->lines_captured == VFS7552_IMAGE_HEIGHT)
    return CHUNK_READ_FINISHED;
  else
    return CHUNK_READ_NEED_MORE;
}

static void
chunk_capture_callback (FpiUsbTransfer *transfer, FpDevice *device,
                        gpointer user_data, GError *error)
{
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (device);

  if (error)
    {
      if (!self->deactivating)
        {
          fp_err ("Failed to capture data");
          fpi_ssm_mark_failed (transfer->ssm, error);
        }
      else
        {
          // Clear the cancel error, because we are handling deactivation separately
          g_error_free (error);
          fpi_ssm_mark_completed (transfer->ssm);
        }
    }
  else
    {
      switch (process_chunk (self, transfer))
        {
        case CHUNK_READ_FINISHED:
          fpi_ssm_next_state (transfer->ssm);
          break;

        case CHUNK_READ_NEED_MORE:
          fpi_ssm_jump_to_state (transfer->ssm, CAPTURE_REQUEST_CHUNK);
          break;

        case CHUNK_READ_ERROR:
          fpi_ssm_mark_failed (transfer->ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Failed to decode image packet of length %d",
                                                         (int) transfer->actual_length));
        }
    }
}

static void
capture_chunk_async (FpiSsm *ssm, FpDevice *_dev, guint timeout)
{
  FpiUsbTransfer *transfer;

  transfer = fpi_usb_transfer_new (_dev);

  fpi_usb_transfer_fill_bulk (transfer, VFS7552_IN_ENDPOINT,
                              VFS7552_RECEIVE_BUF_SIZE);

  transfer->ssm = ssm;
  fpi_usb_transfer_submit (transfer, timeout, NULL,
                           chunk_capture_callback, NULL);
}

static void
reset_state (FpDeviceVfs7552 *self)
{
  self->lines_captured = 0;
}

/* ================ Run States ================= */

static void
deactivate_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEACTIVATE_ENTER:
      fpi_ssm_next_state_delayed (ssm, 10);
      break;

    case DEACTIVATE_DISABLE_SENSOR:
      self->init_sequence.stepcount = G_N_ELEMENTS (vfs7552_stop_capture);
      self->init_sequence.actions = vfs7552_stop_capture;
      self->init_sequence.timeout = 1000;
      usb_exchange_async (ssm, &self->init_sequence, "STOP CAPTURE");
      break;
    }
}

static void
capture_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpDeviceVfs7552 *self;
  unsigned char *receive_buf;
  int variance_before;
  int variance_after;

  self = FPI_DEVICE_VFS7552 (_dev);
  if (self->deactivating)
    {
      fp_dbg ("deactivating, marking completed");
      fpi_ssm_mark_completed (ssm);
      return;
    }
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_QUERY_DATA_READY:
      self->init_sequence.stepcount = G_N_ELEMENTS (vfs7552_data_ready_query);
      self->init_sequence.actions = vfs7552_data_ready_query;
      self->init_sequence.timeout = 0; // Do not time out
      usb_exchange_async (ssm, &self->init_sequence, "QUERY DATA READY");
      break;

    case CAPTURE_CHECK_DATA_READY:
      receive_buf = ((unsigned char *) self->init_sequence.last_recv->buffer);

      if (receive_buf[0] == vfs7552_is_image_ready_resp_not_ready[0])
        {
          fpi_ssm_jump_to_state (ssm, CAPTURE_QUERY_DATA_READY);
        }
      else if (receive_buf[0] == vfs7552_is_image_ready_resp_ready[0])
        {
          reset_state (self);
          fpi_ssm_next_state (ssm);
        }
      else if (receive_buf[0] == vfs7552_is_image_ready_resp_finger_off[0])
        {
          fp_dbg ("finger off response received");
          if (self->dev_state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
            {
              reset_state (self);
              fpi_ssm_next_state (ssm);
            }
          else
            {
              fpi_ssm_jump_to_state (ssm, CAPTURE_FINALIZE);
            }
        }
      else
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Unknown response 0x%02x",
                                                         receive_buf[0]));
        }
      break;

    case CAPTURE_REQUEST_CHUNK:
      self->init_sequence.stepcount = G_N_ELEMENTS (vfs7552_request_chunk);
      self->init_sequence.actions = vfs7552_request_chunk;
      self->init_sequence.timeout = 1000;
      usb_exchange_async (ssm, &self->init_sequence, "REQUEST CHUNK");
      break;

    case CAPTURE_READ_CHUNK:
      capture_chunk_async (ssm, _dev, 1000);
      break;

    case CAPTURE_COMPLETE:
      // Store the image as a background reference, if the variance is below the finger off threshold.
      if (!self->background_captured)
        {
          // Calculate the variance of the captured image
          variance_before = fpi_std_sq_dev (self->image, VFS7552_IMAGE_SIZE);
          fp_dbg ("variance_before = %d\n", variance_before);
          self->background_captured = TRUE;
          memcpy (self->background, self->image, VFS7552_IMAGE_SIZE);
          fp_dbg ("background stored");
          fpi_ssm_jump_to_state (ssm, CAPTURE_QUERY_DATA_READY);
          break;
        }

      clean_image (self);
      variance_after = fpi_std_sq_dev (self->image, VFS7552_IMAGE_SIZE);
      fp_dbg ("variance_after = %d\n", variance_after);

      if (self->dev_state == FPI_IMAGE_DEVICE_STATE_CAPTURE ||
          self->dev_state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
        {
          // If the finger is placed on the sensor, the variance should ideally increase above a certain
          // threshold. Otherwise request a new image and test again. Additionally we want to ensure
          // that we don't capture prints with a way too high noise level (this sometimes happens).
          if (variance_after > CAPTURE_VARIANCE_THRESHOLD && variance_after < NOISE_VARIANCE_THRESHOLD)
            fpi_ssm_mark_completed (ssm);
          else
            fpi_ssm_jump_to_state (ssm, CAPTURE_QUERY_DATA_READY);
        }
      else if (self->dev_state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF)
        {
          // If the finger is removed from the sensor, the variance should ideally drop below a certain
          // threshold. Otherwise request a new image and test again.
          if (variance_after < FINGER_OFF_VARIANCE_THRESHOLD)
            fpi_ssm_mark_completed (ssm);
          else
            fpi_ssm_jump_to_state (ssm, CAPTURE_QUERY_DATA_READY);
        }
      break;

    case CAPTURE_FINALIZE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
activate_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpDeviceVfs7552 *self;
  unsigned char *receive_buf;

  self = FPI_DEVICE_VFS7552 (_dev);
  if (self->deactivating)
    {
      fp_dbg ("deactivating, marking completed");
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case ACTIVATE_INIT:
      // This sequence prepares the sensor for capturing the image.
      self->init_sequence.stepcount = G_N_ELEMENTS (vfs7552_initiate_capture);
      self->init_sequence.actions = vfs7552_initiate_capture;
      self->init_sequence.timeout = VFS7552_DEFAULT_WAIT_TIMEOUT;
      usb_exchange_async (ssm, &self->init_sequence, "ACTIVATE INIT");
      break;

    case ACTIVATE_INTERRUPT_QUERY:
      // This sequence configures the sensor to listen to finger placement events.
      self->init_sequence.stepcount = G_N_ELEMENTS (vfs7552_wait_finger_init);
      self->init_sequence.actions = vfs7552_wait_finger_init;
      self->init_sequence.timeout = 0; // Do not time out
      usb_exchange_async (ssm, &self->init_sequence, "ACTIVATE INTERRUPT QUERY");
      break;

    case ACTIVATE_INTERRUPT_CHECK:
      receive_buf = ((unsigned char *) self->init_sequence.last_recv->buffer);
      if (receive_buf[0] == interrupt_ok[0])
        {
          // This seems to mean: "Sensor is all good"
          fpi_ssm_jump_to_state (ssm, ACTIVATE_INTERRUPT_QUERY);
        }
      else if (receive_buf[0] == interrupt_ready[0])
        {
          // This seems to mean: "We detected a finger"
          fpi_ssm_next_state (ssm);
        }
      else if (receive_buf[0] == interrupt_dont_ask[0])
        {
          // This seems to mean: "We already told you we detected a finger, stop asking us"
          // It will not respond to another request on the interrupt endpoint
          fpi_ssm_next_state (ssm);
        }
      else
        {
          fp_dbg ("Unknown response 0x%02x", receive_buf[0]);
          fpi_ssm_next_state (ssm);
        }
      break;

    case ACTIVATE_FINALIZE:
      fpi_ssm_mark_completed (ssm);
      break;
    }
}

static void
open_run_state (FpiSsm *ssm, FpDevice *_dev)
{
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case DEV_OPEN_START:
      self->init_sequence.stepcount = G_N_ELEMENTS (vfs7552_initialization);
      self->init_sequence.actions = vfs7552_initialization;
      self->init_sequence.timeout = VFS7552_DEFAULT_WAIT_TIMEOUT;
      usb_exchange_async (ssm, &self->init_sequence, "DEVICE OPEN");
      break;
    }
}

/* ============= SSM Finalization Functions ============== */

static void
deactivation_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);
  g_clear_pointer (&self->init_sequence.last_recv, fpi_usb_transfer_unref);

  self->dev_state = FPI_IMAGE_DEVICE_STATE_INACTIVE;
  self->loop_running = FALSE;

  if (self->deactivating)
    {
      self->deactivating = FALSE;
      fpi_image_device_deactivate_complete (dev, error);
      return;
    }

  if (!error)
    fpi_image_device_report_finger_status (dev, FALSE);
  else
    fpi_image_device_session_error (dev, error);
}

static void
start_deactivation (FpDevice *_dev)
{
  FpDeviceVfs7552 *self = FPI_DEVICE_VFS7552 (_dev);
  FpiSsm *ssm;

  self->loop_running = TRUE;
  self->dev_state = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF;
  ssm = fpi_ssm_new (_dev, deactivate_run_state, DEACTIVATE_NUM_STATES);
  fpi_ssm_start (ssm, deactivation_complete);
}

static void
report_finger_off (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);
  g_clear_pointer (&self->init_sequence.last_recv, fpi_usb_transfer_unref);

  if (!error)
    start_deactivation (_dev);
  else
    fpi_image_device_session_error (dev, error);
}

static void
start_report_finger_off (FpDevice *_dev)
{
  FpDeviceVfs7552 *self = FPI_DEVICE_VFS7552 (_dev);
  FpiSsm *ssm;

  self->loop_running = TRUE;
  ssm = fpi_ssm_new (_dev, capture_run_state, CAPTURE_NUM_STATES);
  fpi_ssm_start (ssm, report_finger_off);
}

static void
capture_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);
  g_clear_pointer (&self->init_sequence.last_recv, fpi_usb_transfer_unref);

  if (!self->deactivating && !error)
    {
      FpImage *img;
      img = fp_image_new (2 * VFS7552_IMAGE_WIDTH,
                          2 * VFS7552_IMAGE_HEIGHT);
      // Scale the image
      for (int j = 0; j < VFS7552_IMAGE_HEIGHT; j++)
        {
          for (int i = 0; i < VFS7552_IMAGE_WIDTH; i++)
            {
              int ref = j * VFS7552_IMAGE_WIDTH + i;
              int ref_new = 4 * j * VFS7552_IMAGE_WIDTH + 2 * i;
              img->data[ref_new] = self->image[ref];
              img->data[ref_new + 1] = self->image[ref];
              img->data[ref_new + 2 * VFS7552_IMAGE_WIDTH] = self->image[ref];
              img->data[ref_new + 2 * VFS7552_IMAGE_WIDTH + 1] = self->image[ref];
            }
        }

      fp_dbg ("Image captured");
      fpi_image_device_image_captured (dev, img);
    }
  self->loop_running = FALSE;

  if (self->deactivating)
    start_deactivation (_dev);
  else if (error)
    fpi_image_device_session_error (dev, error);
}

static void
start_capture (FpDevice *_dev)
{
  FpDeviceVfs7552 *self = FPI_DEVICE_VFS7552 (_dev);
  FpiSsm *ssm;

  self->loop_running = TRUE;
  ssm = fpi_ssm_new (_dev, capture_run_state, CAPTURE_NUM_STATES);
  fpi_ssm_start (ssm, capture_complete);
}

static void
report_finger_on_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);
  g_clear_pointer (&self->init_sequence.last_recv, fpi_usb_transfer_unref);

  if (!self->deactivating && !error)
    fpi_image_device_report_finger_status (dev, TRUE);
  self->loop_running = FALSE;

  if (self->deactivating)
    start_deactivation (_dev);
  else if (error)
    fpi_image_device_session_error (dev, error);
}

static void
activate_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);
  g_clear_pointer (&self->init_sequence.last_recv, fpi_usb_transfer_unref);

  if (!self->deactivating && !error)
    {
      ssm = fpi_ssm_new (_dev, capture_run_state, CAPTURE_NUM_STATES);
      fpi_ssm_start (ssm, report_finger_on_complete);
      return;
    }

  if (self->deactivating)
    {
      start_deactivation (_dev);
    }
  else if (error)
    {
      self->loop_running = FALSE;
      fpi_image_device_session_error (dev, error);
    }
}

static void
start_report_finger_on (FpDevice *_dev)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (dev);
  FpiSsm *ssm;

  self->deactivating = FALSE;
  self->loop_running = TRUE;
  ssm = fpi_ssm_new (_dev, activate_run_state, ACTIVATE_NUM_STATES);
  fpi_ssm_start (ssm, activate_complete);
}

static void
validity_change_state (FpDevice *_dev, void *data)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (dev);
  FpiImageDeviceState next_state = self->dev_state_next;

  if (self->dev_state == next_state)
    {
      fp_dbg ("already in %d", next_state);
      return;
    }
  else
    {
      fp_dbg ("changing to %d", next_state);
    }

  switch (next_state)
    {
    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON:
      self->dev_state = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON;
      start_report_finger_on (_dev);
      break;

    case FPI_IMAGE_DEVICE_STATE_CAPTURE:
      self->dev_state = FPI_IMAGE_DEVICE_STATE_CAPTURE;
      start_capture (_dev);
      break;

    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF:
      self->dev_state = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF;
      start_report_finger_off (_dev);
      break;

    /* Ignored States */
    case FPI_IMAGE_DEVICE_STATE_INACTIVE:
    case FPI_IMAGE_DEVICE_STATE_ACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_DEACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_IDLE:
      break;
    }
}

static void
open_complete (FpiSsm *ssm, FpDevice *_dev, GError *error)
{
  FpImageDevice *dev = FP_IMAGE_DEVICE (_dev);
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (_dev);
  g_clear_pointer (&self->init_sequence.last_recv, fpi_usb_transfer_unref);

  fpi_image_device_open_complete (dev, error);
}

/* ================== Driver Entrypoints =================== */

static void
dev_close (FpImageDevice *dev)
{
  GError *error = NULL;

  g_usb_device_release_interface (fpi_device_get_usb_device (FP_DEVICE (dev)),
                                  0, 0, &error);

  fpi_image_device_close_complete (dev, error);
}

static void
dev_deactivate (FpImageDevice *dev)
{
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (dev);

  if (self->loop_running)
    self->deactivating = TRUE;
  else
    fpi_image_device_deactivate_complete (dev, NULL);
}

static void
delayed_change_state (FpDevice *_dev, FpiImageDeviceState state)
{
  GSource *timeout;
  char *name;

  // schedule state change instead of calling it directly to allow all actions
  // related to the previous state to complete
  timeout = fpi_device_add_timeout (_dev, 50,
                                    validity_change_state,
                                    NULL, NULL);

  name = g_strdup_printf ("dev_change_state to %d", state);
  g_source_set_name (timeout, name);
  g_free (name);
}

static void
dev_change_state (FpImageDevice *dev, FpiImageDeviceState state)
{
  FpDeviceVfs7552 *self;

  self = FPI_DEVICE_VFS7552 (dev);

  switch (state)
    {
    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON:
      self->dev_state_next = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON;
      delayed_change_state (FP_DEVICE (dev), state);
      break;

    case FPI_IMAGE_DEVICE_STATE_CAPTURE:
      self->dev_state_next = FPI_IMAGE_DEVICE_STATE_CAPTURE;
      delayed_change_state (FP_DEVICE (dev), state);
      break;

    case FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF:
      self->dev_state_next = FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_OFF;
      delayed_change_state (FP_DEVICE (dev), state);
      break;

    /* Ignored States */
    case FPI_IMAGE_DEVICE_STATE_INACTIVE:
    case FPI_IMAGE_DEVICE_STATE_ACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_DEACTIVATING:
    case FPI_IMAGE_DEVICE_STATE_IDLE:
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
dev_activate (FpImageDevice *dev)
{
  fpi_image_device_activate_complete (dev, NULL);
}

static void
dev_open (FpImageDevice *dev)
{
  FpiSsm *ssm;
  GError *error = NULL;

  // First we need to reset the device, otherwise opening will fail at state 13
  if (!g_usb_device_reset (fpi_device_get_usb_device (FP_DEVICE (dev)), &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (FP_DEVICE (dev)), 0, 0, &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  ssm = fpi_ssm_new (FP_DEVICE (dev), open_run_state, DEV_OPEN_NUM_STATES);
  fpi_ssm_start (ssm, open_complete);
}

static const FpIdEntry id_table[] = {
  /* Validity device from some Dell XPS laptops (9560, 9360 at least) */
  { .vid = 0x138a, .pid = 0x0091 },
  { .vid = 0x0, .pid = 0x0 },
};

static void
fpi_device_vfs7552_init (FpDeviceVfs7552 *self)
{
}

static void
fpi_device_vfs7552_class_init (FpDeviceVfs7552Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id = "vfs7552";
  dev_class->full_name = "Validity VFS7552";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_close = dev_close;
  img_class->deactivate = dev_deactivate;
  img_class->change_state = dev_change_state;
  img_class->activate = dev_activate;
  img_class->img_open = dev_open;

  img_class->score_threshold = 20;

  img_class->img_width = VFS7552_IMAGE_WIDTH;
  img_class->img_height = VFS7552_IMAGE_HEIGHT;
}
