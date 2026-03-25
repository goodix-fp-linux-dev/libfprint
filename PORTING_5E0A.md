# Goodix 5e0a Driver -- Technical Porting Document

This document describes the architecture of the existing libfprint goodixtls
driver framework, the specific requirements of the Goodix 5e0a fingerprint
sensor, and exactly what code changes are needed to make it work.

---

## 1. Architecture Overview

### 1.1 File Layout

```
libfprint/drivers/goodixtls/
  goodix_proto.h / .c   -- Wire protocol: pack/unpack, checksum, flag constants
  goodixtls.h / .c      -- OpenSSL TLS server (socketpair + SSL_accept thread)
  goodix.h / .c         -- Core driver: USB I/O, command dispatch, TLS init/handshake, image read
  goodix5xx.h / .c      -- Shared 5xx base class (scan SSM, calibration, frame decode)
  goodix511.h / .c      -- 511-specific: activation SSM, config, crop
  goodix5e0a.h / .c     -- 5e0a-specific: activation SSM, scan SSM, dual-TLS
```

The 5e0a driver does NOT inherit from `FpiDeviceGoodixTls5xx`.  It inherits
directly from `FpiDeviceGoodixTls`, because the 5e0a protocol differs enough
from the 5xx family that the shared scan/calibration SSM does not apply.

### 1.2 FpiSsm Pattern

Every async operation is driven by `FpiSsm` (finite state machine).  Key rules:

- `fpi_ssm_new(dev, run_fn, NUM_STATES)` creates an SSM.
- `fpi_ssm_start(ssm, complete_fn)` begins execution.
- `run_fn(ssm, dev)` is called for each state.  It must either:
  - Call `fpi_ssm_next_state(ssm)` to advance, OR
  - Call `fpi_ssm_mark_failed(ssm, error)` to abort, OR
  - Start an async operation whose callback will do one of the above.
- `fpi_ssm_start_subsm(parent, child)` nests a child SSM; the parent auto-advances when the child completes.
- `complete_fn(ssm, dev, error)` is called when the SSM finishes (success or failure).

### 1.3 Callback Chain for Commands

```
goodix_send_protocol(cmd, ...)
  -> encodes as GoodixProtocol, wraps in GoodixPack with flag 0xa0
  -> sends over USB EP_OUT in 64-byte chunks
  -> stores (cmd, callback, user_data) in priv->

USB read loop (goodix_receive_data):
  -> goodix_receive_data_cb
  -> goodix_receive_pack: decodes GoodixPack, switches on flags:
       0xa0 -> goodix_receive_protocol -> dispatch ACK or reply -> goodix_receive_done
       0xb0 -> goodix_receive_done (TLS handshake data)
       0xb2 -> goodix_receive_done (TLS-encrypted image data)
  -> goodix_receive_done calls priv->callback(dev, data, length, user_data, error)
  -> then resets state (priv->ack, priv->reply, priv->callback = NULL)
```

### 1.4 TLS Architecture (socketpair model)

```
GoodixTlsServer:
  socketpair(AF_UNIX, SOCK_STREAM) -> sock_fd (server), client_fd (client)
  SSL_new() bound to sock_fd
  pthread creates thread that calls SSL_accept() (blocks until handshake completes)

  client_fd side:
    goodix_tls_client_write(server, data, len)  -- write raw bytes IN (from USB/device)
    goodix_tls_client_read(server, buf, len)     -- read raw bytes OUT (to USB/device)

  sock_fd side (via SSL):
    goodix_tls_server_read(server, buf, len)     -- read DECRYPTED data
    (no server_write currently needed)
```

Data flow during TLS handshake:

```
1. goodix_tls_server_init() -> creates socketpair, starts SSL_accept thread
2. SSL_accept writes ServerHello to sock_fd
3. goodix_tls_client_read() reads ServerHello from client_fd
4. Send ServerHello to device via goodix_send_pack(GOODIX_FLAGS_TLS, ...)
5. Device responds with ClientKeyExchange (0xb0 packet from USB)
6. goodix_tls_client_write() feeds it into client_fd -> SSL_accept processes it
7. Repeat for ChangeCipherSpec, Finished
8. SSL_accept returns -> handshake done
9. Send GOODIX_CMD_TLS_SUCCESSFULLY_ESTABLISHED (0xd4) to device
```

