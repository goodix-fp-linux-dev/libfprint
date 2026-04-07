/*
 * Goodix 53x5 series driver for libfprint
 * Copyright (C) 2024 libfprint contributors
 *
 * Based on the reference Python implementation by goodix-fp-dump contributors
 * https://github.com/goodix-fp-linux-dev/goodix-fp-dump
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

#include "goodix53x5_proto.h"

#include <glib.h>
#include <string.h>
#include <nss.h>
#include <pk11pub.h>

/* ---------- Default sensor config (256 bytes from reference impl) ---------- */
const guint8 goodix53x5_default_config[256] = {
  0x40, 0x11, 0x6c, 0x7d, 0x28, 0xa5, 0x28, 0xcd, 0x1c, 0xe9, 0x10, 0xf9, 0x00, 0xf9, 0x00, 0xf9,
  0x00, 0x04, 0x02, 0x00, 0x00, 0x08, 0x00, 0x11, 0x11, 0xba, 0x00, 0x01, 0x80, 0xca, 0x00, 0x07,
  0x00, 0x84, 0x00, 0xbe, 0xb2, 0x86, 0x00, 0xc5, 0xb9, 0x88, 0x00, 0xb5, 0xad, 0x8a, 0x00, 0x9d,
  0x95, 0x8c, 0x00, 0x00, 0xbe, 0x8e, 0x00, 0x00, 0xc5, 0x90, 0x00, 0x00, 0xb5, 0x92, 0x00, 0x00,
  0x9d, 0x94, 0x00, 0x00, 0xaf, 0x96, 0x00, 0x00, 0xbf, 0x98, 0x00, 0x00, 0xb6, 0x9a, 0x00, 0x00,
  0xa7, 0x30, 0x00, 0x6c, 0x1c, 0x50, 0x00, 0x01, 0x05, 0xd0, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00,
  0x00, 0x72, 0x00, 0x78, 0x56, 0x74, 0x00, 0x34, 0x12, 0x26, 0x00, 0x00, 0x12, 0x20, 0x00, 0x10,
  0x40, 0x12, 0x00, 0x03, 0x04, 0x02, 0x02, 0x16, 0x21, 0x2c, 0x02, 0x0a, 0x03, 0x2a, 0x01, 0x02,
  0x00, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x32, 0x00, 0x80, 0x00, 0x05, 0x04, 0x5c, 0x00, 0x00,
  0x01, 0x56, 0x00, 0x28, 0x20, 0x58, 0x00, 0x01, 0x00, 0x32, 0x00, 0x24, 0x02, 0x82, 0x00, 0x80,
  0x0c, 0x20, 0x02, 0x88, 0x0d, 0x2a, 0x01, 0x92, 0x07, 0x22, 0x00, 0x01, 0x20, 0x24, 0x00, 0x14,
  0x00, 0x80, 0x00, 0x05, 0x04, 0x5c, 0x00, 0x00, 0x01, 0x56, 0x00, 0x08, 0x20, 0x58, 0x00, 0x03,
  0x00, 0x32, 0x00, 0x08, 0x04, 0x82, 0x00, 0x80, 0x0c, 0x20, 0x02, 0x88, 0x0d, 0x2a, 0x01, 0x18,
  0x04, 0x5c, 0x00, 0x80, 0x00, 0x54, 0x00, 0x00, 0x01, 0x62, 0x00, 0x09, 0x03, 0x64, 0x00, 0x18,
  0x00, 0x82, 0x00, 0x80, 0x0c, 0x20, 0x02, 0x88, 0x0d, 0x2a, 0x01, 0x18, 0x04, 0x5c, 0x00, 0x80,
  0x00, 0x52, 0x00, 0x08, 0x00, 0x54, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x61, 0x4f,
};

/* ---------- OTP custom CRC-8 lookup table ---------- */
const guint8 goodix53x5_otp_hash_table[256] = {
  0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
  0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
  0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
  0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
  0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
  0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
  0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
  0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
  0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
  0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
  0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
  0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
  0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
  0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
  0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
  0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3,
};

/* ---------- Message encoding / decoding ---------- */

/**
 * goodix53x5_encode_message:
 *
 * USB wire format for the 53x5 family:
 *   command_byte  = (category << 4) | (command << 1)
 *   frame         = [command_byte] [payload_len+1 as u16le] [payload] [checksum]
 *
 * Chunks are 64 bytes each. First chunk starts with command_byte; continuation
 * chunks start with (command_byte | 0x01).  Data is zero-padded to 64 bytes.
 *
 * The function returns a *single* flat buffer of all chunks concatenated.
 */
