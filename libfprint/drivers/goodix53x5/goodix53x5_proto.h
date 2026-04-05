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

#pragma once

#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* USB transport constants */
#define GOODIX53X5_USB_CHUNK_SIZE       0x40   /* 64 bytes per USB bulk chunk */

/* Sensor image dimensions */
#define GOODIX53X5_SENSOR_WIDTH         108
#define GOODIX53X5_SENSOR_HEIGHT        88
#define GOODIX53X5_SENSOR_PIXELS        (GOODIX53X5_SENSOR_WIDTH * GOODIX53X5_SENSOR_HEIGHT)

/* FDT base length (12 × 2-byte values = 24 bytes) */
#define GOODIX53X5_FDT_BASE_LEN         24

/* GTLS session key length */
#define GOODIX53X5_GTLS_SESSION_KEY_LEN 0x44   /* 68 bytes */
#define GOODIX53X5_SYMMETRIC_KEY_LEN    16
#define GOODIX53X5_SYMMETRIC_IV_LEN     16
#define GOODIX53X5_HMAC_KEY_LEN         32
#define GOODIX53X5_RANDOM_LEN           32
#define GOODIX53X5_IDENTITY_LEN         32
#define GOODIX53X5_PSK_LEN              32
#define GOODIX53X5_PSK_WHITE_BOX_LEN    96

/* Encrypted image data type tag */
#define GOODIX53X5_IMAGE_DATA_TYPE      0xAA01u

/* Number of interleaved encryption blocks in image payload */
#define GOODIX53X5_ENCRYPT_BLOCKS       15
/* Size of even (GEA) blocks after block 0 */
#define GOODIX53X5_GEA_BLOCK_SIZE       0x3F0
/* Size of first GEA block (smaller) */
#define GOODIX53X5_GEA_FIRST_BLOCK_SIZE 0x3A7
/* Size of each AES-CBC block */
#define GOODIX53X5_AES_BLOCK_SIZE       0x3F0

/* GTLS MCU message types */
#define GOODIX53X5_MCU_CLIENT_HELLO     0xFF01u
#define GOODIX53X5_MCU_SERVER_IDENTITY  0xFF02u
#define GOODIX53X5_MCU_CLIENT_IDENTITY  0xFF03u
#define GOODIX53X5_MCU_SERVER_DONE      0xFF04u

/* Production read/write types */
#define GOODIX53X5_PROD_SEALED_PSK      0xB001u
#define GOODIX53X5_PROD_PSK_WHITE_BOX   0xB002u
#define GOODIX53X5_PROD_PSK_HASH        0xB003u

/* Message categories */
#define GOODIX53X5_CAT_IMAGE            0x2
#define GOODIX53X5_CAT_FDT              0x3
#define GOODIX53X5_CAT_SLEEP            0x6
#define GOODIX53X5_CAT_READ_REG         0x8
#define GOODIX53X5_CAT_CONFIG           0x9
#define GOODIX53X5_CAT_RESET            0xA
#define GOODIX53X5_CAT_ACK              0xB
#define GOODIX53X5_CAT_MCU              0xD
#define GOODIX53X5_CAT_PRODUCTION       0xE

/* Message commands within categories */
#define GOODIX53X5_CMD_IMAGE_GET        0x0
#define GOODIX53X5_CMD_FDT_DOWN         0x1
#define GOODIX53X5_CMD_FDT_UP           0x2
#define GOODIX53X5_CMD_FDT_MANUAL       0x3
#define GOODIX53X5_CMD_SLEEP            0x0
#define GOODIX53X5_CMD_READ_REG         0x1
#define GOODIX53X5_CMD_CONFIG_UPLOAD    0x0
#define GOODIX53X5_CMD_RESET            0x1
#define GOODIX53X5_CMD_READ_OTP         0x3
#define GOODIX53X5_CMD_FW_VERSION       0x4
#define GOODIX53X5_CMD_EC_CONTROL       0x7
#define GOODIX53X5_CMD_MCU              0x1
#define GOODIX53X5_CMD_PROD_WRITE       0x1
#define GOODIX53X5_CMD_PROD_READ        0x2

/* FDT operation opcode bytes */
#define GOODIX53X5_FDT_DOWN_OPCODE      0x0C
#define GOODIX53X5_FDT_UP_OPCODE        0x0E
/* Manual FDT opcodes: 0x0D (TX on), 0x8D (TX off) */
#define GOODIX53X5_FDT_MANUAL_TX_ON     0x0D
#define GOODIX53X5_FDT_MANUAL_TX_OFF    0x8D

/* Image capture opcode flags */
#define GOODIX53X5_IMG_TX_ENABLE        0x01
#define GOODIX53X5_IMG_TX_DISABLE       0x81
#define GOODIX53X5_IMG_FINGER_FLAG      0x40
#define GOODIX53X5_IMG_HV_VALUE         0x06
#define GOODIX53X5_IMG_HV_DISABLED      0x10

/* Default sensor config (256 bytes) - from reference implementation */
extern const guint8 goodix53x5_default_config[256];

/* Config register tags */
#define GOODIX53X5_CFG_TCODE_TAG        0x005Cu
#define GOODIX53X5_CFG_DAC_L_TAG        0x0220u
#define GOODIX53X5_CFG_DELTA_DOWN_TAG   0x0082u

/* OTP custom CRC-8 table (256 bytes) */
extern const guint8 goodix53x5_otp_hash_table[256];

/*
 * Calibration parameters derived from OTP
 */
