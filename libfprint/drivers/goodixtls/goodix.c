/* Goodix TLS driver for libfprint
 *
 * Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
 * Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
 * Copyright (C) 2021 Natasha England-Elbro <natasha@natashaee.me>
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

#include "fpi-log.h"
#include "fpi-ssm.h"
#include "fpi-usb-transfer.h"
#define FP_COMPONENT "goodixtls"

#include <gio/gio.h>
#include <glib.h>
#include <gusb.h>
#include <openssl/ssl.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix_proto.h"
#include "goodixtls.h"

typedef struct
{
  GoodixTlsServer    *tls_hop;

  GSource            *timeout;

  guint8              cmd;

  gboolean            ack;
  gboolean            reply;

  GoodixCmdCallback   callback;
  gpointer            user_data;

  guint8             *data;
  guint32             length;

  GoodixCallbackInfo *tls_ready_callback;

  /* Stale-activation guard: bumped on (re)activation start and teardown;
   * TLS completion callbacks drop on mismatch. */
  guint         activation_gen;

  /* Boot sequence counter coupled to actual USB reset. */
  guint         boot_seq;

  /* Conditional USB reset flag: TRUE only when previous session closed cleanly.
   * FALSE is the safe direction (reset taken on open). */
  gboolean      clean_close;

  /* USB device identity snapshot to detect kernel re-enumeration. */
  guint8        last_usb_bus;
  guint8        last_usb_addr;
  guint8        last_usb_port;
  guint16       last_usb_vid;
  guint16       last_usb_pid;
  gboolean      usb_identity_valid;

  GCancellable *transfer_cancel_tkn;
  gboolean      inited;
} FpiDeviceGoodixTlsPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (FpiDeviceGoodixTls, fpi_device_goodixtls,
                                     FP_TYPE_IMAGE_DEVICE);

static GoodixCallbackInfo *
make_cb_info (GCallback callback, gpointer user_data)
{
  GoodixCallbackInfo *cb_info = g_new0 (GoodixCallbackInfo, 1);

  cb_info->callback = callback;
  cb_info->user_data = user_data;

  return cb_info;
}

/* Defined below; needed early by goodix_receive_data_cb. */
static void goodix_receive_data (FpDevice *dev);

gchar *
data_to_str (guint8 *data, guint32 length)
{
  gchar *string;

  if (data == NULL || length == 0 || length > 4096)
    return g_strdup ("");

  string = g_malloc ((length * 2) + 1);

  for (guint32 i = 0; i < length; i++)
    g_snprintf (string + i * 2, 3, "%02x", data[i]);

  return string;
}

static void
goodix_receive_done (FpDevice *dev, guint8 *data, guint16 length,
                     GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GoodixCmdCallback callback = priv->callback;
  gpointer user_data = priv->user_data;

  if (!(priv->ack || priv->reply))
    {
      if (error)
        g_error_free (error);
      return;
    }

  goodix_reset_state (dev);
  if (!error)
    fp_dbg ("Completed command: 0x%02x", priv->cmd);

  if (callback)
    callback (dev, data, length, user_data, error);
  else if (error)
    g_error_free (error);
}

void
goodix_receive_none (FpDevice *dev, guint8 *data, guint16 length,
                     gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixNoneCallback callback = (GoodixNoneCallback) cb_info->callback;

  callback (dev, cb_info->user_data, error);
}

static void
goodix_receive_none_tolerant (FpDevice *dev, guint8 *data, guint16 length,
                              gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixNoneCallback callback = (GoodixNoneCallback) cb_info->callback;

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
    g_clear_error (&error); /* Buffer already empty. */

  callback (dev, cb_info->user_data, error);
}

void
goodix_receive_default (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixDefaultCallback callback = (GoodixDefaultCallback) cb_info->callback;

  callback (dev, data, length, cb_info->user_data, error);
}

static void
goodix_receive_success (FpDevice *dev, guint8 *data, guint16 length,
                        gpointer user_data, GError *error)
{
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixSuccessCallback callback = (GoodixSuccessCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  if (length != sizeof (guint8) * 2)
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid success reply length: %d", length);
      callback (dev, FALSE, cb_info->user_data, error);
      return;
    }

  callback (dev, data[0] == 0x00 ? FALSE : TRUE, cb_info->user_data, NULL);
}

static void
goodix_receive_preset_psk_read (FpDevice *dev, guint8 *data, guint16 length,
                                gpointer user_data, GError *error)
{
  guint32 psk_len;
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixPresetPskReadCallback callback =
    (GoodixPresetPskReadCallback) cb_info->callback;

  if (error)
    {
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;
    }

  if (length < sizeof (guint8))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid preset PSK read reply length: %d", length);
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;
    }

  if (data[0] != 0x00)
    {
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, NULL);
      return;
    }

  if (length < sizeof (guint8) + sizeof (GoodixPresetPsk))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid preset PSK read reply length: %d", length);
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;
    }

  {
    memcpy (&psk_len, data + sizeof (guint8) + G_STRUCT_OFFSET (GoodixPresetPsk, length), sizeof (psk_len));
    psk_len = GUINT32_FROM_LE (psk_len);
  }

  if (length < psk_len + sizeof (guint8) + sizeof (GoodixPresetPsk))
    {
      g_set_error (&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid preset PSK read reply length: %d", length);
      callback (dev, FALSE, 0x00000000, NULL, 0, cb_info->user_data, error);
      return;
    }

  {
    guint32 psk_flags;
    memcpy (&psk_flags, data + sizeof (guint8) + G_STRUCT_OFFSET (GoodixPresetPsk, flags), sizeof (psk_flags));
    callback (dev, TRUE, GUINT32_FROM_LE (psk_flags),
              data + sizeof (guint8) + sizeof (GoodixPresetPsk), psk_len,
              cb_info->user_data, NULL);
  }
}

