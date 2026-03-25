# Driver Research: Small Fingerprint Sensor Best Practices for NBIS

Date: 2026-03-22

## 1. How the Goodix 511 Achieves Matching on 64x80 Images

### 1.1 Full Scan Flow

```
SCAN_STAGE_QUERY_MCU        -> query MCU state
SCAN_STAGE_SWITCH_TO_FDT_MODE -> FDT mode (finger detect)
SCAN_STAGE_CALIBRATE        -> CALIBRATION sub-SSM:
    CALIBRATION_STAGE_FDT_UP     -> switch to FDT_UP (no finger state)
    CALIBRATION_STAGE_NAV0       -> send NAV_0 command
    CALIBRATION_STAGE_GET_IMG    -> capture baseline frame (no finger present)
                                   -> decoded into calibration_img (16-bit pixels)
SCAN_STAGE_SWITCH_TO_FDT_DOWN -> wait for finger placement
SCAN_STAGE_GET_IMG           -> capture fingerprint frame (finger present)
    -> goodixtls5xx_decode_frame()           [4/6-byte unpack, skip 8-byte header + 5-byte footer]
    -> linear_subtract_inplace()             [subtract calibration baseline]
    -> goodixtls5xx_squash_frame_linear()    [16-bit -> 8-bit via min/max linear normalization]
    -> crop_frame()                          [88-wide raw -> 64-wide output, sets FPI_IMAGE_PARTIAL]
SCAN_STAGE_SWITCH_TO_FTD_UP   -> switch back to FDT_UP
SCAN_STAGE_SWITCH_TO_FTD_DONE -> report finger_status=FALSE
```

### 1.2 Calibration: What It Does and Why It Matters

The 511 calibration captures a **baseline image with no finger present** (during FDT_UP state).
This baseline is then subtracted from each scan:

```c
// linear_subtract_inplace: removes fixed-pattern noise
src[n] = MAX(0, max - ((max - src[n]) - (max - by[n])));
```

This is essentially `src - baseline` with saturation. The purpose is to remove:
- Fixed sensor noise (hot pixels, dark pixels)
- Ambient light contamination
- Manufacturing variations in pixel sensitivity

**Why the 511 needs it:** The sensor's 12-bit ADC captures a lot of fixed pattern noise.
Without subtraction, the fingerprint ridges are buried in the sensor's DC offset.

**Why the 5e0a might differ:** The 5e0a comment says "calibration subtraction disabled --
it destroys signal for this sensor." This could mean:
1. The 5e0a's image TLS decryption already handles DC offset removal
2. The 5e0a captures data differently (no header/footer in raw data)
3. The calibration image was captured incorrectly (wrong timing, wrong sensor state)

**Recommendation:** Re-investigate calibration for the 5e0a. The current code has the
calibration state that just calls `fpi_ssm_next_state(ssm)` (skipped). Try:
1. Capture calibration frame before FDT_DOWN (when no finger is present)
2. Verify the calibration frame has reasonable pixel values (not all zeros)
3. If subtraction destroys signal, the calibration may be captured with wrong timing

### 1.3 Key 511 Parameters

| Parameter | Value |
|-----------|-------|
| Raw scan width | 88 pixels |
| Raw scan height | 80 pixels |
| Output width | 64 pixels (cropped from 88) |
| Output height | 80 pixels |
| bz3_threshold | 24 |
| nr_enroll_stages | 20 |
| FPI_IMAGE_PARTIAL | YES |
| ppmm | default (19.685 = 500 DPI) |
| Image resize/upscale | NO |

### 1.4 Key 5e0a Parameters (Current)

| Parameter | Value |
|-----------|-------|
| Scan width | 80 pixels |
| Scan height | 88 pixels |
| Output width | 80 pixels |
| Output height | 88 pixels |
| bz3_threshold | 12 |
| nr_enroll_stages | 10 |
| FPI_IMAGE_PARTIAL | NOT SET |
| ppmm | 19.685 (explicitly set) |
| Image resize/upscale | NO |
| Calibration subtraction | DISABLED |
| Unsharp mask | radius=3, strength=4.0 |

---

## 2. How the Elan Driver Handles Small Sensors