Data flow during image read (existing 5xx):

```
1. goodix_send_mcu_get_image (cmd 0x20) -> device ACKs, then returns 0xb0 packet
2. goodix_tls_ready_image_handler receives the 0xb0 payload
3. goodix_tls_client_write(tls_hop, data, length) -> feeds encrypted data to client_fd
4. goodix_tls_server_read(tls_hop, buf, 0xffff) -> reads decrypted data via SSL_read
5. Callback receives decrypted frame bytes
```

---

## 2. What Already Exists for 5e0a

The current codebase already has a PARTIAL 5e0a implementation.  Here is what
is already done and what still needs work.

### 2.1 Already Implemented

**goodix5e0a.c / .h** -- The complete driver skeleton:
- `FpiDeviceGoodixTls5e0a` struct with `image_psk[32]` and `has_image_psk`
- Activation SSM with all stages:
  - `ACTIVATE_READ_AND_NOP1` -- start read loop, send NOP
  - `ACTIVATE_ENABLE_CHIP` -- enable chip
  - `ACTIVATE_NOP2` -- second NOP
  - `ACTIVATE_CHECK_PSK` -- read PSK hash from device (flags 0xbb020001)
  - `ACTIVATE_CMD_TLS` -- command TLS with PSK=zeros via `goodix_tls_init()`
  - `ACTIVATE_POV_IMAGE_CHECK` -- send POV image check (cmd 0xd6)
  - `ACTIVATE_IMG_TLS` -- image TLS with device-specific PSK via `goodix_tls_init_image()`
  - `ACTIVATE_DONE`
- Scan SSM:
  - `SCAN_QUERY_MCU` -- query MCU state
  - `SCAN_FDT_DOWN` -- wait for finger via FDT_DOWN with 5e0a-specific payload
  - `SCAN_GET_IMAGE` -- capture image via `goodix_tls_read_image_5e0a()`
  - `SCAN_DONE` -- report finger off
- Frame decoding: `goodix_5e0a_decode_frame()` (no 8+5 header/footer, just raw 10560 bytes)
- Frame squashing to 8-bit grayscale
- Image creation (80x88, FPI_IMAGE_PARTIAL)
- Proper deactivation with `goodix_shutdown_tls()` + `goodix_shutdown_image_tls()`
- Device constants: VID/PID 27c6:5e0a, interface 0, EP_IN 0x83, EP_OUT 0x01

**goodix.h** -- Declarations already added:
- `goodix_tls_init_image()` -- init image TLS with custom PSK
- `goodix_tls_read_image_5e0a()` -- read image via 0xb2 packets
- `goodix_shutdown_image_tls()`
- `goodix_send_pov_image_check()`

**goodix.c** -- Backend implementations already added:
- `FpiDeviceGoodixTlsPrivate` already has `image_tls_hop` and `image_tls_ready_callback`
- `goodix_receive_pack()` already handles `GOODIX_FLAGS_TLS_DATA` (0xb2)
- `goodix_tls_init_image()` -- creates a second GoodixTlsServer with custom PSK
- Image TLS handshake SSM (mirrors the command TLS handshake)
- `goodix_tls_read_image_5e0a()` / `goodix_tls_ready_image_5e0a_handler()`:
  - Sends MCU_GET_IMAGE
  - Receives 0xb2 response
  - Strips 9-byte Goodix framing
  - Feeds TLS record into `image_tls_hop`
  - Reads decrypted data via `goodix_tls_server_read()`

**goodixtls.c / .h** -- TLS server supports custom PSK:
- `GoodixTlsServer` has `psk_data` and `psk_len` fields
- `goodix_tls_server_init_with_psk()` stores custom PSK
- `tls_server_psk_server_callback()` uses custom PSK when available, else zeros
- SSL ex_data mechanism to pass server pointer to PSK callback

**goodix_proto.h** -- Protocol constants:
- `GOODIX_FLAGS_TLS_DATA` (0xb2) defined
- `GOODIX_CMD_POV_IMAGE_CHECK` (0xd6) defined

**meson.build** -- Build integration:
- `goodixtls5e0a` driver entry pointing to `goodix5e0a.c`

