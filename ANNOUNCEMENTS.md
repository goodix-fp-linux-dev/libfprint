# Goodix 27c6:5e0a Linux Fingerprint Driver -- Announcement Documents

---

## 1. Pull Request Description (goodix-fp-linux-dev/libfprint, target: goodixtls branch)

### Title: Add Goodix 27c6:5e0a (fw 10034) driver with dual TLS-PSK and SIGFM

### Description

This PR adds full enrollment and verification support for the Goodix 27c6:5e0a
fingerprint sensor (firmware `GFUSB_GM168SEC_APP_10034`), commonly found in
realme Book Prime and other laptop models.

#### Device details

- USB ID: `27c6:5e0a`
- Firmware: `GFUSB_GM168SEC_APP_10034`
- Sensor: 80x88 pixels, 12-bit depth, 4/6 packed encoding
- Chip family: Goodix "Chicago H" / Milan HU series

#### What was reverse-engineered

The Windows driver (`Wbdi.dll` V3.0.141.150) was analyzed with radare2 to
reconstruct:

- The complete MCU command map (0x00 NOP through 0xe4 PresetPskRead)
- OTP (one-time programmable) data parsing with triple-redundancy validation
- Chip configuration construction from OTP values (tcode, DAC, FDT delta)
- FDT calibration algorithm (`GetFdtManualBase` / `CalcFdtBase`)
- The dual-TLS session architecture (command channel vs. image channel)
- Frame decoding (4/6 byte packing, no header/footer unlike 5110)
- Scan state machine transitions (FDT_DOWN, capture, FDT_UP cycle)

#### Key technical contributions

1. **Dual TLS-PSK architecture.** The 5e0a establishes two independent TLS-PSK
   sessions over a single USB pipe: a *command channel* (PSK = all zeros,
   flags `0xbb020001`) for MCU commands, and an *image channel* using a
   device-specific PSK extracted from the Windows driver via DPAPI. The driver
   manages both sessions through the existing goodixtls socketpair model.

2. **Dynamic FDT calibration.** Instead of using static FDT thresholds, the
   driver reads the sensor's FDT manual base data (cmd 0x36) at init time and
   computes per-zone touch detection thresholds dynamically. This matches the
   Windows driver's `CalcFdtBase` algorithm and is essential for reliable
   finger-down/finger-up detection across different hardware units.