File: `libfprint/drivers/elan.c`

### 2.1 Elan's Approach: Swipe-to-Enlarge

The Elan driver's comment (line 23-37) is brutally honest:

> "The algorithm which libfprint uses to match fingerprints doesn't like small
> images like the ones these drivers produce. There's just not enough minutiae
> (recognizable print-specific points) on them for a reliable match."

Elan's solution: **Require the user to swipe**, assembling multiple small frames
into a larger composite image via `fpi_assemble_frames()`.

### 2.2 Elan's Image Processing Pipeline

1. **Calibration/Background capture** (`elan_save_background`): Captures a frame with
   no finger, stores it as `background` (16-bit pixels)

2. **Frame capture loop**: Captures 7-30 frames while finger swipes across sensor

3. **Background subtraction** (`elan_save_img_frame`): Each frame has the background
   subtracted: `frame[i] -= elandev->background[i]`

4. **Non-linear normalization** (`elan_process_frame_thirds`): A 3-tier percentile
   mapping that maps pixel values to 0-255 using the 35th, 65th percentile as
   breakpoints. This gives much better contrast than simple linear mapping:
   - Bottom 35% pixels -> 0-99
   - Middle 30% pixels -> 99-155
   - Top 35% pixels -> 155-255

   Some devices use linear normalization instead (`elan_process_frame_linear`).

5. **Frame assembly**: Movement estimation + stitching into composite image

6. **Flags**: `FPI_IMAGE_PARTIAL` is set

### 2.3 Key Elan Parameters

| Parameter | Value |
|-----------|-------|
| Sensor size | 144x64 or 96x96 |
| scan_type | FP_SCAN_TYPE_SWIPE |
| bz3_threshold | 24 |
| nr_enroll_stages | 5 (default) |
| Frame height crop | max 50px (to improve stitching) |
| Min frames | 7 |
| Max frames | 30 |
| Skip last frames | 2 (finger lifting produces bad frames) |
| Calibration | YES (background + mean comparison) |
| FPI_IMAGE_PARTIAL | YES |

### 2.4 Lessons from Elan for the 5e0a

- Background subtraction is **critical** even for Elan
- The non-linear "thirds" normalization produces better contrast than linear min/max
- Elan uses bz3_threshold=24 successfully (NOT a lowered value)
- The comment about swipe being necessary is relevant: press-type small sensors
  are inherently harder for NBIS

---

## 3. How the Egis0570 Driver Works

File: `libfprint/drivers/egis0570.c`

### 3.1 Pipeline

1. **Background capture**: First packet set captures 5 frames, takes per-pixel minimum
   as background: `background[i] = MIN(background[i], frame[i])`

2. **Background subtraction**: `frame[i] -= background[i]` (with margin threshold)

3. **Finger detection**: Mean pixel value per frame; if > EGIS0570_MIN_MEAN (20),
   finger is present

4. **Frame assembly**: Multiple frames stitched via `fpi_assemble_frames()`

5. **Image resize**: **2x upscale** via `fpi_image_resize(img, 2, 2)`

6. **Flags**: `FPI_IMAGE_COLORS_INVERTED | FPI_IMAGE_PARTIAL`

### 3.2 Key Parameters

| Parameter | Value |
|-----------|-------|
| Sensor size | 114x57 per frame |
| scan_type | FP_SCAN_TYPE_SWIPE |
| bz3_threshold | 25 |
| Image resize | 2x (bilinear upscale) |
| FPI_IMAGE_PARTIAL | YES |

### 3.3 Developer Comment on bz3_threshold (from egis0570.h)

> "This sensor is small so I decided to reduce bz3_threshold from
> 40 to 10 to have more success to fail ratio.
> Bozorth3 Algorithm seems not fine at the end.
> forget about security :))"

The threshold was later raised back to 25.

---

## 4. Other Small Sensor Drivers: Key Comparison

### 4.1 aes3k (AuthenTec AES3500/AES4000) -- Smallest Press Sensor

- **Sensor**: 128x128 (AES3500), but very low quality
- **Key trick**: **2x bilinear upscale** with comment "this is an ugly hack to make
  the image big enough for NBIS to process reliably"