guint8 *
goodix53x5_encode_message (guint8        category,
                           guint8        command,
                           const guint8 *payload,
                           gsize         payload_len,
                           gsize        *out_len)
{
  guint8 command_byte;
  guint16 size_field;
  guint8 checksum = 0;
  GByteArray *frame;
  GByteArray *chunks;
  guint8 *data;
  gsize i;

  g_assert (category <= 0xF);
  g_assert (command <= 0x7);

  command_byte = (guint8) ((category << 4) | (command << 1));

  /* Build the logical frame: [cmd] [size_le16] [payload] [checksum] */
  frame = g_byte_array_new ();
  g_byte_array_append (frame, &command_byte, 1);
  size_field = GUINT16_TO_LE ((guint16) (payload_len + 1));
  g_byte_array_append (frame, (guint8 *) &size_field, 2);
  if (payload_len > 0)
    g_byte_array_append (frame, payload, payload_len);

  /* Compute checksum: (0xAA - sum(frame)) & 0xFF */
  checksum = 0xAA;
  for (i = 0; i < frame->len; i++)
    checksum -= frame->data[i];
  g_byte_array_append (frame, &checksum, 1);

  /* Split into 64-byte chunks */
  chunks = g_byte_array_new ();

  data = frame->data;
  i = 0;

  /* First chunk */
  {
    gsize chunk_len = MIN ((gsize) GOODIX53X5_USB_CHUNK_SIZE, frame->len);
    guint8 chunk[GOODIX53X5_USB_CHUNK_SIZE];
    memset (chunk, 0, sizeof (chunk));
    memcpy (chunk, data, chunk_len);
    g_byte_array_append (chunks, chunk, GOODIX53X5_USB_CHUNK_SIZE);
    i = chunk_len;
  }

  /* Continuation chunks */
  while (i < frame->len)
    {
      guint8 chunk[GOODIX53X5_USB_CHUNK_SIZE];
      gsize avail = frame->len - i;
      gsize chunk_payload = MIN ((gsize) (GOODIX53X5_USB_CHUNK_SIZE - 1), avail);

      memset (chunk, 0, sizeof (chunk));
      chunk[0] = command_byte | 0x01;
      memcpy (&chunk[1], data + i, chunk_payload);
      g_byte_array_append (chunks, chunk, GOODIX53X5_USB_CHUNK_SIZE);
      i += chunk_payload;
    }

  *out_len = chunks->len;
  data = g_steal_pointer (&chunks->data);
  g_byte_array_free (frame, TRUE);
  /* Must free the GByteArray struct but NOT the data (already stolen) */
  chunks->data = NULL;
  chunks->len = 0;
  g_byte_array_free (chunks, FALSE);

  return data;
}

/**
 * goodix53x5_parse_message:
 *
 * Parse the first complete logical message from a raw USB buffer (which may
 * span multiple 64-byte chunks). The buffer is expected to contain exactly
 * one full message (with possible trailing zeros from a single bulk read).
 */
gboolean
goodix53x5_parse_message (const guint8 *buf,
                          gsize         buf_len,
                          guint8       *out_category,
                          guint8       *out_command,
                          guint8      **out_payload,
                          gsize        *out_payload_len)
{
  guint8 command_byte;
  guint16 message_size;
  gsize total_needed;
  GByteArray *assembled;
  guint8 received_checksum;
  guint8 computed_checksum;
  gsize i;

  if (buf_len < 3)
    {
      fp_warn ("goodix53x5: message too short (%zu bytes)", buf_len);
      return FALSE;
    }

  command_byte = buf[0];
  message_size = GUINT16_FROM_LE (*(guint16 *) (buf + 1));
  total_needed = 3 + (gsize) message_size;  /* cmd + size_field + payload + checksum */

  /* Reassemble from chunks if needed */
  assembled = g_byte_array_new ();
  g_byte_array_append (assembled, buf, (guint) MIN (buf_len, (gsize) GOODIX53X5_USB_CHUNK_SIZE));

  {
    const guint8 *remaining = buf + GOODIX53X5_USB_CHUNK_SIZE;
    gsize remaining_len = (buf_len > GOODIX53X5_USB_CHUNK_SIZE) ? buf_len - GOODIX53X5_USB_CHUNK_SIZE : 0;

    while (assembled->len < total_needed && remaining_len >= GOODIX53X5_USB_CHUNK_SIZE)
      {
        guint8 contd_cmd = remaining[0];
        if ((contd_cmd & 0x01) == 0 || (contd_cmd & 0xFE) != command_byte)
          {
            fp_warn ("goodix53x5: bad continuation byte 0x%02x (assembled=%u total=%zu)",
                     contd_cmd, assembled->len, total_needed);
            g_byte_array_free (assembled, TRUE);
            return FALSE;
          }
        g_byte_array_append (assembled, remaining + 1, GOODIX53X5_USB_CHUNK_SIZE - 1);
        remaining += GOODIX53X5_USB_CHUNK_SIZE;
        remaining_len -= GOODIX53X5_USB_CHUNK_SIZE;
      }
  }

  if (assembled->len < total_needed)
    {
      fp_warn ("goodix53x5: incomplete message: have %u, need %zu", assembled->len, total_needed);
      g_byte_array_free (assembled, TRUE);
      return FALSE;
    }

  /* Verify checksum */
  received_checksum = assembled->data[total_needed - 1];
  if (received_checksum != 0x88)
    {
      computed_checksum = 0xAA;
      for (i = 0; i < total_needed - 1; i++)
        computed_checksum -= assembled->data[i];
      if (received_checksum != computed_checksum)
        {
          fp_warn ("goodix53x5: checksum mismatch: expected 0x%02x, got 0x%02x",
                   computed_checksum, received_checksum);
          g_byte_array_free (assembled, TRUE);
          return FALSE;
        }
    }

  *out_category = command_byte >> 4;
  *out_command = (command_byte & 0x0F) >> 1;
  *out_payload_len = (gsize) message_size - 1;   /* exclude checksum byte from message_size */

  if (*out_payload_len > 0)
    {
      *out_payload = g_malloc (*out_payload_len);
      memcpy (*out_payload, assembled->data + 3, *out_payload_len);
    }
  else
    {
      *out_payload = NULL;
    }

  g_byte_array_free (assembled, TRUE);
  return TRUE;
}