static void
goodix_receive_firmware_version (FpDevice *dev, guint8 *data,
                                 guint16 length, gpointer user_data,
                                 GError *error)
{
  g_autofree gchar *payload = g_malloc (length + sizeof (gchar));
  g_autofree GoodixCallbackInfo *cb_info = user_data;
  GoodixFirmwareVersionCallback callback =
    (GoodixFirmwareVersionCallback) cb_info->callback;

  if (error)
    {
      callback (dev, NULL, cb_info->user_data, error);
      return;
    }

  memcpy (payload, data, length);

  /* Some devices send the firmware without the null terminator. */
  payload[length] = 0x00;

  callback (dev, payload, cb_info->user_data, NULL);
}

static void
goodix_receive_ack (FpDevice *dev, guint8 *data, guint16 length,
                    gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  guint8 cmd, flags;

  if (length != sizeof (GoodixAck))
    {
      fp_warn ("Invalid ACK length: %u", length);
      return;
    }

  if (data == NULL)
    {
      fp_warn ("Invalid ACK: NULL payload");
      return;
    }

  /* Byte ops, not bitfields: layout is wire-defined. */
  cmd = data[0];
  flags = data[1];

  if (!(flags & 0x01))
    {
      fp_warn ("Invalid ACK flags: 0x%02x", flags);
      return;
    }

  if (flags & 0x02)
    fp_warn ("MCU has no config");

  if (priv->cmd != cmd)
    {
      fp_warn ("Invalid ACK command: 0x%02x", cmd);
      return;
    }

  if (!priv->ack)
    {
      fp_warn ("Didn't expect an ACK for command: 0x%02x", priv->cmd);
      return;
    }

  if (!priv->reply)
    {
      G_DEBUG_HERE ();
      goodix_receive_done (dev, NULL, 0, NULL);
      return;
    }

  priv->ack = FALSE;
}

static void
goodix_receive_protocol (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  guint8 cmd;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum, valid_null_checksum;

  if (!goodix_decode_protocol (data, length, &cmd, &payload, &payload_len,
                               &valid_checksum, &valid_null_checksum))
    {
      fp_dbg ("Dropping short protocol frame, size: %u", length);
      return;
    }

  /* Either the standard checksum or the null checksum is accepted; some
   * replies use the null value. Checksum mismatches are logged but do not
   * block delivery: the calculation is not yet trusted against hardware
   * quirks, and dropping here turns into command timeouts. */
  if (!valid_checksum && !valid_null_checksum)
    {
      fp_dbg ("protocol frame with bad checksum");
    }

  if (cmd == GOODIX_CMD_ACK)
    {
      fp_dbg ("got ack");
      goodix_receive_ack (dev, payload, payload_len, NULL, NULL);
      return;
    }

  if (priv->cmd != cmd)
    {
      fp_warn ("Invalid protocol command: 0x%02x", cmd);
      return;
    }

  if (!priv->reply)
    {
      fp_warn ("Didn't expect a reply for command: 0x%02x", priv->cmd);
      return;
    }

  if (priv->ack)
    fp_warn ("Missing ACK for command: 0x%02x", priv->cmd);

  goodix_receive_done (dev, payload, payload_len, NULL);
}

static void
goodix_receive_pack (FpDevice *dev, guint8 *data, guint32 length)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  guint8 flags;
  g_autofree guint8 *payload = NULL;
  guint16 payload_len;
  gboolean valid_checksum;

  if (length == 0)
    return;

  priv->data = g_realloc (priv->data, priv->length + length);
  memcpy (priv->data + priv->length, data, length);
  priv->length += length;

  if (!goodix_decode_pack (priv->data, priv->length, &flags, &payload,
                           &payload_len, &valid_checksum))
    {
      fp_dbg ("not full packet");
      return;
    }

  if (!valid_checksum)
    {
      /* Logged only: the checksum calculation is not yet trusted
       * against hardware quirks. */
      fp_dbg ("pack with bad checksum");
    }

  switch (flags)
    {
    case GOODIX_FLAGS_MSG_PROTOCOL:
      fp_dbg ("Got protocol msg");
      goodix_receive_protocol (dev, payload, payload_len);
      break;

    case GOODIX_FLAGS_TLS:
    case GOODIX_FLAGS_TLS_DATA:
      fp_dbg ("Got TLS msg (%u bytes)", payload_len);
      if (priv->cmd == GOODIX_CMD_MCU_GET_IMAGE ||
          priv->cmd == GOODIX_CMD_REQUEST_TLS_CONNECTION ||
          (priv->reply && priv->callback != NULL && priv->cmd == 0))
        goodix_receive_done (dev, payload, payload_len, NULL);
      else
        fp_dbg ("Discarding stale TLS msg while waiting for cmd 0x%02x",
                priv->cmd);
      break;

    default:
      fp_dbg ("Unknown flags: 0x%02x", flags);
      break;
    }

  g_clear_pointer (&priv->data, g_free);
  priv->length = 0;
}

static void
goodix_receive_data_cb (FpiUsbTransfer *transfer, FpDevice *dev,
                        gpointer user_data, GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if ((priv->transfer_cancel_tkn && g_cancellable_is_cancelled (priv->transfer_cancel_tkn)) ||
      g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
      fp_dbg ("transfer cancelled, aborting read loop...");
      if (error)
        g_error_free (error);
      return;
    }
  if (error)
    {
      fp_warn ("Receive data error: %s", error->message);
      g_error_free (error);

      goodix_receive_data (dev);
      return;
    }

  goodix_receive_pack (dev, transfer->buffer, transfer->actual_length);

  goodix_receive_data (dev);
}