- **bz3_threshold**: **9** (lowest of ANY driver, with comment "Extremely low due to low image quality")
- **scan_type**: FP_SCAN_TYPE_PRESS

### 4.2 elanspi (Elan SPI sensors)

- **Sensor**: Various small sizes
- **Key trick**: **2x upscale** via `fpi_image_resize(img, 2, 2)` after stitching
- **bz3_threshold**: 24
- **nr_enroll_stages**: 7 (with comment "these sensors are very hit or miss, may as well record a few extras")
- **Frame difference detection**: Uses `elanspi_get_frame_diff_stddev_sq()` to detect
  whether the finger has actually moved between frames
- **scan_type**: FP_SCAN_TYPE_SWIPE

### 4.3 bz3_threshold Summary Across All Drivers

| Driver | Sensor Size | Type | bz3_threshold | Upscale |
|--------|-------------|------|---------------|---------|
| nb1010 | 256x180 | press | 24 | no |
| vfs0050 | large | press | 24 | no |
| vfs7552 | large | press | 20 | no |
| elan | 144x64/96x96 | swipe | 24 | no |
| elanspi | various small | swipe | 24 | 2x |
| egis0570 | 114x57 frames | swipe | 25 | 2x |
| aes3k | 128x128 | press | **9** | 2x |
| aes1610 | small | swipe | 20 | no |
| aes1660/2660 | small | swipe | 20 | no |
| goodix511 | 64x80 | press | 24 | no |
| goodix5e0a | 80x88 | press | **12** | no |
| upektc_img | medium | press | 20 | no |
| vfs5011 | medium | swipe | 20 | no |
| upeksonly | medium | swipe | 25 | no |
| vfs101 | medium | swipe | 24 | no |
| vfs301 | medium | swipe | 24 | no |
| **Default** | - | - | **40** | - |

**Key insight:** The goodix511 achieves bz3_threshold=24 on a 64x80 image with NO
upscaling. This means NBIS CAN work on small images IF the image quality is good enough.

---

## 5. NBIS Minimum Image Size and Block Analysis

### 5.1 NBIS Internal Parameters (LFSPARMS V2)

From `libfprint/nbis/include/lfs.h`:

| Parameter | Value | Meaning |
|-----------|-------|---------|
| MAP_BLOCKSIZE_V2 | 8 | Block size for directional maps |
| MAP_WINDOWSIZE_V2 | 24 | Window size for direction estimation |
| MAP_WINDOWOFFSET_V2 | 8 | Window offset |
| PAD_VALUE | 128 | Padding value for image borders |

### 5.2 Minimum Image Size Calculation

NBIS divides the image into blocks of `MAP_BLOCKSIZE_V2` (8) pixels. For a useful
directional map, you need at least ~3x3 blocks = 24x24 pixels absolute minimum.

However, the direction estimation window is `MAP_WINDOWSIZE_V2` (24) pixels wide.
This means the **effective minimum** for useful minutiae detection is approximately
**48x48 pixels** (2 windows + overlap).

For a 64x80 image (511): ~8x10 blocks = 80 blocks. This is workable.
For an 80x88 image (5e0a): ~10x11 blocks = 110 blocks. This is even better.

**Conclusion:** The 5e0a at 80x88 has MORE working area than the 511 at 64x80.
The image size is NOT the bottleneck. Image QUALITY is the bottleneck.

### 5.3 remove_perimeter_pts

When `FPI_IMAGE_PARTIAL` is set, libfprint sets `lfsparms->remove_perimeter_pts = TRUE`.
This removes minutiae detected near the image edges, which are unreliable for partial
images (press-type sensors). This is CRITICAL for small sensors because:
- Edge minutiae on partial images are often artifacts
- Removing them improves match reliability even if total count drops

**THE 5e0a DOES NOT SET FPI_IMAGE_PARTIAL!** This is a bug. Without it, NBIS will
keep unreliable edge minutiae that hurt matching accuracy.

---

## 6. What Preprocessing Improves NBIS Detection on Small Sensors

### 6.1 Techniques Used by Working Drivers

1. **Background/calibration subtraction** (elan, egis0570, goodix511):
   Removes fixed-pattern noise. Almost universally used.

