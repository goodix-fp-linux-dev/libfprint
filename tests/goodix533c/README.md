# goodix533c umockdev test fixture

Real USB traffic captured from a physical `27c6:533c` sensor via
`usbmon`/`tshark`, for `umockdev-run -p` replay -- same mechanism used by
`tests/goodixmoc/`, `tests/fpcmoc/`, `tests/elanmoc/`, etc. in this tree.

This is a copy of the fixture originally captured and vetted in the parent
project at `tests/goodix533c/` (outside this submodule); see that
directory's own README.md for the full capture provenance notes. The files
here are byte-identical copies, renamed to match this tree's convention
(`custom.pcapng`/`custom.py`, per `tests/umockdev-test.py`) rather than
moved -- the original is left in place.

- `device` -- `umockdev-record`'s sysfs/udev description of the real
  device (vendor/product IDs, descriptors, interfaces, endpoints).
- `custom.pcapng` -- one full session: `nop -> reset -> read chip ID ->
  read OTP -> TLS-PSK handshake -> upload_config_mcu -> FDT baseline ->
  one mcu_get_image capture (reference frame, gain 0xc2)`.
- `custom.py` -- driven by `tests/umockdev-test.py` (invoked via `meson
  test`), exercises device discovery and feature-flag assertions against
  the replayed session. It deliberately stops there and does not call
  `open_sync()` -- see "Replay status" below for why.

## Deliberately finger-absent

This fixture stops after the no-finger reference-frame capture and never
calls `wait_for_finger()`/captures a live frame. The PSK for this whole
device family is public (all-zero), so anyone with the pcapng can decrypt
every `mcu_get_image` payload in it. A live capture would be a real,
recoverable fingerprint image committed to a public repo -- so it was
deliberately not what got recorded.

**No new capture may ever be added here that contains a finger-present
`mcu_get_image` reply, for any reason** -- not to test finger-detect-wait,
not to test live-capture/flat-field, and not to test SIGFM
enroll/verify/identify (see below -- those vfuncs are wired up in the
driver now, but nothing in this fixture can safely exercise them). Any
such fixture must be captured and vetted by a human outside of an
automated agent, exactly as this one was.

## Replay status (important -- read before trusting this fixture)

### The original zero-payload bug: root cause found, and fixed

The first cut of this fixture (still the version described in stale form
below until this section was rewritten) could not replay past the
driver's second open() command (`0xa8`, `GOODIX_CMD_FIRMWARE_VERSION`):
every bulk-IN (`0x83`) completion in the capture had `usb.data_len == 0`
despite `usb.urb_len` correctly reporting the real transfer size --
metadata preserved, payload always redacted. Tool choice was
conclusively ruled out first: `tools/recapture_fixture_dumpcap.sh` in the
parent project captures via `dumpcap` directly (bypassing tshark's
wrapper) and reproduces the *identical* symptom, including on the
14,338-byte real image-transfer frame.

**Root cause: Linux kernel lockdown mode (`confidentiality`), which
redacts USB payload capture system-wide, including for root.** Confirmed
directly on the host that produced every earlier attempt:

- `cat /sys/kernel/security/lockdown` reports `none [integrity]
  confidentiality` -- confidentiality mode active.
- The usbmon **text** interface (`/sys/kernel/debug/usb/usbmon/<N>u`)
  returns `Operation not permitted` (EPERM) even as root -- the kernel's
  `LOCKDOWN_USB` restriction blocking a debugfs interface outright, not a
  DAC permission issue (root bypasses DAC; it cannot bypass a lockdown
  LSM check).
- The usbmon **binary** interface (what both `tshark` and `dumpcap` use)
  stays readable, but has its captured-data length forced to 0 on every
  bulk-IN completion for this device, while `urb_len` (the real transfer
  size) stays correct -- exactly the "metadata preserved, payload
  redacted" shape `LOCKDOWN_USB` produces, and exactly what both tool
  choices independently reproduced.

This is intentional kernel behavior (typically auto-enabled by Secure
Boot), not a bug in the driver, the test harness, or any capture tool --
and not something to work around by changing lockdown/Secure Boot
settings on a real machine.

