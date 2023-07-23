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

// 1 seconds USB timeout
#define GOODIX_TIMEOUT (1000)

G_DECLARE_DERIVABLE_TYPE (FpiDeviceGoodixTls, fpi_device_goodixtls, FPI,
                          DEVICE_GOODIXTLS, FpImageDevice)

#define FPI_TYPE_DEVICE_GOODIXTLS (fpi_device_goodixtls_get_type ())

struct _FpiDeviceGoodixTlsClass
{
  FpImageDeviceClass parent;

  gint               interface;
  guint8             ep_in;
  guint8             ep_out;
};

typedef struct __attribute__((__packed__)) _GoodixCallbackInfo
{
  GCallback callback;
  gpointer user_data;
} GoodixCallbackInfo;

typedef void (*GoodixCmdCallback)(FpDevice *dev,
                                  guint8   *data,
                                  guint16   length,
                                  gpointer  user_data,
                                  GError   *error);

typedef void (*GoodixFirmwareVersionCallback)(FpDevice *dev,
                                              gchar    *firmware,
                                              gpointer  user_data,
                                              GError   *error);

typedef void (*GoodixPresetPskReadCallback)(FpDevice *dev,
                                            gboolean  success,
                                            guint32   flags,
                                            guint8   *psk,
                                            guint16   length,
                                            gpointer  user_data,
                                            GError   *error);

typedef void (*GoodixSuccessCallback)(FpDevice *dev,
                                      gboolean  success,
                                      gpointer  user_data,
                                      GError   *error);

typedef void (*GoodixResetCallback)(FpDevice *dev,
                                    gboolean  success,
                                    guint16   number,
                                    gpointer  user_data,
                                    GError   *error);

typedef void (*GoodixNoneCallback)(FpDevice *dev,
                                   gpointer  user_data,
                                   GError   *error);

typedef void (*GoodixDefaultCallback)(FpDevice *dev,
                                      guint8   *data,
                                      guint16   length,
                                      gpointer  user_data,
                                      GError   *error);
typedef GoodixDefaultCallback GoodixTlsCallback;

typedef void (*GoodixImageCallback)(FpDevice *dev,
                                    guint8   *data,
                                    guint16   length,
                                    gpointer  user_data,
                                    GError   *error);

gchar *data_to_str (guint8 *data,
                    guint32 length);

// ---- GOODIX RECEIVE SECTION START ----

/**
 * @defgroup goodixrecv Goodix receive functions
 * These functions are callbacks for receiving data from the device
 * and should not be called from driver code directly
 * @{
 *
 */
void goodix_receive_done (FpDevice *dev,
                          guint8   *data,
                          guint16   length,
                          GError   *error);

void goodix_receive_success (FpDevice *dev,
                             guint8   *data,
                             guint16   length,
                             gpointer  user_data,
                             GError   *error);

void goodix_receive_reset (FpDevice *dev,
                           guint8   *data,
                           guint16   length,
                           gpointer  user_data,
                           GError   *error);

void goodix_receive_none (FpDevice *dev,
                          guint8   *data,
                          guint16   length,
                          gpointer  user_data,
                          GError   *error);

void goodix_receive_default (FpDevice *dev,
                             guint8   *data,
                             guint16   length,
                             gpointer  user_data,
                             GError   *error);

void goodix_receive_preset_psk_read (FpDevice *dev,
                                     guint8   *data,
                                     guint16   length,
                                     gpointer  user_data,
                                     GError   *error);

void goodix_receive_preset_psk_write (FpDevice *dev,
                                      guint8   *data,
                                      guint16   length,
                                      gpointer  user_data,
                                      GError   *error);

void goodix_receive_ack (FpDevice *dev,
                         guint8   *data,
                         guint16   length,
                         gpointer  user_data,
                         GError   *error);

void goodix_receive_firmware_version (FpDevice *dev,
                                      guint8   *data,
                                      guint16   length,
                                      gpointer  user_data,
                                      GError   *error);

void goodix_receive_protocol (FpDevice *dev,
                              guint8   *data,
                              guint32   length);

void goodix_receive_pack (FpDevice *dev,
                          guint8   *data,
                          guint32   length);

void goodix_receive_data_cb (FpiUsbTransfer *transfer,
                             FpDevice       *dev,
                             gpointer        user_data,
                             GError         *error);

void goodix_receive_timeout_cb (FpDevice *dev,
                                gpointer  user_data);

void goodix_receive_data (FpDevice *dev);

/** @} */

/**
 * @brief Start the read loop that lets us get data from the device asynchronously. Should be called upon device activation
 *
 * @param dev
 */
void goodix_start_read_loop (FpDevice *dev);
// ---- GOODIX RECEIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- GOODIX SEND SECTION START ----