2. **Image upscaling (2x bilinear)** (aes3k, egis0570, elanspi):
   The single most common technique for helping NBIS with small images.
   Comment in aes3k.c: "ugly hack to make the image big enough for NBIS
   to process reliably." Despite being called a hack, it works.

3. **Non-linear histogram normalization** (elan thirds):
   Better contrast than simple linear min/max mapping.

4. **FPI_IMAGE_PARTIAL flag**: Removes unreliable edge minutiae.

5. **High enroll stage count** (511: 20, elanspi: 7, 5e0a: 10):
   More enrollment samples = more minutiae templates to match against.

### 6.2 Techniques NOT Currently Used but Recommended

1. **CLAHE (Contrast Limited Adaptive Histogram Equalization)**:
   Academic literature strongly recommends CLAHE for fingerprint enhancement.
   CLAHE divides the image into tiles and equalizes each independently,
   enhancing local contrast without amplifying noise. This would replace
   or complement the current unsharp mask.

2. **Gabor filtering**: Oriented bandpass filters that enhance ridge/valley
   patterns. Well-established in fingerprint preprocessing literature.
   However, this is complex to implement and may be overkill for now.

3. **Morphological operations**: Opening/closing to clean up noise after
   contrast enhancement.

### 6.3 SIGFM: An Alternative to NBIS

