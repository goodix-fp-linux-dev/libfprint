/*
 * Goodix 27c6:533c native driver for libfprint
 *
 * Copyright (C) 2026 libfprint contributors
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

#define FP_COMPONENT "goodix533c"

#include <string.h>

#include <openssl/ssl.h>

#include "drivers_api.h"
#include "fpi-ssm.h"
#include "fpi-usb-transfer.h"

#include "../goodixtls/goodix_proto.h"
#include "../goodixtls/goodixtls.h"

#include "goodix533c.h"
#include "goodix533c-private.h"
#include "goodix533c-enroll.h"
#include "goodix533c-auth.h"

/* ---- device-level constants (all hardware-verified, see
 * findings/native-driver-architecture.md) ---- */

#define GOODIX533C_USB_INTERFACE (0)
#define GOODIX533C_EP_IN  (0x83)
#define GOODIX533C_EP_OUT (0x01)

#define GOODIX533C_TIMEOUT_MS (1000)

/* mcu_get_image reply pack flags: encrypted TLS application data, distinct
 * from GOODIX_FLAGS_TLS (raw handshake bytes). Not in goodix_proto.h --
 * that header only knows about 0xa0/0xb0. Matches goodix.py's
 * FLAGS_TRANSPORT_LAYER_SECURITY_DATA. */
#define GOODIX533C_FLAGS_TLS_DATA (0xb2)

/* Number of bytes preceding the raw TLS record inside a TLS_DATA pack's
 * payload. Reverse-engineered value from driver_53xc.py's capture():
 * `frame[9:]` before decrypt_record(). Not a generic protocol constant --
 * device/firmware specific, taken as-is from the proven Python driver. */
#define GOODIX533C_IMAGE_REPLY_HEADER_LEN (9)

#define GOODIX533C_IMAGE_FLAGS_CALIBRATE (0x01)
#define GOODIX533C_IMAGE_GAIN (0xc2)

/* Live (finger-present) frame: flags = 0x01 | 0x40 per the findings doc.
 * Gain is a *deliberate deviation* from driver_53xc.py's default -- see
 * GOODIX533C_LIVE_IMAGE_GAIN below. */
#define GOODIX533C_IMAGE_FLAGS_LIVE (0x41)

/* driver_53xc.py's run_driver() uses gain 0x86 for the live capture
 * (tuned for nikicat's XPS 13 9310). This project's own empirical finding
 * (NOTES.md, "Ridge visibility resolved: gain calibration, not protocol")
 * is that 0x86 clips ~47% of pixels on the hardware this project tests
 * against, while 0xc2 -- the same gain already used for the reference
 * frame -- is headroom-safe (0 clipped pixels) for *both* frame types on
 * this unit. Using 0xc2 here too, not 0x86, is intentional and
 * hardware-verified for this unit, not an oversight. A production driver
 * would need a per-unit gain check rather than a hardcoded value, since
 * the safe gain is apparently unit-specific -- out of scope here. */
#define GOODIX533C_LIVE_IMAGE_GAIN (0xc2)

#define GOODIX533C_CAPTURE_REGISTER (0x022c)
static const guint8 capture_on[2] = { 0x0a, 0x03 };
static const guint8 capture_off[2] = { 0x0a, 0x02 };

/* Not in goodix_proto.h (0x60) -- defined locally like
 * GOODIX533C_FLAGS_TLS_DATA above. */
#define GOODIX533C_CMD_MCU_SWITCH_TO_SLEEP_MODE (0x60)

/* FDT command prefixes -- fixed 2-byte prefix, each suffixed with the same
 * 24-byte per-session template read during OPEN_STAGE_FDT_BASELINE (see
 * open_run() further down). fdt_mode_idle (above) is the fourth member of
 * this family, used with 24 zero bytes to *measure* the template; these
 * three arm/query it. */
static const guint8 fdt_mode_armed[2] = { 0x8d, 0x01 };
static const guint8 fdt_down_armed[2] = { 0x0c, 0x01 };
static const guint8 fdt_up_armed[2] = { 0x0e, 0x01 };

/* driver_53xc.py reads mcu_switch_to_fdt_up()'s reply with timeout=None
 * (block indefinitely) -- the sensor isn't waiting on any further
 * external input at this point (finger already detected), so a generous
 * bounded timeout stands in safely for "no timeout" here. */
#define GOODIX533C_FDT_UP_TIMEOUT_MS (5000)

#define GOODIX533C_PSK_LENGTH (32)
#define GOODIX533C_PSK_FLAGS (0xbb020001)
/* sha256(bytes(32)) -- expected PSK hash, all-zero PSK per this whole
 * device family's convention. See findings doc: this driver must never
 * write a PSK, so we only ever compare. */
#define GOODIX533C_PSK_SHA256 \
  "66687aadf862bd776c8fc18b8e9f8e20089714856ee233b3902a591d0d5f2925"

#define GOODIX533C_FIRMWARE_REGEX "^GF5288_GM168SEC_APP_1[0-9]{4}$"

static const guint8 fdt_mode_idle[2] = { 0x0d, 0x01 };

/* Captured from the real vendor driver -- see findings doc. Not
 * byte-identical to goodix53x5's default config. */
static const guint8 device_config[256] = {
  0x40, 0x11, 0x6c, 0x7d, 0x28, 0xa5, 0x28, 0xcd, 0x1c, 0xe9, 0x10, 0xf9,
  0x00, 0xf9, 0x00, 0xf9, 0x00, 0x04, 0x02, 0x00, 0x00, 0x08, 0x00, 0x11,
  0x11, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x07, 0x00, 0x84, 0x00, 0xbe,
  0xb2, 0x86, 0x00, 0xc5, 0xb9, 0x88, 0x00, 0xb5, 0xad, 0x8a, 0x00, 0x9d,
  0x95, 0x8c, 0x00, 0x00, 0xbe, 0x8e, 0x00, 0x00, 0xc5, 0x90, 0x00, 0x00,
  0xb5, 0x92, 0x00, 0x00, 0x9d, 0x94, 0x00, 0x00, 0xaf, 0x96, 0x00, 0x00,
  0xbf, 0x98, 0x00, 0x00, 0xb6, 0x9a, 0x00, 0x00, 0xa7, 0x30, 0x00, 0x6c,
  0x1c, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00,
  0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x26, 0x00, 0x00,
  0x12, 0x20, 0x00, 0x10, 0x40, 0x12, 0x00, 0x03, 0x04, 0x02, 0x02, 0x16,
  0x21, 0x2c, 0x02, 0x0a, 0x03, 0x2a, 0x01, 0x02, 0x00, 0x22, 0x00, 0x01,
  0x20, 0x24, 0x00, 0x32, 0x00, 0x80, 0x00, 0x05, 0x04, 0x5c, 0x00, 0x00,
  0x01, 0x56, 0x00, 0x28, 0x20, 0x58, 0x00, 0x01, 0x00, 0x32, 0x00, 0x24,
  0x02, 0x82, 0x00, 0x80, 0x0c, 0x20, 0x02, 0x88, 0x0d, 0x2a, 0x01, 0x92,
  0x07, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14, 0x00, 0x80, 0x00, 0x05,
  0x04, 0x5c, 0x00, 0x94, 0x00, 0x56, 0x00, 0x08, 0x20, 0x58, 0x00, 0x03,
  0x00, 0x32, 0x00, 0x08, 0x04, 0x82, 0x00, 0x80, 0x11, 0x20, 0x02, 0x28,
  0x0c, 0x2a, 0x01, 0x18, 0x04, 0x5c, 0x00, 0x94, 0x00, 0x54, 0x00, 0x00,
  0x01, 0x62, 0x00, 0x09, 0x03, 0x64, 0x00, 0x18, 0x00, 0x82, 0x00, 0x80,
  0x0c, 0x20, 0x02, 0x28, 0x0c, 0x2a, 0x01, 0x18, 0x04, 0x5c, 0x00, 0x94,
  0x00, 0x52, 0x00, 0x08, 0x00, 0x54, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x51, 0x13,
};

#define GOODIX533C_IMAGE_BYTES \
  (GOODIX533C_SENSOR_WIDTH * GOODIX533C_SENSOR_HEIGHT * 3 / 2)
#define GOODIX533C_IMAGE_PIXELS \
  (GOODIX533C_SENSOR_WIDTH * GOODIX533C_SENSOR_HEIGHT)

G_DEFINE_TYPE (FpiDeviceGoodix533c, fpi_device_goodix533c, FP_TYPE_DEVICE)

/* ===========================================================================
 * Low level receive/dispatch, ported from goodix.c's
 * goodix_receive_{data,data_cb,pack,protocol,ack,done} and
 * goodix_start_read_loop / goodix_send_{data,pack,protocol}.
 * ======================================================================= */

static void receive_data (FpDevice *dev);