static void
goodix_receive_timeout_cb (FpDevice *dev, gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GError *error = NULL;

  g_set_error (&error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
               "Command timed out: 0x%02x", priv->cmd);
  goodix_receive_done (dev, NULL, 0, error);
}

void
goodix_start_read_loop (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->inited)
    return;

  if (priv->transfer_cancel_tkn && g_cancellable_is_cancelled (priv->transfer_cancel_tkn))
    g_cancellable_reset (priv->transfer_cancel_tkn);

  priv->inited = TRUE;
  g_clear_pointer (&priv->data, g_free);
  priv->length = 0;

  goodix_receive_data (dev);
}

void
goodix_stop_read_loop (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  fp_dbg ("Stopping read loop");
  if (priv->transfer_cancel_tkn)
    g_cancellable_cancel (priv->transfer_cancel_tkn);

  priv->inited = FALSE;
  g_clear_pointer (&priv->data, g_free);
  priv->length = 0;
}

static void
goodix_receive_data (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  transfer->short_is_error = FALSE;

  fpi_usb_transfer_fill_bulk (transfer, class->ep_in,
                              GOODIX_EP_IN_MAX_BUF_SIZE);

  fpi_usb_transfer_submit (transfer, 0, priv->transfer_cancel_tkn,
                           goodix_receive_data_cb, NULL);
}

static gboolean
goodix_send_data (FpDevice *dev, guint8 *data, guint32 length,
                  GDestroyNotify free_func, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);

  for (guint32 i = 0; i < length; i += GOODIX_EP_OUT_MAX_BUF_SIZE)
    {
      guint32 chunk_len = MIN ((guint32) GOODIX_EP_OUT_MAX_BUF_SIZE, length - i);
      FpiUsbTransfer *transfer = fpi_usb_transfer_new (dev);
      transfer->short_is_error = TRUE;

      fpi_usb_transfer_fill_bulk_full (transfer, class->ep_out, data + i,
                                       chunk_len, NULL);

      if (!fpi_usb_transfer_submit_sync (transfer, GOODIX_TIMEOUT,
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
goodix_send_pack (FpDevice *dev, guint8 flags, guint8 *payload,
                  guint16 length, GDestroyNotify free_func,
                  GError **error)
{
  guint8 *data;
  guint32 data_len;

  goodix_encode_pack (flags, payload, length, TRUE, &data, &data_len);
  if (free_func)
    free_func (payload);

  return goodix_send_data (dev, data, data_len, g_free, error);
}

void
goodix_send_protocol (
  FpDevice *dev, guint8 cmd, const guint8 *payload, guint16 length,
  GDestroyNotify free_func, gboolean calc_checksum, guint timeout_ms,
  gboolean reply, GoodixCmdCallback callback, gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GError *error = NULL;
  guint8 *data;
  guint32 data_len;

  if (priv->ack || priv->reply || priv->timeout)
    {
      /* A command is already running. */
      guint8 busy_cmd = priv->cmd;
      GError *collision_error;
      fp_warn ("A command is already running: 0x%02x", busy_cmd);
      if (free_func)
        free_func ((void *) payload);
      collision_error =
        g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY,
                     "A command is already running: 0x%02x", busy_cmd);
      goodix_receive_done (dev, NULL, 0, collision_error);
      /* Fail the incoming waiter as well, otherwise its SSM stalls. */
      if (callback)
        {
          GError *incoming_error =
            g_error_new (G_IO_ERROR, G_IO_ERROR_BUSY,
                         "A command is already running: 0x%02x", busy_cmd);
          callback (dev, NULL, 0, user_data, incoming_error);
        }
      return;
    }

  fp_dbg ("Running command: 0x%02x", cmd);

  if (timeout_ms)
    priv->timeout = fpi_device_add_timeout (
      dev, timeout_ms, goodix_receive_timeout_cb, NULL, NULL);
  priv->cmd = cmd;
  priv->ack = TRUE;
  priv->reply = reply;
  priv->callback = callback;
  priv->user_data = user_data;

  goodix_encode_protocol (cmd, payload, length, calc_checksum, FALSE,
                          &data, &data_len);
  if (free_func)
    free_func ((void *) payload);

  if (!goodix_send_pack (dev, GOODIX_FLAGS_MSG_PROTOCOL, data, data_len,
                         g_free, &error))
    {
      goodix_receive_done (dev, NULL, 0, error);
      return;
    }
}
void
goodix_send_nop (FpDevice *dev, GoodixNoneCallback callback,
                 gpointer user_data)
{
  GoodixNop payload = {.unknown = 0x00000000};
  GoodixCallbackInfo *cb_info;

  /* Flush command: silence from the MCU indicates success. */
  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_NOP, (guint8 *) &payload,
                            sizeof (payload), NULL, FALSE, GOODIX_NOP_TIMEOUT, FALSE,
                            goodix_receive_none_tolerant, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_NOP, (guint8 *) &payload, sizeof (payload),
                        NULL, FALSE, GOODIX_NOP_TIMEOUT, FALSE, NULL, NULL);
}

static void
goodix_send_mcu_get_image (FpDevice *dev, GoodixImageCallback callback,
                           gpointer user_data)
{
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (dev);
  GoodixCallbackInfo *cb_info;
  GoodixDefault payload_default = {.unused_flags = 0x01};
  const guint8 *payload = (const guint8 *) &payload_default;
  guint16 len = sizeof (payload_default);

  if (class->capture_payload != NULL && class->capture_payload_len > 0)
    {
      payload = class->capture_payload;
      len = class->capture_payload_len;
    }

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_MCU_GET_IMAGE, payload,
                            len, NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_GET_IMAGE, payload,
                        len, NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                        NULL, NULL);
}

