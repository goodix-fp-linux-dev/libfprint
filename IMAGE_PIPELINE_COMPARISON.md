# Image Processing Pipeline Comparison: goodix511 vs goodix5e0a

## 1. Pipeline Overview

### goodix511 (working) — full pipeline in goodix5xx.c + goodix511.c

```
raw_data (from TLS)
  -> goodixtls5xx_decode_frame()      [4/6-byte unpack, skips 8-byte header, stops 5 bytes before end]
  -> linear_subtract_inplace()         [subtract calibration baseline — signed, with 0xFFFF bias]
  -> goodixtls5xx_squash_frame_linear() [16-bit -> 8-bit via min/max normalization]
  -> crop_frame()                      [88-wide -> 64-wide crop, sets FPI_IMAGE_PARTIAL]
  -> fpi_image_device_image_captured()
```

### goodix5e0a (failing) — standalone pipeline in goodix5e0a.c

```
raw_data (from TLS)
  -> goodix_5e0a_decode_frame()        [4/6-byte unpack, NO header skip, processes all bytes]
  -> (calibration subtraction DISABLED)
  -> goodix_5e0a_squash_frame()        [16-bit -> 8-bit, skips zero pixels, fills dead zone with 0xFF]
  -> memcpy into FpImage               [no crop, no FPI_IMAGE_PARTIAL flag]
  -> fpi_image_device_image_captured()
```

---

## 2. Detailed Differences

### 2.1 decode_frame — Header/Footer Handling

**511 (goodix5xx.c:425-437):**
```c
for (int i = 8; i != frame_size - 5; i += 6)  // skip 8-byte header, stop 5 bytes before end
```
- Starts at byte offset **8** (skips 8-byte header)
- Stops at `frame_size - 5` (skips 5-byte footer)
- `GOODIX511_RAW_FRAME_SIZE = 8 + (80*88)/4*6 + 5 = 10573`

**5e0a (goodix5e0a.c:148-155):**
```c
for (guint32 i = 0; i < raw_size; i += 6)  // NO header skip, processes ALL bytes
```
- Starts at byte offset **0** (no header skip)
- Processes the entire `raw_size` (10560 bytes)
- `GOODIX_5E0A_RAW_FRAME_SIZE = 10560` (comment says "no 8+5 header/footer")

**FINDING:** The 5e0a header comment claims no header/footer. This could be correct if the 5e0a protocol strips them before delivery, OR it could be wrong and the first 8 bytes are garbage being decoded as pixels. **This needs verification with actual packet dumps.** If the raw data DOES have a header, the first 5-6 decoded pixels will be corrupted.

**Verification method:** Check `/tmp/goodix_5e0a_raw.bin` — if the first 8 bytes are NOT valid pixel data (e.g., they look like a packet header with length/type fields), then the 5e0a decode needs to skip them too. Expected raw frame size with header would be 10573 (same as 511).

### 2.2 Calibration Subtraction

**511 (goodix5xx.c:311-316):**
```c
static void linear_subtract_inplace(GoodixTls5xxPix* src, GoodixTls5xxPix* by, guint16 len) {
  const guint16 max = -1;  // 0xFFFF
  for (guint16 n = 0; n != len; ++n) {
    src[n] = MAX(0, max - ((max - src[n]) - (max - by[n])));
  }
}
```
This simplifies to: `src[n] = MAX(0, 0xFFFF + src[n] - by[n])`.
Since pixel values are 12-bit (0-4095), the result is always `src[n] - by[n] + 65535`, which is always positive. This is a **signed subtraction with bias** — negative differences are preserved (as values near 65535) and the subsequent squash_frame_linear normalizes everything.

- Calibration is **always performed** (captured during CALIBRATION_STAGE_GET_IMG)
- The calibration frame is captured at activation with no finger on sensor

**5e0a (goodix5e0a.c:98-111):**
```c
static void linear_subtract_5e0a(Goodix5e0aPix *src, const Goodix5e0aPix *baseline, guint16 len) {
  for (guint16 i = 0; i < len; i++) {
    if (src[i] > baseline[i])
      src[i] = src[i] - baseline[i];
    else
      src[i] = 0;
  }
}
```
This is a **clamped subtraction** — negative differences are clipped to 0, losing information.

- Calibration is **DISABLED** (line 349: `fpi_ssm_next_state(ssm)` — skips calibration)
- Subtraction call is also commented out in scan_on_read_img_5e0a (line 404)

**ISSUE:** Even if calibration were enabled, the subtraction algorithm is wrong compared to the 511. The 511's biased subtraction preserves relative differences for the squash step; the 5e0a's clamped subtraction destroys them. If calibration is re-enabled, the 511's algorithm should be used.

