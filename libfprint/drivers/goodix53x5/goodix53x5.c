/*
 * Goodix 53x5 series driver for libfprint
 * Copyright (C) 2024 libfprint contributors
 *
 * Based on the reference Python implementation by goodix-fp-dump contributors
 * https://github.com/nickel-org/goodix-fp-dump
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

#define FP_COMPONENT "goodix53x5"
#include "fpi-log.h"

#include "goodix53x5.h"
#include "goodix53x5_proto.h"

#include "drivers_api.h"

#include <nss.h>

#include <glib.h>
#include <string.h>

/* ============================================================
 * USB endpoint addresses (bulk)
 * ============================================================ */

#define GOODIX53X5_EP_OUT  (0x03 | FPI_USB_ENDPOINT_OUT)
#define GOODIX53X5_EP_IN   (0x81 | FPI_USB_ENDPOINT_IN)

/* USB transfer timeout (ms) */
#define GOODIX53X5_TIMEOUT 5000

/* Maximum receive buffer size (big enough for the largest encrypted image) */
#define GOODIX53X5_MAX_RECV (64 * 1024)

/* ============================================================
 * GObject boilerplate
 * ============================================================ */

G_DEFINE_TYPE (FpiDeviceGoodix53x5, fpi_device_goodix53x5, FP_TYPE_IMAGE_DEVICE)

/* ============================================================
 * Forward declarations
 * ============================================================ */

static void goodix53x5_start_capture_ssm (FpImageDevice *dev);

/* ============================================================
 * Helpers: send and receive raw USB messages
 * ============================================================ */

/* Forward declaration for ack_recv_cb */
static void ack_recv_cb (FpiUsbTransfer *transfer,
                         FpDevice       *dev,
                         gpointer        user_data,
                         GError         *error);

/*
 * Callback: called after a bulk-OUT transfer carrying an encoded message has
 * completed.  On success, posts an IN transfer to consume the device ACK.
 * The expected ACK command_byte is stored in user_data (cast to GINT_TO_POINTER).
 */
static void
msg_send_cb (FpiUsbTransfer *transfer,
             FpDevice       *dev,
             gpointer        user_data,
             GError         *error)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  FpiUsbTransfer *ack_transfer;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  /* Reset recv buffer and post an IN transfer to read the ACK */
  g_byte_array_set_size (self->recv_buf, 0);

  ack_transfer = fpi_usb_transfer_new (dev);
  ack_transfer->ssm = transfer->ssm;
  fpi_usb_transfer_fill_bulk (ack_transfer,
                              self->ep_in,
                              GOODIX53X5_USB_CHUNK_SIZE);
  fpi_usb_transfer_submit (ack_transfer,
                           GOODIX53X5_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           ack_recv_cb,
                           user_data);  /* command_byte expected in ACK */
}

/*
 * Callback: accumulates ACK chunks and validates the ACK message.
 * Once complete, advances the SSM.
 */
static void
ack_recv_cb (FpiUsbTransfer *transfer,
             FpDevice       *dev,
             gpointer        user_data,
             GError         *error)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  guint8 expected_cmd_byte = (guint8) GPOINTER_TO_INT (user_data);
  gsize chunk_len;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  chunk_len = (gsize) transfer->actual_length;
  if (chunk_len == 0)
    {
      /* Empty read — re-submit */
      fpi_usb_transfer_submit (fpi_usb_transfer_ref (transfer),
                               GOODIX53X5_TIMEOUT,
                               fpi_device_get_cancellable (dev),
                               ack_recv_cb,
                               user_data);
      return;
    }

  g_byte_array_append (self->recv_buf, transfer->buffer, (guint) chunk_len);

  /* Check if we have a complete message */
  if (self->recv_buf->len >= 3)
    {
      guint16 message_size = GUINT16_FROM_LE (*(guint16 *) &self->recv_buf->data[1]);
      gsize total_needed = 3 + (gsize) message_size;

      if (self->recv_buf->len >= total_needed)
        {
          /* Validate it's an ACK: cat=0xB, cmd=0x0 */
          guint8 cmd_byte = self->recv_buf->data[0];
          guint8 cat = cmd_byte >> 4;
          guint8 cmd = (cmd_byte & 0xF) >> 1;

          if (cat != GOODIX53X5_CAT_ACK || cmd != 0x0)
            {
              fpi_ssm_mark_failed (transfer->ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "goodix53x5: expected ACK (cat=0xB cmd=0), "
                                                             "got cat=0x%x cmd=0x%x",
                                                             cat, cmd));
              return;
            }

          /* Validate ACK payload echoes the right command_byte */
          if (message_size >= 1 && self->recv_buf->data[3] != expected_cmd_byte)
            {
              fpi_ssm_mark_failed (transfer->ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "goodix53x5: ACK echoes wrong "
                                                             "command byte 0x%02x (expected 0x%02x)",
                                                             self->recv_buf->data[3],
                                                             expected_cmd_byte));
              return;
            }

          /* ACK is valid — advance SSM */
          fpi_ssm_next_state (transfer->ssm);
          return;
        }
    }

  /* Need more data */
  if (self->recv_buf->len >= GOODIX53X5_MAX_RECV)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: ACK receive buffer overflow"));
      return;
    }

  fpi_usb_transfer_submit (fpi_usb_transfer_ref (transfer),
                           GOODIX53X5_TIMEOUT,
                           fpi_device_get_cancellable (dev),
                           ack_recv_cb,
                           user_data);
}

/*
 * Send an encoded message over bulk-OUT.
 * After the OUT completes, the ACK from the device is consumed automatically.
 * The SSM is advanced only after the ACK is validated.
 */
static void
goodix53x5_send_msg (FpiDeviceGoodix53x5 *self,
                     guint8               category,
                     guint8               command,
                     const guint8        *payload,
                     gsize                payload_len)
{
  FpiUsbTransfer *transfer;
  guint8 *encoded;
  gsize encoded_len;
  guint8 command_byte;

  encoded = goodix53x5_encode_message (category, command, payload, payload_len, &encoded_len);

  /* command_byte is the first byte of the encoded message */
  command_byte = encoded[0];

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  transfer->ssm = self->ssm;
  fpi_usb_transfer_fill_bulk_full (transfer,
                                   self->ep_out,
                                   encoded,
                                   encoded_len,
                                   g_free);
  fpi_usb_transfer_submit (transfer,
                           GOODIX53X5_TIMEOUT,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           msg_send_cb,
                           GINT_TO_POINTER ((gint) command_byte));
}

/*
 * Callback: called when a bulk-IN transfer has completed.
 * Accumulates chunks into self->recv_buf until we have a complete message,
 * then advances the SSM.
 *
 * user_data carries the timeout (as GUINT_TO_POINTER) to use when
 * re-submitting for more data.  0 means no timeout (unlimited wait).
 */