/* ---------- OTP parsing ---------- */

guint8
goodix53x5_compute_otp_hash (const guint8 *data,
                             gsize         len)
{
  guint8 checksum = 0;

  for (gsize i = 0; i < len; i++)
    checksum = goodix53x5_otp_hash_table[checksum ^ data[i]];

  return (guint8) (~checksum & 0xFF);
}

gboolean
goodix53x5_parse_otp (const guint8          *otp,
                      gsize                  otp_len,
                      Goodix53x5CalibParams *calib_params)
{
  guint8 hash_data[64];
  guint8 computed_hash;
  guint8 received_hash;
  guint8 diff;
  guint8 tmp, tmp2;

  if (otp_len < 32)
    {
      fp_warn ("goodix53x5: OTP too short: %zu bytes", otp_len);
      return FALSE;
    }

  /* Hash covers otp[0:25] + otp[26:] (i.e., excluding otp[25]) */
  memcpy (hash_data, otp, 25);
  memcpy (hash_data + 25, otp + 26, otp_len - 26);
  received_hash = otp[25];
  computed_hash = goodix53x5_compute_otp_hash (hash_data, 25 + (otp_len - 26));

  if (received_hash != computed_hash)
    {
      fp_warn ("goodix53x5: OTP hash mismatch: expected 0x%02x, got 0x%02x",
               computed_hash, received_hash);
      return FALSE;
    }

  /* tcode */
  calib_params->tcode = (otp[23] != 0) ? (guint16) (otp[23] + 1) : 0;

  /* diff[5:1] from otp[17] */
  diff = (otp[17] >> 1) & 0x1F;

  if (diff == 0)
    {
      calib_params->delta_fdt = 0;
      calib_params->delta_down = 0x0D;
      calib_params->delta_up = 0x0B;
    }
  else
    {
      tmp = (guint8) (diff + 5);
      tmp2 = (guint8) ((tmp * 0x32u) >> 4);
      calib_params->delta_fdt = tmp2 / 5;
      calib_params->delta_down = tmp2 / 3;
      calib_params->delta_up = (guint8) (calib_params->delta_down - 2);
    }

  /* DAC values */
  if (otp[17] == 0 || otp[22] == 0 || otp[31] == 0)
    {
      calib_params->dac_h = 0x97;
      calib_params->dac_l = 0xD0;
    }
  else
    {
      calib_params->dac_h = (guint16) (((guint16) otp[17] << 8 ^ (guint16) otp[22]) & 0x1FF);
      calib_params->dac_l = (guint16) (((guint16) otp[17] & 0x40u) << 2 ^ (guint16) otp[31]);
    }

  memset (calib_params->fdt_base_down, 0, GOODIX53X5_FDT_BASE_LEN);
  memset (calib_params->fdt_base_up, 0, GOODIX53X5_FDT_BASE_LEN);
  memset (calib_params->fdt_base_manual, 0, GOODIX53X5_FDT_BASE_LEN);

  fp_dbg ("goodix53x5: OTP parsed: tcode=%u, delta_fdt=%u, delta_down=%u, "
          "delta_up=%u, dac_h=0x%x, dac_l=0x%x",
          calib_params->tcode, calib_params->delta_fdt,
          calib_params->delta_down, calib_params->delta_up,
          calib_params->dac_h, calib_params->dac_l);

  return TRUE;
}

/* ---------- Sensor configuration ---------- */

static void
replace_value_in_section (guint8 *config,
                          guint8  section_num,
                          guint16 tag,
                          guint16 value)
{
  const guint8 *section_table = config + 1;
  guint8 section_base = section_table[section_num * 2];
  guint8 section_size = section_table[section_num * 2 + 1];
  guint8 entry_base;

  for (entry_base = section_base;
       entry_base < section_base + section_size;
       entry_base += 4)
    {
      guint16 entry_tag = GUINT16_FROM_LE (*(guint16 *) &config[entry_base]);
      if (entry_tag == tag)
        {
          guint16 val_le = GUINT16_TO_LE (value);
          memcpy (&config[entry_base + 2], &val_le, 2);
        }
    }
}