### 2.3 Squash (16-bit to 8-bit Conversion)

**511 (goodix5xx.c:288-310):**
```c
void goodixtls5xx_squash_frame_linear(GoodixTls5xxPix *frame, guint8 *squashed, guint16 frame_size) {
  // Find global min and max (including zero pixels)
  // Map: squashed[i] = (pix - min) * 0xff / (max - min)
  // Zero pixels map to 0
}
```
- All pixels participate in min/max calculation
- Zero-value pixels become 0 (black)
- Simple linear normalization

**5e0a (goodix5e0a.c:158-198):**
```c
static void goodix_5e0a_squash_frame(Goodix5e0aPix *frame, guint8 *squashed, guint16 frame_size) {
  // Skip zero pixels when computing min/max
  // Zero pixels are filled with 0xFF (white)
  // Non-zero pixels: linear normalization same as 511
}
```
- Zero pixels are **excluded** from min/max (treats them as dead zone)
- Zero pixels become **0xFF** (white/background) instead of 0 (black)
- Otherwise same linear normalization

**ISSUE:** The zero-pixel handling makes sense for a circular sensor with dead zones. However, filling dead pixels with 0xFF means NBIS sees them as background, which is correct only if the fingerprint ridges are dark. If the image contrast is inverted, this could cause problems. Check if FPI_IMAGE_COLORS_INVERTED is needed.

### 2.4 Cropping and FPI_IMAGE_PARTIAL

**511 (goodix511.c:279-294):**
```c
static FpImage *crop_frame(guint8 *frame) {
  FpImage *img = fp_image_new(GOODIX511_WIDTH, GOODIX511_HEIGHT);  // 64x80
  img->flags |= FPI_IMAGE_PARTIAL;
  for (int y = 0; y != GOODIX511_HEIGHT; ++y) {
    for (int x = 0; x != GOODIX511_WIDTH; ++x) {
      const int idx = x + y * GOODIX511_SCAN_WIDTH;     // reads from 88-wide
      img->data[x + y * GOODIX511_WIDTH] = frame[idx];  // writes to 64-wide
    }
  }
  return img;
}
```
- Scan width is **88**, output width is **64** (crops 24 columns from the right)
- Sets **FPI_IMAGE_PARTIAL** flag
- `FPI_IMAGE_PARTIAL` causes `lfsparms->remove_perimeter_pts = TRUE` in NBIS, which removes minutiae detected at image edges — critical for partial images

**5e0a (goodix5e0a.c:411-417):**
```c
FpImage *img = fp_image_new(GOODIX_5E0A_HEIGHT, GOODIX_5E0A_WIDTH);  // 88x80
// No flags set — no FPI_IMAGE_PARTIAL
memcpy(img->data, squashed, GOODIX_5E0A_FRAME_SIZE);
```
- No cropping — full 88x80 frame is used
- **FPI_IMAGE_PARTIAL is NOT set**
- NBIS will NOT remove perimeter minutiae, leading to false minutiae at edges

**ISSUE:** The 5e0a should almost certainly set `FPI_IMAGE_PARTIAL` since it's a small press sensor. Without it, NBIS finds spurious minutiae along the border (especially in dead zones or at transitions to zero-fill areas). This alone could cause matching failures.

### 2.5 Image Dimensions and Orientation

**511:**
- `img_width = 64` (GOODIX511_WIDTH)
- `img_height = 80` (GOODIX511_HEIGHT)
- `scan_width = 88` (GOODIX511_SCAN_WIDTH)
- Image created as `fp_image_new(64, 80)` — width=64, height=80

**5e0a:**
- `GOODIX_5E0A_WIDTH = 80`, `GOODIX_5E0A_HEIGHT = 88`
- `img_width = GOODIX_5E0A_HEIGHT = 88` (note: HEIGHT used for img_width!)
- `img_height = GOODIX_5E0A_WIDTH = 80` (note: WIDTH used for img_height!)
- Image created as `fp_image_new(GOODIX_5E0A_HEIGHT, GOODIX_5E0A_WIDTH)` = `fp_image_new(88, 80)`

**ISSUE:** The naming is confusing but appears intentionally swapped — the comment says "Sensor frame layout: 88 pixels wide, 80 pixels tall". The image is 88 wide, 80 tall. The `img_dev_class->img_width` and `img_height` are set accordingly (88 and 80). This seems correct IF the sensor data is indeed 88 columns by 80 rows. But verify that the class fields match the FpImage constructor args.