static void
msg_recv_cb (FpiUsbTransfer *transfer,
             FpDevice       *dev,
             gpointer        user_data,
             GError         *error)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  guint recv_timeout = GPOINTER_TO_UINT (user_data);
  gsize chunk_len;

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  chunk_len = (gsize) transfer->actual_length;
  if (chunk_len == 0)
    {
      /* Empty read — re-submit (device can return zero-length packets) */
      fpi_usb_transfer_submit (fpi_usb_transfer_ref (transfer),
                               recv_timeout,
                               fpi_device_get_cancellable (dev),
                               msg_recv_cb,
                               user_data);
      return;
    }

  g_byte_array_append (self->recv_buf, transfer->buffer, (guint) chunk_len);

  /* Check if we have enough data for a complete message.
   * A message starts with: [cmd_byte] [size_le16] ...
   * total_needed_logical = 3 + message_size  (where message_size = payload_len + 1 for checksum)
   *
   * However, recv_buf contains RAW USB chunks (64 bytes each).
   * First chunk: 64 bytes of logical data.
   * Each continuation chunk: 1 byte header + 63 bytes of logical data.
   * So raw bytes needed = ceil((logical - 64) / 63 + 1) * 64   if logical > 64
   *                     = 64                                     if logical <= 64
   */
  if (self->recv_buf->len >= 3)
    {
      guint16 message_size = GUINT16_FROM_LE (*(guint16 *) &self->recv_buf->data[1]);
      gsize total_needed_logical = 3 + (gsize) message_size;
      gsize total_needed_raw;

      if (total_needed_logical <= GOODIX53X5_USB_CHUNK_SIZE)
        total_needed_raw = GOODIX53X5_USB_CHUNK_SIZE;
      else
        {
          gsize remaining = total_needed_logical - GOODIX53X5_USB_CHUNK_SIZE;
          gsize extra_chunks = (remaining + (GOODIX53X5_USB_CHUNK_SIZE - 2)) / (GOODIX53X5_USB_CHUNK_SIZE - 1);
          total_needed_raw = (1 + extra_chunks) * GOODIX53X5_USB_CHUNK_SIZE;
        }

      if (self->recv_buf->len >= total_needed_raw)
        {
          /* We have a complete message — advance SSM */
          fpi_ssm_next_state (transfer->ssm);
          return;
        }
    }

  /* Need more data — re-submit the same transfer */
  if (self->recv_buf->len >= GOODIX53X5_MAX_RECV)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: receive buffer overflow"));
      return;
    }

  fpi_usb_transfer_submit (fpi_usb_transfer_ref (transfer),
                           recv_timeout,
                           fpi_device_get_cancellable (dev),
                           msg_recv_cb,
                           user_data);
}

/*
 * Post a bulk-IN transfer to receive the next message from the device.
 * When the message is complete, the SSM is advanced.
 * self->recv_buf is reset before reading.
 *
 * @timeout: milliseconds to wait for each USB chunk, or 0 for no timeout.
 *           Use GOODIX53X5_TIMEOUT for normal commands and 0 for states
 *           that wait for human interaction (e.g. finger placement).
 */
static void
goodix53x5_recv_msg (FpiDeviceGoodix53x5 *self,
                     guint                timeout)
{
  FpiUsbTransfer *transfer;

  g_byte_array_set_size (self->recv_buf, 0);

  transfer = fpi_usb_transfer_new (FP_DEVICE (self));
  transfer->ssm = self->ssm;
  fpi_usb_transfer_fill_bulk (transfer,
                              self->ep_in,
                              GOODIX53X5_USB_CHUNK_SIZE);
  fpi_usb_transfer_submit (transfer,
                           timeout,
                           fpi_device_get_cancellable (FP_DEVICE (self)),
                           msg_recv_cb,
                           GUINT_TO_POINTER (timeout));
}

/*
 * Parse the accumulated recv_buf into (category, command, payload).
 * Returns TRUE on success; marks the SSM failed on error.
 * @out_payload is heap-allocated; caller must g_free().
 */
static gboolean
goodix53x5_parse_recv (FpiDeviceGoodix53x5  *self,
                       guint8               *out_category,
                       guint8               *out_command,
                       guint8              **out_payload,
                       gsize                *out_payload_len)
{
  gboolean ok;

  ok = goodix53x5_parse_message (self->recv_buf->data,
                                 self->recv_buf->len,
                                 out_category,
                                 out_command,
                                 out_payload,
                                 out_payload_len);
  if (!ok)
    {
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: failed to parse device message"));
    }
  return ok;
}

/*
 * Parse recv_buf and verify the category/command match expected values.
 * On mismatch the SSM is failed.
 */
static gboolean
goodix53x5_check_recv (FpiDeviceGoodix53x5  *self,
                       guint8                expected_cat,
                       guint8                expected_cmd,
                       guint8              **out_payload,
                       gsize                *out_payload_len)
{
  guint8 cat, cmd;

  if (!goodix53x5_parse_recv (self, &cat, &cmd, out_payload, out_payload_len))
    return FALSE;

  if (cat != expected_cat || cmd != expected_cmd)
    {
      if (out_payload && *out_payload)
        {
          g_free (*out_payload);
          *out_payload = NULL;
        }
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: unexpected reply cat=0x%x cmd=0x%x "
                                                     "(expected cat=0x%x cmd=0x%x)",
                                                     cat, cmd, expected_cat, expected_cmd));
      return FALSE;
    }
  return TRUE;
}

/* ============================================================
 * MCU (GTLS) send/recv helpers
 * ============================================================ */

/*
 * Send an MCU message: category=0xD, command=0x1
 * Payload layout: [data_type u32le] [total_len u32le (header+data)] [data]
 */
static void
goodix53x5_send_mcu (FpiDeviceGoodix53x5 *self,
                     guint32              data_type,
                     const guint8        *data,
                     gsize                data_len)
{
  guint8 *payload;
  gsize payload_len;
  guint32 header_and_data_len;

  payload_len = 8 + data_len;
  payload = g_malloc (payload_len);

  guint32 type_le = GUINT32_TO_LE (data_type);
  memcpy (payload, &type_le, 4);

  header_and_data_len = GUINT32_TO_LE ((guint32) (data_len + 8));
  memcpy (payload + 4, &header_and_data_len, 4);

  if (data_len > 0)
    memcpy (payload + 8, data, data_len);

  goodix53x5_send_msg (self, GOODIX53X5_CAT_MCU, GOODIX53X5_CMD_MCU,
                       payload, payload_len);
  g_free (payload);
}