The goodix-fp-linux-dev team created SIGFM (https://github.com/goodix-fp-linux-dev/sigfm),
a matcher specifically designed for 64x80 low-resolution sensors. It uses SIFT features
instead of minutiae. However:
- It is experimental and not integrated into mainline libfprint
- The team still describes it as "very unstable"
- Integrating it requires significant changes to libfprint's matching pipeline
- Sticking with NBIS/bozorth3 and improving image quality is the more practical path

---

## 7. Enrollment Diversity Problem

### 7.1 How libfprint Handles Enrollment

From `fpi-image-device.c` lines 294-317:

```c
if (action == FPI_DEVICE_ACTION_ENROLL) {
    if (print) {
        fpi_print_add_print(enroll_print, print);   // Add xyt to list
        priv->enroll_stage += 1;
    }
    fpi_device_enroll_progress(device, priv->enroll_stage, ...);
    if (priv->enroll_stage == fp_device_get_nr_enroll_stages(device)) {
        // Done enrolling
    } else {
        // Wait for finger off, then on again
        fp_image_device_enroll_maybe_await_finger_on(...);
    }
}
```

**CRITICAL FINDING: libfprint does NOT check enrollment image diversity!**

Every image that produces minutiae is blindly added to the print template.
There is no check that:
- The new image is sufficiently different from previously enrolled images
- The new image has enough minutiae to be useful
- The finger was actually lifted and repositioned between scans

### 7.2 The "Lift Finger Between Scans" Problem

The state machine goes:
1. `AWAIT_FINGER_ON` -> user places finger
2. Driver reports `finger_status = TRUE`
3. Image captured, minutiae extracted
4. Driver reports `finger_status = FALSE` (finger lifted)
5. State goes to `AWAIT_FINGER_ON` again
6. Wait for `finger_status = TRUE` (finger placed again)

The 5e0a driver currently:
- Reports `finger_status = TRUE` immediately when starting image capture
- Reports `finger_status = FALSE` after image is captured
- Goes back to `AWAIT_FINGER_ON`

This SHOULD force the user to lift and replace their finger. However, if the
finger detection (FDT_DOWN) triggers too easily or doesn't properly wait for
finger removal first, the user might get duplicate images.

### 7.3 How This Should Be Fixed

**Option A: Check enrollment diversity in the driver (recommended)**

Before calling `fpi_image_device_image_captured()`, compare the current image
against the previous one using MSE (Mean Squared Error):

```c
static int
compute_mse(const guint8 *img1, const guint8 *img2, int size)
{
    long long sum = 0;
    for (int i = 0; i < size; i++) {
        int diff = (int)img1[i] - (int)img2[i];
        sum += diff * diff;
    }
    return (int)(sum / size);
}

// In scan_on_read_img_5e0a, before fpi_image_device_image_captured:
int mse = compute_mse(current_img, previous_img, GOODIX_5E0A_FRAME_SIZE);
if (mse < DIVERSITY_THRESHOLD) {
    // Images too similar -- request retry
    fpi_image_device_retry_scan(img_dev, FP_DEVICE_RETRY_CENTER_FINGER);
    return;
}
```

**Option B: Cross-check bozorth3 scores between enrollment images**

After minutiae extraction, compare the new print against all previously enrolled
prints. If the bozorth3 score is too HIGH (meaning the images are too similar),
reject the duplicate:

```c
// Pseudo-code for enrollment diversity check
for (i = 0; i < enrolled_prints_count; i++) {
    score = bozorth3_match(new_print, enrolled_prints[i]);
    if (score > DUPLICATE_THRESHOLD) {
        // Too similar to an existing enrollment image
        fpi_image_device_retry_scan(img_dev, FP_DEVICE_RETRY_CENTER_FINGER);
        return;
    }
}
```

This is conceptually better because it checks at the minutiae level, not the
pixel level. However, it is harder to implement because the driver doesn't have
direct access to the enrolled print's xyt data.

**Option C: Minimum minutiae count check**

Reject enrollment images with fewer than N minutiae:

```c
GPtrArray *minutiae = fp_image_get_minutiae(image);
if (minutiae->len < MIN_ENROLLMENT_MINUTIAE) {
    // Not enough detail -- reject
    fpi_image_device_retry_scan(img_dev, FP_DEVICE_RETRY_GENERAL);
}
```

This is already somewhat handled by `fpi_print_add_from_image()` which fails
if `minutiae->len == 0`, but it doesn't check for "too few" (e.g., < 5).

---

## 8. Concrete, Actionable Recommendations for goodix5e0a

### Priority 1: Quick Wins (Fix Obvious Issues)

1. **Set FPI_IMAGE_PARTIAL flag** in `scan_on_read_img_5e0a()`:
   ```c
   img->flags |= FPI_IMAGE_PARTIAL;
   ```
   This enables `remove_perimeter_pts` in NBIS, removing unreliable edge minutiae.
   The 511 sets this. The 5e0a currently does NOT. This is a one-line fix.

2. **Add 2x image upscaling** before submitting to libfprint:
   ```c
   FpImage *scaled = fpi_image_resize(img, 2, 2);
   fpi_image_device_image_captured(img_dev, scaled);
   ```
   This transforms the 80x88 image to 160x176, giving NBIS much more room
   to work. Three other small-sensor drivers (aes3k, egis0570, elanspi) do this.

3. **Increase bz3_threshold to 20-24**: The current value of 12 is too low and
   will cause false positives. If image quality improves with the other fixes,
   threshold 20-24 should be achievable.

### Priority 2: Image Quality Improvements

4. **Re-enable and fix calibration subtraction**:
   - Capture baseline during activation (before FDT_DOWN, with no finger)
   - Subtract baseline from each scan
   - If the current `linear_subtract_5e0a()` function "destroys signal," the
     calibration image is likely being captured at the wrong time or wrong state
   - Try capturing calibration image during ACTIVATE_CALIBRATE state, after
     image TLS is established but before FDT_DOWN

5. **Replace unsharp mask with CLAHE or "thirds" normalization**:
   - The current unsharp mask (radius=3, strength=4.0) is aggressive
   - Try Elan's "thirds" percentile normalization first (simpler, proven)
   - If that doesn't help enough, implement CLAHE

6. **Handle dead zone pixels properly**:
   - Current: dead pixels set to 0xFF
   - Better: fill dead zone with the mean of active pixels (128 gray)
   - Or: pad with PAD_VALUE (128) which NBIS uses internally
   - NOTE: The code comments say "Do NOT fill dead zone with average (reduces
     minutiae to 0)" -- this needs retesting AFTER the FPI_IMAGE_PARTIAL fix

### Priority 3: Enrollment Quality

7. **Increase nr_enroll_stages from 10 to 15-20**: The 511 uses 20 stages.
   More enrollment images = more minutiae templates = better match probability.

