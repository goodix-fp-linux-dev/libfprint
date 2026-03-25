# Online Research: Goodix 5e0a Small Sensor Matching

Date: 2026-03-22

---

## 1. Small Sensors and NBIS Minutiae Extraction

**Problem:** Small sensors (like 80x88 px) capture only ~1/4 of a fingerprint. NBIS mindtct
was designed for full fingerprint card scans and aggressively removes "unreliable" minutiae,
leaving too few for bozorth3 to match.

**Key finding — MR !198 on libfprint GitLab:**
- Title: "Remove less minutiae for small sensors"
- Approach: Disable certain false-minutia removal steps by setting their thresholds to
  impossible values, so more minutiae are retained
- Target: Elan 0c63 (small sensor, previously unusable)
- Result: Made sensor usable in swipe mode
- This is a "clean" reimplementation of MR !188, addressing issue #272

**Practical takeaway:** We can tune mindtct parameters to retain more minutiae. The tradeoff
is more false minutiae, but for small sensors it's necessary.

Source: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/198

---

## 2. Goodix Fingerprint Verify Failures on Linux

**Widespread problem.** Multiple reports across Arch Linux, Linux Mint, Ubuntu, and Dell forums.

**Root causes identified:**
- Small sensor resolution means NBIS can't find enough minutiae for reliable matching
- Missing `FPI_IMAGE_PARTIAL` flag causes NBIS to find false minutiae at image borders
- Sensor orientation issues (90-degree rotation during capture)
- Some experimental drivers accept any finger or reject all fingers

**Our specific situation (5e0a):**
- 80x88 pixel sensor is very small
- `bz3_threshold` was set to 5 (extremely permissive) in earlier code, now 12
- `FPI_IMAGE_PARTIAL` was NOT being set (now fixed per our analysis)
- No background subtraction or normalization was being done

Sources:
- https://bbs.archlinux.org/viewtopic.php?id=285848
- https://aur.archlinux.org/packages/libfprint-goodix-521d
- https://forums.linuxmint.com/viewtopic.php?t=454134

---

## 3. Image Preprocessing in libfprint

**Current libfprint preprocessing is minimal:**
- `fp_img_standardize()` normalizes orientation and color (black-on-white vs white-on-black)
- Images are 8-bit greyscale
- No contrast enhancement, no frequency filtering, no ridge enhancement

**What the elan driver does (best reference for preprocessing):**
1. **Background subtraction:** Captures a background frame during calibration, subtracts it
   from every fingerprint frame
2. **Non-linear normalization:** Elantech's recommended 2-step process:
   - Step 1: Subtract background (14-bit -> difference values)
   - Step 2: Map to 8-bit using either:
     - **"Thirds" method:** Sort pixels, divide into 3 groups by intensity (35%/30%/35%),
       map each group to different output ranges (0-99, 99-155, 155-255)
     - **Linear method:** Simple min-max normalization to 0-255
3. **Frame assembly:** For swipe sensors, uses movement estimation to stitch frames

**What we should implement for 5e0a:**
- Background subtraction (critical for removing sensor noise)
- Contrast normalization (histogram equalization or percentile-based)
- Consider Gabor-based ridge enhancement before minutiae extraction

Sources:
- https://fprint.freedesktop.org/libfprint-stable/libfprint-Image-operations.html
- https://github.com/iafilatov/libfprint (elan driver source)

---

## 4. NBIS mindtct Minimum Image Size

**Block size constants in our codebase (lfs.h):**
- `IMAP_BLOCKSIZE = 24` — pixel dimensions of each block in the direction map
- `MAP_BLOCKSIZE_V2 = 8` — pixel dimension of analysis blocks
- `MAP_WINDOWSIZE_V2 = 24` — pixel dimension of surrounding analysis window
- `MAP_WINDOWOFFSET_V2 = 8` — offset from block origin to window origin

**Padding calculation:**
- `get_max_padding_V2()` computes maximum padding needed for DFT analysis and
  directional binarization
- The image is padded before processing; the padded image must accommodate the
  analysis windows

**Minimum practical dimensions:**
- Image must be larger than `MAP_WINDOWSIZE_V2` (24) in both dimensions just to
  get a single analysis block
- For meaningful results, need at least a few blocks in each dimension
- With 80x88 pixels and block size 8: we get ~10x11 blocks, which is workable
  but minimal
- The 24x24 analysis windows mean edge blocks (within 12px of border) produce
  unreliable results -- this is why `FPI_IMAGE_PARTIAL` and `remove_perimeter_pts`
  are critical

**Practical implication:** Our 80x88 image loses ~24px on each edge to border
effects, leaving an effective area of roughly 56x64 -- about half the image.
Every pixel of quality matters.

Sources:
- NBIS source code: `libfprint/nbis/include/lfs.h` (line 316)
- NIST IR 7392: https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7392.pdf

---

## 5. Gabor Filter Fingerprint Enhancement