/*
 * Parse an incoming MCU message from recv_buf.
 * Returns the inner data (after the 8-byte header) as a newly allocated
 * buffer, or NULL on error.  Caller must g_free().
 */
static guint8 *
goodix53x5_parse_mcu_recv (FpiDeviceGoodix53x5 *self,
                            guint32              expected_type,
                            gsize               *out_data_len)
{
  guint8 *payload = NULL;
  gsize payload_len = 0;
  guint32 msg_type;
  guint32 msg_total_len;

  if (!goodix53x5_check_recv (self,
                              GOODIX53X5_CAT_MCU,
                              GOODIX53X5_CMD_MCU,
                              &payload,
                              &payload_len))
    return NULL;

  if (payload_len < 8)
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: MCU reply too short"));
      return NULL;
    }

  msg_type = GUINT32_FROM_LE (*(guint32 *) payload);
  msg_total_len = GUINT32_FROM_LE (*(guint32 *) (payload + 4));

  if (msg_type != expected_type)
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: MCU wrong type 0x%08x "
                                                     "(expected 0x%08x)",
                                                     msg_type, expected_type));
      return NULL;
    }

  if (msg_total_len != (guint32) payload_len)
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: MCU length mismatch"));
      return NULL;
    }

  *out_data_len = payload_len - 8;
  guint8 *data = g_malloc (*out_data_len);
  memcpy (data, payload + 8, *out_data_len);
  g_free (payload);
  return data;
}

/* ============================================================
 * Production read/write helpers
 * ============================================================ */

/*
 * Send a production READ request.
 * Payload: [read_type u32le]
 */
static void
goodix53x5_prod_read (FpiDeviceGoodix53x5 *self,
                      guint32              read_type)
{
  guint8 payload[4];
  guint32 type_le = GUINT32_TO_LE (read_type);

  memcpy (payload, &type_le, 4);
  goodix53x5_send_msg (self, GOODIX53X5_CAT_PRODUCTION,
                       GOODIX53X5_CMD_PROD_READ,
                       payload, sizeof (payload));
}

/*
 * Parse a production READ response from recv_buf.
 * Returns the data payload on success; caller must g_free().
 */
static guint8 *
goodix53x5_parse_prod_read_recv (FpiDeviceGoodix53x5 *self,
                                  guint32              expected_type,
                                  gsize               *out_len)
{
  guint8 *payload = NULL;
  gsize payload_len = 0;
  guint32 mcu_status;
  guint32 msg_type;
  guint32 data_size;

  if (!goodix53x5_check_recv (self,
                              GOODIX53X5_CAT_PRODUCTION,
                              GOODIX53X5_CMD_PROD_READ,
                              &payload,
                              &payload_len))
    return NULL;

  if (payload_len < 9)
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: prod-read reply too short"));
      return NULL;
    }

  mcu_status = payload[0];
  msg_type = GUINT32_FROM_LE (*(guint32 *) (payload + 1));
  data_size = GUINT32_FROM_LE (*(guint32 *) (payload + 5));

  if (mcu_status != 0)
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: prod-read MCU failed (status=%u)",
                                                     mcu_status));
      return NULL;
    }

  if (msg_type != expected_type)
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: prod-read wrong type 0x%08x",
                                                     msg_type));
      return NULL;
    }

  if (data_size != (guint32) (payload_len - 9))
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: prod-read data size mismatch"));
      return NULL;
    }

  *out_len = (gsize) data_size;
  guint8 *data = g_malloc (*out_len);
  memcpy (data, payload + 9, *out_len);
  g_free (payload);
  return data;
}

/*
 * Send a production WRITE request.
 * Payload: [data_type u32le] [data_size u32le] [data]
 */
static void
goodix53x5_prod_write (FpiDeviceGoodix53x5 *self,
                       guint32              data_type,
                       const guint8        *data,
                       gsize                data_len)
{
  guint8 *payload;
  gsize payload_len = 8 + data_len;

  payload = g_malloc (payload_len);

  guint32 type_le = GUINT32_TO_LE (data_type);
  memcpy (payload, &type_le, 4);

  guint32 size_le = GUINT32_TO_LE ((guint32) data_len);
  memcpy (payload + 4, &size_le, 4);

  if (data_len > 0)
    memcpy (payload + 8, data, data_len);

  goodix53x5_send_msg (self, GOODIX53X5_CAT_PRODUCTION,
                       GOODIX53X5_CMD_PROD_WRITE,
                       payload, payload_len);
  g_free (payload);
}

/*
 * Parse a production WRITE response — just checks MCU status byte.
 */
static gboolean
goodix53x5_parse_prod_write_recv (FpiDeviceGoodix53x5 *self)
{
  guint8 *payload = NULL;
  gsize payload_len = 0;

  if (!goodix53x5_check_recv (self,
                              GOODIX53X5_CAT_PRODUCTION,
                              GOODIX53X5_CMD_PROD_WRITE,
                              &payload,
                              &payload_len))
    return FALSE;

  if (payload_len < 1 || payload[0] != 0)
    {
      g_free (payload);
      fpi_ssm_mark_failed (self->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "goodix53x5: prod-write failed"));
      return FALSE;
    }

  g_free (payload);
  return TRUE;
}

/* ============================================================
 * Initialization SSM
 * ============================================================ */

