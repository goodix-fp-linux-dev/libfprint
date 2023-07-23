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

#include <glib.h>

struct _GoodixTlsServer;


/**
 * @brief TLS server context for goodix devices
 * @details This works by creating an openssl TLS server and a socket for the client and server end.
 * Raw data is read and written on the client-side and is intended to be passed directly to and from
 * the sensor (so it acts as the client). The server-side meanwhile provides a way to decrypt the data
 * from the sensor by reading out of the server socket with openssl
 * @struct GoodixTlsServer
 */
typedef struct _GoodixTlsServer
{
  gpointer  user_data;  // Passed to all callbacks

  SSL_CTX  *ssl_ctx;
  SSL      *ssl_layer;

  int       sock_fd;
  int       client_fd;

  pthread_t serve_thread;
} GoodixTlsServer;

/**
 * @brief Initalise the server
 *
 * @param self context to init
 * @param error output error
 * @return gboolean TRUE on success, FALSE otherwise
 */
gboolean goodix_tls_server_init (GoodixTlsServer *self,
                                 GError         **error);

/**
 * @brief Read a message from the server side of the TLS connection.
 *  i.e. decrypt the message last written with goodix_tls_client_write()
 *
 * @param self server context
 * @param data buffer to read the data into
 * @param length length of the data expected, buffer should be at least this size
 * @param error output error
 * @return int bytes read or <=0 in case of error
 */
int goodix_tls_server_read (GoodixTlsServer *self,
                            guint8          *data,
                            guint32          length,
                            GError         **error);

/**
 * @brief Write a message directly to the client end of the TLS connection.
 *  This should be used to dump encrypted data directly from the device
 *  to be decrypted by goodix_tls_server_read()
 *
 * @param self
 * @param data buffer to write
 * @param length length of data to be written
 * @return int bytes written or -1 in case of error
 */
int goodix_tls_client_write (GoodixTlsServer *self,
                             guint8          *data,
                             guint16          length);

/**
 * @brief Read an encrypted response from the client end of the TLS connection.
 *  This is needed for e.g. handshaking
 *
 * @param self
 * @param data buffer to read into
 * @param length length of buffer
 * @return int bytes read or -1 for error or 0 for EOF
 */
int goodix_tls_client_read (GoodixTlsServer *self,
                            guint8          *data,
                            guint16          length);

/**
 * @brief Shutdown the TLS server
 *
 * @param self context to shutdown
 * @param error output error
 * @return gboolean TRUE on success, FALSE otherwise
 */
gboolean goodix_tls_server_deinit (GoodixTlsServer *self,
                                   GError         **error);