static void
deliver_reply (FpDevice *dev, guint8 *data, guint16 length, GError *error)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  Goodix533cCmdCallback callback = self->callback;
  gpointer user_data = self->user_data;

  if (!(self->ack_pending || self->reply_pending))
    {
      g_clear_error (&error);
      return;
    }

  if (self->timeout_src)
    g_clear_pointer (&self->timeout_src, g_source_destroy);
  self->ack_pending = FALSE;
  self->reply_pending = FALSE;
  self->callback = NULL;
  self->user_data = NULL;

  if (callback)
    callback (dev, data, length, user_data, error);
  else
    g_clear_error (&error);
}

static void
handle_ack (FpDevice *dev, guint8 *payload, guint16 length)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  GoodixAck *ack = (GoodixAck *) payload;

  if (length != sizeof (GoodixAck))
    {
      fp_warn ("Invalid ACK length: %d", length);
      return;
    }

  if (!ack->always_true)
    {
      fp_warn ("Invalid ACK flags: 0x%02x", payload[1]);
      return;
    }

  if (ack->has_no_config)
    fp_warn ("MCU has no config");

  if (self->cmd != ack->cmd)
    {
      fp_warn ("Invalid ACK command: 0x%02x (expected 0x%02x)", ack->cmd,
               self->cmd);
      return;
    }

  if (!self->ack_pending)
    {
      fp_warn ("Didn't expect an ACK for command: 0x%02x", self->cmd);
      return;
    }

  if (!self->reply_pending)
    {
      deliver_reply (dev, NULL, 0, NULL);
      return;
    }

  self->ack_pending = FALSE;
}

static void
handle_protocol_pack (FpDevice *dev, guint8 *payload, guint32 length)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  guint8 cmd;
  g_autofree guint8 *inner = NULL;
  guint16 inner_len;
  gboolean valid_checksum, valid_null_checksum;

  if (!goodix_decode_protocol (payload, length, &cmd, &inner, &inner_len,
                               &valid_checksum, &valid_null_checksum))
    {
      fp_warn ("Incomplete protocol message, size: %u", length);
      return;
    }

  if (cmd == GOODIX_CMD_ACK)
    {
      handle_ack (dev, inner, inner_len);
      return;
    }

  if (self->cmd != cmd)
    {
      fp_warn ("Unexpected protocol command: 0x%02x (expected 0x%02x)", cmd,
               self->cmd);
      return;
    }

  if (!self->reply_pending)
    {
      fp_warn ("Didn't expect a reply for command: 0x%02x", self->cmd);
      return;
    }

  deliver_reply (dev, inner, inner_len, NULL);
}

static void
receive_pack (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  guint8 flags;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum;

  self->rx_buf = g_realloc (self->rx_buf, self->rx_len + length);
  memcpy (self->rx_buf + self->rx_len, data, length);
  self->rx_len += length;

  if (!goodix_decode_pack (self->rx_buf, self->rx_len, &flags, &payload,
                           &payload_len, &valid_checksum))
    {
      /* Not a full pack yet -- wait for more data. */
      return;
    }

  switch (flags)
    {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      handle_protocol_pack (dev, payload, payload_len);
      break;

    case GOODIX_FLAGS_TLS:
    case GOODIX533C_FLAGS_TLS_DATA:
      /* Raw payload, delivered unconditionally to whatever command is
       * currently expecting a reply -- matches goodix.c's handling of
       * GOODIX_FLAGS_TLS packs (used both for handshake bytes and, here,
       * for TLS_DATA-flagged mcu_get_image replies). */
      deliver_reply (dev, payload, payload_len, NULL);
      break;

    default:
      fp_warn ("Unknown pack flags: 0x%02x", flags);
      break;
    }

  g_clear_pointer (&self->rx_buf, g_free);
  self->rx_len = 0;
}

static void
receive_data_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                 gpointer user_data, GError *error)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  if (g_cancellable_is_cancelled (self->transfer_cancel_tkn))
    return;

  if (error)
    {
      fp_warn ("Receive data error: %s", error->message);
      g_error_free (error);
      receive_data (dev);
      return;
    }

  receive_pack (dev, transfer->buffer, (guint32) transfer->actual_length);
  receive_data (dev);
}

static void
receive_data (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

  transfer->short_is_error = FALSE;
  fpi_usb_transfer_fill_bulk (transfer, GOODIX533C_EP_IN,
                              GOODIX_EP_IN_MAX_BUF_SIZE);
  fpi_usb_transfer_submit (transfer, 0, self->transfer_cancel_tkn,
                           receive_data_cb, NULL);
}

static void
start_read_loop (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  if (self->read_loop_started)
    return;

  self->read_loop_started = TRUE;
  if (g_cancellable_is_cancelled (self->transfer_cancel_tkn))
    g_cancellable_reset (self->transfer_cancel_tkn);

  receive_data (dev);
}

static gboolean
send_data (FpDevice *dev, guint8 *data, guint32 length,
          GDestroyNotify free_func, GError **error)
{
  for (guint32 i = 0; i < length; i += GOODIX_EP_OUT_MAX_BUF_SIZE)
    {
      FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);

      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, GOODIX533C_EP_OUT, data + i,
                                       GOODIX_EP_OUT_MAX_BUF_SIZE, NULL);

      if (!fpi_usb_transfer_submit_sync (transfer, GOODIX533C_TIMEOUT_MS,
                                         error))
        {
          if (free_func)
            free_func (data);
          fpi_usb_transfer_unref (transfer);
          return FALSE;
        }
      fpi_usb_transfer_unref (transfer);
    }

  if (free_func)
    free_func (data);
  return TRUE;
}

static gboolean
send_pack (FpDevice *dev, guint8 flags, guint8 *payload, guint16 length,
          GDestroyNotify free_func, GError **error)
{
  guint8 *data;
  guint32 data_len;

  goodix_encode_pack (flags, payload, length, TRUE, &data, &data_len);
  if (free_func)
    free_func (payload);

  return send_data (dev, data, data_len, g_free, error);
}

static void
on_command_timeout (FpDevice *dev, gpointer user_data)
{
  GError *error = NULL;
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
              "Command timed out: 0x%02x", self->cmd);
  deliver_reply (dev, NULL, 0, error);
}

static void
send_protocol (FpDevice *dev, guint8 cmd, const guint8 *payload,
              guint16 length, gboolean calc_checksum, guint timeout_ms,
              gboolean expect_ack, gboolean expect_reply,
              Goodix533cCmdCallback callback, gpointer user_data)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  GError *error = NULL;
  guint8 *data;
  guint32 data_len;

  if (self->ack_pending || self->reply_pending)
    {
      fp_warn ("A command is already running: 0x%02x", self->cmd);
      return;
    }

  fp_dbg ("Running command: 0x%02x", cmd);

  if (timeout_ms)
    self->timeout_src = fpi_device_add_timeout (dev, timeout_ms,
                                                on_command_timeout, NULL,
                                                NULL);
  self->cmd = cmd;
  self->ack_pending = expect_ack;
  self->reply_pending = expect_reply;
  self->callback = callback;
  self->user_data = user_data;

  goodix_encode_protocol (cmd, payload, length, calc_checksum, FALSE, &data,
                          &data_len);

  if (!send_pack (dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len, g_free,
                  &error))
    {
      deliver_reply (dev, NULL, 0, error);
      return;
    }
}

/* ===========================================================================
 * Specific commands actually needed by the capture sequence in
 * capture_golden_session.py. Every payload shape below is taken directly
 * from driver_53xc.py / goodix.py, not from goodix.c (goodix.c's
 * preset_psk_read and fdt_down/fdt_up payload shapes disagree with the
 * Python driver -- see the discrepancies noted in the final report).
 * ======================================================================= */

static void
cmd_nop (FpDevice *dev, Goodix533cCmdCallback callback, gpointer user_data)
{
  guint8 payload[4] = { 0x00, 0x00, 0x00, 0x00 };

  /* Ack-only, no data reply -- matches goodix.py's nop(), which only ever
   * calls _expect_ack(). Some sensors don't answer NOP at all, which the
   * caller is expected to tolerate as a timeout, not an error. */
  send_protocol (dev, GOODIX_CMD_NOP, payload, sizeof (payload), FALSE,
                 GOODIX533C_TIMEOUT_MS, TRUE, FALSE, callback, user_data);
}

static void
cmd_firmware_version (FpDevice *dev, Goodix533cCmdCallback callback,
                      gpointer user_data)
{
  guint8 payload[2] = { 0x00, 0x00 };

  send_protocol (dev, GOODIX_CMD_FIRMWARE_VERSION, payload, sizeof (payload),
                 TRUE, GOODIX533C_TIMEOUT_MS, TRUE, TRUE, callback,
                 user_data);
}

static void
cmd_preset_psk_read (FpDevice *dev, guint32 flags, guint32 length,
                     guint32 offset, Goodix533cCmdCallback callback,
                     gpointer user_data)
{
  guint8 payload[16];
  guint32 length_le = GUINT32_TO_LE (length);
  guint32 offset_le = GUINT32_TO_LE (offset);
  guint32 flags_le = GUINT32_TO_LE (flags);
  guint32 zero_le = GUINT32_TO_LE (0);

  /* payload isn't guaranteed 4-byte aligned, so store via memcpy rather
   * than an unaligned guint32* cast (UB, and a real SIGBUS risk on
   * strict-alignment architectures). */
  memcpy (payload + 0, &length_le, sizeof (length_le));
  memcpy (payload + 4, &offset_le, sizeof (offset_le));
  memcpy (payload + 8, &flags_le, sizeof (flags_le));
  memcpy (payload + 12, &zero_le, sizeof (zero_le));

  send_protocol (dev, GOODIX_CMD_PRESET_PSK_READ, payload, sizeof (payload),
                 TRUE, GOODIX533C_TIMEOUT_MS, TRUE, TRUE, callback,
                 user_data);
}