8. **Add image diversity check**: Compare consecutive enrollment images and
   reject duplicates (see Section 7.3). Use MSE > threshold as diversity check.

9. **Add minimum minutiae count**: Reject enrollment images with fewer than
   3-4 minutiae. Currently any image with >= 1 minutia is accepted.

### Priority 4: Advanced Improvements

10. **Investigate SIGFM integration**: If NBIS continues to struggle despite
    all improvements, SIGFM (https://github.com/goodix-fp-linux-dev/sigfm)
    is specifically designed for 64x80 Goodix sensors. This would require
    significant refactoring of the matching pipeline.

11. **Multi-frame averaging**: Capture 2-3 frames per press and average them
    to reduce noise. This is not done by any current driver but could help
    with the 5e0a's noisy images.

---

## 9. Implementation Order and Expected Impact

| Step | Change | Effort | Expected Impact |
|------|--------|--------|-----------------|
| 1 | Set FPI_IMAGE_PARTIAL | 1 line | Medium: removes edge artifacts |
| 2 | 2x image upscale | 3 lines | HIGH: biggest single improvement for NBIS |
| 3 | Raise bz3_threshold to 20 | 1 line | Medium: reduces false positives |
| 4 | Fix calibration subtraction | ~20 lines | HIGH: removes fixed-pattern noise |
| 5 | Thirds normalization | ~30 lines | Medium: better contrast distribution |
| 6 | Increase enroll stages to 20 | 1 line | Medium: more templates to match against |
| 7 | Enrollment diversity check | ~30 lines | Medium: prevents duplicate templates |
| 8 | Min minutiae count | ~10 lines | Low-Medium: rejects garbage images |

**Start with steps 1 + 2 + 6. These are the lowest-effort, highest-impact changes.**

---

## 10. References and Sources

### Code Files Analyzed

- `/home/carmine/dev/libfprint-goodix/libfprint/drivers/goodixtls/goodix5xx.c` -- 511 base class
- `/home/carmine/dev/libfprint-goodix/libfprint/drivers/goodixtls/goodix511.c` -- 511 driver
- `/home/carmine/dev/libfprint-goodix/libfprint/drivers/goodixtls/goodix5e0a.c` -- 5e0a driver
- `/home/carmine/dev/libfprint-goodix/libfprint/drivers/elan.c` -- Elan driver
- `/home/carmine/dev/libfprint-goodix/libfprint/drivers/egis0570.c` -- Egis driver
- `/home/carmine/dev/libfprint-goodix/libfprint/drivers/aes3k.c` -- AES3k driver (smallest press sensor)
- `/home/carmine/dev/libfprint-goodix/libfprint/drivers/elanspi.c` -- Elan SPI driver
- `/home/carmine/dev/libfprint-goodix/libfprint/fpi-image-device.c` -- Enrollment flow
- `/home/carmine/dev/libfprint-goodix/libfprint/fpi-print.c` -- bz3 matching
- `/home/carmine/dev/libfprint-goodix/libfprint/fp-image.c` -- Minutiae detection
- `/home/carmine/dev/libfprint-goodix/libfprint/fpi-image.c` -- Image resize
- `/home/carmine/dev/libfprint-goodix/libfprint/nbis/include/lfs.h` -- NBIS parameters

### External Sources

- [SIGFM: Fingerprint matcher for low-resolution sensors](https://github.com/goodix-fp-linux-dev/sigfm)
- [goodix-fp-dump: Goodix sensor research](https://github.com/goodix-fp-linux-dev/goodix-fp-dump)
- [NIST NBIS Documentation](https://www.nist.gov/services-resources/software/nist-biometric-image-software-nbis)
- [NBIS User's Guide (NISTIR 7392)](https://nvlpubs.nist.gov/nistpubs/Legacy/IR/nistir7392.pdf)
- [CLAHE for fingerprint enhancement (ResearchGate)](https://www.researchgate.net/publication/44262481_Image_Enhancement_for_Fingerprint_Minutiae-Based_Algorithms_Using_CLAHE_Standard_Deviation_Analysis_and_Sliding_Neighborhood)
- [Latent fingerprint enhancement for minutiae detection (arXiv)](https://arxiv.org/html/2409.11802)