void
goodix_send_mcu_switch_to_fdt_down (FpDevice *dev, const guint8 *mode, guint16 length,
                                    GDestroyNotify free_func,
                                    GoodixDefaultCallback callback,
                                    gpointer user_data)
{
  GoodixCallbackInfo *cb_info = NULL;
  GoodixDefaultCallback cb = NULL;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);
      cb = goodix_receive_default;
    }

  if (mode && length > 0 && mode[0] == 0x01)
    {
      guint8 *payload = g_malloc (sizeof (guint8) * (length + 1));
      memcpy (payload + 1, mode, length);
      payload[0] = 0xc;
      if (free_func)
        free_func ((void *) mode);
      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, payload, length + 1,
                            g_free, TRUE, 0, TRUE, cb, cb_info);
    }
  else
    {
      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_DOWN, mode, length,
                            free_func, TRUE, 0, TRUE, cb, cb_info);
    }
}

void
goodix_send_mcu_switch_to_fdt_up (FpDevice *dev, const guint8 *mode, guint16 length,
                                  GDestroyNotify free_func,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  GoodixCallbackInfo *cb_info = NULL;
  GoodixDefaultCallback cb = NULL;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);
      cb = goodix_receive_default;
    }

  if (mode && length > 0 && mode[0] == 0x01)
    {
      guint8 *payload = g_malloc (sizeof (guint8) * (length + 1));
      memcpy (payload + 1, mode, length);
      payload[0] = 0xe;
      if (free_func)
        free_func ((void *) mode);
      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, payload, length + 1,
                            g_free, TRUE, 0, TRUE, cb, cb_info);
    }
  else
    {
      goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_UP, mode, length,
                            free_func, TRUE, 0, TRUE, cb, cb_info);
    }
}

void
goodix_send_mcu_switch_to_fdt_mode (FpDevice *dev, const guint8 *mode,
                                    guint16 length, GDestroyNotify free_func,
                                    GoodixNoneCallback callback,
                                    gpointer user_data)
{
  GoodixCallbackInfo *cb_info = NULL;
  GoodixCmdCallback cb = NULL;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);
      cb = goodix_receive_none;
    }

  goodix_send_protocol (dev, GOODIX_CMD_MCU_SWITCH_TO_FDT_MODE, mode, length,
                        free_func, TRUE, GOODIX_TIMEOUT, FALSE, cb,
                        cb_info);

}

void
goodix_send_nav_0 (FpDevice *dev, GoodixDefaultCallback callback,
                   gpointer user_data)
{
  GoodixDefault payload = {.unused_flags = 0x01};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_NAV_0, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_NAV_0, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_read_sensor_register (FpDevice *dev, guint16 address,
                                  guint8 length,
                                  GoodixDefaultCallback callback,
                                  gpointer user_data)
{
  /* Only support one address. */

  GoodixReadSensorRegister payload = {
    .multiples = FALSE, .address = GUINT16_TO_LE (address), .length = length
  };
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_READ_SENSOR_REGISTER,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            GOODIX_TIMEOUT, TRUE, goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_READ_SENSOR_REGISTER, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_upload_config_mcu (FpDevice *dev, guint8 *config,
                               guint16 length, GDestroyNotify free_func,
                               GoodixSuccessCallback callback,
                               gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                            free_func, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_success, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_UPLOAD_CONFIG_MCU, config, length,
                        free_func, TRUE, GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

void
goodix_send_enable_chip (FpDevice *dev, gboolean enable,
                         GoodixNoneCallback callback, gpointer user_data)
{
  GoodixEnableChip payload = {.enable = enable ? TRUE : FALSE};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                            goodix_receive_none, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_ENABLE_CHIP, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE, NULL,
                        NULL);
}

void
goodix_send_query_firmware_version (FpDevice                     *dev,
                                    GoodixFirmwareVersionCallback callback,
                                    gpointer                      user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_firmware_version, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_FIRMWARE_VERSION, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE, NULL,
                        NULL);
}

void
goodix_send_query_mcu_state (FpDevice *dev, GoodixNoneCallback callback,
                             gpointer user_data)
{
  GoodixQueryMcuState payload = {.unused_flags = 0x55};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE,
                            goodix_receive_none, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_QUERY_MCU_STATE, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, FALSE, NULL,
                        NULL);
}

static void
goodix_send_request_tls_connection (FpDevice             *dev,
                                    GoodixDefaultCallback callback,
                                    gpointer              user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE, 0,
                            TRUE, goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_REQUEST_TLS_CONNECTION,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        GOODIX_TIMEOUT, TRUE, NULL, NULL);
}

static void
goodix_send_tls_successfully_established (FpDevice          *dev,
                                          GoodixNoneCallback callback,
                                          gpointer           user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);
      /* Timeout needs to be at least 10ms. */

      goodix_send_protocol (dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                            (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                            2000, TRUE, goodix_receive_none, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED,
                        (guint8 *) &payload, sizeof (payload), NULL, TRUE,
                        2000, TRUE, NULL, NULL);
}

