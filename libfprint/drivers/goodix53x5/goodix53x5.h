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

#pragma once

#include "drivers_api.h"
#include "goodix53x5_proto.h"

G_DECLARE_FINAL_TYPE (FpiDeviceGoodix53x5, fpi_device_goodix53x5,
                      FPI, DEVICE_GOODIX53X5, FpImageDevice)

/*
 * PSK white-box blob (96 bytes).
 * This encodes the all-zero 32-byte PSK in the device's white-box format.
 */
#define GOODIX53X5_PSK_WHITE_BOX \
        "\xec\x35\xae\x3a\xbb\x45\xed\x3f\x12\xc4\x75\x1f\x1e\x5c\x2c\xc0" \
        "\x5b\x3c\x54\x52\xe9\x10\x4d\x9f\x2a\x31\x18\x64\x4f\x37\xa0\x4b" \
        "\x6f\xd6\x6b\x1d\x97\xcf\x80\xf1\x34\x5f\x76\xc8\x4f\x03\xff\x30" \
        "\xbb\x51\xbf\x30\x8f\x2a\x98\x75\xc4\x1e\x65\x92\xcd\x2a\x2f\x9e" \
        "\x60\x80\x9b\x17\xb5\x31\x60\x37\xb6\x9b\xb2\xfa\x5d\x4c\x8a\xc3" \
        "\x1e\xdb\x33\x94\x04\x6e\xc0\x6b\xbd\xac\xc5\x7d\xa6\xa7\x56\xc5"

/* SHA-256 of the all-zero 32-byte PSK */
#define GOODIX53X5_PSK_HASH \
        "\x66\x68\x7a\xad\xf8\x62\xbd\x77\x6c\x8f\xc1\x8b\x8e\x9f\x8e\x20" \
        "\x08\x97\x14\x85\x6e\xe2\x33\xb3\x90\x2a\x59\x1d\x0d\x5f\x29\x25"

/*
 * Init SSM states — run once on img_open:
 *  ping → read firmware version → soft reset → read chip ID →
 *  read OTP → verify/write PSK → GTLS handshake → upload config →
 *  calibration (FDT+images) → sleep → done
 */
typedef enum {
  INIT_PING = 0,
  INIT_PING_RECV,                  /* dead — ACK consumed by send_cb */
  INIT_FW_VERSION,
  INIT_FW_VERSION_RECV,            /* posts recv for real reply */
  INIT_FW_VERSION_PARSE,           /* parses firmware version string */
  INIT_RESET,
  INIT_RESET_RECV,                 /* dead — ACK consumed by send_cb */
  INIT_READ_CHIP_ID,
  INIT_READ_CHIP_ID_RECV,          /* posts recv for real reply */
  INIT_READ_CHIP_ID_PARSE,         /* logs chip ID */
  INIT_READ_OTP,
  INIT_READ_OTP_RECV,              /* posts recv for real reply */
  INIT_READ_OTP_PARSE,             /* parses OTP calibration data */
  INIT_READ_PSK_HASH,
  INIT_READ_PSK_HASH_RECV,         /* posts recv for prod-read reply */
  INIT_READ_PSK_HASH_PARSE,        /* validates hash, sets need_psk_write */
  INIT_WRITE_PSK,                  /* only entered if PSK hash mismatch */
  INIT_WRITE_PSK_RECV,             /* posts recv for prod-write reply */
  INIT_WRITE_PSK_PARSE,            /* validates write result */
  INIT_GTLS_CLIENT_HELLO,
  INIT_GTLS_CLIENT_HELLO_RECV,     /* dead — ACK consumed by send_cb */
  INIT_GTLS_SERVER_IDENTITY_RECV,  /* posts recv for server identity push */
  INIT_GTLS_SERVER_IDENTITY_PARSE, /* derives session key + verifies identity */
  INIT_GTLS_CLIENT_IDENTITY,
  INIT_GTLS_CLIENT_IDENTITY_RECV,  /* dead — ACK consumed by send_cb */
  INIT_GTLS_SERVER_DONE_RECV,      /* posts recv for server-done push */
  INIT_GTLS_SERVER_DONE_PARSE,     /* verifies handshake result */
  INIT_UPLOAD_CONFIG,
  INIT_UPLOAD_CONFIG_RECV,         /* posts recv for config reply */
  INIT_UPLOAD_CONFIG_PARSE,        /* validates config result */
  INIT_CALIB_FDT_DOWN_TX_ON,
  INIT_CALIB_FDT_DOWN_TX_ON_RECV,  /* posts recv */
  INIT_CALIB_FDT_DOWN_TX_ON_PARSE, /* extracts FDT base data */
  INIT_CALIB_IMG_TX_ON,
  INIT_CALIB_IMG_TX_ON_RECV,       /* posts recv */
  INIT_CALIB_IMG_TX_ON_PARSE,      /* discards calibration image */
  INIT_CALIB_FDT_DOWN_TX_OFF,
  INIT_CALIB_FDT_DOWN_TX_OFF_RECV, /* posts recv */
  INIT_CALIB_FDT_DOWN_TX_OFF_PARSE,/* validates FDT bases */
  INIT_CALIB_IMG_TX_OFF,
  INIT_CALIB_IMG_TX_OFF_RECV,      /* posts recv */
  INIT_CALIB_IMG_TX_OFF_PARSE,     /* discards calibration image */
  INIT_CALIB_FDT_DOWN_TX_ON_2,
  INIT_CALIB_FDT_DOWN_TX_ON_2_RECV,  /* posts recv */
  INIT_CALIB_FDT_DOWN_TX_ON_2_PARSE, /* discards second FDT event */
  INIT_SLEEP,
  INIT_SLEEP_RECV,                 /* dead — ACK consumed by send_cb */
  INIT_NUM_STATES,
} FpiDeviceGoodix53x5InitState;

