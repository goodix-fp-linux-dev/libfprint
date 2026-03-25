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