/**
 * @brief Send raw data to the device over USB
 * @note You should never need to call this directly from your driver!
 *
 * @param dev
 * @param data data to be sent
 * @param length length of the data
 * @param free_func free function for the data or NULL
 * @param error error output
 * @return gboolean TRUE if successful, FALSE otherwise
 */
gboolean goodix_send_data (FpDevice      *dev,
                           guint8        *data,
                           guint32        length,
                           GDestroyNotify free_func,
                           GError       **error);

/**
 * @brief Send a single packet to the device
 * @note You should never need to call this directly from your driver!
 *
 * @param dev
 * @param flags
 * @param payload
 * @param length
 * @param free_func
 * @param error
 * @return gboolean
 */
gboolean goodix_send_pack (FpDevice      *dev,
                           guint8         flags,
                           guint8        *payload,
                           guint16        length,
                           GDestroyNotify free_func,
                           GError       **error);

/**
 * @brief Low level function to send a protocol message to the device
 * @note You should never need to call this directly from your driver!
 *
 * @param dev
 * @param cmd command identifier
 * @param payload command payload
 * @param length length of payload
 * @param free_func function to free the payload or NULL
 * @param calc_checksum calculate and add the checksum to the data?
 * @param timeout_ms timeout for recieving data back or 0 for none
 * @param reply do we expect a reply from the device?
 * @param callback
 * @param user_data
 */
void goodix_send_protocol (FpDevice         *dev,
                           guint8            cmd,
                           const guint8     *payload,
                           guint16           length,
                           GDestroyNotify    free_func,
                           gboolean          calc_checksum,
                           guint             timeout_ms,
                           gboolean          reply,
                           GoodixCmdCallback callback,
                           gpointer          user_data);

/**
 * @brief Send nop to the device
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_send_nop (FpDevice          *dev,
                      GoodixNoneCallback callback,
                      gpointer           user_data);

/**
 * @brief Tell the device we want an image from it. The response will be TLS encrypted so you probably don't
 * want to call this from your driver, checkout goodix_tls_read_image() if you want an image from the device
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_send_mcu_get_image (FpDevice           *dev,
                                GoodixImageCallback callback,
                                gpointer            user_data);

/**
 * @brief Tell the device we want to wait for the user to present their finger
 *
 * @param dev
 * @param mode magic bytes
 * @param free_func function to free the mode bytes or NULL
 * @param callback called once the user has presented their finger or with an error. Note may be called at once if the mode bytes are wrong
 * @param user_data
 */
void goodix_send_mcu_switch_to_fdt_down (FpDevice             *dev,
                                         const guint8         *mode,
                                         guint16               length,
                                         GDestroyNotify        free_func,
                                         GoodixDefaultCallback callback,
                                         gpointer              user_data);


/**
 * @brief Tell the device we want to wait for the user to lift their finger off
 *
 * @param dev
 * @param mode magic bytes
 * @param free_func function to free the mode bytes or NULL
 * @param callback called once the user has presented their finger or with an error. Note may be called at once if the mode bytes are wrong
 * @param user_data
 */
void goodix_send_mcu_switch_to_fdt_up (FpDevice             *dev,
                                       const guint8        * mode,
                                       guint16               length,
                                       GDestroyNotify        free_func,
                                       GoodixDefaultCallback callback,
                                       gpointer              user_data);

/**
 * @brief Prep the device for fdt down and fdt up commands
 *
 * @param dev
 * @param mode magic bytes
 * @param free_func function to free mode bytes or NULL
 * @param callback
 * @param user_data
 */
void goodix_send_mcu_switch_to_fdt_mode (FpDevice             *dev,
                                         const guint8        * mode,
                                         guint16               length,
                                         GDestroyNotify        free_func,
                                         GoodixDefaultCallback callback,
                                         gpointer              user_data);

void goodix_send_nav_0 (FpDevice             *dev,
                        GoodixDefaultCallback callback,
                        gpointer              user_data);

void goodix_send_mcu_switch_to_idle_mode (FpDevice          *dev,
                                          guint8             sleep_time,
                                          GoodixNoneCallback callback,
                                          gpointer           user_data);

void goodix_send_write_sensor_register (FpDevice          *dev,
                                        guint16            address,
                                        guint16            value,
                                        GoodixNoneCallback callback,
                                        gpointer           user_data);

void goodix_send_read_sensor_register (FpDevice             *dev,
                                       guint16               address,
                                       guint8                length,
                                       GoodixDefaultCallback callback,
                                       gpointer              user_data);

/**
 * @brief Upload an MCU config to the device. Config may vary by device
 *
 * @param dev
 * @param config config buffer
 * @param length length of buffer
 * @param free_func free function or NULL
 * @param callback
 * @param user_data
 */
void goodix_send_upload_config_mcu (FpDevice             *dev,
                                    guint8               *config,
                                    guint16               length,
                                    GDestroyNotify        free_func,
                                    GoodixSuccessCallback callback,
                                    gpointer              user_data);