static void
fix_config_checksum (guint8 *config)
{
  guint32 checksum = 0xA5A5;
  guint16 sum_pair;
  guint16 result;

  for (gsize i = 0; i < 256 - 2; i += 2)
    {
      sum_pair = GUINT16_FROM_LE (*(guint16 *) &config[i]);
      checksum = (checksum + sum_pair) & 0xFFFF;
    }
  result = (guint16) ((0x10000 - checksum) & 0xFFFF);
  guint16 result_le = GUINT16_TO_LE (result);
  memcpy (&config[254], &result_le, 2);
}

void
goodix53x5_build_config (const Goodix53x5CalibParams *calib_params,
                         guint8                      *out_config)
{
  memcpy (out_config, goodix53x5_default_config, 256);

  /* Patch TCODE in sections 2, 3, 4 */
  replace_value_in_section (out_config, 2, GOODIX53X5_CFG_TCODE_TAG, calib_params->tcode);
  replace_value_in_section (out_config, 3, GOODIX53X5_CFG_TCODE_TAG, calib_params->tcode);
  replace_value_in_section (out_config, 4, GOODIX53X5_CFG_TCODE_TAG, calib_params->tcode);

  /* Patch DAC_L in sections 2, 3 */
  replace_value_in_section (out_config, 2, GOODIX53X5_CFG_DAC_L_TAG,
                            (guint16) ((calib_params->dac_l << 4) | 8));
  replace_value_in_section (out_config, 3, GOODIX53X5_CFG_DAC_L_TAG,
                            (guint16) ((calib_params->dac_l << 4) | 8));

  /* Patch DELTA_DOWN in section 2 */
  replace_value_in_section (out_config, 2, GOODIX53X5_CFG_DELTA_DOWN_TAG,
                            (guint16) ((calib_params->delta_down << 8) | 0x80));

  fix_config_checksum (out_config);
}

/* ---------- GTLS session key derivation (TLS 1.2 P_SHA256) ---------- */

/*
 * HMAC-SHA256 computed using GLib's GChecksum infrastructure.
 * Returns heap-allocated 32-byte digest. Caller must g_free().
 */
static guint8 *
hmac_sha256 (const guint8 *key,
             gsize         key_len,
             const guint8 *data,
             gsize         data_len)
{
  GHmac *hmac;
  guint8 *digest;
  gsize digest_len = 32;

  digest = g_malloc (digest_len);
  hmac = g_hmac_new (G_CHECKSUM_SHA256, key, key_len);
  g_hmac_update (hmac, data, (gssize) data_len);
  g_hmac_get_digest (hmac, digest, &digest_len);
  g_hmac_unref (hmac);
  return digest;
}

void
goodix53x5_gtls_derive_session_key (const guint8      *psk,
                                    const guint8      *client_random,
                                    const guint8      *server_random,
                                    Goodix53x5GTLSCtx *ctx)
{
  /* seed = "master secret" || client_random || server_random */
  static const guint8 label[] = "master secret";
  guint8 seed[13 + GOODIX53X5_RANDOM_LEN + GOODIX53X5_RANDOM_LEN];
  gsize seed_len = sizeof (seed);
  guint8 session_key[GOODIX53X5_GTLS_SESSION_KEY_LEN + 32]; /* extra to avoid re-deriving */
  gsize generated = 0;
  guint8 *A;
  gsize A_len;
  guint8 *A_next;
  guint8 *block;
  guint8 *A_plus_seed;

  memcpy (seed, label, 13);
  memcpy (seed + 13, client_random, GOODIX53X5_RANDOM_LEN);
  memcpy (seed + 13 + GOODIX53X5_RANDOM_LEN, server_random, GOODIX53X5_RANDOM_LEN);

  /* P_SHA256: A(0) = seed (77 bytes); A(i) = HMAC(psk, A(i-1)) which is 32 bytes */
  A = g_malloc (seed_len);
  memcpy (A, seed, seed_len);
  A_len = seed_len;

  while (generated < GOODIX53X5_GTLS_SESSION_KEY_LEN)
    {
      /* A(i) = HMAC(psk, A(i-1)) — result is always 32 bytes */
      A_next = hmac_sha256 (psk, GOODIX53X5_PSK_LEN, A, A_len);
      g_free (A);
      A = A_next;
      A_len = 32;  /* after first iteration A is always 32 bytes */

      /* block = HMAC(psk, A || seed) */
      A_plus_seed = g_malloc (A_len + seed_len);
      memcpy (A_plus_seed, A, A_len);
      memcpy (A_plus_seed + A_len, seed, seed_len);
      block = hmac_sha256 (psk, GOODIX53X5_PSK_LEN, A_plus_seed, A_len + seed_len);
      g_free (A_plus_seed);

      gsize to_copy = MIN ((gsize) 32, GOODIX53X5_GTLS_SESSION_KEY_LEN - generated);
      memcpy (session_key + generated, block, to_copy);
      generated += to_copy;
      g_free (block);
    }

  g_free (A);

  /* Slice the session key into sub-keys */
  memcpy (ctx->symmetric_key, session_key, GOODIX53X5_SYMMETRIC_KEY_LEN);
  memcpy (ctx->symmetric_iv, session_key + GOODIX53X5_SYMMETRIC_KEY_LEN, GOODIX53X5_SYMMETRIC_IV_LEN);
  memcpy (ctx->hmac_key, session_key + GOODIX53X5_SYMMETRIC_KEY_LEN + GOODIX53X5_SYMMETRIC_IV_LEN,
          GOODIX53X5_HMAC_KEY_LEN);
  ctx->hmac_client_counter = GUINT16_FROM_LE (
    *(guint16 *) &session_key[GOODIX53X5_SYMMETRIC_KEY_LEN + GOODIX53X5_SYMMETRIC_IV_LEN + GOODIX53X5_HMAC_KEY_LEN]);
  ctx->hmac_server_counter = GUINT16_FROM_LE (
    *(guint16 *) &session_key[GOODIX53X5_SYMMETRIC_KEY_LEN + GOODIX53X5_SYMMETRIC_IV_LEN + GOODIX53X5_HMAC_KEY_LEN + 2]);

  fp_dbg ("goodix53x5: session_key derived: client_ctr=0x%04x server_ctr=0x%04x",
          ctx->hmac_client_counter, ctx->hmac_server_counter);
}