static void
init_ssm_state (FpiSsm   *ssm,
                FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  guint8 img_request[4];

  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* --- ping --- */
    case INIT_PING:
      {
        guint8 ping_payload[2] = { 0x00, 0x00 };
        goodix53x5_send_msg (self, 0x0, 0x0, ping_payload, 2);
      }
      break;

    case INIT_PING_RECV:
      /* ACK is consumed automatically by goodix53x5_send_msg(); skip through */
      fpi_ssm_next_state (ssm);
      break;

    /* --- firmware version --- */
    case INIT_FW_VERSION:
      {
        guint8 fwv_payload[2] = { 0x00, 0x00 };
        goodix53x5_send_msg (self, GOODIX53X5_CAT_RESET,
                             GOODIX53X5_CMD_FW_VERSION,
                             fwv_payload, 2);
      }
      break;

    case INIT_FW_VERSION_RECV:
      /* Receive the real firmware version reply (ACK already consumed) */
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_FW_VERSION_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_RESET,
                                    GOODIX53X5_CMD_FW_VERSION,
                                    &payload, &plen))
          break;
        if (plen > 0)
          {
            guint8 *nul = memchr (payload, 0, plen);
            gsize slen = nul ? (gsize) (nul - payload) : plen;
            g_autofree gchar *fw_str = g_strndup ((gchar *) payload, slen);
            fp_dbg ("goodix53x5: firmware version: %s", fw_str);
          }
        g_free (payload);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- soft reset --- */
    case INIT_RESET:
      {
        /* reset_type=0: msg = 0b001 | (20 << 8) = 0x1401 */
        guint16 rst_le = GUINT16_TO_LE (0x1401);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_RESET,
                             GOODIX53X5_CMD_RESET,
                             (guint8 *) &rst_le, 2);
      }
      break;

    case INIT_RESET_RECV:
      /* ACK is consumed automatically by goodix53x5_send_msg(); skip through */
      fpi_ssm_next_state (ssm);
      break;

    /* --- read chip ID --- */
    case INIT_READ_CHIP_ID:
      {
        guint8 rd_payload[5];
        rd_payload[0] = 0x00;
        guint16 addr_le = GUINT16_TO_LE (0x0000);
        guint16 size_le = GUINT16_TO_LE (0x0004);
        memcpy (&rd_payload[1], &addr_le, 2);
        memcpy (&rd_payload[3], &size_le, 2);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_READ_REG,
                             GOODIX53X5_CMD_READ_REG,
                             rd_payload, 5);
      }
      break;

    case INIT_READ_CHIP_ID_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_READ_CHIP_ID_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_READ_REG,
                                    GOODIX53X5_CMD_READ_REG,
                                    &payload, &plen))
          break;
        if (plen >= 4)
          {
            guint32 chip_id = GUINT32_FROM_LE (*(guint32 *) payload);
            fp_dbg ("goodix53x5: chip ID: 0x%08x", chip_id);
          }
        g_free (payload);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- read OTP --- */
    case INIT_READ_OTP:
      {
        guint8 otp_payload[2] = { 0x00, 0x00 };
        goodix53x5_send_msg (self, GOODIX53X5_CAT_RESET,
                             GOODIX53X5_CMD_READ_OTP,
                             otp_payload, 2);
      }
      break;

    case INIT_READ_OTP_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_READ_OTP_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_RESET,
                                    GOODIX53X5_CMD_READ_OTP,
                                    &payload, &plen))
          break;
        if (!goodix53x5_parse_otp (payload, plen, &self->calib))
          {
            g_free (payload);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: OTP parse failed"));
            break;
          }
        g_free (payload);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- read PSK hash to verify --- */
    case INIT_READ_PSK_HASH:
      goodix53x5_prod_read (self, GOODIX53X5_PROD_PSK_HASH);
      break;

    case INIT_READ_PSK_HASH_RECV:
      /* Async receive — SSM advances automatically when data arrives */
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_READ_PSK_HASH_PARSE:
      {
        guint8 *hash = NULL;
        gsize hash_len = 0;
        static const guint8 expected_hash[32] = GOODIX53X5_PSK_HASH;

        hash = goodix53x5_parse_prod_read_recv (self, GOODIX53X5_PROD_PSK_HASH, &hash_len);
        if (!hash)
          break;  /* SSM already failed */

        if (hash_len != 32 || memcmp (hash, expected_hash, 32) != 0)
          {
            fp_info ("goodix53x5: PSK hash mismatch, will write white-box PSK");
            self->need_psk_write = TRUE;
          }
        else
          {
            fp_dbg ("goodix53x5: PSK hash OK");
            self->need_psk_write = FALSE;
          }
        g_free (hash);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- write PSK white-box (only if needed) --- */
    case INIT_WRITE_PSK:
      if (self->need_psk_write)
        {
          static const guint8 psk_wb[GOODIX53X5_PSK_WHITE_BOX_LEN] = GOODIX53X5_PSK_WHITE_BOX;
          goodix53x5_prod_write (self, GOODIX53X5_PROD_PSK_WHITE_BOX, psk_wb,
                                 GOODIX53X5_PSK_WHITE_BOX_LEN);
        }
      else
        {
          fpi_ssm_next_state (ssm);
        }
      break;

    case INIT_WRITE_PSK_RECV:
      if (self->need_psk_write)
        goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      else
        fpi_ssm_next_state (ssm);
      break;

    case INIT_WRITE_PSK_PARSE:
      if (self->need_psk_write)
        {
          if (!goodix53x5_parse_prod_write_recv (self))
            break;
        }
      fpi_ssm_next_state (ssm);
      break;

    /* --- GTLS client hello --- */
    case INIT_GTLS_CLIENT_HELLO:
      {
        /* Generate 32 random bytes for client_random */
        for (guint i = 0; i < GOODIX53X5_RANDOM_LEN; i++)
          self->gtls.client_random[i] = (guint8) g_random_int ();

        memset (self->gtls.psk, 0, GOODIX53X5_PSK_LEN);  /* all-zero PSK */

        goodix53x5_send_mcu (self, GOODIX53X5_MCU_CLIENT_HELLO,
                             self->gtls.client_random,
                             GOODIX53X5_RANDOM_LEN);
      }
      break;

    case INIT_GTLS_CLIENT_HELLO_RECV:
      /* ACK consumed automatically by send_cb; skip through */
      fpi_ssm_next_state (ssm);
      break;

    /* --- GTLS server identity --- */
    case INIT_GTLS_SERVER_IDENTITY_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_GTLS_SERVER_IDENTITY_PARSE:
      {
        guint8 *data = NULL;
        gsize data_len = 0;
        guint8 computed_identity[GOODIX53X5_IDENTITY_LEN];

        data = goodix53x5_parse_mcu_recv (self, GOODIX53X5_MCU_SERVER_IDENTITY, &data_len);
        if (!data)
          break;

        if (data_len < GOODIX53X5_RANDOM_LEN + GOODIX53X5_IDENTITY_LEN)
          {
            g_free (data);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: server identity too short"));
            break;
          }

        memcpy (self->gtls.server_random, data, GOODIX53X5_RANDOM_LEN);

        /* Derive session key */
        goodix53x5_gtls_derive_session_key (self->gtls.psk,
                                            self->gtls.client_random,
                                            self->gtls.server_random,
                                            &self->gtls);

        /* Compute client identity = HMAC(hmac_key, client_random || server_random) */
        goodix53x5_gtls_compute_identity (self->gtls.hmac_key,
                                          self->gtls.client_random,
                                          self->gtls.server_random,
                                          computed_identity);

        /* Verify server identity matches */
        const guint8 *server_identity = data + GOODIX53X5_RANDOM_LEN;
        if (memcmp (server_identity, computed_identity, GOODIX53X5_IDENTITY_LEN) != 0)
          {
            g_free (data);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: GTLS identity mismatch "
                                                           "(wrong PSK?)"));
            break;
          }

        g_free (data);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- GTLS client identity --- */
    case INIT_GTLS_CLIENT_IDENTITY:
      {
        /* Send identity + 4 padding bytes (0xEE) */
        guint8 identity_payload[GOODIX53X5_IDENTITY_LEN + 4];
        guint8 identity[GOODIX53X5_IDENTITY_LEN];

        goodix53x5_gtls_compute_identity (self->gtls.hmac_key,
                                          self->gtls.client_random,
                                          self->gtls.server_random,
                                          identity);

        memcpy (identity_payload, identity, GOODIX53X5_IDENTITY_LEN);
        memset (identity_payload + GOODIX53X5_IDENTITY_LEN, 0xEE, 4);

        goodix53x5_send_mcu (self, GOODIX53X5_MCU_CLIENT_IDENTITY,
                             identity_payload,
                             sizeof (identity_payload));
      }
      break;

    case INIT_GTLS_CLIENT_IDENTITY_RECV:
      /* ACK consumed automatically by send_cb; skip through */
      fpi_ssm_next_state (ssm);
      break;

    /* --- GTLS server done --- */
    case INIT_GTLS_SERVER_DONE_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_GTLS_SERVER_DONE_PARSE:
      {
        guint8 *data = NULL;
        gsize data_len = 0;
        guint32 result;

        data = goodix53x5_parse_mcu_recv (self, GOODIX53X5_MCU_SERVER_DONE, &data_len);
        if (!data)
          break;

        if (data_len < 4)
          {
            g_free (data);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: server-done too short"));
            break;
          }

        result = GUINT32_FROM_LE (*(guint32 *) data);
        g_free (data);

        if (result != 0)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: GTLS handshake failed "
                                                           "(result=%u)", result));
            break;
          }

        self->gtls.connected = TRUE;
        fp_dbg ("goodix53x5: GTLS handshake successful");
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- upload config --- */
    case INIT_UPLOAD_CONFIG:
      {
        guint8 config[256];
        goodix53x5_build_config (&self->calib, config);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_CONFIG,
                             GOODIX53X5_CMD_CONFIG_UPLOAD,
                             config, 256);
      }
      break;

    case INIT_UPLOAD_CONFIG_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_UPLOAD_CONFIG_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_CONFIG,
                                    GOODIX53X5_CMD_CONFIG_UPLOAD,
                                    &payload, &plen))
          break;
        if (plen < 1)
          {
            g_free (payload);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: config reply too short"));
            break;
          }
        /* Result is returned as a little-endian integer of whatever length the device sends */
        guint32 result = 0;
        for (gsize ri = 0; ri < MIN (plen, (gsize) 4); ri++)
          result |= ((guint32) payload[ri]) << (ri * 8);
        g_free (payload);
        if (result != 1)
          {
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: config upload failed (result=%u)",
                                                           result));
            break;
          }
        fp_dbg ("goodix53x5: config uploaded");
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- calibration: FDT manual TX on --- */
    case INIT_CALIB_FDT_DOWN_TX_ON:
      {
        /* execute_fdt_operation(MANUAL, [0x0D, 0x01, fdt_base_manual]) */
        guint8 fdt_payload[2 + GOODIX53X5_FDT_BASE_LEN];
        fdt_payload[0] = GOODIX53X5_FDT_MANUAL_TX_ON;  /* opcode */
        fdt_payload[1] = 0x01;                          /* always 1 */
        memcpy (&fdt_payload[2], self->calib.fdt_base_manual, GOODIX53X5_FDT_BASE_LEN);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_FDT,
                             GOODIX53X5_CMD_FDT_MANUAL,
                             fdt_payload, sizeof (fdt_payload));
      }
      break;

    case INIT_CALIB_FDT_DOWN_TX_ON_RECV:
      /* Async receive; SSM advances when data arrives */
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_CALIB_FDT_DOWN_TX_ON_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;

        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_FDT,
                                    GOODIX53X5_CMD_FDT_MANUAL,
                                    &payload, &plen))
          break;
        if (plen >= 4 + GOODIX53X5_FDT_BASE_LEN)
          {
            /* payload: [irq_status u16] [touch_flag u16] [fdt_data 24 bytes] */
            const guint8 *fdt_data = payload + 4;
            /* Store as fdt_base_down, fdt_base_up, fdt_base_manual */
            goodix53x5_generate_fdt_base (fdt_data, self->calib.fdt_base_down);
            memcpy (self->calib.fdt_base_up, self->calib.fdt_base_down, GOODIX53X5_FDT_BASE_LEN);
            memcpy (self->calib.fdt_base_manual, self->calib.fdt_base_down, GOODIX53X5_FDT_BASE_LEN);
          }
        g_free (payload);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- calibration: get image TX on --- */
    case INIT_CALIB_IMG_TX_ON:
      {
        /* get_image(tx=True, hv=True, dac=dac_l) */
        img_request[0] = GOODIX53X5_IMG_TX_ENABLE;
        img_request[1] = GOODIX53X5_IMG_HV_VALUE;
        guint16 dac_le = GUINT16_TO_LE (self->calib.dac_l);
        memcpy (&img_request[2], &dac_le, 2);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_IMAGE,
                             GOODIX53X5_CMD_IMAGE_GET,
                             img_request, 4);
      }
      break;

    case INIT_CALIB_IMG_TX_ON_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_CALIB_IMG_TX_ON_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_IMAGE,
                                    GOODIX53X5_CMD_IMAGE_GET,
                                    &payload, &plen))
          break;
        /* Decrypt to advance hmac_server_counter; discard the image itself */
        guint8 *raw = NULL;
        gsize raw_len = 0;
        if (!goodix53x5_gtls_decrypt_image (&self->gtls, payload, plen, &raw, &raw_len))
          fp_warn ("goodix53x5: calibration image TX-on HMAC failed (non-fatal)");
        g_free (raw);
        g_free (payload);
        fp_dbg ("goodix53x5: calibration image TX-on received");
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- calibration: FDT manual TX off --- */
    case INIT_CALIB_FDT_DOWN_TX_OFF:
      {
        guint8 fdt_payload[2 + GOODIX53X5_FDT_BASE_LEN];
        fdt_payload[0] = GOODIX53X5_FDT_MANUAL_TX_OFF;
        fdt_payload[1] = 0x01;
        memcpy (&fdt_payload[2], self->calib.fdt_base_manual, GOODIX53X5_FDT_BASE_LEN);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_FDT,
                             GOODIX53X5_CMD_FDT_MANUAL,
                             fdt_payload, sizeof (fdt_payload));
      }
      break;

    case INIT_CALIB_FDT_DOWN_TX_OFF_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_CALIB_FDT_DOWN_TX_OFF_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;

        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_FDT,
                                    GOODIX53X5_CMD_FDT_MANUAL,
                                    &payload, &plen))
          break;

        if (plen >= 4 + GOODIX53X5_FDT_BASE_LEN)
          {
            const guint8 *fdt_tx_off = payload + 4;
            /* Validate FDT bases are consistent */
            if (!goodix53x5_is_fdt_base_valid (self->calib.fdt_base_down,
                                               fdt_tx_off,
                                               self->calib.delta_fdt))
              {
                fp_warn ("goodix53x5: FDT base validation failed during calibration "
                         "(delta_fdt=%u)", self->calib.delta_fdt);
                /* Non-fatal: continue anyway */
              }
          }
        g_free (payload);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- calibration: image TX off --- */
    case INIT_CALIB_IMG_TX_OFF:
      {
        img_request[0] = GOODIX53X5_IMG_TX_DISABLE;
        img_request[1] = GOODIX53X5_IMG_HV_VALUE;
        guint16 dac_le = GUINT16_TO_LE (self->calib.dac_l);
        memcpy (&img_request[2], &dac_le, 2);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_IMAGE,
                             GOODIX53X5_CMD_IMAGE_GET,
                             img_request, 4);
      }
      break;

    case INIT_CALIB_IMG_TX_OFF_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_CALIB_IMG_TX_OFF_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_IMAGE,
                                    GOODIX53X5_CMD_IMAGE_GET,
                                    &payload, &plen))
          break;
        /* Decrypt to advance hmac_server_counter; discard the image itself */
        guint8 *raw = NULL;
        gsize raw_len = 0;
        if (!goodix53x5_gtls_decrypt_image (&self->gtls, payload, plen, &raw, &raw_len))
          fp_warn ("goodix53x5: calibration image TX-off HMAC failed (non-fatal)");
        g_free (raw);
        g_free (payload);
        fp_dbg ("goodix53x5: calibration image TX-off received");
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- calibration: second FDT TX on (cross-check) --- */
    case INIT_CALIB_FDT_DOWN_TX_ON_2:
      {
        guint8 fdt_payload[2 + GOODIX53X5_FDT_BASE_LEN];
        fdt_payload[0] = GOODIX53X5_FDT_MANUAL_TX_ON;
        fdt_payload[1] = 0x01;
        memcpy (&fdt_payload[2], self->calib.fdt_base_manual, GOODIX53X5_FDT_BASE_LEN);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_FDT,
                             GOODIX53X5_CMD_FDT_MANUAL,
                             fdt_payload, sizeof (fdt_payload));
      }
      break;

    case INIT_CALIB_FDT_DOWN_TX_ON_2_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case INIT_CALIB_FDT_DOWN_TX_ON_2_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;

        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_FDT,
                                    GOODIX53X5_CMD_FDT_MANUAL,
                                    &payload, &plen))
          break;
        g_free (payload);
        fp_dbg ("goodix53x5: calibration FDT 2nd TX-on done");
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- sleep --- */
    case INIT_SLEEP:
      {
        guint8 sleep_payload[2] = { 0x01, 0x00 };
        goodix53x5_send_msg (self, GOODIX53X5_CAT_SLEEP,
                             GOODIX53X5_CMD_SLEEP,
                             sleep_payload, 2);
      }
      break;

    case INIT_SLEEP_RECV:
      /* ACK consumed automatically by send_cb; skip through */
      fpi_ssm_next_state (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
init_ssm_done (FpiSsm   *ssm,
               FpDevice *dev,
               GError   *error)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);

  self->ssm = NULL;

  if (error)
    {
      fp_err ("goodix53x5: initialization failed: %s", error->message);
      fpi_image_device_open_complete (idev, error);
      return;
    }

  fp_dbg ("goodix53x5: initialization complete");
  fpi_image_device_open_complete (idev, NULL);
}

