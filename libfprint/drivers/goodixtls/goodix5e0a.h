/*
 * Goodix driver for USB devices 27c6:5e0a
 *
 * Copyright (C) 2026 The libfprint Goodix 5e0a contributors
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

#define GOODIX_5E0A_INTERFACE (0)
#define GOODIX_5E0A_EP_IN (0x3 | FPI_USB_ENDPOINT_IN)
#define GOODIX_5E0A_EP_OUT (0x1 | FPI_USB_ENDPOINT_OUT)

#define GOODIX_5E0A_FIRMWARE_VERSION ("GFUSB_GM168SEC_APP_10036")
#define GOODIX_5E0A_PSK_FLAGS (0xbb020001)
#define GOODIX_5E0A_RESET_NUMBER (2048)

#define GOODIX_5E0A_WIDTH (64)
#define GOODIX_5E0A_HEIGHT (80)
#define GOODIX_5E0A_SCALED_WIDTH (128)
#define GOODIX_5E0A_SCALED_HEIGHT (160)
#define GOODIX_5E0A_SCAN_WIDTH (64)
#define GOODIX_5E0A_SCAN_HEIGHT (80)
#define GOODIX_5E0A_FRAME_SIZE (GOODIX_5E0A_WIDTH * GOODIX_5E0A_HEIGHT)
#define GOODIX_5E0A_FRAME_BLOCKS (80)
#define GOODIX_5E0A_BLOCK_BYTES (132)
#define GOODIX_5E0A_BLOCK_ACTIVE_BYTES (96)
#define GOODIX_5E0A_ACT_BYTES (GOODIX_5E0A_FRAME_BLOCKS * GOODIX_5E0A_BLOCK_ACTIVE_BYTES) /* 7680 */
#define GOODIX_5E0A_FRAME_WIRE_BYTES (GOODIX_5E0A_FRAME_BLOCKS * GOODIX_5E0A_BLOCK_BYTES + 4) /* 10564 */

#define GOODIX_5E0A_CONTRAST_GAIN (1.0f)
#define GOODIX_5E0A_ENROLL_MIN_MINUTIAE (12)
#define GOODIX_5E0A_FRAMES_PER_TOUCH 3


/* Sensor Analog Front-End (AFE) Gain/Exposure Register Configuration */
#define GOODIX_5E0A_REG_GAIN_EXPOSURE (0x022c)
#define GOODIX_5E0A_REG_GAIN_EXPOSURE_VAL (0x0305)         /* Little-endian 16-bit: \x05\x03 */
#define GOODIX_5E0A_REG_GAIN_EXPOSURE_CALIB_VAL (0x030a)   /* Little-endian 16-bit: \x0a\x03 */
#define GOODIX_5E0A_REG_GAIN_EXPOSURE_RESET_VAL (0x020a)   /* Little-endian 16-bit: \x0a\x02 */


/* Wire tables and PSK live in goodix5e0a.c as file-static consts. */