void
goodix53x5_gtls_compute_identity (const guint8 *hmac_key,
                                  const guint8 *client_random,
                                  const guint8 *server_random,
                                  guint8       *out_identity)
{
  guint8 data[GOODIX53X5_RANDOM_LEN * 2];
  guint8 *digest;

  memcpy (data, client_random, GOODIX53X5_RANDOM_LEN);
  memcpy (data + GOODIX53X5_RANDOM_LEN, server_random, GOODIX53X5_RANDOM_LEN);
  digest = hmac_sha256 (hmac_key, GOODIX53X5_HMAC_KEY_LEN, data, sizeof (data));
  memcpy (out_identity, digest, GOODIX53X5_IDENTITY_LEN);
  g_free (digest);
}

/* ---------- AES-128-CBC decryption (via NSS) ---------- */

static gboolean
aes128_cbc_decrypt (const guint8 *key,
                    const guint8 *iv,
                    const guint8 *ciphertext,
                    gsize         ciphertext_len,
                    guint8       *plaintext)
{
  PK11SlotInfo *slot = NULL;
  PK11SymKey *symkey = NULL;
  SECItem iv_item  = { siBuffer, (unsigned char *) iv, 16 };
  SECItem *param    = NULL;
  PK11Context *ctx      = NULL;
  int outlen   = 0;
  unsigned int finallen = 0;
  gboolean ok       = FALSE;

  slot = PK11_GetBestSlot (CKM_AES_CBC, NULL);
  if (!slot)
    goto out;

  {
    SECItem key_item = { siBuffer, (unsigned char *) key, GOODIX53X5_SYMMETRIC_KEY_LEN };
    symkey = PK11_ImportSymKey (slot, CKM_AES_CBC, PK11_OriginUnwrap,
                                CKA_DECRYPT, &key_item, NULL);
  }
  if (!symkey)
    goto out;

  param = PK11_ParamFromIV (CKM_AES_CBC, &iv_item);
  if (!param)
    goto out;

  ctx = PK11_CreateContextBySymKey (CKM_AES_CBC, CKA_DECRYPT, symkey, param);
  if (!ctx)
    goto out;

  if (PK11_CipherOp (ctx, plaintext, &outlen, (int) ciphertext_len,
                     (unsigned char *) ciphertext, (int) ciphertext_len) != SECSuccess)
    goto out;

  if (PK11_DigestFinal (ctx, plaintext + outlen, &finallen,
                        (unsigned int) ciphertext_len - (unsigned int) outlen) != SECSuccess)
    goto out;

  ok = TRUE;

out:
  if (ctx)
    PK11_DestroyContext (ctx, PR_TRUE);
  if (param)
    SECITEM_FreeItem (param, PR_TRUE);
  if (symkey)
    PK11_FreeSymKey (symkey);
  if (slot)
    PK11_FreeSlot (slot);
  return ok;
}

/* ---------- GEA stream cipher decryption ---------- */