### 2.2 Known Issues and Gaps

The code is structurally complete. The primary remaining issues are:

#### Issue 1: PSK Provisioning

The biggest unsolved problem is obtaining the device-specific image PSK.
Currently `has_image_psk` is FALSE and 32 zero bytes are used as a placeholder.
The real PSK is unique per device and must be:

- Extracted from the Windows driver registry (encrypted via DPAPI), or
- Read from a user-provided config file (e.g., `~/.config/libfprint/goodix-5e0a.psk`), or
- Obtained through some device-side protocol not yet reverse-engineered.

**Action needed:** Implement PSK loading from a config file.  Proposed approach:

```c
// In fpi_device_goodixtls5e0a_init() or at start of activation:
static gboolean
load_image_psk (FpiDeviceGoodixTls5e0a *self)
{
  g_autofree gchar *path = NULL;
  g_autofree gchar *contents = NULL;
  gsize len = 0;
  GError *error = NULL;

  // Try XDG config dir first
  path = g_build_filename (g_get_user_config_dir (),
                           "libfprint", "goodix-5e0a.psk", NULL);
  if (!g_file_get_contents (path, &contents, &len, &error))
    {
      fp_warn ("Could not load PSK from %s: %s", path, error->message);
      g_error_free (error);
      return FALSE;
    }

  if (len == 32)
    {
      // Binary PSK file
      memcpy (self->image_psk, contents, 32);
    }
  else if (len == 64 || len == 65)
    {
      // Hex-encoded PSK (64 hex chars, optional newline)
      for (int i = 0; i < 32; i++)
        sscanf (contents + i * 2, "%2hhx", &self->image_psk[i]);
    }
  else
    {
      fp_warn ("PSK file has unexpected length %zu (expected 32 or 64)", len);
      return FALSE;
    }

  self->has_image_psk = TRUE;
  return TRUE;
}
```

This should be called early in the activation SSM (before `ACTIVATE_IMG_TLS`).

#### Issue 2: 0xb2 Framing Offset

The `goodix_tls_ready_image_5e0a_handler()` strips 9 bytes of Goodix framing
before the TLS record.  This offset (`TLS_FRAMING_BYTES = 9`) was determined
from USB captures but may need adjustment.  If decryption fails with
`SSL_ERROR_SSL` or produces garbage data, this is the first thing to check.

Specifically, the 0xb2 payload structure appears to be:

```
[0]    : some kind of sub-command or sequence byte
[1-2]  : 16-bit length (LE) of the TLS record that follows
[3-8]  : additional framing / padding (6 bytes)
[9...] : actual TLS Application Data record
```

The exact layout must be verified against USB captures.  If the TLS record
starts at a different offset, change `TLS_FRAMING_BYTES` accordingly.

#### Issue 3: Multiple 0xb2 Packets

For large images, the device may split the encrypted data across multiple
0xb2 USB packets.  The current implementation assumes a single 0xb2 packet
delivers the entire TLS record.  If the image is larger than one USB transfer:

**Action needed:** Accumulate 0xb2 data until we have a complete TLS record.

The `goodix_receive_pack()` function already handles packet reassembly for
incomplete pack headers (it accumulates in `priv->data`).  However, it
resets after each complete pack.  For 0xb2, we may need:

```c
// In FpiDeviceGoodixTlsPrivate, add:
guint8  *image_data;
guint32  image_data_len;
guint32  image_data_expected;

// In goodix_receive_pack, for GOODIX_FLAGS_TLS_DATA:
// Parse the expected total length from the first 0xb2 header
// Accumulate until we have all the data, then call goodix_receive_done
```

However, this may not be necessary -- USB bulk transfers can be up to 64KB
and the encrypted image (10560 bytes + TLS overhead ~= 10600 bytes) likely
fits in a single transfer given `GOODIX_EP_IN_MAX_BUF_SIZE = 0x10000` (64KB).

#### Issue 4: Command Sequencing During Image TLS Handshake

The image TLS handshake reuses `goodix_send_request_tls_connection()` (cmd 0xd0),
which is the same command used for the first (command) TLS handshake.  The
device distinguishes between the two because the command TLS session is
already established -- so the second 0xd0 triggers a second TLS session.