/*
 * Capture SSM states — run for each finger-down / finger-up cycle:
 *  EC on → FDT down → wait FDT event → read image → FDT up →
 *  wait FDT up event → sleep → EC off → report
 */
typedef enum {
  CAPTURE_EC_ON = 0,
  CAPTURE_EC_ON_RECV,           /* posts recv for real EC-on reply */
  CAPTURE_EC_ON_PARSE,          /* verifies EC-on reply */
  CAPTURE_FDT_DOWN,
  CAPTURE_FDT_DOWN_RECV,        /* dead — ACK consumed by send_cb */
  CAPTURE_WAIT_FINGER_DOWN,     /* posts recv for FDT down event */
  CAPTURE_WAIT_FINGER_DOWN_PARSE,/* processes FDT event, reports finger on */
  CAPTURE_MANUAL_FDT,
  CAPTURE_MANUAL_FDT_RECV,      /* posts recv for manual FDT reply */
  CAPTURE_MANUAL_FDT_PARSE,     /* discards manual FDT reply */
  CAPTURE_GET_IMAGE,
  CAPTURE_GET_IMAGE_RECV,       /* posts recv for image reply */
  CAPTURE_GET_IMAGE_PARSE,      /* decrypts + decodes + reports image */
  CAPTURE_FDT_UP,
  CAPTURE_FDT_UP_RECV,          /* dead — ACK consumed by send_cb */
  CAPTURE_WAIT_FINGER_UP,       /* posts recv for FDT up event */
  CAPTURE_WAIT_FINGER_UP_PARSE, /* processes FDT up event, reports finger off */
  CAPTURE_SLEEP,
  CAPTURE_SLEEP_RECV,           /* dead — ACK consumed by send_cb */
  CAPTURE_EC_OFF,
  CAPTURE_EC_OFF_RECV,          /* posts recv for real EC-off reply */
  CAPTURE_EC_OFF_PARSE,         /* verifies EC-off reply */
  CAPTURE_NUM_STATES,
} FpiDeviceGoodix53x5CaptureState;

/*
 * Per-device instance data
 */
struct _FpiDeviceGoodix53x5
{
  FpImageDevice parent;

  /* USB endpoints (discovered during open) */
  guint8 ep_in;
  guint8 ep_out;

  /* Current running SSM */
  FpiSsm *ssm;

  /* Receive buffer — large enough for the biggest message we expect
   * (encrypted image: ~0x5000+ bytes in many chunks) */
  GByteArray *recv_buf;

  /* Calibration data derived from OTP */
  Goodix53x5CalibParams calib;

  /* GTLS context — valid after handshake */
  Goodix53x5GTLSCtx gtls;

  /* Deactivation requested flag */
  gboolean deactivating;

  /* PSK hash mismatch: need to write white-box PSK */
  gboolean need_psk_write;

};