static void
gea_decrypt (const guint8 *gea_key_bytes,
             const guint8 *encrypted,
             gsize         encrypted_len,
             guint8       *decrypted)
{
  guint32 key = GUINT32_FROM_LE (*(guint32 *) gea_key_bytes);
  gsize idx;

  for (idx = 0; idx + 1 < encrypted_len; idx += 2)
    {
      guint32 uVar3, uVar2, uVar1;
      guint16 input_element, stream_val, result;

      uVar3 = (key >> 1 ^ key) & 0xFFFFFFFF;
      uVar2 = (((((((
                      (((key >> 0xF) & 0x2000) | (key & 0x1000000)) >> 1 | (key & 0x20000)) >> 2
                    | (key & 0x1000)) >> 3 | ((key >> 7 ^ key) & 0x80000)) >> 1 |
                  ((key >> 0xF ^ key) & 0x4000)) >> 2 | (key & 0x2000)) >> 2
                | (uVar3 & 0x40) | (key & 0x20)) >> 1 |
               ((key >> 9 ^ key << 8) & 0x800) | ((key >> 0x14 ^ key * 2) & 4) |
               ((key * 8 ^ key >> 0x10) & 0x4000) |
               ((key >> 2 ^ key >> 0x10) & 0x80) |
               ((key << 6 ^ key >> 7) & 0x100) | ((key & 0x100) << 7));
      uVar2 &= 0xFFFFFFFF;
      uVar1 = key & 0xFFFF;

      key = (guint32) (((key ^ (uVar3 >> 0x14 ^ key) >> 10) << 0x1F | key >> 1) & 0xFFFFFFFF);

      input_element = GUINT16_FROM_LE (*(guint16 *) (encrypted + idx));
      stream_val = (guint16) ((((uVar2 >> 8) & 0xFFFF) + (guint16) (((uVar2 & 0xFF) | (uVar1 & 1)) * 0x100)) & 0xFFFF);
      result = GUINT16_TO_LE ((guint16) (input_element ^ stream_val));
      memcpy (decrypted + idx, &result, 2);
    }

  /* Handle odd byte if any (just copy it) */
  if (encrypted_len % 2 != 0)
    decrypted[encrypted_len - 1] = encrypted[encrypted_len - 1];
}

/* ---------- GTLS image decryption ---------- */

