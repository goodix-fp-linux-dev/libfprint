// Goodix Tls driver for libfprint

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

#include "drivers_api.h"
#include "goodix.h"

#define FPI_TYPE_DEVICE_GOODIXTLS5XX (fpi_device_goodixtls5xx_get_type ())

G_DECLARE_DERIVABLE_TYPE (FpiDeviceGoodixTls5xx, fpi_device_goodixtls5xx, FPI, DEVICE_GOODIXTLS5XX, FpiDeviceGoodixTls);

/**
 * @brief API for the common parts of communication between the goodixtls 5xx device
 * @details This API is designed to make it easier to write and maintain
 * drivers for the goodixtls 5xx (usb) devices. For an example out goodix511.c.
 *
 * @par The bare minimum needed is to provide get_mcu_cfg and process_frame to
 * FpiDeviceGoodixTls5xxClass and activate to FpImageDeviceClass (activate
 * varies from device to device)
 *
 * @par There are also quite a few helper functions in the goodixtls5xx_*
 * namespace, the check functions expect a state machine as the user data and
 * will advance it or mark it as failed as appropriate
 * @struct FpiDeviceGoodixTls5xx
 *
 */

typedef guint16 GoodixTls5xxPix;


typedef struct
{
  guint16        data_len;
  void (*free_fn)(void *);
  const guint8 * data;
} GoodixTls5xxMcuConfig;

typedef struct
{
  guint16         width;
  guint16         height;
  unsigned char * data;
} GoodixTls5xxImage;

typedef enum {
  GOODIXTLS5XX_FDT_MODE,
  GOODIXTLS5XX_FDT_DOWN,
  GOODIXTLS5XX_FDT_UP,
} GoodixTls5xxFdtKind;

typedef FpImage *(*GoodixTls5xxProcessFrameFn)(guint8 * pix);
typedef GoodixTls5xxMcuConfig (*GoodixTls5xxGetMcuFn)(void);
typedef GoodixTls5xxMcuConfig (*GoodixTls5xxGetFdtFn)(GoodixTls5xxFdtKind kind);
typedef void (*GoodixTls5xxResetStateFn)(FpDevice *);

struct _FpiDeviceGoodixTls5xxClass
{
  FpiDeviceGoodixTlsClass    parent;

  GoodixTls5xxGetMcuFn       get_mcu_cfg; ///< provide the mcu config before fdt commands
  GoodixTls5xxProcessFrameFn process_frame; ///< process a frame after it is decoded (e.g. crop it)
  GoodixTls5xxResetStateFn   reset_state; ///< callback to reset the state, may be NULL

  guint16                    scan_width; ///< width of the raw scanner image
  guint16                    scan_height; ///< height of the raw scanner image

  const char               * firmware_version; ///< only needed if goodixtls5xx_check_firmware_version() is used

  /// only needed if goodixtls5xx_check_preset_psk_read() is used
  int            psk_flags;
  guint16        psk_len;
  const guint8 * psk;

  int            reset_number; ///< only needed if goodixtls5xx_check_reset() is used

  /// optional: per-mode FDT bytes; if NULL get_mcu_cfg is used for every mode.
  /// For FDT_DOWN/FDT_UP the leading 0x0c/0x0e byte is added by goodix.c,
  /// for FDT_MODE the bytes are sent as-is.
  GoodixTls5xxGetFdtFn get_fdt_cfg;

  /// optional: MCU_GET_IMAGE payload (default 01 00)
  const guint8 * get_image_payload;
  guint16        get_image_payload_len;

  /// bytes before / after the packed pixel data in a decrypted frame
  /// (defaults 8 / 5 as on the 5110)
  guint16 frame_header;
  guint16 frame_trailer;

  /// calibrate from the FDT_MODE state only (no FDT_UP + NAV_0 beforehand)
  gboolean skip_nav_calibration;

  /// optional: state machine run after the TLS handshake and before the
  /// activation is reported complete (newer sensors upload their MCU config
  /// only once TLS is up)
  FpiSsmHandlerCallback post_tls_run;
  int                   post_tls_num_states;

  /// don't send QUERY_MCU_STATE (payload 0x55) at the start of every scan
  gboolean skip_query_mcu;

  /// re-arm FDT_UP when its reply values still show a finger (5503 firmware
  /// reports spurious finger-up on light touches)
  gboolean verify_fdt_up;
};

/**
 * @brief Check the reply to a reset command
 *
 * @param dev
 * @param success
 * @param number
 * @param user_data
 * @param error
 */
void goodixtls5xx_check_reset (FpDevice *dev,
                               gboolean  success,
                               guint16   number,
                               gpointer  user_data,
                               GError   *error);