void
goodix_send_read_otp (FpDevice *dev, GoodixDefaultCallback callback,
                      gpointer user_data)
{
  GoodixNone payload = {};
  GoodixCallbackInfo *cb_info;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);

      goodix_send_protocol (dev, GOODIX_CMD_READ_OTP, (guint8 *) &payload,
                            sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                            goodix_receive_default, cb_info);
      return;
    }

  goodix_send_protocol (dev, GOODIX_CMD_READ_OTP, (guint8 *) &payload,
                        sizeof (payload), NULL, TRUE, GOODIX_TIMEOUT, TRUE,
                        NULL, NULL);
}

/* Shared 0xe4 PSK-read sender. The two public wrappers below build
 * differently-shaped request bodies (generic flags+length vs 5e0a
 * length+offset+flags+reserved) around this single protocol call. */
static void
goodix_send_preset_psk_read_payload (FpDevice                   *dev,
                                     const guint8               *payload,
                                     guint16                     length,
                                     GoodixPresetPskReadCallback callback,
                                     gpointer                    user_data)
{
  GoodixCallbackInfo *cb_info = NULL;
  GoodixCmdCallback cb = NULL;

  if (callback)
    {
      cb_info = make_cb_info (G_CALLBACK (callback), user_data);
      cb = goodix_receive_preset_psk_read;
    }

  goodix_send_protocol (dev, GOODIX_CMD_PRESET_PSK_READ, payload, length,
                        NULL, TRUE, GOODIX_TIMEOUT, TRUE, cb, cb_info);
}

void
goodix_send_preset_psk_read (FpDevice *dev, guint32 flags, guint16 length,
                             GoodixPresetPskReadCallback callback,
                             gpointer user_data)
{
  GoodixPresetPsk payload = {.flags = GUINT32_TO_LE (flags),
                             .length = GUINT32_TO_LE (length)};

  goodix_send_preset_psk_read_payload (dev, (const guint8 *) &payload,
                                       sizeof (payload), callback, user_data);
}

void
goodix_send_preset_psk_read_5e0a (FpDevice                   *dev,
                                  guint32                     flags,
                                  guint32                     length,
                                  guint32                     offset,
                                  GoodixPresetPskReadCallback callback,
                                  gpointer                    user_data)
{
  /* 5e0a CMD 0xe4 payload: length + offset + flags + reserved (4B LE each). */
  struct __attribute__((__packed__)) {
    guint32 length;
    guint32 offset;
    guint32 flags;
    guint32 reserved;
  } payload = {
    .length = GUINT32_TO_LE (length),
    .offset = GUINT32_TO_LE (offset),
    .flags = GUINT32_TO_LE (flags),
    .reserved = 0,
  };

  goodix_send_preset_psk_read_payload (dev, (const guint8 *) &payload,
                                       sizeof (payload), callback, user_data);
}

gboolean
goodix_dev_init (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->timeout = NULL;
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->callback = NULL;
  priv->user_data = NULL;
  priv->data = NULL;
  priv->length = 0;
  priv->transfer_cancel_tkn = g_cancellable_new ();

  /* Conditional USB reset: skip reset only when previous session on
   * this USB device closed cleanly. Re-enumeration or dirty state forces reset. */
  {
    GUsbDevice *usb = fpi_device_get_usb_device (dev);
    gboolean reenumerated = FALSE;
    gboolean take_reset;

    if (usb != NULL)
      {
        guint8 bus = g_usb_device_get_bus (usb);
        guint8 addr = g_usb_device_get_address (usb);
        guint8 port = g_usb_device_get_port_number (usb);
        guint16 vid = g_usb_device_get_vid (usb);
        guint16 pid = g_usb_device_get_pid (usb);

        /* Address is unstable across suspend and re-enumeration; bus
         * identifies the host controller and port the topology, so compare
         * bus+port+vid+pid. The address snapshot is kept for diagnostics
         * only. */
        if (priv->usb_identity_valid
            && (bus != priv->last_usb_bus || port != priv->last_usb_port
                || vid != priv->last_usb_vid || pid != priv->last_usb_pid))
          {
            reenumerated = TRUE;
            priv->clean_close = FALSE;
          }
        priv->last_usb_bus = bus;
        priv->last_usb_addr = addr;
        priv->last_usb_port = port;
        priv->last_usb_vid = vid;
        priv->last_usb_pid = pid;
        priv->usb_identity_valid = TRUE;
      }
    else
      {
        reenumerated = TRUE;
        priv->clean_close = FALSE;
      }

    take_reset = !priv->clean_close;
    if (take_reset)
      {
        priv->boot_seq++;
        if (reenumerated)
          fp_dbg ("USB reset taken (re-enumerated device, boot_seq=%u)",
                  priv->boot_seq);
        else
          fp_dbg ("USB reset taken (dirty close, boot_seq=%u)",
                  priv->boot_seq);
        g_usb_device_reset (fpi_device_get_usb_device (dev), NULL);
      }
    else
      {
        fp_dbg ("USB reset skipped (clean close, boot_seq=%u)",
                priv->boot_seq);
      }
  }

  {
    gboolean ok = g_usb_device_claim_interface (fpi_device_get_usb_device (dev),
                                                class->interface, 0, error);

    if (!ok)
      priv->clean_close = FALSE;
    return ok;
  }
}
void
goodix_reset_state (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->timeout)
    g_clear_pointer (&priv->timeout, g_source_destroy);
  priv->ack = FALSE;
  priv->reply = FALSE;
  priv->cmd = 0;
  priv->callback = NULL;
  priv->user_data = NULL;
  g_clear_pointer (&priv->data, g_free);
  priv->length = 0;
}

/* Stale-activation guard counter (live paths untouched). */
guint
goodix_activation_gen_get (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  return priv->activation_gen;
}

guint
goodix_activation_gen_bump (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  return ++priv->activation_gen;
}