gboolean
goodix53x5_gtls_decrypt_image (Goodix53x5GTLSCtx *ctx,
                               const guint8      *encrypted_msg,
                               gsize              encrypted_msg_len,
                               guint8           **out_raw,
                               gsize             *out_raw_len)
{
  guint32 data_type;
  guint32 msg_length;
  const guint8 *payload;
  gsize payload_len;
  const guint8 *payload_hmac;
  GByteArray *gea_data;
  guint8 hmac_input[4 + 0x400];
  guint8 *hmac_digest;
  guint8 *gea_decrypted;
  guint32 reported_crc;
  guint32 computed_crc;

  if (encrypted_msg_len < 8 + 32)
    {
      fp_warn ("goodix53x5: encrypted message too short");
      return FALSE;
    }

  data_type = GUINT32_FROM_LE (*(guint32 *) encrypted_msg);
  if (data_type != GOODIX53X5_IMAGE_DATA_TYPE)
    {
      fp_warn ("goodix53x5: unexpected image data type: 0x%08x", data_type);
      return FALSE;
    }

  msg_length = GUINT32_FROM_LE (*(guint32 *) (encrypted_msg + 4));
  if (msg_length != (guint32) encrypted_msg_len)
    {
      fp_warn ("goodix53x5: image length mismatch: reported %u, actual %zu",
               msg_length, encrypted_msg_len);
      return FALSE;
    }

  /* Layout: [8 bytes header] [encrypted payload] [32 bytes HMAC] */
  payload = encrypted_msg + 8;
  payload_len = encrypted_msg_len - 8 - 32;
  payload_hmac = encrypted_msg + encrypted_msg_len - 32;

  /* Reassemble GEA-encrypted data from 15 interleaved blocks:
   * even blocks (0,2,4,...14): GEA-encrypted  (raw/untouched here)
   * odd blocks  (1,3,5,...13): AES-CBC encrypted → decrypt into gea buffer
   *
   * We build gea_data as the concatenation of:
   *   - even blocks as-is
   *   - odd blocks AES-CBC decrypted (decrypted bytes go in the same positions)
   *
   * The reference places decrypted AES blocks directly into the gea buffer,
   * interleaved with the GEA blocks. The HMAC is computed over the last 0x400
   * bytes of gea_data.
   */
  gea_data = g_byte_array_new ();

  {
    const guint8 *p = payload;
    guint8 aes_plain[GOODIX53X5_AES_BLOCK_SIZE];

    for (guint block_idx = 0; block_idx < GOODIX53X5_ENCRYPT_BLOCKS; block_idx++)
      {
        if (block_idx % 2 == 0)
          {
            /* GEA block (appended as-is) */
            gsize block_size;
            if (block_idx == 0)
              {
                block_size = GOODIX53X5_GEA_FIRST_BLOCK_SIZE;
              }
            else if (block_idx == GOODIX53X5_ENCRYPT_BLOCKS - 1)
              {
                /* Last even block: take remaining payload.
                 * Before block 14 we have consumed:
                 *   block 0                           → GEA_FIRST_BLOCK_SIZE
                 *   blocks 2,4,6,8,10,12 (6 blocks)  → 6 * GEA_BLOCK_SIZE
                 *   blocks 1,3,5,7,9,11,13 (7 blocks) → 7 * AES_BLOCK_SIZE
                 * Note: ENCRYPT_BLOCKS/2 == 7, which is the number of odd blocks,
                 * but only 6 non-first even blocks precede block 14. */
                gsize used = GOODIX53X5_GEA_FIRST_BLOCK_SIZE +
                             ((GOODIX53X5_ENCRYPT_BLOCKS / 2) - 1) * GOODIX53X5_GEA_BLOCK_SIZE +
                             (GOODIX53X5_ENCRYPT_BLOCKS / 2) * GOODIX53X5_AES_BLOCK_SIZE;
                block_size = payload_len - used;
              }
            else
              {
                block_size = GOODIX53X5_GEA_BLOCK_SIZE;
              }

            g_byte_array_append (gea_data, p, (guint) block_size);
            p += block_size;
          }
        else
          {
            /* AES-CBC block: decrypt and append */
            memset (aes_plain, 0, sizeof (aes_plain));
            if (!aes128_cbc_decrypt (ctx->symmetric_key, ctx->symmetric_iv,
                                     p, GOODIX53X5_AES_BLOCK_SIZE, aes_plain))
              {
                fp_warn ("goodix53x5: AES decryption failed at block %u", block_idx);
                g_byte_array_free (gea_data, TRUE);
                return FALSE;
              }
            g_byte_array_append (gea_data, aes_plain, GOODIX53X5_AES_BLOCK_SIZE);
            p += GOODIX53X5_AES_BLOCK_SIZE;
          }
      }
  }

  /* Verify HMAC-SHA256 over (hmac_server_counter_le32 || gea_data[-0x400:]) */
  {
    gsize gea_len = gea_data->len;
    gsize tail_off = (gea_len >= 0x400) ? gea_len - 0x400 : 0;
    gsize tail_len = gea_len - tail_off;
    guint32 counter_le = GUINT32_TO_LE ((guint32) ctx->hmac_server_counter);

    memcpy (hmac_input, &counter_le, 4);
    memcpy (hmac_input + 4, gea_data->data + tail_off, tail_len);

    hmac_digest = hmac_sha256 (ctx->hmac_key, GOODIX53X5_HMAC_KEY_LEN,
                               hmac_input, 4 + tail_len);

    fp_dbg ("goodix53x5: HMAC counter=0x%04x gea_len=%zu",
            ctx->hmac_server_counter, gea_len);

    gboolean hmac_ok = (memcmp (hmac_digest, payload_hmac, 32) == 0);
    g_free (hmac_digest);

    /* Always advance the server counter: the device increments its own counter
     * for every image it sends, regardless of whether we verify it. */
    ctx->hmac_server_counter = (guint16) ((ctx->hmac_server_counter + 1) & 0xFFFF);

    if (!hmac_ok)
      {
        fp_warn ("goodix53x5: HMAC verification failed");
        g_byte_array_free (gea_data, TRUE);
        return FALSE;
      }
  }

  /* Discard first 5 bytes (alignment padding per reference impl) */
  if (gea_data->len < 5 + 4)
    {
      fp_warn ("goodix53x5: GEA data too short after trim");
      g_byte_array_free (gea_data, TRUE);
      return FALSE;
    }
  {
    guint8 *gea_buf = gea_data->data + 5;
    gsize gea_len = gea_data->len - 5;

    /* Last 4 bytes are CRC32/MPEG-2 of the data before them */
    if (gea_len < 4)
      {
        fp_warn ("goodix53x5: GEA data too short for CRC");
        g_byte_array_free (gea_data, TRUE);
        return FALSE;
      }
    gea_len -= 4;
    /* CRC is stored in the custom decode_u32 byte ordering from the reference
     * implementation: bytes [b0,b1,b2,b3] → value = b2<<24 | b3<<16 | b0<<8 | b1
     * (each 16-bit word is byte-swapped, then the two words are big-endian). */
    reported_crc = ((guint32) gea_buf[gea_len + 2] << 24)
                   | ((guint32) gea_buf[gea_len + 3] << 16)
                   | ((guint32) gea_buf[gea_len + 0] << 8)
                   |  (guint32) gea_buf[gea_len + 1];

    /* CRC32/MPEG-2 = CRC32 with initial value 0xFFFFFFFF, no final XOR,
     * bit-reversed poly 0x04C11DB7.
     * Computed manually (same as Python crccheck.crc.Crc32Mpeg2). */
    {
      guint32 crc = 0xFFFFFFFF;
      for (gsize i = 0; i < gea_len; i++)
        {
          crc ^= (guint32) gea_buf[i] << 24;
          for (guint b = 0; b < 8; b++)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
        }
      computed_crc = crc;
    }

    if (reported_crc != computed_crc)
      {
        fp_warn ("goodix53x5: GEA CRC mismatch: reported 0x%08x, computed 0x%08x",
                 reported_crc, computed_crc);
        g_byte_array_free (gea_data, TRUE);
        return FALSE;
      }

    /* Decrypt GEA data */
    gea_decrypted = g_malloc (gea_len);
    gea_decrypt (ctx->symmetric_key, gea_buf, gea_len, gea_decrypted);

    *out_raw = gea_decrypted;
    *out_raw_len = gea_len;
  }

  g_byte_array_free (gea_data, TRUE);
  return TRUE;
}