static void
cmd_reset (FpDevice *dev, gboolean reset_sensor, gboolean soft_reset_mcu,
          guint8 sleep_time, Goodix533cCmdCallback callback,
          gpointer user_data)
{
  guint8 payload[2];

  payload[0] = (reset_sensor ? 0x1 : 0x0) | (soft_reset_mcu ? 0x1 : 0x0) << 1 |
    (reset_sensor ? 0x1 : 0x0) << 2;
  payload[1] = sleep_time;

  send_protocol (dev, GOODIX_CMD_RESET, payload, sizeof (payload), TRUE,
                 GOODIX533C_TIMEOUT_MS, TRUE, TRUE, callback, user_data);
}

static void
cmd_read_sensor_register (FpDevice *dev, guint16 address, guint8 length,
                          Goodix533cCmdCallback callback, gpointer user_data)
{
  guint8 payload[4];
  guint16 address_le = GUINT16_TO_LE (address);

  payload[0] = 0x00;
  memcpy (payload + 1, &address_le, sizeof (address_le));
  payload[3] = length;

  send_protocol (dev, GOODIX_CMD_READ_SENSOR_REGISTER, payload,
                 sizeof (payload), TRUE, GOODIX533C_TIMEOUT_MS, TRUE, TRUE,
                 callback, user_data);
}

static void
cmd_write_sensor_register (FpDevice *dev, guint16 address,
                           const guint8 value[2],
                           Goodix533cCmdCallback callback, gpointer user_data)
{
  guint8 payload[5];
  guint16 address_le = GUINT16_TO_LE (address);

  payload[0] = 0x00;
  memcpy (payload + 1, &address_le, sizeof (address_le));
  payload[3] = value[0];
  payload[4] = value[1];

  /* Ack-only, no data reply -- matches goodix.py's write_sensor_register(),
   * which only ever calls _expect_ack(). */
  send_protocol (dev, GOODIX_CMD_WRITE_SENSOR_REGISTER, payload,
                 sizeof (payload), TRUE, GOODIX533C_TIMEOUT_MS, TRUE, FALSE,
                 callback, user_data);
}

static void
cmd_read_otp (FpDevice *dev, Goodix533cCmdCallback callback,
             gpointer user_data)
{
  guint8 payload[2] = { 0x00, 0x00 };

  send_protocol (dev, GOODIX_CMD_READ_OTP, payload, sizeof (payload), TRUE,
                 GOODIX533C_TIMEOUT_MS, TRUE, TRUE, callback, user_data);
}

static void
cmd_upload_config_mcu (FpDevice *dev, const guint8 *config, guint16 length,
                       Goodix533cCmdCallback callback, gpointer user_data)
{
  send_protocol (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length, TRUE,
                 GOODIX533C_TIMEOUT_MS, TRUE, TRUE, callback, user_data);
}

static void
cmd_mcu_switch_to_fdt_mode (FpDevice *dev, const guint8 *mode, guint16 length,
                            gboolean expect_reply,
                            Goodix533cCmdCallback callback,
                            gpointer user_data)
{
  send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length, TRUE,
                 GOODIX533C_TIMEOUT_MS, TRUE, expect_reply, callback,
                 user_data);
}

static void
cmd_mcu_get_image_gain (FpDevice *dev, guint8 flags, guint8 gain,
                        Goodix533cCmdCallback callback, gpointer user_data)
{
  guint8 payload[4] = { flags, 0x06, gain, 0x00 };

  send_protocol (dev, GOODIX_CMD_MCU_GET_IMAGE, payload, sizeof (payload),
                 TRUE, GOODIX533C_TIMEOUT_MS, TRUE, TRUE, callback,
                 user_data);
}

static void
cmd_mcu_switch_to_sleep_mode (FpDevice *dev, Goodix533cCmdCallback callback,
                              gpointer user_data)
{
  guint8 payload[2] = { 0x01, 0x00 };

  /* Ack-only -- matches goodix.py's mcu_switch_to_sleep_mode(), which only
   * ever calls _expect_ack(). */
  send_protocol (dev, GOODIX533C_CMD_MCU_SWITCH_TO_SLEEP_MODE, payload,
                 sizeof (payload), TRUE, GOODIX533C_TIMEOUT_MS, TRUE, FALSE,
                 callback, user_data);
}

static void
cmd_query_mcu_state (FpDevice *dev, const guint8 *payload, guint16 length,
                     Goodix533cCmdCallback callback, gpointer user_data)
{
  /* This driver's one call site (run_driver()'s query_mcu_state(b"\x01\x00
   * \x01", False) right after mcu_switch_to_sleep_mode()) always passes
   * reply=False in driver_53xc.py -- ACK-only here, matching that. The
   * reply=True data-read path (goodix.py's query_mcu_state()) is unused
   * and not implemented. */
  send_protocol (dev, GOODIX_CMD_QUERY_MCU_STATE, payload, length, TRUE,
                 GOODIX533C_TIMEOUT_MS, TRUE, FALSE, callback, user_data);
}

static void
cmd_mcu_switch_to_fdt_down (FpDevice *dev, const guint8 *mode, guint16 length,
                            Goodix533cCmdCallback callback, gpointer user_data)
{
  /* Ack-only -- arms finger detection. The actual touch notification
   * arrives later as a separate, asynchronous protocol pack tagged with
   * this same command (see await_fdt_down_push() below), not as a reply
   * to this call. Matches driver_53xc.py's one call site,
   * mcu_switch_to_fdt_down(mode, False). */
  send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length, TRUE,
                 GOODIX533C_TIMEOUT_MS, TRUE, FALSE, callback, user_data);
}

static void
cmd_mcu_switch_to_fdt_up (FpDevice *dev, const guint8 *mode, guint16 length,
                          Goodix533cCmdCallback callback, gpointer user_data)
{
  /* ACK, then always a data reply -- see GOODIX533C_FDT_UP_TIMEOUT_MS. */
  send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length, TRUE,
                 GOODIX533C_FDT_UP_TIMEOUT_MS, TRUE, TRUE, callback,
                 user_data);
}

/**
 * await_fdt_down_push: wait for the device's unsolicited "finger touched"
 * notification.
 *
 * No request is sent here -- the device pushes this pack on its own, some
 * time after cmd_mcu_switch_to_fdt_down() armed detection, once (and only
 * once) a finger actually lands. Manual protocol-reply state set, same
 * shape as await_raw_pack() above (used for the TLS handshake's raw
 * packs), just matched against a specific command byte instead of
 * bypassing cmd matching entirely.
 *
 * driver_53xc.py's wait_for_finger() polls with a sequence of short (2s)
 * blocking reads for up to 30s, working around a PyUSB limitation on long
 * reads. FpiUsbTransfer has no such limitation -- the read loop
 * (receive_data()) already has one bulk IN transfer permanently
 * in-flight, so a single bounded timeout on the reply we're waiting for
 * does the same job without polling.
 */
static void
await_fdt_down_push (FpDevice *dev, guint timeout_ms,
                     Goodix533cCmdCallback callback, gpointer user_data)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  if (self->ack_pending || self->reply_pending)
    {
      /* Must not silently hang the caller -- report it as a real failure,
       * same as send_protocol() would if it could (it can only fp_warn()
       * and drop, since it has no callback contract for this case; here
       * we do have one, so use it). */
      GError *error = NULL;

      fp_warn ("A command is already running: 0x%02x", self->cmd);
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_BUSY,
                  "Cannot wait for finger: command 0x%02x still in flight",
                  self->cmd);
      callback (dev, NULL, 0, user_data, error);
      return;
    }

  if (timeout_ms)
    self->timeout_src = fpi_device_add_timeout (dev, timeout_ms,
                                                on_command_timeout, NULL,
                                                NULL);
  self->cmd = GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN;
  self->ack_pending = FALSE;
  self->reply_pending = TRUE;
  self->callback = callback;
  self->user_data = user_data;
}

static void
cmd_request_tls_connection (FpDevice *dev, Goodix533cCmdCallback callback,
                            gpointer user_data)
{
  guint8 payload[2] = { 0x00, 0x00 };

  /* No timeout, matching goodix.c: the handshake round trip through the
   * embedded TLS server can legitimately take a little while. */
  send_protocol (dev, GOODIX_CMD_REQUEST_TLS_CONNECTION, payload,
                 sizeof (payload), TRUE, 0, TRUE, TRUE, callback, user_data);
}