### 2.6 ppmm (Pixels Per Millimeter)

**511:**
- **ppmm is NOT set** anywhere in the driver
- FpImage is created via `fp_image_new()` which zero-initializes the GObject
- Therefore **ppmm = 0.0**
- NBIS receives `ppmm=0.0` and uses it for quality assessment (in `combined_minutia_quality`)
- This means NBIS's quality radius calculation (`RADIUS_MM * ppmm`) gives radius_pix = 0
- Despite this, the 511 driver **works** — NBIS still finds minutiae, the quality scoring is just degraded

**5e0a:**
- **ppmm = 19.685** (set explicitly, 500 DPI)
- This tells NBIS the image is high-resolution, affecting quality map calculations

**FINDING:** The 511 works with ppmm=0, so the 5e0a's ppmm=19.685 is unlikely to be the root cause of failures, but it does change NBIS behavior. Setting it is technically more correct. However, if the actual sensor resolution is different from 500 DPI, this could cause NBIS to miscalculate feature sizes.

**For a 5e0a sensor with 88x80 pixel area:** If the physical sensor area is approximately 4.5mm x 4mm, the real resolution would be ~88/4.5 = 19.6 ppmm, making the 19.685 value correct. If the physical area is different, this value should be adjusted.

### 2.7 bz3_threshold

**511:** `bz3_threshold = 24`
**5e0a:** `bz3_threshold = 5`

**ISSUE:** The bz3_threshold controls how strict the bozorth3 matcher is. A value of 5 is very permissive and may cause false positives, but it won't cause false negatives (failure to match). The 511's value of 24 is much stricter. The low threshold of 5 suggests the developer was trying to compensate for poor image quality by lowering the match bar.

### 2.8 nr_enroll_stages

**511:** `nr_enroll_stages = 20`
**5e0a:** `nr_enroll_stages = 10`

This affects how many samples are collected during enrollment. Fewer stages = faster enrollment but potentially less robust prints.

---

## 3. Summary of Required Changes in 5e0a

### CRITICAL (likely causing matching failures):

1. **Set FPI_IMAGE_PARTIAL flag** — Without this, NBIS finds false minutiae at image borders, corrupting the minutiae template and causing match failures.
   ```c
   img->flags |= FPI_IMAGE_PARTIAL;
   ```

2. **Verify header offset in decode_frame** — Check if raw image data has an 8-byte header like the 511. If `/tmp/goodix_5e0a_raw.bin` starts with non-pixel bytes, change the decode loop to start at offset 8 and adjust RAW_FRAME_SIZE to 10573.

3. **Enable calibration subtraction** — The 511 ALWAYS subtracts a calibration baseline. Re-enable calibration capture and subtraction in the 5e0a, using the 511's biased algorithm:
   ```c
   // Use the 511's algorithm, not clamped subtraction:
   src[n] = MAX(0, 0xFFFF + src[n] - by[n]);
   // This is equivalent to:
   src[n] = MAX(0, max - ((max - src[n]) - (max - by[n])));
   ```

### IMPORTANT (may affect quality):

4. **Consider cropping** — The 511 crops from 88 to 64 pixels wide, removing 24 columns of edge/dead pixels. If the 5e0a sensor also has dead columns, crop them. The current approach of filling dead pixels with 0xFF may work but is less clean.

5. **Increase bz3_threshold** — A value of 5 is very low. Consider 12-20 range once image quality improves.

6. **Increase nr_enroll_stages** — 10 may not be enough for reliable enrollment. Consider 15-20.

### MINOR / VERIFY:

7. **ppmm value** — The 19.685 value is reasonable if it matches the physical sensor DPI. The 511 leaves it at 0 and still works. Could try setting to 0 to match 511 behavior and rule out NBIS quality calculation issues.

8. **Squash zero-pixel handling** — The 0xFF fill for dead pixels is reasonable but different from the 511. If calibration subtraction is enabled and working, there should be fewer zero pixels, and the standard squash algorithm may work better.

9. **Image orientation** — Verify that width/height are not transposed. The naming is confusing (`GOODIX_5E0A_WIDTH=80`, `GOODIX_5E0A_HEIGHT=88`). The FpImage is created as 88x80 (width=88, height=80). Make sure this matches the actual sensor data layout.

---

## 4. Recommended Fix Order

1. Add `FPI_IMAGE_PARTIAL` flag (quick, high-impact fix)
2. Dump and examine raw frame to verify header presence/absence
3. Enable calibration with the 511's biased subtraction algorithm
4. Test matching — if still failing, try cropping dead columns
5. Fine-tune bz3_threshold once images are good