typedef struct
{
  guint16 tcode;
  guint16 dac_h;
  guint16 dac_l;

  guint8 delta_fdt;
  guint8 delta_down;
  guint8 delta_up;
  guint16 delta_img;

  guint8 fdt_base_down[GOODIX53X5_FDT_BASE_LEN];
  guint8 fdt_base_up[GOODIX53X5_FDT_BASE_LEN];
  guint8 fdt_base_manual[GOODIX53X5_FDT_BASE_LEN];
} Goodix53x5CalibParams;

/*
 * GTLS (Goodix TLS) session context
 */
typedef struct
{
  guint8  psk[GOODIX53X5_PSK_LEN];
  guint8  client_random[GOODIX53X5_RANDOM_LEN];
  guint8  server_random[GOODIX53X5_RANDOM_LEN];
  guint8  symmetric_key[GOODIX53X5_SYMMETRIC_KEY_LEN];
  guint8  symmetric_iv[GOODIX53X5_SYMMETRIC_IV_LEN];
  guint8  hmac_key[GOODIX53X5_HMAC_KEY_LEN];
  guint16 hmac_client_counter;
  guint16 hmac_server_counter;
  gboolean connected;
} Goodix53x5GTLSCtx;

/* ---- Protocol utility functions ---- */

/**
 * goodix53x5_encode_message:
 * Encode a message into the USB wire format.
 * Returns heap-allocated buffer + sets @out_len.
 * Caller must g_free().
 */
guint8 * goodix53x5_encode_message (guint8         category,
                                    guint8         command,
                                    const guint8  *payload,
                                    gsize          payload_len,
                                    gsize         *out_len);

/**
 * goodix53x5_parse_message:
 * Parse the first complete message from @buf / @buf_len.
 * Returns TRUE on success. Decoded category/command/payload returned in out params.
 * @out_payload is heap-allocated, caller must g_free().
 */
gboolean goodix53x5_parse_message (const guint8  *buf,
                                   gsize          buf_len,
                                   guint8        *out_category,
                                   guint8        *out_command,
                                   guint8       **out_payload,
                                   gsize         *out_payload_len);

/**
 * goodix53x5_compute_otp_hash:
 * Compute the custom CRC-8 over @data of @len bytes.
 */
guint8 goodix53x5_compute_otp_hash (const guint8 *data,
                                    gsize         len);

/**
 * goodix53x5_parse_otp:
 * Parse OTP data and fill @calib_params.
 * Returns TRUE on success.
 */
gboolean goodix53x5_parse_otp (const guint8        *otp,
                                gsize                otp_len,
                                Goodix53x5CalibParams *calib_params);

/**
 * goodix53x5_build_config:
 * Build a patched config blob from DEFAULT_CONFIG + calibration params.
 * @out_config must be 256 bytes.
 */
void goodix53x5_build_config (const Goodix53x5CalibParams *calib_params,
                              guint8                       *out_config);

/**
 * goodix53x5_gtls_derive_session_key:
 * Derive the GTLS session key using HMAC-SHA256 PRF (TLS 1.2-style).
 * @psk: 32-byte pre-shared key
 * @client_random: 32-byte client random
 * @server_random: 32-byte server random
 * @ctx: output context to fill with derived key material
 */
void goodix53x5_gtls_derive_session_key (const guint8      *psk,
                                         const guint8      *client_random,
                                         const guint8      *server_random,
                                         Goodix53x5GTLSCtx *ctx);

/**
 * goodix53x5_gtls_compute_identity:
 * Compute HMAC-SHA256(hmac_key, client_random || server_random).
 * @out_identity must be GOODIX53X5_IDENTITY_LEN bytes.
 */
void goodix53x5_gtls_compute_identity (const guint8 *hmac_key,
                                       const guint8 *client_random,
                                       const guint8 *server_random,
                                       guint8       *out_identity);

/**
 * goodix53x5_gtls_decrypt_image:
 * Decrypt the encrypted image payload using the GTLS context.
 * @ctx: must be in connected state
 * @encrypted_msg: full encrypted message blob from device
 * @encrypted_msg_len: length of @encrypted_msg
 * @out_raw: heap-allocated raw (GEA-decrypted) pixel data, caller must g_free()
 * @out_raw_len: length of @out_raw
 * Returns TRUE on success.
 */
gboolean goodix53x5_gtls_decrypt_image (Goodix53x5GTLSCtx *ctx,
                                        const guint8      *encrypted_msg,
                                        gsize              encrypted_msg_len,
                                        guint8           **out_raw,
                                        gsize             *out_raw_len);

/**
 * goodix53x5_decode_image:
 * Decode 12-bit packed pixel data into an array of guint16 pixel values.
 * @raw: input raw bytes (6 bytes → 4 pixels)
 * @raw_len: length of @raw
 * @out_pixels: heap-allocated array of GOODIX53X5_SENSOR_PIXELS guint16, caller must g_free()
 * Returns TRUE on success.
 */
gboolean goodix53x5_decode_image (const guint8  *raw,
                                  gsize          raw_len,
                                  guint16       **out_pixels);

/**
 * goodix53x5_update_fdt_base:
 * Convert FDT event data into calibrated FDT base values.
 */
void goodix53x5_generate_fdt_base (const guint8 *fdt_data,
                                   guint8       *out_fdt_base);

/**
 * goodix53x5_generate_fdt_up_base:
 * Generate fdt_base_up from touch event fdt_data and calibration params.
 */
void goodix53x5_generate_fdt_up_base (const guint8                *fdt_data,
                                      guint16                      touch_flag,
                                      const Goodix53x5CalibParams *calib_params,
                                      guint8                      *out_fdt_base_up);

/**
 * goodix53x5_is_fdt_base_valid:
 * Check whether two FDT data snapshots are within @max_delta of each other.
 */
gboolean goodix53x5_is_fdt_base_valid (const guint8 *fdt_data_1,
                                       const guint8 *fdt_data_2,
                                       guint8        max_delta);