**Standard pipeline (Hong, Wan, Jain 1998):**
1. **Normalization:** Standardize mean and variance of pixel intensities
2. **Orientation estimation:** Compute local ridge orientation in blocks
3. **Frequency estimation:** Compute local ridge frequency (spacing between ridges)
4. **Region mask:** Identify foreground (ridge area) vs background
5. **Gabor filtering:** Apply oriented Gabor filters tuned to local ridge
   orientation and frequency

**Gabor filter properties:**
- Frequency-selective AND orientation-selective
- Enhances ridges along their local direction while suppressing noise
- Parameters: orientation (theta), frequency (f), bandwidth (sigma_x, sigma_y)
- Produces clean, high-contrast ridge images ideal for minutiae detection

**Advanced alternatives:**
- **Modified Gabor Filter (MGF):** Image-independent parameter selection, avoids
  artifacts from the traditional approach
- **Log-Gabor Filter:** Better bandwidth properties, tuned to smallest spatial extent,
  reportedly better enhancement quality
- **Circular Gabor Filter:** Handles curved ridges better (cores/deltas)

**Python implementation available:** https://github.com/Utkarsh-Deshmukh/Fingerprint-Enhancement-Python
- Uses oriented Gabor filter banks
- Based on Hong et al. (1998)
- Could be used as reference for C implementation or as a preprocessing step

**Practical approach for 5e0a:**
- We could implement Gabor filtering in C within our driver or as a preprocessing step
- OpenCV has Gabor filter support (cv::getGaborKernel)
- Key parameters needed: local ridge orientation map + local ridge frequency
- NBIS mindtct already computes orientation maps internally; we could reuse that logic

Sources:
- https://github.com/Utkarsh-Deshmukh/Fingerprint-Enhancement-Python
- Hong et al. 1998: "Fingerprint image enhancement: Algorithm and performance evaluation"
- https://pmc.ncbi.nlm.nih.gov/articles/PMC10280261/

---

## 6. bz3_threshold Values for Small Sensors

**Default threshold:** `BOZORTH3_DEFAULT_THRESHOLD = 40` (in fp-image-device.c)

**Thresholds used by drivers in our codebase:**

| Driver | Threshold | Sensor Type |
|--------|-----------|-------------|
| Default | 40 | Standard |
| upektc | 30 | Standard |
| upeksonly | 25 | Standard |
| goodix511 | 24 | Small press |
| elan | 24 | Swipe (assembled) |
| elanspi | 24 | Small |
| vfs0050 | 24 | Standard |
| vfs101 | 24 | Swipe |
| vfs301 | 24 | Swipe |
| nb1010 | 24 | Standard |
| vfs5011 | (default 40) | Standard |
| vfs7552 | 20 | Standard |
| aes1610 | 20 | Swipe |
| aes1660 | 20 | Small |
| aes2660 | 20 | Small |
| upektc_img | 20 | Standard |
| **goodix5e0a** | **12** | **Small press (80x88)** |
| aes3k | 9 | Very small |
| egis0570 | varies | Small |

**Pattern:** Small press sensors use 9-24. Our 5e0a uses 12, which is reasonable
for a small sensor. The aes3k at 9 is the lowest. The goodix511 (which is the
closest comparable device to 5e0a) uses 24.

**Recommendation:** Once image quality improves (with preprocessing), we should
try increasing to 15-20 to reduce false positives. With poor images, a low
threshold is necessary but masks the real problem.

---

## 7. goodix-fp-dump Project

**Repository:** https://github.com/goodix-fp-linux-dev/goodix-fp-dump

**Purpose:** Reverse-engineering tool for Goodix fingerprint sensors. Primarily
focused on **image capture and protocol analysis**, NOT fingerprint matching.

**Supported devices:** Drivers for 51x0, 51x7, 52xd, 53x5, 53xd, 5503, 55x4
(NOT 5e0a specifically, but protocol knowledge is transferable)

**Status:** Explicitly marked as "very unstable" and not recommended for
production use.

**Relevance to us:**
- Protocol knowledge from this project informed the goodixtls drivers
- Does NOT include its own matching algorithm — relies on libfprint/NBIS
- Image processing is minimal in the dump scripts
- The libfprint drivers (goodix-fp-linux-dev/libfprint) are the production version

Source: https://github.com/goodix-fp-linux-dev/goodix-fp-dump

---

## 8. Alternative Matching Algorithms

### SIGFM (SIFT Is Good For Matching) -- MOST PROMISING

**What:** A fingerprint matcher designed specifically for low-resolution sensors,
using SIFT (Scale-Invariant Feature Transform) instead of minutiae.

**Target:** 64x80 pixel images (close to our 80x88!)

**Integration with libfprint:**
- MR !418 on libfprint GitLab adds SIGFM support
- Drivers only need: `img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM`
- libfprint handles the rest automatically
- `bz3_threshold` renamed to `score_threshold` in the MR
- Written in C++17, requires OpenCV as dependency
- Built as a separate library alongside NBIS

