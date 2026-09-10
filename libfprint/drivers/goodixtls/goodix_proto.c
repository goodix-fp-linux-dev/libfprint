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

#include <glib.h>
#include <string.h>

#include "fpi-compat.h"
#include "goodix_proto.h"

static guint8
goodix_calc_checksum (guint8 *data, guint16 length)
{
  guint8 checksum = 0;

  for (guint16 i = 0; i < length; i++)
    checksum += data[i];

  return checksum;
}

void
goodix_encode_pack (guint8 flags, guint8 *payload, guint16 payload_len,
                    gboolean pad_data, guint8 **data, guint32 *data_len)
{
  GoodixPack *pack;

  *data_len = sizeof (GoodixPack) + sizeof (guint8) + payload_len;

  if (pad_data && *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    *data_len +=
      GOODIX_EP_OUT_MAX_BUF_SIZE - *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0 (*data_len);
  pack = (GoodixPack *) *data;

  pack->flags = flags;
  pack->length = GUINT16_TO_LE (payload_len);
  (*data)[sizeof (GoodixPack)] = goodix_calc_checksum (*data, sizeof (GoodixPack));

  memcpy (*data + sizeof (GoodixPack) + sizeof (guint8), payload, payload_len);
}

void
goodix_encode_protocol (guint8 cmd, const guint8 *payload, guint16 payload_len,
                        gboolean calc_checksum, gboolean pad_data,
                        guint8 **data, guint32 *data_len)
{
  GoodixProtocol *protocol;

  *data_len = sizeof (GoodixProtocol) + payload_len + sizeof (guint8);

  if (pad_data && *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE)
    *data_len +=
      GOODIX_EP_OUT_MAX_BUF_SIZE - *data_len % GOODIX_EP_OUT_MAX_BUF_SIZE;

  *data = g_malloc0 (*data_len);
  protocol = (GoodixProtocol *) *data;

  protocol->cmd = cmd;
  protocol->length = GUINT16_TO_LE (payload_len + sizeof (guint8));

  memcpy (*data + sizeof (GoodixProtocol), payload, payload_len);

  if (calc_checksum)
    (*data)[sizeof (GoodixProtocol) + payload_len] =
      0xaa -
      goodix_calc_checksum (*data, sizeof (GoodixProtocol) + payload_len);
  else
    (*data)[sizeof (GoodixProtocol) + payload_len] = GOODIX_NULL_CHECKSUM;
}

gboolean
goodix_decode_pack (guint8 *data, guint32 data_len, guint8 *flags,
                    guint8 **payload, guint16 *payload_len,
                    gboolean *valid_checksum)
{
  guint16 length;
  guint16 wire_len;

  if (data == NULL || flags == NULL || payload == NULL ||
      payload_len == NULL || valid_checksum == NULL)
    return FALSE;

  if (data_len < sizeof (GoodixPack) + sizeof (guint8))
    return FALSE;

  memcpy (&wire_len, data + sizeof (guint8), sizeof (wire_len));
  length = GUINT16_FROM_LE (wire_len);

  if (data_len < (guint32) length + sizeof (GoodixPack) + sizeof (guint8))
    return FALSE;

  *flags = data[0];
  if (length > 0)
    *payload = g_memdup2 (data + sizeof (GoodixPack) + sizeof (guint8), length);
  else
    *payload = NULL;
  *payload_len = length;
  *valid_checksum = goodix_calc_checksum (data, sizeof (GoodixPack)) ==
                    data[sizeof (GoodixPack)];

  return TRUE;
}

gboolean
goodix_decode_protocol (guint8 *data, guint32 data_len, guint8 *cmd,
                        guint8 **payload, guint16 *payload_len,
                        gboolean *valid_checksum,
                        gboolean *valid_null_checksum)
{
  guint16 wire_len, length;

  if (data == NULL || cmd == NULL || payload == NULL ||
      payload_len == NULL || valid_checksum == NULL ||
      valid_null_checksum == NULL)
    return FALSE;

  if (data_len < sizeof (GoodixProtocol) + sizeof (guint8))
    return FALSE;

  memcpy (&wire_len, data + sizeof (guint8), sizeof (wire_len));
  wire_len = GUINT16_FROM_LE (wire_len);

  /* Wire length includes the trailing checksum byte. */
  if (wire_len < sizeof (guint8))
    return FALSE;
  length = wire_len - sizeof (guint8);

  if (data_len < (guint32) length + sizeof (GoodixProtocol) + sizeof (guint8))
    return FALSE;

  *cmd = data[0];
  if (length > 0)
    *payload = g_memdup2 (data + sizeof (GoodixProtocol), length);
  else
    *payload = NULL;
  *payload_len = length;
  *valid_checksum =
    0xaa - goodix_calc_checksum (data, sizeof (GoodixProtocol) + length) ==
    data[sizeof (GoodixProtocol) + length];
  *valid_null_checksum =
    GOODIX_NULL_CHECKSUM == data[sizeof (GoodixProtocol) + length];

  return TRUE;
}