/* ============================================================
 * Capture SSM
 * ============================================================ */

static void
capture_ssm_done (FpiSsm   *ssm,
                  FpDevice *dev,
                  GError   *error)
{
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);

  self->ssm = NULL;

  if (self->deactivating)
    {
      fpi_image_device_deactivate_complete (idev, NULL);
      return;
    }

  if (error)
    {
      fpi_image_device_session_error (idev, error);
      return;
    }

  /* Report finger-off here, after the SSM is fully torn down (self->ssm = NULL).
   * This causes the framework to transition AWAIT_FINGER_OFF → IDLE →
   * AWAIT_FINGER_ON, which calls our change_state() vfunc →
   * goodix53x5_start_capture_ssm() for the next cycle.
   *
   * We must NOT do this from inside a SSM state handler (e.g. EC_OFF_PARSE)
   * because change_state() is called synchronously by the framework and would
   * start a new SSM before fpi_ssm_next_state() returns to complete the old
   * one, causing an 'machine != NULL' assertion failure. */
  fpi_image_device_report_finger_status (idev, FALSE);
}

static void
capture_ssm_state (FpiSsm   *ssm,
                   FpDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  FpImageDevice *idev = FP_IMAGE_DEVICE (dev);
  guint8 img_request[4];

  if (self->deactivating)
    {
      fpi_ssm_mark_completed (ssm);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    /* --- power on EC --- */
    case CAPTURE_EC_ON:
      {
        /* ec_control("on"): payload = [0x01, 0x01, 0x00] */
        guint8 ec_payload[3] = { 0x01, 0x01, 0x00 };
        goodix53x5_send_msg (self, GOODIX53X5_CAT_RESET,
                             GOODIX53X5_CMD_EC_CONTROL,
                             ec_payload, 3);
      }
      break;

    case CAPTURE_EC_ON_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case CAPTURE_EC_ON_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_RESET,
                                    GOODIX53X5_CMD_EC_CONTROL,
                                    &payload, &plen))
          break;
        if (plen < 1)
          {
            g_free (payload);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: EC on failed (reply too short)"));
            break;
          }
        {
          guint32 result = 0;
          for (gsize ri = 0; ri < MIN (plen, (gsize) 4); ri++)
            result |= ((guint32) payload[ri]) << (ri * 8);
          g_free (payload);
          if (result != 1)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "goodix53x5: EC on failed (result=%u)",
                                                             result));
              break;
            }
        }
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- setup FDT down --- */
    case CAPTURE_FDT_DOWN:
      {
        guint8 fdt_payload[2 + GOODIX53X5_FDT_BASE_LEN];
        fdt_payload[0] = GOODIX53X5_FDT_DOWN_OPCODE;
        fdt_payload[1] = 0x01;
        memcpy (&fdt_payload[2], self->calib.fdt_base_down, GOODIX53X5_FDT_BASE_LEN);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_FDT,
                             GOODIX53X5_CMD_FDT_DOWN,
                             fdt_payload, sizeof (fdt_payload));
      }
      break;

    case CAPTURE_FDT_DOWN_RECV:
      /* ACK consumed automatically by send_cb; skip through */
      fpi_ssm_next_state (ssm);
      break;

    /* --- wait for finger down event --- */
    case CAPTURE_WAIT_FINGER_DOWN:
      goodix53x5_recv_msg (self, 0); /* no timeout: waiting for human finger */
      break;

    case CAPTURE_WAIT_FINGER_DOWN_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_FDT,
                                    GOODIX53X5_CMD_FDT_DOWN,
                                    &payload, &plen))
          break;

        if (plen >= 4 + GOODIX53X5_FDT_BASE_LEN)
          {
            guint16 touch_flag = GUINT16_FROM_LE (*(guint16 *) (payload + 2));
            const guint8 *fdt_data = payload + 4;

            /* Build fdt_base_up from finger-down event */
            goodix53x5_generate_fdt_up_base (fdt_data, touch_flag,
                                             &self->calib,
                                             self->calib.fdt_base_up);
          }
        g_free (payload);

        fpi_image_device_report_finger_status (idev, TRUE);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- manual FDT to verify real finger --- */
    case CAPTURE_MANUAL_FDT:
      {
        guint8 fdt_payload[2 + GOODIX53X5_FDT_BASE_LEN];
        fdt_payload[0] = GOODIX53X5_FDT_MANUAL_TX_OFF;
        fdt_payload[1] = 0x01;
        memcpy (&fdt_payload[2], self->calib.fdt_base_manual, GOODIX53X5_FDT_BASE_LEN);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_FDT,
                             GOODIX53X5_CMD_FDT_MANUAL,
                             fdt_payload, sizeof (fdt_payload));
      }
      break;

    case CAPTURE_MANUAL_FDT_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case CAPTURE_MANUAL_FDT_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_FDT,
                                    GOODIX53X5_CMD_FDT_MANUAL,
                                    &payload, &plen))
          break;
        g_free (payload);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- capture image --- */
    case CAPTURE_GET_IMAGE:
      {
        /* get_image(tx=True, hv=True, is_finger=True, dac=dac_h) */
        img_request[0] = GOODIX53X5_IMG_TX_ENABLE | GOODIX53X5_IMG_FINGER_FLAG;
        img_request[1] = GOODIX53X5_IMG_HV_VALUE;
        guint16 dac_le = GUINT16_TO_LE (self->calib.dac_h);
        memcpy (&img_request[2], &dac_le, 2);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_IMAGE,
                             GOODIX53X5_CMD_IMAGE_GET,
                             img_request, 4);
      }
      break;

    case CAPTURE_GET_IMAGE_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case CAPTURE_GET_IMAGE_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        guint8 *raw = NULL;
        gsize raw_len = 0;
        guint16 *pixels = NULL;

        fp_dbg ("goodix53x5: image recv_buf len=%u, first bytes: %02x %02x %02x %02x",
                self->recv_buf->len,
                self->recv_buf->len > 0 ? self->recv_buf->data[0] : 0,
                self->recv_buf->len > 1 ? self->recv_buf->data[1] : 0,
                self->recv_buf->len > 2 ? self->recv_buf->data[2] : 0,
                self->recv_buf->len > 3 ? self->recv_buf->data[3] : 0);

        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_IMAGE,
                                    GOODIX53X5_CMD_IMAGE_GET,
                                    &payload, &plen))
          break;

        /* Decrypt the image using GTLS context */
        if (!goodix53x5_gtls_decrypt_image (&self->gtls, payload, plen, &raw, &raw_len))
          {
            g_free (payload);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: image decrypt failed"));
            break;
          }
        g_free (payload);

        /* Decode 12-bit packed pixels */
        if (!goodix53x5_decode_image (raw, raw_len, &pixels))
          {
            g_free (raw);
            fpi_ssm_mark_failed (ssm,
                                 fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                           "goodix53x5: image decode failed"));
            break;
          }
        g_free (raw);

        /* Convert 12-bit pixel data to 8-bit for libfprint */
        FpImage *img = fp_image_new (GOODIX53X5_SENSOR_WIDTH, GOODIX53X5_SENSOR_HEIGHT);
        for (guint i = 0; i < GOODIX53X5_SENSOR_PIXELS; i++)
          img->data[i] = (guint8) (pixels[i] >> 4);  /* 12→8 bit */
        g_free (pixels);

        /* Upscale: the raw 108×88 sensor gives MINDTCT too few minutiae
         * (typically <10, below Bozorth3's minimum of 10).  2× bilinear
         * resize gives 216×176 which yields enough minutiae for matching. */
        FpImage *scaled = fpi_image_resize (img, GOODIX53X5_ENLARGE_FACTOR,
                                            GOODIX53X5_ENLARGE_FACTOR);
        g_object_unref (img);

        fpi_image_device_image_captured (idev, scaled);
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- setup FDT up --- */
    case CAPTURE_FDT_UP:
      {
        guint8 fdt_payload[2 + GOODIX53X5_FDT_BASE_LEN];
        fdt_payload[0] = GOODIX53X5_FDT_UP_OPCODE;
        fdt_payload[1] = 0x01;
        memcpy (&fdt_payload[2], self->calib.fdt_base_up, GOODIX53X5_FDT_BASE_LEN);
        goodix53x5_send_msg (self, GOODIX53X5_CAT_FDT,
                             GOODIX53X5_CMD_FDT_UP,
                             fdt_payload, sizeof (fdt_payload));
      }
      break;

    case CAPTURE_FDT_UP_RECV:
      /* ACK consumed automatically by send_cb; skip through */
      fpi_ssm_next_state (ssm);
      break;

    /* --- wait for finger up event --- */
    case CAPTURE_WAIT_FINGER_UP:
      goodix53x5_recv_msg (self, 0); /* no timeout: waiting for human finger removal */
      break;

    case CAPTURE_WAIT_FINGER_UP_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_FDT,
                                    GOODIX53X5_CMD_FDT_UP,
                                    &payload, &plen))
          break;

        if (plen >= 4 + GOODIX53X5_FDT_BASE_LEN)
          {
            const guint8 *fdt_data = payload + 4;
            /* Update fdt_base_down from finger-up event */
            goodix53x5_generate_fdt_base (fdt_data, self->calib.fdt_base_down);
          }
        g_free (payload);

        /* Do NOT report finger-off yet: we still need to send SLEEP and
         * EC_OFF before the SSM completes.  Reporting here causes libfprint
         * to restart the capture SSM immediately, creating a race where the
         * sleep ACK is consumed by the new cycle's EC-ON recv.  We report
         * finger-off at the end of CAPTURE_EC_OFF_PARSE instead. */
        fpi_ssm_next_state (ssm);
      }
      break;

    /* --- sleep --- */
    case CAPTURE_SLEEP:
      {
        guint8 sleep_payload[2] = { 0x01, 0x00 };
        goodix53x5_send_msg (self, GOODIX53X5_CAT_SLEEP,
                             GOODIX53X5_CMD_SLEEP,
                             sleep_payload, 2);
      }
      break;

    case CAPTURE_SLEEP_RECV:
      /* ACK consumed automatically by send_cb; skip through */
      fpi_ssm_next_state (ssm);
      break;

    /* --- power off EC --- */
    case CAPTURE_EC_OFF:
      {
        guint8 ec_payload[3] = { 0x00, 0x00, 0x00 };
        goodix53x5_send_msg (self, GOODIX53X5_CAT_RESET,
                             GOODIX53X5_CMD_EC_CONTROL,
                             ec_payload, 3);
      }
      break;

    case CAPTURE_EC_OFF_RECV:
      goodix53x5_recv_msg (self, GOODIX53X5_TIMEOUT);
      break;

    case CAPTURE_EC_OFF_PARSE:
      {
        guint8 *payload = NULL;
        gsize plen = 0;
        if (!goodix53x5_check_recv (self,
                                    GOODIX53X5_CAT_RESET,
                                    GOODIX53X5_CMD_EC_CONTROL,
                                    &payload, &plen))
          break;
        g_free (payload);
        fpi_ssm_next_state (ssm);
      }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