The existing implementation handles this correctly:
1. Command TLS handshake uses `priv->tls_hop`
2. Image TLS handshake uses `priv->image_tls_hop`
3. The handshake relay functions use the correct server for each

But there is a subtle issue: `goodix_read_tls()` sets `priv->cmd = 0` to
receive the next 0xb0 packet as a TLS handshake message.  During the image
TLS handshake, these 0xb0 packets need to go to `image_tls_hop`, not
`tls_hop`.  The current code handles this via the callback chain:
`on_goodix_image_tls_read_handshake` writes to `priv->image_tls_hop`.

#### Issue 5: MCU_GET_IMAGE Response Type

For 5xx devices, `goodix_send_mcu_get_image()` expects a protocol reply
(0xa0 packet).  For 5e0a, after the ACK, the device sends a 0xb2 packet
instead.  The current `goodix_send_mcu_get_image()` is configured with
`reply=TRUE`, which means it expects both an ACK (0xa0/0xb0 cmd) and a
reply.  The 0xb2 packet will arrive as a separate pack and be delivered
via `goodix_receive_done()`.

This should work because:
1. MCU_GET_IMAGE is sent with `reply=TRUE`
2. Device sends ACK (0xa0 with cmd=0xb0) -> `goodix_receive_ack` processes it, sets `priv->ack = FALSE`
3. Device sends 0xb2 data -> `goodix_receive_pack` sees `GOODIX_FLAGS_TLS_DATA` -> calls `goodix_receive_done`
4. `goodix_receive_done` calls the stored callback (`goodix_tls_ready_image_5e0a_handler`)

If the device does NOT send an ACK before 0xb2 data, then we need to change
`goodix_send_mcu_get_image` to use `reply=FALSE` for the 5e0a variant, or
add a new `goodix_send_mcu_get_image_5e0a()` function.

---

## 3. Protocol Sequence (5e0a vs 5xx)

### 3.1 Existing 5xx Sequence

```
ACTIVATE:
  start_read_loop -> NOP -> enable_chip -> NOP -> check_fw_ver ->
  check_psk -> reset -> idle -> read_otp -> upload_config ->
  set_powerdown_freq -> goodix_tls_init(PSK=stored) -> DONE

SCAN (triggered by AWAIT_FINGER_ON):
  query_mcu -> fdt_mode -> calibrate(fdt_up + nav0 + get_img) ->
  fdt_down -> get_img(TLS decrypt via tls_hop) ->
  fdt_up -> report_finger_off
```

### 3.2 Required 5e0a Sequence

```
ACTIVATE:
  start_read_loop -> NOP -> enable_chip -> NOP -> check_psk(0xbb020001) ->
  goodix_tls_init(PSK=zeros) ->              [1st TLS: command channel]
  pov_image_check(0xd6) ->
  goodix_tls_init_image(PSK=device_key) ->   [2nd TLS: image channel]
  DONE

SCAN (triggered by AWAIT_FINGER_ON):
  query_mcu -> fdt_down(5e0a_payload) ->
  mcu_get_image -> receive 0xb2 -> strip 9-byte header ->
  decrypt via image_tls_hop -> decode frame (no 8+5) ->
  squash to 8-bit -> create FpImage(80x88) ->
  report_finger_off
```

### 3.3 Key Differences

| Aspect | 5xx (e.g., 511) | 5e0a |
|--------|-----------------|------|
| TLS sessions | 1 (command+image) | 2 (command=zeros, image=device PSK) |
| Image data flag | 0xb0 | 0xb2 |
| Image TLS PSK | Same as command PSK | Device-specific, 32 bytes |
| Frame format | 8-byte header + data + 5-byte footer | Raw data, no header/footer |
| Image size | 64x80 (511) | 80x88 |
| Raw frame size | 10573 (511) | 10560 |
| Calibration | Yes (calibrate sub-SSM) | No |
| FDT sequence | fdt_mode -> fdt_up -> nav0 -> fdt_down | fdt_down only |
| Inheritance | FpiDeviceGoodixTls5xx -> FpiDeviceGoodixTls | FpiDeviceGoodixTls directly |
| POV image check | No | Yes (cmd 0xd6, between TLS sessions) |

---

## 4. Specific Code Changes Needed

