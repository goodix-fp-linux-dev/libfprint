// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Natasha England-Elbro <natasha@natashaee.me>

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

#include <arpa/inet.h>
#include <errno.h>
#include <glib.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/tls1.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>

#include "drivers_api.h"
#include "fp-device.h"
#include "fpi-device.h"
#include "glibconfig.h"
#include "goodix.h"
#include "goodixtls.h"

// Index for storing GoodixTlsServer pointer in SSL ex_data
static int goodix_tls_server_ex_data_idx = -1;

static GError *
err_from_ssl (void)
{
  GError *err = malloc (sizeof (GError));
  unsigned long code = ERR_get_error ();

  err->code = code;
  const char *msg = ERR_reason_error_string (code);

  err->message = malloc (strlen (msg));
  strcpy (err->message, msg);
  return err;
}

static unsigned int
tls_server_psk_server_callback (SSL           *ssl,
                                const char    *identity,
                                unsigned char *psk,
                                unsigned int   max_psk_len)
{
  GoodixTlsServer *server = NULL;

  if (goodix_tls_server_ex_data_idx >= 0)
    server = SSL_get_ex_data (ssl, goodix_tls_server_ex_data_idx);

  guint len = 32;
  const guint8 *psk_source = NULL;

  if (server && server->psk_data && server->psk_len > 0)
    {
      psk_source = server->psk_data;
      len = server->psk_len;
    }

  fp_dbg ("PSK callback: identity='%s', max_psk_len=%d, server=%p, "
          "psk_data=%p, psk_len=%u, using %s PSK, len=%u",
          identity ? identity : "(null)", max_psk_len,
          (void *)server,
          server ? (void *)server->psk_data : NULL,
          server ? server->psk_len : 0,
          psk_source ? "custom" : "zero", len);

  if (len > max_psk_len)
    {
      fp_err ("max psk length (%d) too short (needs %d)", max_psk_len, len);
      return 0;
    }

  if (psk_source)
    memcpy (psk, psk_source, len);
  else
    memset (psk, 0, len);

  // Log first 8 bytes of PSK being returned
  fp_dbg ("PSK returned (%u bytes): %02x %02x %02x %02x %02x %02x %02x %02x ...",
          len, psk[0], psk[1], psk[2], psk[3], psk[4], psk[5], psk[6], psk[7]);

  return len;
}

static SSL_CTX *
tls_server_create_ctx (void)
{
  const SSL_METHOD *method;

  method = TLS_server_method ();

  SSL_CTX *ctx = SSL_CTX_new (method);

  if (!ctx)
    return NULL;

  return ctx;
}

static void
tls_server_config_ctx (SSL_CTX *ctx)
{
  SSL_CTX_set_ecdh_auto (ctx, 1);
  SSL_CTX_set_dh_auto (ctx, 1);
  SSL_CTX_set_cipher_list (ctx, "PSK-AES128-CBC-SHA256:ALL");
  SSL_CTX_set_min_proto_version (ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (ctx, TLS1_2_VERSION);
  SSL_CTX_set_psk_server_callback (ctx, tls_server_psk_server_callback);
}

int
goodix_tls_client_write (GoodixTlsServer *self, guint8 *data, guint16 length)
{
  return write (self->client_fd, data, length * sizeof (guint8));
}
int
goodix_tls_client_read (GoodixTlsServer *self, guint8 *data, guint16 length)
{
  return read (self->client_fd, data, length * sizeof (guint8));
}

int
goodix_tls_server_read (GoodixTlsServer *self, guint8 *data,
                        guint32 length, GError **error)
{
  int retr = SSL_read (self->ssl_layer, data, length * sizeof (guint8));

  if (retr <= 0)
    *error = err_from_ssl ();
  return retr;
}

static void
tls_config_ssl (SSL *ssl)
{
  SSL_set_min_proto_version (ssl, TLS1_2_VERSION);
  SSL_set_max_proto_version (ssl, TLS1_2_VERSION);
  SSL_set_psk_server_callback (ssl, tls_server_psk_server_callback);
  SSL_set_cipher_list (ssl, "PSK-AES128-CBC-SHA256:ALL");
}

static void *
goodix_tls_init_serve (void *me)
{
  GoodixTlsServer *self = me;

  fp_dbg ("TLS server waiting to accept...");
  int retr = SSL_accept (self->ssl_layer);

  fp_dbg ("TLS server accept done");
  if (retr <= 0)
    fp_warn ("server accept failed (may be expected for command TLS): %s",
             ERR_reason_error_string (ERR_get_error ()));
  else
    fp_dbg ("TLS connection ready");
  return NULL;
}

gboolean
goodix_tls_server_deinit (GoodixTlsServer *self, GError **error)
{
  SSL_shutdown (self->ssl_layer);
  SSL_free (self->ssl_layer);

  close (self->client_fd);
  close (self->sock_fd);

  SSL_CTX_free (self->ssl_ctx);
  pthread_join (self->serve_thread, NULL);

  g_clear_pointer (&self->psk_data, g_free);
  self->psk_len = 0;

  return TRUE;
}

static gboolean
goodix_tls_server_init_internal (GoodixTlsServer *self, GError **error)
{
  SSL_load_error_strings ();
  OpenSSL_add_ssl_algorithms ();
  SSL_library_init ();
  self->ssl_ctx = tls_server_create_ctx ();
  tls_server_config_ctx (self->ssl_ctx);

  int socks[2] = {0, 0};
  if (socketpair (AF_UNIX, SOCK_STREAM, 0, socks) != 0)
    {
      g_set_error (error, G_FILE_ERROR, errno,
                   "failed to create socket pair: %s", strerror (errno));
      return FALSE;
    }
  self->sock_fd = socks[0];
  self->client_fd = socks[1];

  if (self->ssl_ctx == NULL)
    {
      fp_dbg ("Unable to create TLS server context\n");
      *error = fpi_device_error_new_msg (FP_DEVICE_ERROR_GENERAL, "Unable to "
                                                                  "create TLS "
                                                                  "server "
                                                                  "context");
      return FALSE;
    }
  self->ssl_layer = SSL_new (self->ssl_ctx);
  tls_config_ssl (self->ssl_layer);

  // Register the ex_data index once
  if (goodix_tls_server_ex_data_idx < 0)
    goodix_tls_server_ex_data_idx = SSL_get_ex_new_index (0, NULL, NULL, NULL, NULL);

  // Store the server struct pointer so the PSK callback can find it
  SSL_set_ex_data (self->ssl_layer, goodix_tls_server_ex_data_idx, self);

  SSL_set_fd (self->ssl_layer, self->sock_fd);

  pthread_create (&self->serve_thread, 0, goodix_tls_init_serve, self);

  return TRUE;
}

gboolean
goodix_tls_server_init (GoodixTlsServer *self, GError **error)
{
  self->psk_data = NULL;
  self->psk_len = 0;
  return goodix_tls_server_init_internal (self, error);
}

gboolean
goodix_tls_server_init_with_psk (GoodixTlsServer *self,
                                  const guint8    *psk,
                                  guint            psk_len,
                                  GError         **error)
{
  self->psk_data = g_memdup (psk, psk_len);
  self->psk_len = psk_len;
  return goodix_tls_server_init_internal (self, error);
}