goodix53x5_start_capture_ssm (FpImageDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  FpiSsm *ssm;

  ssm = fpi_ssm_new (FP_DEVICE (dev), capture_ssm_state, CAPTURE_NUM_STATES);
  self->ssm = ssm;
  fpi_ssm_start (ssm, capture_ssm_done);
}

/* ============================================================
 * FpImageDevice vfuncs
 * ============================================================ */

static void
goodix53x5_img_open (FpImageDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (FP_DEVICE (dev));
  GError *error = NULL;

  if (NSS_NoDB_Init (".") != SECSuccess)
    {
      fpi_image_device_open_complete (dev,
                                     fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                                               "Failed to initialise NSS"));
      return;
    }

  /* Reset the device to clear any stale state from a previous session.
   * The device may be unresponsive without this. */
  if (!g_usb_device_reset (usb_dev, &error))
    {
      /* Non-fatal: log and continue — the device may still work */
      fp_warn ("goodix53x5: USB reset failed: %s", error->message);
      g_clear_error (&error);
    }

  /* The device presents as a CDC device with two interfaces:
   *   Interface 0: CDC Communications (interrupt EP 0x82)
   *   Interface 1: CDC Data (bulk OUT 0x03, bulk IN 0x81)
   * The kernel cdc_acm driver claims both. We must claim both to detach it.
   */
  if (!g_usb_device_claim_interface (usb_dev, 0,
                                     G_USB_DEVICE_CLAIM_INTERFACE_NONE,
                                     &error))
    {
      fpi_image_device_open_complete (dev, error);
      return;
    }

  if (!g_usb_device_claim_interface (usb_dev, 1,
                                     G_USB_DEVICE_CLAIM_INTERFACE_NONE,
                                     &error))
    {
      g_usb_device_release_interface (usb_dev, 0, 0, NULL);
      fpi_image_device_open_complete (dev, error);
      return;
    }

  self->ep_in  = GOODIX53X5_EP_IN;
  self->ep_out = GOODIX53X5_EP_OUT;
  self->recv_buf = g_byte_array_new ();
  self->deactivating = FALSE;
  self->need_psk_write = FALSE;
  memset (&self->gtls, 0, sizeof (self->gtls));
  memset (&self->calib, 0, sizeof (self->calib));

  FpiSsm *ssm = fpi_ssm_new (FP_DEVICE (dev), init_ssm_state, INIT_NUM_STATES);
  self->ssm = ssm;
  fpi_ssm_start (ssm, init_ssm_done);
}