**Status:** MR !418 has 18 commits, 37 file changes, but is NOT merged as of
Jan 2023. The repository is at https://github.com/goodix-fp-linux-dev/sigfm

**Fork with integration:** https://github.com/bertin0/libfprint-sigfm
(experimental, for Goodix 27c6:5110 sensor at 80x64 resolution)

**THIS IS DIRECTLY APPLICABLE to our 5e0a sensor.** SIFT-based matching
doesn't depend on minutiae count and works with small images.

### open-fprintd

**What:** fprintd replacement that allows custom matching backends via DBus.

**Architecture:** Client <-> open-fprintd <-> Backend (any matching implementation)

**Relevance:** Allows plugging in alternative matchers without modifying libfprint
itself. Could use SourceAFIS, OpenAFIS, or custom matching.

**Status:** Work in progress, no security enforcement on DBus.

Source: https://github.com/uunicorn/open-fprintd

### OpenAFIS

**What:** High-performance 1:N fingerprint matching in C++17.

**Key detail:** Matching only -- does NOT extract minutiae from images. Takes
pre-extracted ISO 19794-2:2005 templates.

**Relevance:** Could replace bozorth3 for matching if we can produce ISO
templates, but doesn't help with the minutiae extraction problem.

Source: https://github.com/neilharan/openafis

### SourceAFIS

**What:** Complete fingerprint recognition in Java/.NET. Takes images as input,
produces similarity scores.

**Relevance:** Not directly usable from C/libfprint without significant effort.
Could be useful for testing/validation in a separate process.

Source: https://sourceafis.machinezoo.com/

Sources:
- https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/418
- https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/485
- https://github.com/goodix-fp-linux-dev/sigfm

---

## 9. Elan Driver Image Processing (Reference Implementation)

The elan driver is the best reference for small-sensor image processing in libfprint.

**Complete preprocessing pipeline:**

1. **Calibration:** Sends calibration command, waits for OK (0x03), retries if needed
2. **Background capture:** Saves a background frame (no finger present)
3. **Background subtraction:** For each fingerprint frame, subtracts stored background
   pixel-by-pixel. If result is darker than background, clamps to 0.
4. **Zero-frame detection:** If sum of all subtracted pixels is 0, rejects the frame
   ("finger present during calibration?")
5. **Normalization:** Two methods:
   - **Linear:** Find min/max, map linearly to 0-255
   - **Thirds (non-linear):** Sort pixels, divide into 35%/30%/35% groups, map each
     group to 0-99, 99-155, 155-255 ranges for better contrast distribution
6. **Frame assembly:** (swipe mode) Movement estimation + frame stitching
7. **Flag setting:** `img->flags |= FPI_IMAGE_PARTIAL`
8. **Minutiae extraction:** `fpi_img_detect_minutiae()` via NBIS

**What 5e0a is missing compared to elan:**
- No background subtraction
- No normalization (just raw pixel copy)
- No FPI_IMAGE_PARTIAL flag (now identified as critical fix)
- No contrast enhancement

Source: `/home/carmine/dev/libfprint-goodix/libfprint/drivers/elan.c` (lines 153-300)

---

## 10. SourceAFIS with libfprint

**Direct integration: NOT practical.** SourceAFIS is Java/.NET only. There is no C
library version.

**However, the concept is valid:** SourceAFIS proves that alternative matching
algorithms (not based on NBIS minutiae) can work well with fingerprint images.
SourceAFIS uses its own feature extraction pipeline that is more robust than
minutiae-only approaches.

**Better alternatives for our use case:**
1. **SIGFM** (C++, designed for our exact problem, has libfprint integration code)
2. **OpenAFIS** (C++, but matching-only, needs minutiae input)
3. **Custom Gabor enhancement + NBIS** (stay with NBIS but improve input quality)

Source: https://sourceafis.machinezoo.com/

---

## Summary: Recommended Action Plan

### Quick Wins (implement now)
1. **Set FPI_IMAGE_PARTIAL flag** -- prevents false border minutiae
2. **Add background subtraction** -- like elan driver does
3. **Add contrast normalization** -- percentile-based (elan "thirds" method) or
   histogram equalization
4. **Tune bz3_threshold** -- currently 12, adjust after image quality improves

### Medium-term (significant improvement expected)
5. **Gabor filter enhancement** -- standard fingerprint preprocessing, will
   dramatically improve ridge clarity for NBIS minutiae extraction
6. **Tune mindtct parameters** -- relax false-minutia removal per MR !198 approach

### Long-term (best solution for small sensors)
7. **Integrate SIGFM** -- purpose-built for our exact sensor size (64x80 target),
   SIFT-based matching that doesn't depend on minutiae count. Fork exists at
   https://github.com/bertin0/libfprint-sigfm with Goodix integration.
   Requires OpenCV dependency.

### Priority order: 1 > 2 > 3 > 5 > 4 > 6 > 7