/**
 * @brief Check the reply to a firmware version query matches configured firmware
 * @note Requires firmware_version field to be set
 *
 * @param dev
 * @param firmware
 * @param user_data
 * @param error
 */
void goodixtls5xx_check_firmware_version (FpDevice *dev,
                                          gchar    *firmware,
                                          gpointer  user_data,
                                          GError   *error);


/**
 * @brief Check the reply to a preset psk query matched configured psk
 * @note Requires psk_flags, psk_len, and psk fields to be set
 *
 * @param dev
 * @param success
 * @param flags
 * @param psk
 * @param length
 * @param user_data
 * @param error
 */
void goodixtls5xx_check_preset_psk_read (FpDevice *dev,
                                         gboolean  success,
                                         guint32   flags,
                                         guint8   *psk,
                                         guint16   length,
                                         gpointer  user_data,
                                         GError   *error);

/**
 * @brief Check the reply to an idle command
 *
 * @param dev
 * @param user_data
 * @param err
 */
void goodixtls5xx_check_idle (FpDevice *dev,
                              gpointer  user_data,
                              GError   *err);

/**
 * @brief Check the reply to uploading an mcu config
 *
 * @param dev
 * @param success
 * @param user_data
 * @param error
 */
void goodixtls5xx_check_config_upload (FpDevice *dev,
                                       gboolean  success,
                                       gpointer  user_data,
                                       GError   *error);

/**
 * @brief Check the reply to a powerdown_scan_freq command
 *
 * @param dev
 * @param success
 * @param user_data
 * @param error
 */
void goodixtls5xx_check_powerdown_scan_freq (FpDevice *dev,
                                             gboolean  success,
                                             gpointer  user_data,
                                             GError   *error);

/**
 * @brief General check for GoodixNoneCallback replies
 *
 * @param dev
 * @param user_data state machine to advance
 * @param error
 */
void goodixtls5xx_check_none (FpDevice *dev,
                              gpointer  user_data,
                              GError   *error);

/**
 * @brief General callback for GoodixDefaultCallback replies
 *
 * @param dev
 * @param data
 * @param len
 * @param ssm State machine to advance (userdata)
 * @param err
 */
void goodixtls5xx_check_none_cmd (FpDevice *dev,
                                  guint8   *data,
                                  guint16   len,
                                  gpointer  ssm,
                                  GError   *err);

/**
 * @brief Start a scan
 * @note This is called automatically for you unless you overwrote change_state in FpImageDeviceClass
 *
 * @param dev
 */
void goodixtls5xx_scan_start (FpiDeviceGoodixTls5xx * dev);

/**
 * @brief Decode a goodixtls frame
 * @details Decodes the weird 4/6 byte packing: https://blog.th0m.as/misc/fingerprint-reversing/
 * @note Doesn't decrypt it
 *
 * @param frame
 * @param frame_size
 * @param raw_frame
 */
void goodixtls5xx_decode_frame (GoodixTls5xxPix * frame,
                                guint32           frame_size,
                                const guint8     *raw_frame);

/**
 * @brief Bounds checked goodixtls5xx_decode_frame() with configurable framing
 *
 * @param frame output, room for max_pixels pixels
 * @param max_pixels
 * @param frame_size size of raw_frame in bytes
 * @param raw_frame
 * @param header bytes to skip at the start
 * @param trailer bytes to ignore at the end
 * @return number of decoded pixels
 */
guint32 goodixtls5xx_decode_frame_ex (GoodixTls5xxPix * frame,
                                      guint32           max_pixels,
                                      guint32           frame_size,
                                      const guint8     *raw_frame,
                                      guint16           header,
                                      guint16           trailer);


/**
 * @brief Initalise the TLS for the device
 * @note You probably want to call this directly after device activation
 *
 * @param dev
 */
void goodixtls5xx_init_tls (FpDevice * dev);

/**
 * @brief Save an image to pgm
 * @details Designed for debugging purposes, shouldn't be used in a non-debug code path
 *
 * @param img
 * @param path
 * @return gboolean
 */
gboolean goodixtls5xx_save_image_to_pgm (FpImage    *img,
                                         const char *path);

/**
 * @brief Squashes the 2 byte pixels of a raw frame into the 1 byte pixels used
 * by libfprint.
 * @details Borrowed from the elan driver. We reduce frames to
 * within the max and min.
 *
 * @param frame
 * @param squashed
 */
void goodixtls5xx_squash_frame_linear (GoodixTls5xxPix *frame,
                                       guint8          *squashed,
                                       guint16          frame_size);

/**
 * @brief Cleans up the state after activation. If you replaced the deactivate callback
 * then you will need to call this, otherwise don't worry its done for you
 *
 * @param dev device to cleanup the state for
 */
void goodixtls5xx_cleanup(FpiDeviceGoodixTls5xx* dev);