3. **SIGFM matcher integration.** The 80x88 sensor produces images too small
   for NBIS/Bozorth3 to match reliably (typically only 5-12 minutiae). This
   PR integrates [SIGFM](https://github.com/bertin0/libfprint-sigfm), a
   SIFT-based matcher that extracts far more features from small images.
   SIGFM uses OpenCV SIFT keypoints with Lowe's ratio test, geometric
   verification (vector length + angle consistency), and requires a minimum
   of 25 keypoints per image and 5 good matches for a positive result.

4. **Baseline subtraction.** A calibration frame is captured at init (no finger
   present) and subtracted from every scan to remove fixed-pattern noise.

5. **"Thirds" normalization.** Pixel intensities are mapped through a
   percentile-based three-band normalization (similar to Elan drivers) rather
   than simple min/max scaling, improving contrast on the small sensor area.

6. **Correct FDT_UP prefix.** The FDT_UP command uses prefix `0x0E` (not
   `0x0C`), confirmed through RE of the Windows driver. This is critical for
   proper finger-lift detection between enrollment stages.

#### PSK extraction

The image-channel PSK is device-specific and must be extracted from a Windows
installation where the Goodix driver has been used at least once:

1. Boot Windows and enroll at least one fingerprint.
2. Locate the PSK blob stored by the Goodix driver (protected with DPAPI).
3. Decrypt using the machine DPAPI master key (tools like `mimikatz` or
   `dpapi.py` from Impacket work).
4. Place the resulting 32-byte hex key at `/etc/libfprint/goodix-5e0a.psk`.

Python tooling for the extraction process:
https://github.com/carminezac/libfprint-goodix/blob/goodixtls/tools/extract_psk.py

#### Build dependencies

- Standard libfprint dependencies (glib, libusb, OpenSSL, meson, ninja)
- **OpenCV 4.x** (`libopencv-dev` / `opencv` package) -- required for the SIGFM matcher
- C++17 compiler -- SIGFM is written in C++

#### Testing status

- Enrollment: 20/20 stages passed, 0 retries required
- Verification: match on first attempt
- Tested on a single hardware unit with firmware 10034
- GNOME/GDM integration: not yet tested

#### Files changed

New files:
- `libfprint/drivers/goodixtls/goodix5e0a.c` (~1000 lines) -- driver implementation
- `libfprint/drivers/goodixtls/goodix5e0a.h` -- constants, FDT payloads, frame geometry
- `libfprint/sigfm/sigfm.cpp` -- SIFT extraction and matching
- `libfprint/sigfm/sigfm.hpp` -- C API header
- `libfprint/sigfm/binary.hpp` -- serialization for enrolled print storage
- `libfprint/sigfm/img-info.hpp` -- keypoint/descriptor container
- `libfprint/sigfm/meson.build` -- build config for libsigfm

Modified files:
- `meson.build` -- add 5e0a driver, OpenCV dependency, SIGFM subdir
- `libfprint/drivers/goodixtls/goodix.c` -- dual-TLS session support
- `libfprint/drivers/goodixtls/goodix.h` -- image PSK field in device struct

Documentation:
- `RE_INIT_SEQUENCE.md` -- full MCU command map and init flow
- `RE_FDT_CALIBRATION.md` -- FDT threshold computation algorithm
- `RE_SCAN_FLOW.md` -- scan state machine and FDT response parsing
- `RE_IMAGE_PROCESSING.md` -- frame decode and normalization
- `RE_OTP_SENSOR_CONFIG.md` -- OTP data layout and config construction

---

## 2. Upstream libfprint Issue (gitlab.freedesktop.org/libfprint/libfprint)

### Title: Support for Goodix 27c6:5e0a (firmware GFUSB_GM168SEC_APP_10034)

### Body

#### Device identification

- **USB ID:** `27c6:5e0a`
- **Firmware version:** `GFUSB_GM168SEC_APP_10034`
- **Chip family:** Goodix "Chicago H" / Milan HU series
- **Sensor:** 80x88 pixels, 12-bit depth
- **Found in:** realme Book Prime (and likely other OEM models using this sensor)

`lsusb` output:
```
Bus 001 Device 003: ID 27c6:5e0a Shenzhen Goodix Technology Co.,Ltd.
```

#### Current status

A **fully working driver** exists in the community fork at
[goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint)
(branch: `goodixtls`). Both enrollment and verification are functional.

https://github.com/goodix-fp-linux-dev/libfprint/pull/34

#### Technical summary

The 5e0a uses a protocol that differs from the existing Goodix 5110/511 drivers
in several important ways:

1. **Dual TLS-PSK sessions.** The device requires two independent TLS-PSK
   connections over one USB interface: a command channel (PSK = zeros) and an
   image channel (device-specific PSK). The image PSK must be extracted from
   the Windows driver's DPAPI-protected storage, as it is provisioned
   per-device during manufacturing.

2. **Dynamic FDT calibration.** Finger detection thresholds are not static;
   they must be computed at runtime from the sensor's FDT manual base data,
   following the algorithm in the Windows driver's `CalcFdtBase` function.

3. **Small sensor, NBIS-unfriendly.** The 80x88 image produces too few
   minutiae for reliable Bozorth3 matching. The working driver uses
   [SIGFM](https://github.com/bertin0/libfprint-sigfm), a SIFT-based
   matcher built on OpenCV, which introduces a C++ / OpenCV dependency.

4. **Per-device PSK requirement.** Unlike sensors that use a fixed or
   firmware-derived key, the 5e0a's image encryption key is unique to each
   hardware unit. Users must extract it from a Windows installation. Python
   tooling for this is available: [TODO: link to extraction tool].

#### What would be needed for upstream

- Decision on whether the OpenCV/SIGFM dependency is acceptable upstream, or
  whether an alternative small-sensor matcher could be developed.
- PSK provisioning UX: the current approach (manual file at
  `/etc/libfprint/goodix-5e0a.psk`) works but is not user-friendly. A
  `fprintd` helper or first-run wizard could improve this.
- Testing on additional hardware units with the same USB ID but potentially
  different firmware versions.
- Review of the reverse-engineered protocol for correctness and robustness.

#### References

- Community fork PR: https://github.com/carminezac/libfprint-goodix
- Python protocol tools: [TODO: link to goodix-fp-dump]
- SIGFM matcher: https://github.com/bertin0/libfprint-sigfm
- Neodyme/tlambertz Goodix TLS research: https://blog.neodyme.io/posts/goodix-fingerprint-reader

---

## 3. Reddit Post (r/linux)

### Title: I reverse-engineered the Goodix 27c6:5e0a fingerprint sensor and wrote a working Linux driver

### Body

**TL;DR:** The Goodix 27c6:5e0a fingerprint sensor (found in the realme Book Prime and possibly other
the realme Book Prime and other laptops) now works on Linux. Enrollment and verification
are fully functional. The driver is available in the goodix-fp-linux-dev
community fork of libfprint.

---

Like many of you, I bought a laptop and discovered the fingerprint sensor was
completely unsupported on Linux. `lsusb` showed `27c6:5e0a Shenzhen Goodix
Technology` and that was about where the fun ended -- no driver, no
documentation, nothing.

So I reverse-engineered the Windows driver.

**What I found was... complicated.** The 5e0a is not a simple imaging sensor.
It uses an encrypted protocol with *two* independent TLS-PSK sessions running
over a single USB pipe. One session (with a fixed all-zeros key) carries MCU
commands. The other (with a per-device key provisioned at the factory) carries
the actual fingerprint image data. The device-specific key is stored in
Windows under DPAPI protection, which means extracting it requires decrypting
Microsoft's credential storage. This is a one-time operation, but it does
mean you need a working Windows installation (or at least a disk image) to
get started.

**The second challenge was finger detection.** The sensor uses a
"Finger Detection Threshold" (FDT) system where touch sensitivity must be
calibrated dynamically at startup. The Windows driver reads baseline capacitance
values from the sensor and computes per-zone thresholds using an algorithm I
had to reconstruct from x86_64 assembly (shoutout to radare2). Getting this
wrong means the sensor either never detects a finger or triggers constantly.

**The third challenge was matching.** The sensor is tiny -- 80x88 pixels. The
standard fingerprint matching algorithm in libfprint (NBIS/Bozorth3) relies on
detecting minutiae (ridge endings and bifurcations), but an image this small
only produces 5-12 minutiae, which is not enough for reliable matching. I
integrated SIGFM, an alternative matcher that uses OpenCV's SIFT algorithm to
extract scale-invariant features. This was the key to getting verification
working -- SIFT finds far more usable features in small images than
minutiae-based approaches.

**Current status:**

- Enrollment: works (20/20 stages, no retries)
- Verification: works (matches on first attempt)
- Available in the [goodix-fp-linux-dev/libfprint](https://github.com/goodix-fp-linux-dev/libfprint) fork (goodixtls branch)
- Requires: OpenCV (for SIGFM matcher), one-time PSK extraction from Windows

**Looking for testers!** If you have a laptop with USB ID `27c6:5e0a` (check
with `lsusb`), I would love to know if the driver works on your hardware. The
biggest unknown right now is whether different firmware versions or hardware
revisions need different handling.

**How to check if this is your sensor:**

```
lsusb | grep 27c6:5e0a
```

If you see `27c6:5e0a`, this driver is for you.

**Acknowledgements:**

This would not have been possible without the work of the
[goodix-fp-linux-dev](https://github.com/goodix-fp-linux-dev) community, who
built the foundational goodixtls driver framework that supports other Goodix
sensors (5110, 5395, etc.). The TLS-PSK protocol analysis by
[tlambertz and the Neodyme team](https://blog.neodyme.io/posts/goodix-fingerprint-reader)
was essential for understanding the encrypted transport layer. The
[SIGFM](https://github.com/bertin0/libfprint-sigfm) matcher by Matthieu
Charette, Natasha England-Elbro, and Timur Mangliev solved the small-sensor
matching problem. And the broader reverse engineering community's tooling
(radare2, Wireshark USB captures) made this feasible at all.

The reverse engineering documentation is included in the repository for anyone
working on similar Goodix sensors.

Happy to answer questions about the RE process or the driver architecture.

---

## 4. README Section for the libfprint Fork

### Goodix 27c6:5e0a (GFUSB_GM168SEC_APP_10034)

#### Overview

Full enrollment and verification support for the Goodix 27c6:5e0a fingerprint
sensor, a small (80x88) capacitive sensor found in realme Book Prime and other
laptop models. The driver implements a dual TLS-PSK protocol with dynamic FDT
calibration and uses SIFT-based fingerprint matching (SIGFM) instead of
NBIS/Bozorth3.

#### Quick start

1. **Verify your sensor:**

   ```
   lsusb | grep 27c6:5e0a
   ```

2. **Extract the device PSK (one-time, requires Windows):**

   The 5e0a encrypts fingerprint images with a per-device pre-shared key. You
   must extract this key from the Windows Goodix driver installation:

   - Boot into Windows and ensure at least one fingerprint is enrolled.
   - Use the PSK extraction tool: [TODO: link to tool/instructions]
   - The tool will decrypt the DPAPI-protected key blob.
   - Save the 32-byte hex key to `/etc/libfprint/goodix-5e0a.psk`:

     ```
     echo "your_64_hex_char_key_here" | sudo tee /etc/libfprint/goodix-5e0a.psk
     sudo chmod 600 /etc/libfprint/goodix-5e0a.psk
     ```

3. **Install dependencies:**

   Debian/Ubuntu:
   ```
   sudo apt install libglib2.0-dev libusb-1.0-0-dev libssl-dev \
     libopencv-dev meson ninja-build g++
   ```

   Arch/Manjaro:
   ```
   sudo pacman -S glib2 libusb openssl opencv meson ninja gcc
   ```

   Fedora:
   ```
   sudo dnf install glib2-devel libusb1-devel openssl-devel \
     opencv-devel meson ninja-build gcc-c++
   ```

4. **Build and install:**

   ```
   meson setup builddir
   meson compile -C builddir
   sudo meson install -C builddir
   ```

5. **Test enrollment and verification:**

   ```
   # Enroll a fingerprint
   fprintd-enroll

   # Verify
   fprintd-verify
   ```

#### Technical details

| Property | Value |
|----------|-------|
| USB ID | `27c6:5e0a` |
| Firmware | `GFUSB_GM168SEC_APP_10034` |
| Sensor resolution | 80 x 88 pixels |
| Pixel depth | 12-bit (packed as 4 pixels per 6 bytes) |
| Command TLS PSK | All zeros (fixed) |
| Image TLS PSK | Per-device, DPAPI-protected |
| PSK flags | `0xbb020001` |
| Matcher | SIGFM (SIFT-based, via OpenCV) |
| Enrollment stages | 20 |

#### Architecture

The driver inherits directly from `FpiDeviceGoodixTls` (not `FpiDeviceGoodixTls5xx`)
because the 5e0a protocol diverges significantly from the 511/5110 family:

- **Dual TLS sessions** over a single USB interface
- **No frame header/footer** (unlike the 5110)
- **Dynamic FDT thresholds** computed from sensor base data at init
- **FDT_UP prefix `0x0E`** (different from other Goodix sensors)

Driver files:
- `libfprint/drivers/goodixtls/goodix5e0a.c` -- main driver (~1000 lines)
- `libfprint/drivers/goodixtls/goodix5e0a.h` -- constants and default payloads
- `libfprint/sigfm/` -- SIFT-based fingerprint matcher (C++ / OpenCV)

#### Reverse engineering documentation

The following documents describe the protocols and algorithms reconstructed
from the Windows driver (`Wbdi.dll` V3.0.141.150):

| Document | Contents |
|----------|----------|
| `RE_INIT_SEQUENCE.md` | Complete MCU command map and initialization flow |
| `RE_FDT_CALIBRATION.md` | FDT threshold computation algorithm from OTP data |
| `RE_SCAN_FLOW.md` | Scan state machine, FDT response parsing |
| `RE_IMAGE_PROCESSING.md` | Frame decode, baseline subtraction, normalization |
| `RE_OTP_SENSOR_CONFIG.md` | OTP data layout, chip config construction |
| `RE_FDT_PAYLOAD_DETAIL.md` | FDT command payload byte-level format |
| `RE_SCAN_STATE_MACHINE.md` | Detailed scan SSM state transitions |
| `PORTING_5E0A.md` | Driver architecture and porting rationale |
| `SIGFM_INTEGRATION.md` | SIGFM algorithm analysis and integration notes |

#### Troubleshooting

**"No PSK file found" error:**
Ensure `/etc/libfprint/goodix-5e0a.psk` exists and contains exactly 64 hex
characters (32 bytes). The file must be readable by the fprintd process.

**Finger detection not working:**
If the sensor never detects finger placement, the FDT calibration may have
failed. Check the libfprint debug log:
```
G_MESSAGES_DEBUG=all fprintd-enroll
```
Look for "FDT base data" and "Calibration frame captured" messages.

**Verification always fails:**
Ensure OpenCV is installed and SIGFM was built. If libfprint was built without
OpenCV, it falls back to NBIS/Bozorth3, which does not work reliably with the
80x88 sensor.

**Wrong firmware version:**
The driver expects firmware `GFUSB_GM168SEC_APP_10034`. Devices with different
firmware may need adjustments to the chip configuration template or FDT
parameters. Please open an issue with your firmware string (visible in debug
logs at startup).