static void
cmd_tls_successfully_established (FpDevice *dev,
                                  Goodix533cCmdCallback callback,
                                  gpointer user_data)
{
  guint8 payload[2] = { 0x00, 0x00 };

  /* goodix.c uses a 10ms timeout here and notes in a comment that it
   * "always times out for some reason" on real hardware -- driver_53xc.py's
   * _expect_ack() has no special-cased timeout for this command, so use the
   * same generic one as everything else instead of that known-bad value. */
  send_protocol (dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED, payload,
                 sizeof (payload), TRUE, GOODIX533C_TIMEOUT_MS, TRUE, FALSE,
                 callback, user_data);
}

/* ===========================================================================
 * TLS handshake pump -- ported near-verbatim from goodix.c's
 * on_goodix_tls_read_handshake / tls_handshake_run / tls_handshake_done /
 * do_tls_handshake / on_goodix_request_tls_connection, cross-checked stage
 * for stage against driver_53xc.py's establish_tls(). Only the private
 * struct access changed.
 * ======================================================================= */

enum tls_handshake_stage {
  TLS_STAGE_HELLO_S,
  TLS_STAGE_KH_EXCHANGE,
  TLS_STAGE_CHANGE_CIPHER_C,
  TLS_STAGE_HANDSHAKE_C,
  TLS_STAGE_CHANGE_CIPHER_S,
  TLS_STAGE_NUM,
};

typedef struct
{
  Goodix533cCmdCallback callback;
  gpointer              user_data;
} TlsReadyData;

static void
on_tls_raw_read (FpDevice *dev, guint8 *data, guint16 length,
                 gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  int sent;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  sent = goodix_tls_client_write (&self->tls, data, length);
  if (sent < 0)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, sent,
                                             "failed to write to tls server"));
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
await_raw_pack (FpDevice *dev, Goodix533cCmdCallback callback,
               gpointer user_data)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  self->callback = callback;
  self->user_data = user_data;
  self->reply_pending = TRUE;
  self->ack_pending = FALSE;
  self->cmd = GOODIX_CMD_ACK; /* never matched directly; TLS packs bypass
                               * cmd matching entirely in receive_pack(). */
}

static void
tls_handshake_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  int stage = fpi_ssm_get_cur_state (ssm);
  guint8 buff[2048];
  int size;
  GError *error = NULL;

  switch (stage)
    {
    case TLS_STAGE_HELLO_S:
      size = goodix_tls_client_read (&self->tls, buff, sizeof (buff));
      if (size < 0)
        {
          fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, size,
                                                 "failed to read tls server hello"));
          return;
        }
      if (!send_pack (dev, GOODIX_FLAGS_TLS, buff, (guint16) size, NULL,
                      &error))
        {
          fpi_ssm_mark_failed (ssm, error);
          return;
        }
      fpi_ssm_next_state (ssm);
      break;

    case TLS_STAGE_KH_EXCHANGE:
    case TLS_STAGE_CHANGE_CIPHER_C:
    case TLS_STAGE_HANDSHAKE_C:
      await_raw_pack (dev, on_tls_raw_read, ssm);
      break;

    case TLS_STAGE_CHANGE_CIPHER_S:
      size = goodix_tls_client_read (&self->tls, buff, sizeof (buff));
      if (size < 0)
        {
          fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, size,
                                                 "failed to read final server handshake"));
          return;
        }
      if (!send_pack (dev, GOODIX_FLAGS_TLS, buff, (guint16) size, NULL,
                      &error))
        {
          fpi_ssm_mark_failed (ssm, error);
          return;
        }
      fpi_ssm_next_state (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
on_tls_successfully_established (FpDevice *dev, guint8 *data, guint16 length,
                                 gpointer user_data, GError *error)
{
  TlsReadyData *ready = user_data;

  ready->callback (dev, NULL, 0, ready->user_data, error);
  g_free (ready);
}

static void
tls_handshake_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  TlsReadyData *ready = fpi_ssm_get_data (ssm);

  if (error)
    {
      fp_warn ("TLS handshake failed: %s", error->message);
      ready->callback (dev, NULL, 0, ready->user_data, error);
      g_free (ready);
      return;
    }

  cmd_tls_successfully_established (dev, on_tls_successfully_established,
                                    ready);
}

static void
on_request_tls_connection_reply (FpDevice *dev, guint8 *data, guint16 length,
                                 gpointer user_data, GError *error)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  TlsReadyData *ready = user_data;
  FpiSsm *ssm;

  if (error)
    {
      ready->callback (dev, NULL, 0, ready->user_data, error);
      g_free (ready);
      return;
    }

  /* `data` is the device's raw ClientHello -- feed it into our embedded
   * TLS server's client side, then pump the handshake. */
  goodix_tls_client_write (&self->tls, data, length);

  ssm = fpi_ssm_new (dev, tls_handshake_run, TLS_STAGE_NUM);
  fpi_ssm_set_data (ssm, ready, NULL);
  fpi_ssm_start (ssm, tls_handshake_done);
}

/**
 * tls_connect: full TLS bring-up -- init the embedded TLS-PSK server,
 * request the connection from the device, pump the handshake, and tell the
 * device TLS is established. Ported from goodix.c's goodix_tls_init() +
 * goodix_tls_ready() + on_goodix_request_tls_connection().
 */
static void
tls_connect (FpDevice *dev, Goodix533cCmdCallback callback,
            gpointer user_data)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  TlsReadyData *ready;
  GError *error = NULL;

  g_assert (!self->tls_active);

  if (!goodix_tls_server_init (&self->tls, &error))
    {
      callback (dev, NULL, 0, user_data, error);
      return;
    }
  self->tls_active = TRUE;

  ready = g_new0 (TlsReadyData, 1);
  ready->callback = callback;
  ready->user_data = user_data;

  cmd_request_tls_connection (dev, on_request_tls_connection_reply, ready);
}

/* ===========================================================================
 * Image decode -- ported math from goodixtls5xx_decode_frame() /
 * goodixtls5xx_squash_frame_linear() in goodix5xx.c. Unlike goodix5xx.c's
 * version (which skips an 8-byte header specific to that decrypted payload
 * shape), driver_53xc.py's decode_image() operates on the decrypted
 * payload starting at byte 0 with no header -- the Python file is
 * authoritative for 533c, so no header skip here.
 * ======================================================================= */

static void
decode_frame (guint16 *pixels, const guint8 *raw, guint32 raw_len)
{
  guint16 *pix = pixels;
  guint32 i;

  for (i = 0; i + 6 <= raw_len; i += 6)
    {
      const guint8 *chunk = raw + i;

      *pix++ = (guint16) (((chunk[0] & 0xf) << 8) + chunk[1]);
      *pix++ = (guint16) ((chunk[3] << 4) + (chunk[0] >> 4));
      *pix++ = (guint16) (((chunk[5] & 0xf) << 8) + chunk[2]);
      *pix++ = (guint16) ((chunk[4] << 4) + (chunk[5] >> 4));
    }
}

static void
squash_frame_linear (const guint16 *frame, guint8 *squashed, guint32 count)
{
  guint16 min = 0xffff;
  guint16 max = 0;
  guint32 i;

  for (i = 0; i < count; i++)
    {
      if (frame[i] < min)
        min = frame[i];
      if (frame[i] > max)
        max = frame[i];
    }

  for (i = 0; i < count; i++)
    {
      if (max == min)
        squashed[i] = 0;
      else
        squashed[i] = (guint8) ((frame[i] - min) * 0xff / (max - min));
    }
}

/* ===========================================================================
 * Capture building blocks -- ported logic from capture_golden_session.py /
 * driver_53xc.py's run_driver(), refactored into reusable sub-SSMs.
 *
 * The original single-shot sequence (reset through the no-finger reference
 * frame, sleep/query, arm finger detection, wait for a touch, live frame,
 * flat-field against the reference) is split along a session-scoped vs.
 * attempt-scoped line:
 *
 *  - Session-scoped (reset through FDT baseline measurement) now lives in
 *    open_run() below -- it must only happen once per fp_device_open()
 *    session, not once per enroll/verify attempt, or enroll would mean 8
 *    full USB re-handshakes instead of 8 fast touches.
 *  - Attempt-scoped (reference capture, finger wait, live capture, finger
 *    up) becomes four sub-SSM starter functions
 *    (goodix533c_start_{ref_capture,finger_wait,live_capture,finger_up}_subsm(),
 *    declared in goodix533c-private.h), each usable as a child of any
 *    parent SSM via fpi_ssm_start_subsm(). fpi_device_goodix533c_capture_test()
 *    below chains all four for the standalone test harness;
 *    goodix533c-enroll.c and goodix533c-auth.c each chain them their own way
 *    (enroll repeats all four up to GOODIX533C_ENROLL_SAMPLES times; auth
 *    runs the sequence once, replacing "finger up" cleanup with a match
 *    step in between).
 * ======================================================================= */

static void
on_capture_step_reply (FpDevice *dev, guint8 *data, guint16 length,
                       gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  fpi_ssm_next_state (ssm);
}

static void
on_reset_reply (FpDevice *dev, guint8 *data, guint16 length,
                gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (length < 1 || data[0] != 0x01)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR,
                                             G_IO_ERROR_FAILED,
                                             "reset failed (status=%d)",
                                             length ? data[0] : -1));
      return;
    }

  fp_dbg ("Reset OK");
  fpi_ssm_next_state (ssm);
}