**Fix: capture from inside a VM whose guest kernel has no lockdown
enabled.** The physical sensor was passed through via QEMU
(`-device usb-host,vendorid=0x27c6,productid=0x533c`) to the existing
`vm/` Ubuntu 20.04 cloud image (already used earlier in this project for
a different capture, see `findings/vm-capture-analysis.md`), running the
same finger-absent `capture_fixture_session.py` inside the guest while
`tshark` captured on the guest's own `usbmonN`. Verified byte-exact:
every one of the 26 bulk-IN completions in the resulting capture has
`usb.data_len == usb.urb_len`, summing to 14,835/14,835 bytes across the
whole session, including the full 14,338-byte encrypted image-capture
frame. The reusable capture script is
`vm/usbmon-capture-in-vm.sh` in the parent project (plus a small
`vm/patch_future_annotations.py` helper, needed because the VM's stock
Python 3.8 predates the PEP 604/585 type-hint syntax the vendored
`goodix-fp-dump-nikicat` driver uses) -- read its header comment before
re-running it, since a future finger-present capture (see "Deliberately
finger-absent" above) will need the same mechanism.

### Second bug found and fixed: bus/device-address mismatch

Replaying the payload-complete capture still hit the same-looking
`umockdev-pcap.vala:158: Replay may be stuck: Reaping discard URB of type
BULK, for endpoint 0x01 with length 64 without corresponding submit`
message. Ruled out first (via `G_MESSAGES_DEBUG=all umockdev-run` plus
`tools/decode_capture.py`/`tools/parse_capture.py`-based frame-by-frame
comparison against the old capture): write ordering, `nop`'s
cancelled-read pattern, root-hub traffic interleaving, `urb_id` reuse or
collision (usbmon IDs are raw kernel pointers and get reused constantly
in both captures -- confirmed harmless in both), and reply payload
content itself (redacting every captured byte back to the old capture's
all-zero shape, while keeping the same frame count/structure, still hung
identically).

**Actual cause**: the VM capture recorded the sensor at `bus=1,
device=2` (the VM's own USB topology), but `device` in this fixture still
declares `busnum=3, devnum=6` (the *original* host capture's numbers,
untouched since this fixture's very first version). umockdev's pcap
replay apparently needs the trace's own recorded bus/device address to
match what the mocked `device` file declares, or its submit/complete
matching desyncs -- silently, with no error naming the actual mismatch.
Relabeling every packet's `busnum`/`devnum` fields in the capture (a
mechanical, structure-preserving rewrite -- see the note below) to 3/6
fixed this completely: replay now proceeds correctly through the
*entire* non-TLS open() sequence -- `nop`, `firmware_version`,
`preset_psk_read`, `reset`, `read_sensor_register`, `read_otp`, and
`request_tls_connection` all replay and decode exactly as captured.

### Third, structural limitation: TLS handshake replay is not fixable this way

With the bus/device fix in place, replay gets all the way to the TLS
handshake before failing (`TLS handshake failed: transfer timed out`,
plus one more "stuck" message). Traced directly through
`libfprint/drivers/goodix533c/goodix533c.c`: `on_request_tls_connection_reply`
takes the device's (replayed, real) ClientHello and feeds it into the
driver's own embedded TLS server (`goodix_tls_client_write`, backed by a
genuine `SSL_accept()` in `goodixtls.c`). `tls_handshake_run`'s first
state, `TLS_STAGE_HELLO_S`, then reads that embedded server's own
**freshly generated** `ServerHello` (`goodix_tls_client_read` -- new
random values and a new ECDHE key pair every single run, exactly as real
TLS requires) and sends *that* out over USB.

This is not a umockdev bug, and not something a better capture or a
smarter pcap edit can fix: the driver's outgoing TLS bytes are
genuinely non-deterministic by design, so they can never byte-match (or
even length-match) whatever a *previously recorded* session happened to
produce. Static pcap replay is fundamentally the wrong tool for testing
past this point without either mocking the TLS layer itself for tests
(e.g. a deterministic PRNG hook, out of scope for a driver that must use
real crypto in production) or having umockdev tolerate arbitrary
OUT-direction content past a certain stage (not something this fixture
controls).

**Practical effect**: `custom.py` stays as-is (device discovery and
feature-flag assertions only). `open_sync()` cannot be added back via
this mechanism -- not because the fixture is incomplete, but because the
open() sequence's TLS stage is inherently unreplayable this way. Anyone
revisiting this should treat "get `open_sync()` passing under `custom.py`"
as requiring a different testing strategy for the TLS portion specifically
(e.g. stopping the umockdev-driven test at `request_tls_connection`,
verified up through there now, rather than attempting a full `open()`),
not as a capture-quality problem to keep chasing.

**Note on the bus/device relabeling**: rewriting `busnum`/`devnum` is a
simple in-place edit of each packet's usbmon capture header (`busnum` and
`devnum` are literal fields in that header -- see the format doc at the
top of `tools/parse_capture.py` in the parent project) and touches
nothing else; it was verified afterward that every byte of payload data
was still intact (`usb.data_len == usb.urb_len` for all 26 bulk-IN
completions, 14835/14835 bytes total, same as before relabeling).