### 4.1 PSK Loading (PRIORITY: HIGH)

**File:** `goodix5e0a.c`

Add PSK loading at the start of activation or in `fpi_device_goodixtls5e0a_init()`.

The function should:
1. Try `$XDG_CONFIG_HOME/libfprint/goodix-5e0a.psk` (binary 32 bytes or hex 64 chars)
2. If found, set `self->image_psk` and `self->has_image_psk = TRUE`
3. If not found, warn and continue (will fail at image decrypt time)

This is the ONLY blocking issue preventing the driver from working with real hardware.

### 4.2 Verify 0xb2 Framing (PRIORITY: MEDIUM)

**File:** `goodix.c`, function `goodix_tls_ready_image_5e0a_handler()`

The 9-byte skip (`TLS_FRAMING_BYTES = 9`) needs verification with actual USB
captures.  If decryption fails:

1. Dump the raw 0xb2 payload (add `fp_dbg` with hex dump of first 16 bytes)
2. Look for the TLS record header: byte 0x17 (Application Data), bytes 1-2 = 0x0303 (TLS 1.2), bytes 3-4 = record length
3. Adjust `TLS_FRAMING_BYTES` to the offset where 0x17 0x03 0x03 appears

### 4.3 Error Handling for Missing PSK (PRIORITY: LOW)

**File:** `goodix5e0a.c`, `activate_run_state()` at `ACTIVATE_IMG_TLS`

Currently if `has_image_psk` is FALSE, the code warns but proceeds with zeros.
This will cause SSL_read to return garbage or fail.  Consider:
- Failing the SSM with a clear error message telling the user where to put the PSK file
- Or: attempting the handshake anyway (it will succeed since PSK=zeros is valid for TLS-PSK) but warning that image decryption will produce garbage

### 4.4 Debug Logging (PRIORITY: LOW)

Add hex-dump logging at key points for debugging with real hardware:

```c
// In goodix_tls_ready_image_5e0a_handler, after receiving 0xb2 data:
{
  g_autofree gchar *hex = data_to_str (data, MIN(length, 32));
  fp_dbg ("0xb2 payload (%d bytes): %s...", length, hex);
}

// After TLS decryption:
{
  g_autofree gchar *hex = data_to_str (buff, MIN(read_size, 32));
  fp_dbg ("Decrypted image (%d bytes): %s...", read_size, hex);
}
```

---

## 5. Testing Plan

### 5.1 Without Hardware

1. Verify the code compiles: `meson compile -C builddir`
2. Verify the driver is registered: check `fprintd --list-devices` or
   `libfprint/meson.build` includes `goodixtls5e0a`
3. Review the activation SSM states match the expected protocol sequence

### 5.2 With Hardware

1. Create a PSK file at `~/.config/libfprint/goodix-5e0a.psk` with the correct PSK
2. Run `fprintd-enroll` with `G_MESSAGES_DEBUG=all` to see debug output
3. Check for:
   - "Command TLS established" log message
   - "POV image check OK" log message
   - "Image TLS established" log message
   - "Got TLS data (0xb2) msg" log message
   - "Got decrypted image data: XXXX bytes" log message
4. If decryption fails, check the 0xb2 hex dump for TLS record header offset
5. If the image looks wrong, verify frame dimensions and decode algorithm

### 5.3 PSK Extraction (for testing)

To get the PSK from a Windows installation:
1. In Windows registry: `HKLM\SOFTWARE\Goodix\Fingerprint\...`
2. The PSK is encrypted with DPAPI
3. Use `dpapi-ng` or similar tool to decrypt
4. Save the 32-byte result to the config file

---

## 6. Summary of Current State

The 5e0a driver implementation is **structurally complete**.  All the necessary
plumbing is in place:

- Dual TLS server support (command + image) in goodix.c/goodixtls.c
- 0xb2 packet handling in goodix_receive_pack()
- Image TLS handshake relay (mirrors command TLS handshake)
- 5e0a-specific activation and scan state machines
- Frame decode without 8+5 header/footer
- Proper cleanup of both TLS servers on deactivation

The **one critical missing piece** is PSK provisioning -- loading the
device-specific PSK from a config file so that the image TLS session can
decrypt the fingerprint data.  Everything else is ready for hardware testing.