static void
on_tls_connected (FpDevice *dev, guint8 *data, guint16 length,
                  gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("TLS established");
  fpi_ssm_next_state (ssm);
}

static void
on_upload_config_reply (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (length < 1 || data[0] != 0x01)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "config upload rejected"));
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
on_fdt_baseline_reply (FpDevice *dev, guint8 *data, guint16 length,
                       gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  guint32 i;
  guint32 sample_count;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  /* Reply is a 4-byte header then 12-bit samples as 16-bit LE words. Vendor
   * driver halves each sample and emits it twice as the FDT threshold
   * template -- see fdt_template() in driver_53xc.py. Appended (with a
   * distinct fixed 2-byte prefix) to every later FDT arm/query command in
   * this same session -- see fdt_mode_armed/fdt_down_armed/fdt_up_armed
   * above and their use in the sub-SSM handlers below (finger_wait_ssm_handler,
   * finger_up_ssm_handler). */
  /* Sample count: driver_53xc.py's fdt_template() computes this as
   * len(range(4, length - 1, 2)), which looks off-by-one against the
   * naive (length - 4) / 2 used below at first glance, but is not --
   * range(4, length-1, 2) has floor((length-6)/2)+1 terms (for length>=6,
   * else 0), and floor(x)+1 == floor(x+1) for any real x when 1 is an
   * integer, so that's floor((length-6)/2 + 1) == floor((length-4)/2) --
   * exactly the integer-division formula below. Verified algebraically,
   * not just against this session's one hardware reply (which happened to
   * land on the boundary case, length=28, 12 samples, where both
   * formulas trivially agree). The MIN(..., 12u) cap has no Python
   * equivalent -- Python's `samples` is an unbounded list, but this
   * driver's fdt_template is a fixed 24-byte (12-sample) array because
   * every FDT arm/query payload below is hardcoded to a fixed 2-byte
   * prefix + 24-byte template (matching driver_53xc.py's own
   * FDT_MODE_ARMED + template, etc., which are always built from a
   * 24-byte template in practice); the cap only ever discards *extra*
   * data past the first 12 samples, it does not change which of the
   * first 12 samples are read. */
  memset (self->fdt_template, 0, sizeof (self->fdt_template));
  sample_count = MIN ((guint32) (length > 4 ? (length - 4) / 2 : 0), 12u);
  for (i = 0; i < sample_count; i++)
    {
      guint16 sample = (guint16) (data[4 + i * 2] | (data[4 + i * 2 + 1] << 8));
      self->fdt_template[i * 2] = self->fdt_template[i * 2 + 1] =
        (guint8) (sample >> 1);
    }
  self->have_fdt_template = TRUE;

  fp_dbg ("FDT baseline measured (%u samples)", sample_count);
  fpi_ssm_next_state (ssm);
}

/**
 * decode_get_image_reply: shared by the reference-frame and live-frame
 * GET_IMAGE stages -- decrypt a mcu_get_image reply and decode it to
 * pixels, optionally also min-max squashing to 8 bits. @out_squashed may
 * be NULL if the caller doesn't need that (the live frame is squashed
 * only after flat-fielding, not here).
 */
static gboolean
decode_get_image_reply (FpiDeviceGoodix533c *self, guint8 *data,
                        guint16 length, guint16 **out_raw_pixels,
                        guint8 **out_squashed, GError **error)
{
  guint8 decrypt_buf[65535];
  int decrypted;

  if (length <= GOODIX533C_IMAGE_REPLY_HEADER_LEN)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "image reply too short: %d", length);
      return FALSE;
    }

  /* Skip the pre-record header (see GOODIX533C_IMAGE_REPLY_HEADER_LEN's
   * doc comment), feed the raw TLS record into the embedded server's
   * client side, then read the decrypted plaintext back out. */
  goodix_tls_client_write (&self->tls,
                           data + GOODIX533C_IMAGE_REPLY_HEADER_LEN,
                           (guint16) (length - GOODIX533C_IMAGE_REPLY_HEADER_LEN));

  decrypted = goodix_tls_server_read (&self->tls, decrypt_buf,
                                      sizeof (decrypt_buf), error);
  if (decrypted <= 0)
    {
      if (error && !*error)
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "TLS decrypt failed");
      return FALSE;
    }

  if ((guint32) decrypted < GOODIX533C_IMAGE_BYTES)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                  "short decrypt: %d < %d", decrypted,
                  GOODIX533C_IMAGE_BYTES);
      return FALSE;
    }

  *out_raw_pixels = g_new0 (guint16, GOODIX533C_IMAGE_PIXELS);
  decode_frame (*out_raw_pixels, decrypt_buf, GOODIX533C_IMAGE_BYTES);

  if (out_squashed)
    {
      *out_squashed = g_new0 (guint8, GOODIX533C_IMAGE_PIXELS);
      squash_frame_linear (*out_raw_pixels, *out_squashed,
                           GOODIX533C_IMAGE_PIXELS);
    }

  fp_dbg ("Decoded frame: %d bytes encrypted -> %d bytes plain -> %d pixels",
          length, decrypted, GOODIX533C_IMAGE_PIXELS);

  return TRUE;
}

/**
 * on_ref_get_image_reply: GET_IMAGE reply handler for the no-finger
 * reference capture. Stores the decoded raw12 frame into
 * self->reference_pixels, replacing whatever the previous attempt (or
 * open() session) left there -- each enroll stage / verify attempt
 * re-measures its own fresh reference immediately before its live capture.
 */
static void
on_ref_get_image_reply (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  guint16 *raw_pixels = NULL;
  GError *decode_error = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (!decode_get_image_reply (self, data, length, &raw_pixels, NULL,
                               &decode_error))
    {
      fpi_ssm_mark_failed (ssm, decode_error);
      return;
    }

  g_clear_pointer (&self->reference_pixels, g_free);
  self->reference_pixels = raw_pixels;
  self->have_reference = TRUE;

  fpi_ssm_next_state (ssm);
}

/**
 * on_live_get_image_reply: GET_IMAGE reply handler for the live
 * (finger-present) capture. Stores the decoded raw12 frame into
 * self->live_raw_pixels -- flat-fielding against the reference and
 * computing the clipped-fraction quality metric happens later, in the
 * live-capture sub-SSM's PROCESS state, not here.
 */
static void
on_live_get_image_reply (FpDevice *dev, guint8 *data, guint16 length,
                         gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  guint16 *raw_pixels = NULL;
  GError *decode_error = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (!decode_get_image_reply (self, data, length, &raw_pixels, NULL,
                               &decode_error))
    {
      fpi_ssm_mark_failed (ssm, decode_error);
      return;
    }

  g_clear_pointer (&self->live_raw_pixels, g_free);
  self->live_raw_pixels = raw_pixels;

  fpi_ssm_next_state (ssm);
}

static void
on_wait_finger_reply (FpDevice *dev, guint8 *data, guint16 length,
                      gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        {
          /* Give the caller a specific, actionable error instead of a bare
           * protocol-layer timeout -- this is the expected, well-behaved
           * outcome of running the sequence with no finger on the sensor. */
          g_clear_error (&error);
          error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL,
                                            "No finger detected within %d "
                                            "seconds",
                                            GOODIX533C_FINGER_WAIT_TIMEOUT_MS / 1000);
        }
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fp_dbg ("Finger detected (fdt_down push, %d bytes)", length);
  fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_PRESENT,
                                           FP_FINGER_STATUS_NEEDED);
  fpi_ssm_next_state (ssm);
}

/**
 * flat_field_squash: port of flat_field() in driver_53xc.py (ordinary
 * least-squares regression of the live frame against the reference frame,
 * then subtract the fitted line) followed by a min-max stretch of the
 * (possibly negative, possibly >12-bit) residual to 8 bits -- the same
 * squash technique squash_frame_linear() above uses for a raw frame, just
 * over a double-precision residual instead of guint16 samples.
 */
static void
flat_field_squash (const guint16 *frame, const guint16 *reference,
                   guint32 count, guint8 *out)
{
  double mean_frame = 0, mean_reference = 0;
  double variance = 0, covariance = 0;
  double a, b;
  g_autofree double *residual = g_new (double, count);
  double min = G_MAXDOUBLE, max = -G_MAXDOUBLE;
  guint32 i;

  for (i = 0; i < count; i++)
    {
      mean_frame += frame[i];
      mean_reference += reference[i];
    }
  mean_frame /= count;
  mean_reference /= count;

  for (i = 0; i < count; i++)
    {
      double d = (double) reference[i] - mean_reference;

      variance += d * d;
      covariance += ((double) frame[i] - mean_frame) * d;
    }
  if (variance == 0)
    variance = 1;

  a = covariance / variance;
  b = mean_frame - a * mean_reference;

  for (i = 0; i < count; i++)
    {
      residual[i] = (double) frame[i] - (a * (double) reference[i] + b);
      if (residual[i] < min)
        min = residual[i];
      if (residual[i] > max)
        max = residual[i];
    }

  for (i = 0; i < count; i++)
    {
      if (max <= min)
        out[i] = 0;
      else
        out[i] = (guint8) (((residual[i] - min) * 0xff) / (max - min));
    }
}