static void
goodix53x5_img_close (FpImageDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);
  GUsbDevice *usb_dev = fpi_device_get_usb_device (FP_DEVICE (dev));
  GError *error = NULL;

  g_clear_pointer (&self->recv_buf, g_byte_array_unref);

  g_usb_device_release_interface (usb_dev, 1, 0, &error);
  if (error)
    {
      g_usb_device_release_interface (usb_dev, 0, 0, NULL);
      fpi_image_device_close_complete (dev, error);
      return;
    }
  g_usb_device_release_interface (usb_dev, 0, 0, &error);
  fpi_image_device_close_complete (dev, error);
  NSS_Shutdown ();
}

static void
goodix53x5_activate (FpImageDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);

  self->deactivating = FALSE;
  fpi_image_device_activate_complete (dev, NULL);
}

static void
goodix53x5_deactivate (FpImageDevice *dev)
{
  FpiDeviceGoodix53x5 *self = FPI_DEVICE_GOODIX53X5 (dev);

  self->deactivating = TRUE;

  if (self->ssm == NULL)
    {
      /* No SSM running — complete immediately */
      fpi_image_device_deactivate_complete (dev, NULL);
    }
  /* Otherwise the running SSM will see deactivating==TRUE and finish up */
}

static void
goodix53x5_change_state (FpImageDevice      *dev,
                          FpiImageDeviceState state)
{
  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    goodix53x5_start_capture_ssm (dev);
}