guint
goodix_boot_seq_get (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  return priv->boot_seq;
}

void
goodix_session_mark_clean (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->clean_close = TRUE;
}

void
goodix_session_mark_dirty (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  priv->clean_close = FALSE;
}

gboolean
goodix_session_is_clean (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  return priv->clean_close;
}

gboolean
goodix_dev_deinit (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsClass *class = FPI_DEVICE_GOODIXTLS_GET_CLASS (self);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  gboolean clean_close = priv->clean_close;
  gboolean released;

  if (!clean_close)
    goodix_activation_gen_bump (dev);

  if (priv->transfer_cancel_tkn)
    g_cancellable_cancel (priv->transfer_cancel_tkn);
  g_clear_object (&priv->transfer_cancel_tkn);

  if (!clean_close)
    {
      g_autoptr(GError) tls_err = NULL;

      goodix_shutdown_tls (dev, &tls_err);
      if (tls_err)
        fp_warn ("TLS shutdown warning: %s", tls_err->message);
    }

  goodix_reset_state (dev);
  priv->inited = FALSE;

  released = g_usb_device_release_interface (fpi_device_get_usb_device (dev),
                                             class->interface, 0, error);
  if (!released)
    {
      priv->clean_close = FALSE;
      if (clean_close)
        {
          goodix_activation_gen_bump (dev);
          goodix_shutdown_tls (dev, NULL);
        }
    }

  return released;
}

static void
goodix_read_tls (FpDevice *dev, GoodixTlsCallback callback,
                 gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  fp_dbg ("goodix_read_tls()");
  priv->callback = callback;
  priv->user_data = user_data;
  priv->reply = TRUE;
  priv->cmd = 0;
}

enum tls_states {
  TLS_SERVER_INIT,
  TLS_SERVER_HANDSHAKE_INIT,
  TLS_NUM_STATES,
};


static void
on_goodix_tls_read_handshake (FpDevice *dev, guint8 *data,
                              guint16 length, gpointer user_data,
                              GError *error)
{
  FpiSsm *ssm = user_data;
  FpiDeviceGoodixTls *self;
  FpiDeviceGoodixTlsPrivate *priv;
  int sent;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }
  self = FPI_DEVICE_GOODIXTLS (fpi_ssm_get_data (user_data));
  priv = fpi_device_goodixtls_get_instance_private (self);

  sent = goodix_tls_client_write (priv->tls_hop, data, length);

  if (sent < 0)
    {
      fpi_ssm_mark_failed (ssm, g_error_new (g_io_error_quark (), sent,
                                             "failed to sent data to "
                                             "tls server"));
      return;
    }
  fpi_ssm_next_state (ssm);
}

enum goodix_tls_handshake_stages {
  TLS_HANDSHAKE_STAGE_HELLO_S,
  TLS_HANDSHAKE_STAGE_KH_EXCHANGE,
  TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_C,
  TLS_HANDSHAKE_STAGE_HANDSHAKE_C,
  TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_S,

  TLS_HANDSHAKE_STAGE_NUM,
};

static void
on_tls_successfully_established (FpDevice *dev, gpointer user_data,
                                 GError *error)
{
  FpiDeviceGoodixTls * self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate * priv =
    fpi_device_goodixtls_get_instance_private (self);

  fp_dbg ("HANDSHAKE DONE");
  if (!priv->tls_ready_callback)
    {
      if (error)
        g_error_free (error);
      return;
    }
  /* The MCU never ACKs the notification; a timeout here reports success. */
  if (error)
    g_error_free (error);
  ((GoodixNoneCallback) priv->tls_ready_callback->callback)(
    dev, priv->tls_ready_callback->user_data, NULL);
  g_clear_pointer (&priv->tls_ready_callback, g_free);
}

/* Wait briefly for the TLS serve thread to finish SSL_accept, then report
 * whether the device completed the handshake. In the healthy case accept
 * returns right after the proxied client Finished, well before the proxy
 * SSM ends, so this returns immediately; the deadline only bounds genuinely
 * stuck handshakes. If the outcome is still unknown at the deadline, return
 * TRUE to preserve the previous behavior (never fail a slow-but-healthy
 * handshake on a missing signal). */
static gboolean
tls_accept_wait_ok (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (!priv->tls_hop)
    return TRUE;

  for (int i = 0;
       i < 200 && !g_atomic_int_get (&priv->tls_hop->accept_done);
       i++)
    g_usleep (10000);

  if (!g_atomic_int_get (&priv->tls_hop->accept_done))
    {
      fp_dbg ("TLS accept outcome not ready yet, proceeding");
      return TRUE;
    }

  if (priv->tls_hop->accept_ret <= 0)
    {
      fp_err ("TLS not accepted by device: %s",
              priv->tls_hop->accept_err);
      return FALSE;
    }

  return TRUE;
}