/**
 * compute_clipped_fraction: fraction of raw12 pixels at/above ADC full
 * scale, i.e. the non-contact area of a live frame. decode_frame() uses the
 * same 12-bit packing as the sibling goodix53x5 driver's
 * goodix_device_decode_image() (bit-identical chunk layout), and this
 * project's own gain sweep (NOTES.md, "Ridge visibility resolved") confirms
 * this device's raw samples span the same 0-4095 range, so
 * GOODIX533C_RAW12_CLIP reuses goodix53x5's GOODIX_RAW12_CLIP value as-is.
 */
static double
compute_clipped_fraction (const guint16 *img12)
{
  guint32 clipped = 0;
  guint32 i;

  for (i = 0; i < GOODIX533C_IMAGE_PIXELS; i++)
    if (img12[i] >= GOODIX533C_RAW12_CLIP)
      clipped++;

  return (double) clipped / GOODIX533C_IMAGE_PIXELS;
}

/* ===========================================================================
 * Reference-frame capture sub-SSM (attempt-scoped): power the sensor and
 * capture the TX-off no-finger reference frame into self->reference_pixels.
 * Must run before goodix533c_start_live_capture_subsm().
 * ======================================================================= */

enum ref_capture_stage {
  REF_CAPTURE_ON = 0,
  REF_CAPTURE_GET_IMAGE,
  REF_CAPTURE_OFF,
  REF_CAPTURE_NUM_STATES,
};