/* ============================================================
 * Device ID table and class initialisation
 * ============================================================ */

static const FpIdEntry id_table[] = {
  { .vid = 0x27c6, .pid = 0x5395 },
  { .vid = 0x27c6, .pid = 0x5335 },
  { .vid = 0x27c6, .pid = 0x5385 },
  { .vid = 0,      .pid = 0,      .driver_data = 0 },
};

static void
fpi_device_goodix53x5_init (FpiDeviceGoodix53x5 *self)
{
  (void) self;
}

static void
fpi_device_goodix53x5_class_init (FpiDeviceGoodix53x5Class *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  FpImageDeviceClass *img_class = FP_IMAGE_DEVICE_CLASS (klass);

  dev_class->id        = FP_COMPONENT;
  dev_class->full_name = "Goodix 53x5 series";
  dev_class->type      = FP_DEVICE_TYPE_USB;
  dev_class->id_table  = id_table;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_class->img_width  = GOODIX53X5_SENSOR_WIDTH  * GOODIX53X5_ENLARGE_FACTOR;
  img_class->img_height = GOODIX53X5_SENSOR_HEIGHT * GOODIX53X5_ENLARGE_FACTOR;

  img_class->bz3_threshold = 24;

  img_class->img_open    = goodix53x5_img_open;
  img_class->img_close   = goodix53x5_img_close;
  img_class->activate    = goodix53x5_activate;
  img_class->deactivate  = goodix53x5_deactivate;
  img_class->change_state = goodix53x5_change_state;
}