## Current scope and limitations

`goodix533c.c` currently wires up `dev_class->open`/`->close`/`->enroll`/
`->verify`/`->identify`/`->cancel` (via the concurrent SIGFM work), with
`features` derived by `fpi_device_class_auto_initialize_features()`:
`VERIFY`, `IDENTIFY`, and `ALWAYS_ON` are set; `CAPTURE` is deliberately
NOT set (`dev_class->capture` itself is left NULL -- the open()+one-frame
capture path is only exercised via the test-only
`goodix533c-capture-test` binary, not the public FPrint API); no
`STORAGE*` bits are set (no on-chip storage -- this driver's design is
match-on-host via SIGFM, see `sigfm/` and `libfprint/drivers/goodix533c/`).
Accordingly `custom.py`:

- Does exercise: device enumeration and driver-name/feature-flag
  assertions matching the current wiring.
- Does NOT exercise: `open_sync()`/`close_sync()` (see "Verified replay
  result" above -- this specific capture file cannot support it),
  `enroll_sync()`, `verify_sync()`, `identify_sync()`, or any on-chip
  storage calls. The latter would require driving the device past what
  this fixture could ever safely record (a live finger-present frame),
  which is exactly what must not be committed, on top of the open()
  replay gap making it moot anyway.

**Follow-up needed:**

1. **Add a scoped replay test that stops before TLS.** `custom.pcapng`
   now replays correctly through the entire non-TLS open() sequence (see
   "Replay status" above) -- `nop` through `request_tls_connection` all
   decode exactly as captured. A test that exercises up through there
   (rather than a full `open_sync()`, which requires the TLS stage to
   also replay -- structurally not possible per "Third, structural
   limitation" above) would be genuine, valuable coverage this fixture
   can actually support today. This likely needs a small test-only entry
   point in the driver (there's already a precedent:
   `goodix533c-capture-test`), since `FpDevice`'s public API doesn't
   expose a way to stop mid-open().
2. **Once SIGFM enroll/verify/identify work is complete**, extend
   `custom.py` (or add a second fixture-specific test file) to drive
   `enroll_sync()`/`verify_sync()`/`identify_sync()`, modeled on
   `tests/fpcmoc/custom.py` or `tests/elanmoc/custom.py` (both
   match-on-device though, not match-on-host -- so adapt rather than copy
   the `STORAGE*` assertions; this driver has none of those). This needs
   a *further* new capture that includes real finger-present
   `mcu_get_image` replies -- which, per the constraint above, must be
   captured and safety-reviewed by a human, never generated by an agent,
   and only committed if the human is certain they're comfortable with
   those frames being third-party-decryptable (the PSK is public). Use
   `vm/usbmon-capture-in-vm.sh` (parent project) for the underlying
   capture mechanism -- it's the only one confirmed to retain full
   payload data on a lockdown-enabled host.

## Replay

```sh
umockdev-run -d device \
  -p /sys/devices/pci0000:00/0000:00:14.0/usb3/3-3=custom.pcapng \
  -- <program that talks to 27c6:533c>
```

The syspath is specific to the machine this was captured on but is only
used as a mock sysfs label by umockdev -- any syspath works as long as
the `-p` flag's key matches the `P:` line in `device` with `/sys`
prepended. `tests/umockdev-test.py` derives this automatically from the
`device` file, so `meson test goodix533c` does not need the path spelled
out manually.

**Known limitation** (inherited from the original capture): replaying
against `vendor/goodix-fp-dump-nikicat`'s Python reference driver directly
fails at device-open (PyUSB's `protocol.py` makes `is_kernel_driver_active`/
`set_configuration` calls that a libfprint C driver using `GUsbDevice`/
`g_usb_device_claim_interface` would not). This fixture targets the native
libfprint `goodix533c` driver, not the Python reference implementation --
though see "Verified replay result" above, since even against the native
driver this fixture currently can't complete `open()`.