static void
ref_capture_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case REF_CAPTURE_ON:
      cmd_write_sensor_register (dev, GOODIX533C_CAPTURE_REGISTER,
                                 capture_on, on_capture_step_reply, ssm);
      break;

    case REF_CAPTURE_GET_IMAGE:
      cmd_mcu_get_image_gain (dev, GOODIX533C_IMAGE_FLAGS_CALIBRATE,
                              GOODIX533C_IMAGE_GAIN, on_ref_get_image_reply,
                              ssm);
      break;

    case REF_CAPTURE_OFF:
      cmd_write_sensor_register (dev, GOODIX533C_CAPTURE_REGISTER,
                                 capture_off, on_capture_step_reply, ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

void
goodix533c_start_ref_capture_subsm (FpiSsm *parent_ssm, FpDevice *dev)
{
  FpiSsm *sub = fpi_ssm_new (dev, ref_capture_ssm_handler,
                             REF_CAPTURE_NUM_STATES);

  fpi_ssm_start_subsm (parent_ssm, sub);
}

/* ===========================================================================
 * Finger-wait sub-SSM (attempt-scoped): arm finger-down detection and block
 * until the device's asynchronous touch notification arrives.
 * ======================================================================= */

enum finger_wait_stage {
  FINGER_WAIT_SLEEP = 0,
  FINGER_WAIT_QUERY_MCU_STATE,
  FINGER_WAIT_FDT_ARM_DOWN,
  FINGER_WAIT_WAIT_FOR_FINGER,
  FINGER_WAIT_FDT_MODE_ARM,
  FINGER_WAIT_NUM_STATES,
};

typedef struct
{
  Goodix533cProgressFunc cb;        /* nullable, see goodix533c-private.h */
  gpointer                user_data;
} FingerWaitData;

static void
finger_wait_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FINGER_WAIT_SLEEP:
      fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_PRESENT);
      cmd_mcu_switch_to_sleep_mode (dev, on_capture_step_reply, ssm);
      break;

    case FINGER_WAIT_QUERY_MCU_STATE:
        {
          /* Payload taken verbatim from run_driver()'s
           * query_mcu_state(b"\x01\x00\x01", False) call site. */
          static const guint8 payload[3] = { 0x01, 0x00, 0x01 };

          cmd_query_mcu_state (dev, payload, sizeof (payload),
                               on_capture_step_reply, ssm);
        }
      break;

    case FINGER_WAIT_FDT_ARM_DOWN:
        {
          guint8 mode[26];

          memcpy (mode, fdt_down_armed, sizeof (fdt_down_armed));
          memcpy (mode + sizeof (fdt_down_armed), self->fdt_template,
                 sizeof (self->fdt_template));
          cmd_mcu_switch_to_fdt_down (dev, mode, sizeof (mode),
                                      on_capture_step_reply, ssm);
        }
      break;

    case FINGER_WAIT_WAIT_FOR_FINGER:
        {
          FingerWaitData *data = fpi_ssm_get_data (ssm);

          if (data->cb)
            data->cb (dev, data->user_data);
          await_fdt_down_push (dev, GOODIX533C_FINGER_WAIT_TIMEOUT_MS,
                               on_wait_finger_reply, ssm);
        }
      break;

    case FINGER_WAIT_FDT_MODE_ARM:
        {
          guint8 mode[26];

          memcpy (mode, fdt_mode_armed, sizeof (fdt_mode_armed));
          memcpy (mode + sizeof (fdt_mode_armed), self->fdt_template,
                 sizeof (self->fdt_template));
          /* reply=True, matching driver_53xc.py's
           * mcu_switch_to_fdt_mode(FDT_MODE_ARMED + template, True) call
           * site -- but run_driver() never uses the returned payload
           * either, it just re-arms, so on_capture_step_reply discarding
           * it here is correct, not a shortcut. */
          cmd_mcu_switch_to_fdt_mode (dev, mode, sizeof (mode), TRUE,
                                     on_capture_step_reply, ssm);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

void
goodix533c_start_finger_wait_subsm (FpiSsm                 *parent_ssm,
                                    FpDevice               *dev,
                                    Goodix533cProgressFunc  wait_for_finger_cb,
                                    gpointer                wait_for_finger_data)
{
  FpiSsm *sub = fpi_ssm_new (dev, finger_wait_ssm_handler,
                             FINGER_WAIT_NUM_STATES);
  FingerWaitData *data = g_new0 (FingerWaitData, 1);

  data->cb = wait_for_finger_cb;
  data->user_data = wait_for_finger_data;
  fpi_ssm_set_data (sub, data, g_free);

  fpi_ssm_start_subsm (parent_ssm, sub);
}

/* ===========================================================================
 * Live-capture sub-SSM (attempt-scoped): capture the live (finger-present)
 * frame, decrypt/decode it, then flat-field it against
 * self->reference_pixels and compute the clipped-fraction quality metric --
 * both new relative to the original single-shot flow, needed so
 * enroll/verify/identify can quality-gate and match immediately, before
 * waiting for finger-up.
 * ======================================================================= */

enum live_capture_stage {
  LIVE_CAPTURE_ON = 0,
  LIVE_CAPTURE_GET_IMAGE,
  LIVE_CAPTURE_OFF,
  LIVE_CAPTURE_PROCESS,
  LIVE_CAPTURE_NUM_STATES,
};

static void
live_capture_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case LIVE_CAPTURE_ON:
      cmd_write_sensor_register (dev, GOODIX533C_CAPTURE_REGISTER,
                                 capture_on, on_capture_step_reply, ssm);
      break;

    case LIVE_CAPTURE_GET_IMAGE:
      /* Gain 0xc2, not driver_53xc.py's default 0x86 for the live frame --
       * see GOODIX533C_LIVE_IMAGE_GAIN's doc comment above for why this is
       * a deliberate, hardware-verified deviation on this unit. */
      cmd_mcu_get_image_gain (dev, GOODIX533C_IMAGE_FLAGS_LIVE,
                              GOODIX533C_LIVE_IMAGE_GAIN,
                              on_live_get_image_reply, ssm);
      break;

    case LIVE_CAPTURE_OFF:
      cmd_write_sensor_register (dev, GOODIX533C_CAPTURE_REGISTER,
                                 capture_off, on_capture_step_reply, ssm);
      break;

    case LIVE_CAPTURE_PROCESS:
      if (self->live_raw_pixels == NULL || !self->have_reference)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Missing reference or live frame"));
          return;
        }

      self->captured_clipped_fraction =
        compute_clipped_fraction (self->live_raw_pixels);

      g_clear_pointer (&self->captured_image, g_free);
      self->captured_image = g_new0 (guint8, GOODIX533C_IMAGE_PIXELS);
      flat_field_squash (self->live_raw_pixels, self->reference_pixels,
                         GOODIX533C_IMAGE_PIXELS, self->captured_image);

      fpi_ssm_next_state (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

void
goodix533c_start_live_capture_subsm (FpiSsm *parent_ssm, FpDevice *dev)
{
  FpiSsm *sub = fpi_ssm_new (dev, live_capture_ssm_handler,
                             LIVE_CAPTURE_NUM_STATES);

  fpi_ssm_start_subsm (parent_ssm, sub);
}

/* ===========================================================================
 * Finger-up sub-SSM (attempt-scoped): block until finger lift-off is
 * detected, so a lingering touch is never misread as the next attempt's
 * touch. mcu_switch_to_fdt_up's reply itself blocks until the device sees
 * the down->up transition (hardware-verified this session for a single
 * capture), so this is a thin wrapper around the existing command rather
 * than new detection logic.
 * ======================================================================= */

enum finger_up_stage {
  FINGER_UP_WAIT = 0,
  FINGER_UP_NUM_STATES,
};

static void
on_finger_up_reply (FpDevice *dev, guint8 *data, guint16 length,
                    gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  if (error)
    {
      /* A bare timeout here just means the user has not lifted their
       * finger within GOODIX533C_FDT_UP_TIMEOUT_MS (5s) yet -- unlike the
       * finger-wait timeout above, this is not necessarily user error, and
       * failing the whole enroll/verify/identify action over it would be
       * harsh, especially for enroll, which runs this after every one of
       * GOODIX533C_ENROLL_SAMPLES stages. Treat a timeout as "assume
       * lifted" and proceed instead of aborting the action.
       *
       * This reintroduces some of the staleness risk the wait exists to
       * prevent (a finger still down could be misread as part of the next
       * attempt) and has not been exercised against real hardware with a
       * deliberately slow lift-off -- see the report for what a human
       * needs to validate here. Any other error remains fatal. */
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        {
          fp_warn ("Finger-up wait timed out after %dms; assuming lifted "
                   "and continuing", GOODIX533C_FDT_UP_TIMEOUT_MS);
          g_clear_error (&error);
          fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NONE,
                                                   FP_FINGER_STATUS_PRESENT |
                                                   FP_FINGER_STATUS_NEEDED);
          fpi_ssm_next_state (ssm);
          return;
        }

      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  fpi_device_report_finger_status_changes (dev, FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_PRESENT |
                                           FP_FINGER_STATUS_NEEDED);
  fpi_ssm_next_state (ssm);
}

static void
finger_up_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case FINGER_UP_WAIT:
        {
          guint8 mode[26];

          memcpy (mode, fdt_up_armed, sizeof (fdt_up_armed));
          memcpy (mode + sizeof (fdt_up_armed), self->fdt_template,
                 sizeof (self->fdt_template));
          cmd_mcu_switch_to_fdt_up (dev, mode, sizeof (mode),
                                   on_finger_up_reply, ssm);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

void
goodix533c_start_finger_up_subsm (FpiSsm *parent_ssm, FpDevice *dev)
{
  FpiSsm *sub = fpi_ssm_new (dev, finger_up_ssm_handler,
                             FINGER_UP_NUM_STATES);

  fpi_ssm_start_subsm (parent_ssm, sub);
}

/* ===========================================================================
 * cancel() support: force-fail whatever command is currently in flight.
 * ======================================================================= */

void
goodix533c_cancel_pending_command (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  GError *error = NULL;

  if (!self->ack_pending && !self->reply_pending)
    return;

  g_set_error_literal (&error, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                       "Action cancelled");
  deliver_reply (dev, NULL, 0, error);
}

/* ===========================================================================
 * Test-only capture harness -- chains the four sub-SSMs above in the same
 * order the original monolithic capture_run() used, then synthesizes the
 * legacy Goodix533cCaptureDoneFunc callback shape from whatever
 * self->reference_pixels / self->live_raw_pixels / self->captured_image
 * hold at completion time. Those fields persist past whichever sub-SSM
 * produced them (unlike the old per-call CaptureData struct), so a frame
 * that did succeed is never lost just because a later stage (e.g.
 * finger-wait timing out with no physical touch) failed.
 * ======================================================================= */

enum capture_test_stage {
  CAPTURE_TEST_REF = 0,
  CAPTURE_TEST_FINGER_WAIT,
  CAPTURE_TEST_LIVE,
  CAPTURE_TEST_FINGER_UP,
  CAPTURE_TEST_NUM_STATES,
};

typedef struct
{
  Goodix533cProgressFunc    wait_for_finger_cb;
  Goodix533cCaptureDoneFunc callback;
  gpointer                  user_data;
} CaptureTestData;

static void
capture_test_ssm_handler (FpiSsm *ssm, FpDevice *dev)
{
  CaptureTestData *data = fpi_ssm_get_data (ssm);

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case CAPTURE_TEST_REF:
      goodix533c_start_ref_capture_subsm (ssm, dev);
      break;

    case CAPTURE_TEST_FINGER_WAIT:
      goodix533c_start_finger_wait_subsm (ssm, dev, data->wait_for_finger_cb,
                                          data->user_data);
      break;

    case CAPTURE_TEST_LIVE:
      goodix533c_start_live_capture_subsm (ssm, dev);
      break;

    case CAPTURE_TEST_FINGER_UP:
      goodix533c_start_finger_up_subsm (ssm, dev);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
capture_test_ssm_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  CaptureTestData *data = fpi_ssm_get_data (ssm);
  guint16 *raw_pixels = NULL;
  g_autofree guint8 *squashed = NULL;
  guint8 *corrected = NULL;

  if (self->have_reference && self->reference_pixels)
    {
      raw_pixels = self->reference_pixels;
      squashed = g_new0 (guint8, GOODIX533C_IMAGE_PIXELS);
      squash_frame_linear (self->reference_pixels, squashed,
                           GOODIX533C_IMAGE_PIXELS);
    }

  if (self->live_raw_pixels && self->captured_image)
    corrected = self->captured_image;

  data->callback (dev, raw_pixels, squashed, self->live_raw_pixels,
                  corrected, data->user_data, error);
}

void
fpi_device_goodix533c_capture_test (FpDevice *dev,
                                    Goodix533cProgressFunc wait_for_finger_cb,
                                    Goodix533cCaptureDoneFunc callback,
                                    gpointer user_data)
{
  CaptureTestData *data = g_new0 (CaptureTestData, 1);
  FpiSsm *ssm;

  data->wait_for_finger_cb = wait_for_finger_cb;
  data->callback = callback;
  data->user_data = user_data;

  ssm = fpi_ssm_new (dev, capture_test_ssm_handler, CAPTURE_TEST_NUM_STATES);
  fpi_ssm_set_data (ssm, data, g_free);
  fpi_ssm_start (ssm, capture_test_ssm_done);
}

/* ===========================================================================
 * open()/close() -- claims the interface, starts the read loop, then runs
 * nop -> firmware_version -> preset_psk_read, mirroring driver_53xc.py's
 * init_device(). Ported logic, new SSM (goodix.c has no equivalent
 * standalone open sequence -- that's spread across goodix5xx.c's shared
 * ACTIVATE state machine, which this driver deliberately does not use).
 *
 * RESET through FDT_BASELINE used to be the first six states of the
 * single-shot capture_run() SSM (see capture_test.c's original flow).
 * They are session-scoped -- TLS handshake, config upload, and the FDT
 * threshold template are all valid for the whole open() session, not just
 * one capture -- so they belong here, run once, rather than being repeated
 * by every enroll stage or verify/identify attempt. Everything
 * attempt-scoped (reference capture, finger wait, live capture, finger up)
 * lives in the sub-SSM starter functions below instead; enroll/verify/
 * identify assume open() has already brought the device through
 * FDT_BASELINE and call only those.
 * ======================================================================= */

enum open_stage {
  OPEN_STAGE_NOP,
  OPEN_STAGE_FIRMWARE_VERSION,
  OPEN_STAGE_PSK_READ,
  OPEN_STAGE_RESET,
  OPEN_STAGE_READ_CHIP_ID,
  OPEN_STAGE_READ_OTP,
  OPEN_STAGE_TLS,
  OPEN_STAGE_UPLOAD_CONFIG,
  OPEN_STAGE_FDT_BASELINE,
  OPEN_STAGE_NUM,
};

static void
on_open_nop_reply (FpDevice *dev, guint8 *data, guint16 length,
                   gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;

  /* Some sensors do not answer NOP at all -- goodix.c and driver_53xc.py
   * both treat a NOP timeout as fine. Any other error is fatal. */
  if (error && !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  g_clear_error (&error);
  fpi_ssm_next_state (ssm);
}

static void
on_open_firmware_version_reply (FpDevice *dev, guint8 *data, guint16 length,
                                gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  g_autofree gchar *firmware = NULL;
  g_autoptr(GRegex) regex = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  firmware = g_strndup ((const gchar *) data, length);
  fp_info ("Firmware: %s", firmware);

  regex = g_regex_new (GOODIX533C_FIRMWARE_REGEX, 0, 0, NULL);
  if (!g_regex_match (regex, firmware, 0, NULL))
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "Unsupported firmware: %s",
                                             firmware));
      return;
    }

  fpi_ssm_next_state (ssm);
}