static void
tls_handshake_done (FpiSsm *ssm, FpDevice *dev, GError *error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (error)
    {
      fp_err ("failed to do tls handshake: %s (code: %d)", error->message,
              error->code);
      if (priv->tls_ready_callback)
        {
          ((GoodixNoneCallback) priv->tls_ready_callback->callback)(
            dev, priv->tls_ready_callback->user_data, error);
          g_clear_pointer (&priv->tls_ready_callback, g_free);
        }
      return;
    }

  /* The proxy SSM completing does not prove the openssl server accepted:
   * check the serve thread outcome before declaring the session live.
   * Without this, an accept failure (e.g. peer Finished bad-record-mac from
   * a device key the host does not expect) went unnoticed and activation
   * proceeded on a dead session, hanging later at FDT with a command
   * timeout instead of failing here with a clear TLS error. */
  if (!tls_accept_wait_ok (dev))
    {
      GError *accept_error;

      accept_error = fpi_device_error_new_msg (
        FP_DEVICE_ERROR_GENERAL,
        "TLS handshake failed: device did not complete the handshake "
        "(likely PSK mismatch; see log for accept error)");
      fp_err ("%s", accept_error->message);
      if (priv->tls_ready_callback)
        {
          ((GoodixNoneCallback) priv->tls_ready_callback->callback)(
            dev, priv->tls_ready_callback->user_data, accept_error);
          g_clear_pointer (&priv->tls_ready_callback, g_free);
        }
      else
        {
          g_error_free (accept_error);
        }
      return;
    }

  goodix_send_tls_successfully_established (
    dev, on_tls_successfully_established, NULL);
}

/* Read one relay flight from the TLS server side.
 *
 * The server emits back-to-back TLS records (ServerHello through HelloDone,
 * ChangeCipherSpec and Finished) on the socket pair. A single read returns
 * whatever is pending and truncates the flight, which desynchronises the
 * relay, so whole records are read (5-byte header, then the body) until
 * poll reports the socket idle. Bounded by the caller buffer.
 *
 * Returns the flight size, or -1 when nothing was read or the flight does
 * not fit: a truncated record must never be relayed. */
static int
goodix_tls_read_flight (GoodixTlsServer *tls, guint8 *buf, int buf_size)
{
  struct pollfd pfd;
  int total = 0;

  if (!tls || !buf || buf_size <= 0 || tls->client_fd < 0)
    return -1;

  pfd.fd = tls->client_fd;
  pfd.events = POLLIN;

  for (;;)
    {
      int hdr_got = 0;
      int rec_len;

      while (hdr_got < 5)
        {
          ssize_t n;

          if (total + 5 > buf_size)
            {
              fp_dbg ("TLS relay flight does not fit, failing");
              return -1;
            }
          n = read (tls->client_fd, buf + total + hdr_got, 5 - hdr_got);
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            return total > 0 ? total : -1;
          hdr_got += n;
        }

      rec_len = (buf[total + 3] << 8) | buf[total + 4];
      fp_dbg ("TLS relay record type=0x%02x len=%d",
              buf[total], rec_len);
      total += 5;

      while (rec_len > 0)
        {
          ssize_t n;

          if (total + rec_len > buf_size)
            {
              fp_dbg ("TLS relay flight does not fit, failing");
              return -1;
            }
          n = read (tls->client_fd, buf + total, rec_len);
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            return -1;
          total += n;
          rec_len -= n;
        }

      if (poll (&pfd, 1, 50) <= 0 || !(pfd.revents & POLLIN))
        break;
    }

  return total > 0 ? total : -1;
}

static void
tls_handshake_run (FpiSsm *ssm, FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  int stage = fpi_ssm_get_cur_state (ssm);
  guint8 buff[4096];
  int size;
  GError *err = NULL;

  if (stage == TLS_HANDSHAKE_STAGE_HELLO_S)
    {
      size = goodix_tls_read_flight (priv->tls_hop, buff, sizeof (buff));
      if (size <= 0)
        {
          fpi_ssm_mark_failed (ssm, g_error_new (g_io_error_quark (), size,
                                                 "failed to read tls server "
                                                 "hello"));
          return;
        }
      if (!goodix_send_pack (dev, GOODIX_FLAGS_TLS, buff, size, NULL, &err))
        {
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fpi_ssm_next_state (ssm);
    }
  else if (stage < TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_S)
    {
      /* Still proxying from hardware. */
      fpi_ssm_set_data (ssm, dev, NULL);
      goodix_read_tls (dev, on_goodix_tls_read_handshake, ssm);
    }
  else if (stage == TLS_HANDSHAKE_STAGE_CHANGE_CIPHER_S)
    {
      fp_dbg ("Reading to proxy back");
      size = goodix_tls_read_flight (priv->tls_hop, buff, sizeof (buff));
      if (size <= 0)
        {
          fpi_ssm_mark_failed (ssm, g_error_new (g_io_error_quark (), size,
                                                 "failed to read server "
                                                 "handshake"));

          return;
        }
      if (!goodix_send_pack (dev, GOODIX_FLAGS_TLS, buff, size, NULL, &err))
        {
          fpi_ssm_mark_failed (ssm, err);
          return;
        }
      fpi_ssm_next_state (ssm);
    }
}

static void
do_tls_handshake (FpDevice *dev)
{
  fpi_ssm_start (fpi_ssm_new (dev, tls_handshake_run, TLS_HANDSHAKE_STAGE_NUM),
                 tls_handshake_done);
}

static void
on_goodix_request_tls_connection (FpDevice *dev, guint8 *data,
                                  guint16 length, gpointer user_data,
                                  GError *error)
{
  FpiDeviceGoodixTls *self;
  FpiDeviceGoodixTlsPrivate *priv;
  int sent;

  if (error)
    {
      fp_err ("failed to get tls handshake: %s", error->message);
      /* Forward the error instead of faking success on a dead channel. */
      self = FPI_DEVICE_GOODIXTLS (dev);
      priv = fpi_device_goodixtls_get_instance_private (self);
      if (priv->tls_ready_callback)
        {
          ((GoodixNoneCallback) priv->tls_ready_callback->callback)(
            dev, priv->tls_ready_callback->user_data, error);
          g_clear_pointer (&priv->tls_ready_callback, g_free);
        }
      return;
    }
  self = FPI_DEVICE_GOODIXTLS (user_data);
  priv = fpi_device_goodixtls_get_instance_private (self);

  sent = goodix_tls_client_write (priv->tls_hop, data, length);

  if (sent < 0)
    {
      GError *write_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "failed to send data to tls server");
      if (priv->tls_ready_callback)
        {
          ((GoodixNoneCallback) priv->tls_ready_callback->callback)(
            dev, priv->tls_ready_callback->user_data, write_error);
          g_clear_pointer (&priv->tls_ready_callback, g_free);
        }
      else
        {
          g_error_free (write_error);
        }
      return;
    }

  do_tls_handshake (dev);
}