void goodix_send_set_powerdown_scan_frequency (FpDevice             *dev,
                                               guint16               powerdown_scan_frequency,
                                               GoodixSuccessCallback callback,
                                               gpointer              user_data);

/**
 * @brief Turn the chip on
 *
 * @param dev
 * @param enable
 * @param callback
 * @param user_data
 */
void goodix_send_enable_chip (FpDevice          *dev,
                              gboolean           enable,
                              GoodixNoneCallback callback,
                              gpointer           user_data);

/**
 * @brief Send a reset command to the device
 *
 * @param dev
 * @param reset_sensor
 * @param sleep_time
 * @param callback
 * @param user_data
 */
void goodix_send_reset (FpDevice           *dev,
                        gboolean            reset_sensor,
                        guint8              sleep_time,
                        GoodixResetCallback callback,
                        gpointer            user_data);

/**
 * @brief Ask the device what firmware version it is running. Response is null-terminated string
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_send_query_firmware_version (FpDevice                     *dev,
                                         GoodixFirmwareVersionCallback callback,
                                         gpointer                      user_data);

/**
 * @brief Ask the device what the current mcu state is
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_send_query_mcu_state (FpDevice             *dev,
                                  GoodixDefaultCallback callback,
                                  gpointer              user_data);

/**
 * @brief Tell the device we want to start talking TLS
 * @note You probably don't need to call this from your driver directly, checkout the goodix_tls_* functions
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_send_request_tls_connection (FpDevice             *dev,
                                         GoodixDefaultCallback callback,
                                         gpointer              user_data);

/**
 * @brief Tell the device that we have successfully established TLS communication with it
 * @note You probably don't need to call this from your driver directly, checkout the goodix_tls_* functions
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_send_tls_successfully_established (FpDevice          *dev,
                                               GoodixNoneCallback callback,
                                               gpointer           user_data);

/**
 * @brief Set the device preset psk. May not work for all device firmware versions
 *
 * @param dev
 * @param flags
 * @param psk
 * @param length
 * @param free_func
 * @param callback
 * @param user_data
 */
void goodix_send_preset_psk_write (FpDevice             *dev,
                                   guint32               flags,
                                   guint8               *psk,
                                   guint16               length,
                                   GDestroyNotify        free_func,
                                   GoodixSuccessCallback callback,
                                   gpointer              user_data);

/**
 * @brief Ask the device what preset psk it has
 *
 * @param dev
 * @param flags flags for the command, possibly device specific?
 * @param length
 * @param callback
 * @param user_data
 */
void goodix_send_preset_psk_read (FpDevice                   *dev,
                                  guint32                     flags,
                                  guint16                     length,
                                  GoodixPresetPskReadCallback callback,
                                  gpointer                    user_data);
/**
 * @brief Request the OTP (One Time Password) from the device
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_send_read_otp (FpDevice             *dev,
                           GoodixDefaultCallback callback,
                           gpointer              user_data);

// ---- GOODIX SEND SECTION END ----

// -----------------------------------------------------------------------------

// ---- DEV SECTION START ----

/**
 * @brief Claim the resources used for communcation with the device
 *
 * @param dev
 * @param error
 * @return gboolean
 */
gboolean goodix_dev_init (FpDevice *dev,
                          GError  **error);

/**
 * @brief Cleanup the resources used to communicate with the device
 *
 * @param dev
 * @param error
 * @return gboolean
 */
gboolean goodix_dev_deinit (FpDevice *dev,
                            GError  **error);

/**
 * @brief Reset the internal state of the communication with the device. Use this e.g. on device deactivation (where it may be reactivated again in future)
 *
 * @param dev
 */
void goodix_reset_state (FpDevice *dev);

// ---- DEV SECTION END ----

// -----------------------------------------------------------------------------

// ---- TLS SECTION START ----

/**
 * @brief Read a TLS packet from the device
 * @note You probably won't ever need to call this directly from your driver
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_read_tls (FpDevice         *dev,
                      GoodixTlsCallback callback,
                      gpointer          user_data);

/**
 * @brief Initialise TLS with the device. Performs handshaking and such for you
 *
 * @param dev
 * @param callback
 * @param user_data
 */
void goodix_tls_init (FpDevice          *dev,
                      GoodixNoneCallback callback,
                      gpointer           user_data);

/**
 * @brief Shutdown TLS communication with the device
 *
 * @param dev
 * @param error
 * @return gboolean TRUE if ok, FALSE otherwise
 */
gboolean goodix_shutdown_tls (FpDevice *dev,
                              GError  **error);

/**
 * @brief Read a TLS encrypted image from the device and decrypt it
 *
 * @param dev
 * @param callback Called when the image is decrypted
 * @param user_data
 */
void goodix_tls_read_image (FpDevice           *dev,
                            GoodixImageCallback callback,
                            gpointer            user_data);

// ---- TLS SECTION END ----