static void
on_open_psk_read_reply (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  FpiSsm *ssm = user_data;
  guint32 psk_length;
  g_autofree gchar *hash = NULL;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (length < 1 || data[0] != 0x00)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "preset_psk_read failed"));
      return;
    }

  if (length < 9)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "preset_psk_read reply too short"));
      return;
    }

  {
    guint32 psk_length_le;

    /* data+5 isn't guaranteed 4-byte aligned; memcpy avoids the unaligned
     * guint32* cast (UB, and a real SIGBUS risk on strict-alignment
     * architectures). */
    memcpy (&psk_length_le, data + 5, sizeof (psk_length_le));
    psk_length = GUINT32_FROM_LE (psk_length_le);
  }
  if (length < 9 + psk_length)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "preset_psk_read reply truncated"));
      return;
    }

  /* The device returns the SHA-256 hash of the PSK directly at offset 9
   * (driver_53xc.py's init_device() names this `psk_hash` and compares it
   * as-is, with no extra hashing on our side -- PSK_LENGTH just happens to
   * equal a SHA-256 digest length, 32 bytes, which is a red herring). */
  hash = g_malloc (psk_length * 2 + 1);
    {
      guint32 hi;

      for (hi = 0; hi < psk_length; hi++)
        sprintf (hash + hi * 2, "%02x", (data + 9)[hi]);
      hash[psk_length * 2] = '\0';
    }
  if (g_strcmp0 (hash, GOODIX533C_PSK_SHA256) != 0)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                             "Sensor does not hold the "
                                             "expected all-zero PSK; refusing "
                                             "to provision one"));
      return;
    }

  fp_info ("PSK: all-zero, as expected");
  fpi_ssm_next_state (ssm);
}

static void
open_run (FpiSsm *ssm, FpDevice *dev)
{
  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OPEN_STAGE_NOP:
      cmd_nop (dev, on_open_nop_reply, ssm);
      break;

    case OPEN_STAGE_FIRMWARE_VERSION:
      cmd_firmware_version (dev, on_open_firmware_version_reply, ssm);
      break;

    case OPEN_STAGE_PSK_READ:
      cmd_preset_psk_read (dev, GOODIX533C_PSK_FLAGS, GOODIX533C_PSK_LENGTH,
                           0, on_open_psk_read_reply, ssm);
      break;

    case OPEN_STAGE_RESET:
      cmd_reset (dev, TRUE, FALSE, 20, on_reset_reply, ssm);
      break;

    case OPEN_STAGE_READ_CHIP_ID:
      cmd_read_sensor_register (dev, 0x0000, 4, on_capture_step_reply, ssm);
      break;

    case OPEN_STAGE_READ_OTP:
      cmd_read_otp (dev, on_capture_step_reply, ssm);
      break;

    case OPEN_STAGE_TLS:
      tls_connect (dev, on_tls_connected, ssm);
      break;

    case OPEN_STAGE_UPLOAD_CONFIG:
      cmd_upload_config_mcu (dev, device_config, sizeof (device_config),
                             on_upload_config_reply, ssm);
      break;

    case OPEN_STAGE_FDT_BASELINE:
        {
          guint8 mode[26];

          memcpy (mode, fdt_mode_idle, sizeof (fdt_mode_idle));
          memset (mode + sizeof (fdt_mode_idle), 0,
                 sizeof (mode) - sizeof (fdt_mode_idle));
          cmd_mcu_switch_to_fdt_mode (dev, mode, sizeof (mode), TRUE,
                                     on_fdt_baseline_reply, ssm);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
open_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  fpi_device_open_complete (dev, error);
}

static void
goodix533c_open (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  GError *error = NULL;

  /* Any leftover token from a previous open()/close() cycle is replaced
   * here rather than in close() -- see the comment above the
   * g_cancellable_cancel() call in goodix533c_close() for why it must stay
   * alive (non-NULL) past close() itself. */
  g_clear_object (&self->transfer_cancel_tkn);
  self->transfer_cancel_tkn = g_cancellable_new ();

  if (!g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                     GOODIX533C_USB_INTERFACE, 0, &error))
    {
      fpi_device_open_complete (dev, error);
      return;
    }
  self->interface_claimed = TRUE;

  start_read_loop (dev);

  fpi_ssm_start (fpi_ssm_new (dev, open_run, OPEN_STAGE_NUM), open_done);
}

static void
goodix533c_close (FpDevice *dev)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (dev);
  GError *error = NULL;

  /* Cancel, but deliberately do NOT clear/unref transfer_cancel_tkn here:
   * the in-flight bulk IN transfer's completion callback
   * (receive_data_cb()) can fire after this function returns (once the USB
   * core actually tears the transfer down), and it identifies a
   * post-close callback by checking g_cancellable_is_cancelled() on this
   * same object. Nulling the pointer first would make that check silently
   * pass a NULL cancellable (never "cancelled" per glib), so the stale
   * callback would fall through to its error path and resubmit a new
   * transfer on an already-closed device. open() replaces this token on
   * the next open(); finalize() frees it for good. */
  if (self->transfer_cancel_tkn)
    g_cancellable_cancel (self->transfer_cancel_tkn);

  if (self->tls_active)
    {
      goodix_tls_server_deinit (&self->tls, &error);
      self->tls_active = FALSE;
      g_clear_error (&error);
    }

  if (self->timeout_src)
    g_clear_pointer (&self->timeout_src, g_source_destroy);
  g_clear_pointer (&self->rx_buf, g_free);
  self->rx_len = 0;
  self->ack_pending = FALSE;
  self->reply_pending = FALSE;
  self->callback = NULL;
  self->user_data = NULL;
  self->read_loop_started = FALSE;

  /* Session-scoped state: the FDT template and reference frame are only
   * valid for the session that measured/captured them (see the comments
   * on measure_baseline()/FDT template dynamism in the findings doc).
   * Clearing them here forces a fresh open() to redo both before any live
   * capture can flat-field against a stale reference. */
  g_clear_pointer (&self->reference_pixels, g_free);
  self->have_reference = FALSE;
  self->have_fdt_template = FALSE;

  /* Attempt-scoped enroll/verify/identify state -- also cleared here (not
   * just at the end of each action) in case close() runs mid-action, e.g.
   * the client disconnecting during an enroll. */
  g_clear_pointer (&self->live_raw_pixels, g_free);
  g_clear_pointer (&self->captured_image, g_free);
  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);
  self->enroll_stage = 0;
  self->task_ssm = NULL;
  self->verify_wait_finger_up = FALSE;
  goodix533c_clear_pending_result_report (self);

  if (self->interface_claimed)
    {
      g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                      GOODIX533C_USB_INTERFACE, 0, &error);
      self->interface_claimed = FALSE;
    }

  fpi_device_close_complete (dev, error);
}

static void
goodix533c_enroll (FpDevice *dev)
{
  goodix533c_enroll_start (dev);
}

static void
goodix533c_verify (FpDevice *dev)
{
  goodix533c_auth_start (dev);
}

static void
goodix533c_identify (FpDevice *dev)
{
  goodix533c_auth_start (dev);
}

static void
goodix533c_cancel (FpDevice *dev)
{
  goodix533c_cancel_pending_command (dev);
}

/* ===========================================================================
 * GObject boilerplate
 * ======================================================================= */

static void
fpi_device_goodix533c_init (FpiDeviceGoodix533c *self)
{
}

static void
fpi_device_goodix533c_finalize (GObject *object)
{
  FpiDeviceGoodix533c *self = FPI_DEVICE_GOODIX533C (object);

  g_clear_pointer (&self->rx_buf, g_free);
  g_clear_pointer (&self->reference_pixels, g_free);
  g_clear_pointer (&self->live_raw_pixels, g_free);
  g_clear_pointer (&self->captured_image, g_free);
  g_clear_pointer (&self->enroll_features, g_ptr_array_unref);
  goodix533c_clear_pending_result_report (self);
  g_clear_object (&self->transfer_cancel_tkn);

  G_OBJECT_CLASS (fpi_device_goodix533c_parent_class)->finalize (object);
}

static const FpIdEntry goodix533c_id_table[] = {
  { .vid = 0x27c6, .pid = 0x533c, },
  { .vid = 0, .pid = 0, .driver_data = 0 },
};

static void
fpi_device_goodix533c_class_init (FpiDeviceGoodix533cClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = fpi_device_goodix533c_finalize;

  dev_class->id = "goodix533c";
  dev_class->full_name = "Goodix 27c6:533c Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = goodix533c_id_table;
  dev_class->nr_enroll_stages = GOODIX533C_ENROLL_SAMPLES;
  dev_class->temp_hot_seconds = -1;

  dev_class->open = goodix533c_open;
  dev_class->close = goodix533c_close;
  dev_class->enroll = goodix533c_enroll;
  dev_class->verify = goodix533c_verify;
  dev_class->identify = goodix533c_identify;
  dev_class->cancel = goodix533c_cancel;

  /* No dev_class->capture vfunc is wired -- open() + capture is exercised
   * only via the test-only fpi_device_goodix533c_capture_test() entry
   * point, so auto_initialize_features() correctly does not claim
   * FP_DEVICE_FEATURE_CAPTURE (it only sets that bit when
   * dev_class->capture is non-NULL). It does pick up VERIFY/IDENTIFY from
   * the vfuncs just set, and FP_DEVICE_FEATURE_ALWAYS_ON from
   * temp_hot_seconds < 0 above -- matching the sibling goodixtls511
   * driver's convention of calling this once at the end of class_init()
   * rather than assigning dev_class->features by hand. */
  fpi_device_class_auto_initialize_features (dev_class);
}