/* ---------- Image decoding (12-bit packed pixels) ---------- */

gboolean
goodix53x5_decode_image (const guint8 *raw,
                         gsize         raw_len,
                         guint16     **out_pixels)
{
  gsize n_pixels;
  guint16 *pixels;
  gsize src_idx;
  gsize dst_idx;

  /* 6 bytes → 4 pixels */
  if (raw_len < 6 || (raw_len % 6) != 0)
    {
      fp_warn ("goodix53x5: raw data length %zu not a multiple of 6", raw_len);
      return FALSE;
    }

  n_pixels = (raw_len / 6) * 4;

  if (n_pixels != GOODIX53X5_SENSOR_PIXELS)
    {
      fp_warn ("goodix53x5: unexpected pixel count: %zu (expected %u)",
               n_pixels, GOODIX53X5_SENSOR_PIXELS);
      return FALSE;
    }

  pixels = g_malloc (GOODIX53X5_SENSOR_PIXELS * sizeof (guint16));

  src_idx = 0;
  dst_idx = 0;
  while (src_idx + 5 < raw_len)
    {
      const guint8 *c = raw + src_idx;
      pixels[dst_idx + 0] = (guint16) (((guint16) (c[0] & 0x0F) << 8) | c[1]);
      pixels[dst_idx + 1] = (guint16) (((guint16) c[3] << 4) | (c[0] >> 4));
      pixels[dst_idx + 2] = (guint16) (((guint16) (c[5] & 0x0F) << 8) | c[2]);
      pixels[dst_idx + 3] = (guint16) (((guint16) c[4] << 4) | (c[5] >> 4));
      src_idx += 6;
      dst_idx += 4;
    }

  *out_pixels = pixels;
  return TRUE;
}

/* ---------- FDT base helpers ---------- */

void
goodix53x5_generate_fdt_base (const guint8 *fdt_data,
                              guint8       *out_fdt_base)
{
  for (gsize i = 0; i < GOODIX53X5_FDT_BASE_LEN; i += 2)
    {
      guint16 fdt_val = GUINT16_FROM_LE (*(guint16 *) (fdt_data + i));
      guint32 base_val = (guint32) ((fdt_val & 0xFFFEu) * 0x80u | (fdt_val >> 1));
      guint16 base_val16 = GUINT16_TO_LE ((guint16) (base_val & 0xFFFF));
      memcpy (out_fdt_base + i, &base_val16, 2);
    }
}

void
goodix53x5_generate_fdt_up_base (const guint8                *fdt_data,
                                 guint16                      touch_flag,
                                 const Goodix53x5CalibParams *calib_params,
                                 guint8                      *out_fdt_base_up)
{
  guint16 fdt_vals[12];
  guint16 fdt_base_up_vals[12];

  for (guint i = 0; i < 12; i++)
    fdt_vals[i] = GUINT16_FROM_LE (*(guint16 *) (fdt_data + i * 2));

  for (guint i = 0; i < 12; i++)
    {
      guint16 val = (guint16) ((fdt_vals[i] >> 1) + calib_params->delta_down);
      fdt_base_up_vals[i] = (guint16) (val * 0x100u | val);
    }

  for (guint i = 0; i < 12; i++)
    {
      if (((touch_flag >> i) & 1) == 0)
        {
          guint16 du = calib_params->delta_up;
          fdt_base_up_vals[i] = (guint16) (du * 0x100u | du);
        }
    }

  for (guint i = 0; i < 12; i++)
    {
      guint16 le = GUINT16_TO_LE (fdt_base_up_vals[i]);
      memcpy (out_fdt_base_up + i * 2, &le, 2);
    }
}

gboolean
goodix53x5_is_fdt_base_valid (const guint8 *fdt_data_1,
                              const guint8 *fdt_data_2,
                              guint8        max_delta)
{
  for (gsize i = 0; i < GOODIX53X5_FDT_BASE_LEN; i += 2)
    {
      guint16 v1 = GUINT16_FROM_LE (*(guint16 *) (fdt_data_1 + i));
      guint16 v2 = GUINT16_FROM_LE (*(guint16 *) (fdt_data_2 + i));
      gint delta = (gint) (v2 >> 1) - (gint) (v1 >> 1);
      if (delta < 0)
        delta = -delta;
      if (delta > (gint) max_delta)
        return FALSE;
    }
  return TRUE;
}