void
goodix_tls_init (FpDevice *dev, GoodixNoneCallback callback, gpointer user_data)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);
  GoodixTlsServer *s;
  GError *err = NULL;

  fp_dbg ("Starting up goodix tls server");
  g_assert (priv->tls_hop == NULL);
  priv->tls_hop = g_new0 (GoodixTlsServer, 1);

  g_clear_pointer (&priv->tls_ready_callback, g_free);
  priv->tls_ready_callback = make_cb_info (G_CALLBACK (callback), user_data);
  s = priv->tls_hop;
  s->user_data = self;
  if (!goodix_tls_server_init (priv->tls_hop, &err))
    {
      fp_err ("failed to init tls server, error: %s, code: %d",
              err ? err->message : "unknown",
              err ? err->code : 0);
      if (priv->tls_ready_callback)
        {
          ((GoodixNoneCallback) priv->tls_ready_callback->callback) (
            dev, priv->tls_ready_callback->user_data, err);
          g_clear_pointer (&priv->tls_ready_callback, g_free);
        }
      else
        {
          g_clear_error (&err);
        }
      g_clear_pointer (&priv->tls_hop, g_free);
      return;
    }

  goodix_send_request_tls_connection (dev,
                                      on_goodix_request_tls_connection, dev);
}

gboolean
goodix_shutdown_tls (FpDevice *dev, GError **error)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  if (priv->tls_hop)
    {
      gboolean rs = goodix_tls_server_deinit (priv->tls_hop, error);
      g_free (priv->tls_hop);
      priv->tls_hop = NULL;
      /* Drop any stranded handshake waiter without invoking it. */
      g_clear_pointer (&priv->tls_ready_callback, g_free);
      return rs;
    }
  g_clear_pointer (&priv->tls_ready_callback, g_free);
  return TRUE;
}

gboolean
goodix_tls_is_alive (FpDevice *dev)
{
  FpiDeviceGoodixTls *self = FPI_DEVICE_GOODIXTLS (dev);
  FpiDeviceGoodixTlsPrivate *priv =
    fpi_device_goodixtls_get_instance_private (self);

  return priv->tls_hop != NULL;
}

static void
goodix_tls_ready_image_handler (FpDevice *dev, guint8 *data,
                                guint16 length, gpointer user_data,
                                GError *error)
{
  GoodixCallbackInfo *cb_info = user_data;
  GoodixImageCallback callback = (GoodixImageCallback) cb_info->callback;
  FpiDeviceGoodixTls *self;
  FpiDeviceGoodixTlsPrivate *priv;
  guint8 *tls_data;
  guint16 tls_len;
  int sent;
  guint32 size;
  guint8 *buff;
  GError *err = NULL;
  int read_size;

  if (error)
    {
      callback (dev, NULL, 0, cb_info->user_data, error);
      g_free (cb_info);
      return;
    }
  self = FPI_DEVICE_GOODIXTLS (dev);
  priv = fpi_device_goodixtls_get_instance_private (self);

  tls_data = data;
  tls_len = length;

  if (length > 9 && data[0] == 0x00 && data[1] == 0x20)
    {
      tls_data = data + 9;
      tls_len = length - 9;
    }
  else if (length > 5 && data[0] != 0x17)
    {
      for (guint16 i = 0; i + 5 < length; i++)
        {
          if (data[i] == 0x17 && data[i + 1] == 0x03 && data[i + 2] == 0x03)
            {
              tls_data = data + i;
              tls_len = length - i;
              break;
            }
        }
    }

  sent = goodix_tls_client_write (priv->tls_hop, tls_data, tls_len);

  if (sent < 0)
    {
      GError *write_error = g_error_new (G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "failed to send data to tls server");
      callback (dev, NULL, 0, cb_info->user_data, write_error);
      g_free (cb_info);
      return;
    }

  size = 65535;
  buff = g_malloc (size);
  read_size = goodix_tls_server_read (priv->tls_hop, buff, size, &err);

  if (read_size <= 0)
    {
      callback (dev, NULL, 0, cb_info->user_data, err);
      g_free (buff);
      g_free (cb_info);
      return;
    }

  callback (dev, buff, read_size, cb_info->user_data, NULL);
  g_free (buff);
  g_free (cb_info);
}

void
goodix_tls_read_image (FpDevice *dev, GoodixImageCallback callback,
                       gpointer user_data)
{
  GoodixCallbackInfo *cb_info;

  g_assert (callback);
  cb_info = make_cb_info (G_CALLBACK (callback), user_data);

  goodix_send_mcu_get_image (dev, goodix_tls_ready_image_handler, cb_info);
}

static void
fpi_device_goodixtls_init (FpiDeviceGoodixTls *self)
{
}

static void
fpi_device_goodixtls_class_init (FpiDeviceGoodixTlsClass *class)
{
  /* Generic GoodixDefault MCU_GET_IMAGE payload unless a subclass
   * provides capture_payload/capture_payload_len. */
  class->capture_payload = NULL;
  class->capture_payload_len = 0;
}
