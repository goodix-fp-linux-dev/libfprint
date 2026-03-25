// Goodix Tls driver for libfprint — 5e0a device header

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Natasha England-Elbro <ashenglandelbro@protonmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#pragma once

#include "fpi-usb-transfer.h"

#define GOODIX_5E0A_INTERFACE (0)
#define GOODIX_5E0A_EP_IN (0x3 | FPI_USB_ENDPOINT_IN)
#define GOODIX_5E0A_EP_OUT (0x1 | FPI_USB_ENDPOINT_OUT)
#define GOODIX_5E0A_FIRMWARE_VERSION ("GFUSB_GM168SEC_APP_10034")
#define GOODIX_5E0A_WIDTH 80
#define GOODIX_5E0A_HEIGHT 88
#define GOODIX_5E0A_SCAN_WIDTH 80
#define GOODIX_5E0A_FRAME_SIZE (GOODIX_5E0A_WIDTH * GOODIX_5E0A_HEIGHT)
// 80*88/4*6 = 10560 bytes, no 8+5 header/footer for this device
#define GOODIX_5E0A_RAW_FRAME_SIZE (10560)

#define GOODIX_5E0A_PSK_FLAGS (0xbb020001)

// FDT_DOWN payload for 5e0a (first byte 0x1c = 0x0c + 0x10 device flag)
static const guint8 goodix_5e0a_fdt_down_mode[] = {
  0x1c, 0x01,
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00,
  0x80, 0xb1, 0x80, 0xc7, 0x80, 0xa8, 0x80, 0xbe,
  0x80, 0xb0, 0x80, 0xc1,
  0x00, 0x00, 0x00, 0x00,
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00,
  0x00
};

// FDT_UP payload for 5e0a (first byte 0x0e confirmed from RE of Wbdi.dll)
// Same calibration data as FDT_DOWN but with UP mode prefix
static const guint8 goodix_5e0a_fdt_up_mode[] = {
  0x0e, 0x01,
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00,
  0x80, 0xb1, 0x80, 0xc7, 0x80, 0xa8, 0x80, 0xbe,
  0x80, 0xb0, 0x80, 0xc1,
  0x00, 0x00, 0x00, 0x00,
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00,
  0x00
};

// MCU_GET_IMAGE payload for 5e0a (includes capture configuration)
static const guint8 goodix_5e0a_get_image_payload[] = {
  0x01, 0x00,
  0xa6, 0x00, 0xa7, 0x00,
  0xa6, 0x00, 0xa7, 0x00,
};

static const FpIdEntry goodix_5e0a_id_table[] = {
  {.vid = 0x27c6, .pid = 0x5e0a},
  {.vid = 0, .pid = 0, .driver_data = 0},
};
