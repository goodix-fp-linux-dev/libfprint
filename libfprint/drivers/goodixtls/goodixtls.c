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
#include <unistd.h>
#include <sys/socket.h>

#include "drivers_api.h"
#include "fp-device.h"
#include "fpi-device.h"
#include "glibconfig.h"
#include "goodix.h"
#include "goodixtls.h"

static GError *
err_from_ssl (void)
{
  unsigned long code = ERR_get_error ();
  const char *msg = ERR_reason_error_string (code);

  return g_error_new (g_io_error_quark (), code, "%s",
                      msg ? msg : "Unknown OpenSSL error");
}

static unsigned int
tls_server_psk_server_callback (SSL           *ssl,
                                const char    *identity,
                                unsigned char *psk,
                                unsigned int   max_psk_len)
{
  const int len = 32;

  fp_warn ("TLS diagnostic: PSK callback max=%u identity=%s cipher=%s key_bytes=32",
          max_psk_len,
          identity ? identity : "(null)",
          SSL_get_cipher_name (ssl) ? SSL_get_cipher_name (ssl) : "(unknown)");
  if (len > max_psk_len)
    {
      fp_err ("max psk length (%d) too short (needs %d)", max_psk_len, len);
      return 0;
    }

  // zero out the psk
  for (int n = 0; n != len; ++n)
    psk[n] = 0;

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
  SSL_CTX_set_cipher_list (ctx, "PSK-AES128-CBC-SHA256:ALL:@SECLEVEL=0");
  SSL_CTX_set_min_proto_version (ctx, TLS1_2_VERSION);
  SSL_CTX_set_max_proto_version (ctx, TLS1_2_VERSION);
  SSL_CTX_set_psk_server_callback (ctx, tls_server_psk_server_callback);
}

int
goodix_tls_client_write (GoodixTlsServer *self, guint8 *data, guint16 length)
{
  size_t offset = 0;

  while (offset < length)
    {
      ssize_t written = write (self->client_fd, data + offset, length - offset);

      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0)
        return written < 0 ? -errno : -EPIPE;

      offset += written;
    }

  return offset;
}
static int
read_exact (int fd, guint8 *buf, size_t count, int timeout_ms)
{
  size_t total = 0;
  while (total < count)
    {
      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      int ret = poll (&pfd, 1, timeout_ms);
      if (ret <= 0)
        return -1;
      ssize_t n = read (fd, buf + total, count - total);
      if (n <= 0)
        return -1;
      total += n;
    }
  return total;
}

static int
read_one_record (int fd, guint8 *buf, size_t max_len, int timeout_ms)
{
  if (max_len < 5)
    return -1;
  if (read_exact (fd, buf, 5, timeout_ms) != 5)
    return -1;
  guint16 plen = (buf[3] << 8) | buf[4];
  if (5 + plen > max_len)
    return -1;
  if (read_exact (fd, buf + 5, plen, timeout_ms) != plen)
    return -1;
  return 5 + plen;
}

int
goodix_tls_client_read (GoodixTlsServer *self, guint8 *data, guint16 length)
{
  int total = 0;
  int timeout_ms = 3000;

  int rec_len = read_one_record (self->client_fd, data, length, timeout_ms);
  if (rec_len <= 0)
    return -1;
  total += rec_len;

  /* Check if this is ServerHello (0x16, handshake type 0x02) which requires reading until ServerHelloDone (type 0x0E) */
  if (data[0] == 0x16 && rec_len > 5 && data[5] == 0x02)
    {
      while (total < length)
        {
          /* If the last record was ServerHelloDone (type 0x16, handshake msg 0x0E), flight 1 is complete */
          if (rec_len > 5 && data[total - rec_len] == 0x16 && data[total - rec_len + 5] == 0x0e)
            break;

          rec_len = read_one_record (self->client_fd, data + total, length - total, timeout_ms);
          if (rec_len <= 0)
            break;
          total += rec_len;
        }
    }
  /* Check if this is ChangeCipherSpec (0x14) which is immediately followed by Finished (0x16) */
  else if (data[0] == 0x14)
    {
      rec_len = read_one_record (self->client_fd, data + total, length - total, timeout_ms);
      if (rec_len > 0)
        total += rec_len;
    }

  fp_warn ("TLS diagnostic: OpenSSL response bytes=%d first=%02x %02x %02x %02x %02x",
           total, total > 0 ? data[0] : 0,
           total > 1 ? data[1] : 0,
           total > 2 ? data[2] : 0,
           total > 3 ? data[3] : 0,
           total > 4 ? data[4] : 0);
  return total;
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
  SSL_set_cipher_list (ssl, "PSK-AES128-CBC-SHA256:ALL:@SECLEVEL=0");
}

static int
tls_log_error_stack (const char *message, size_t length, void *user_data)
{
  g_autofree char *line = g_strndup (message, length);

  fp_warn ("TLS OpenSSL error stack: %s", line);
  return 1;
}

static void *
goodix_tls_init_serve (void *me)
{
  GoodixTlsServer *self = me;

  ERR_clear_error ();
  fp_warn ("TLS diagnostic: SSL_accept starting, state=%s",
           SSL_state_string_long (self->ssl_layer));
  int retr = SSL_accept (self->ssl_layer);

  if (retr <= 0)
    {
      int ssl_err = SSL_get_error (self->ssl_layer, retr);
      const SSL_CIPHER *cipher = SSL_get_current_cipher (self->ssl_layer);

      fp_err ("TLS diagnostic: SSL_accept failed ret=%d ssl_err=%d state=%s cipher=%s",
              retr, ssl_err, SSL_state_string_long (self->ssl_layer),
              cipher ? SSL_CIPHER_get_name (cipher) : "(none)");
      ERR_print_errors_cb (tls_log_error_stack, NULL);
    }
  else
    {
      const SSL_CIPHER *cipher = SSL_get_current_cipher (self->ssl_layer);

      fp_warn ("TLS diagnostic: SSL_accept succeeded state=%s cipher=%s",
               SSL_state_string_long (self->ssl_layer),
               cipher ? SSL_CIPHER_get_name (cipher) : "(none)");
    }
  return NULL;
}

gboolean
goodix_tls_server_deinit (GoodixTlsServer *self, GError **error)
{
  if (self->sock_fd >= 0)
    {
      shutdown (self->sock_fd, SHUT_RDWR);
      close (self->sock_fd);
      self->sock_fd = -1;
    }
  if (self->client_fd >= 0)
    {
      shutdown (self->client_fd, SHUT_RDWR);
      close (self->client_fd);
      self->client_fd = -1;
    }

  pthread_join (self->serve_thread, NULL);

  if (self->ssl_layer)
    {
      SSL_shutdown (self->ssl_layer);
      SSL_free (self->ssl_layer);
      self->ssl_layer = NULL;
    }

  if (self->ssl_ctx)
    {
      SSL_CTX_free (self->ssl_ctx);
      self->ssl_ctx = NULL;
    }

  return TRUE;
}

gboolean
goodix_tls_server_init (GoodixTlsServer *self, GError **error)
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
  SSL_set_fd (self->ssl_layer, self->sock_fd);

  pthread_create (&self->serve_thread, 0, goodix_tls_init_serve, self);

  return TRUE;
}
