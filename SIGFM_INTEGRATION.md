# SIGFM Integration Analysis for Goodix 5e0a Driver

## What is SIGFM?

SIGFM (SIFT-based Fingerprint Matcher) is an alternative fingerprint matching
algorithm for libfprint that uses OpenCV's SIFT (Scale-Invariant Feature
Transform) instead of the default NBIS/Bozorth3 minutiae-based matcher.

Source: https://github.com/bertin0/libfprint-sigfm (branch: `unstable`)

Authors:
- Matthieu CHARETTE <matthieu.charette@gmail.com>
- Natasha England-Elbro <ashenglandelbro@protonmail.com>
- Timur Mangliev <tigrmango@gmail.com>

License: LGPL-2.1+

## Why SIGFM Matters for Small Sensors

The default NBIS/Bozorth3 matcher relies on minutiae (ridge endings, bifurcations).
Small sensors like the Goodix 5e0a (80x88 pixels) produce very few minutiae
(typically 5-12), making Bozorth3 unreliable -- match scores are low and
the false-reject rate is high.

SIGFM uses SIFT keypoints and descriptors instead, which can detect many more
features even in small images. This should significantly improve matching
accuracy for the 5e0a sensor.

## How SIGFM Works

### Core Algorithm (sigfm.cpp)

1. **Feature Extraction** (`sigfm_extract`):
   - Takes raw grayscale pixel data (width x height)
   - Creates an OpenCV `cv::Mat` (CV_8UC1)
   - Runs `cv::SIFT::create()->detectAndCompute()` to extract keypoints and descriptors
   - Returns an `SigfmImgInfo` struct containing `vector<cv::KeyPoint>` and `cv::Mat` descriptors

2. **Matching** (`sigfm_match_score`):
   - Uses `cv::BFMatcher` with k-nearest-neighbors (k=2) to find descriptor matches
   - Applies Lowe's ratio test (distance_match = 0.75) to filter good matches
   - Requires minimum 5 good matches (`min_match`)
   - Performs geometric verification:
     - Computes vectors between matched point pairs
     - Checks length consistency (length_match = 0.05 tolerance)
     - Computes angles between vector pairs
     - Checks angle consistency (angle_match = 0.05 tolerance)
   - Returns count of geometrically consistent angle pairs as the score
   - Returns 0 if not enough matches/angles, -1 on error

3. **Serialization** (`sigfm_serialize_binary` / `sigfm_deserialize_binary`):
   - Custom binary serialization in `binary.hpp`
   - Serializes cv::KeyPoint fields (class_id, angle, octave, response, size, pt)
   - Serializes cv::Mat (type, rows, cols, raw data)
   - Used for storing enrolled prints to disk

4. **Quality Check**:
   - `sigfm_keypoints_count` returns keypoint count
   - The extraction thread rejects images with fewer than 25 keypoints

### Key Constants
```
distance_match = 0.75   (Lowe's ratio test threshold)
length_match   = 0.05   (vector length tolerance)
angle_match    = 0.05   (angle consistency tolerance)
min_match      = 5      (minimum good matches required)
min_keypoints  = 25     (minimum keypoints for a valid image)
```

## Files Added by SIGFM

All under `libfprint/sigfm/`:

| File | Purpose |
|------|---------|
| `sigfm.hpp` | C API header -- extern "C" function declarations |
| `sigfm.cpp` | Core SIFT extraction and matching algorithm |
| `binary.hpp` | Template-based binary serialization for cv::Mat, cv::KeyPoint, etc. |
| `img-info.hpp` | `SigfmImgInfo` struct definition (keypoints + descriptors) |
| `meson.build` | Build config for libsigfm static library |
| `tests.cpp` | Unit tests (requires doctest framework) |
| `tests-embedded.hpp` | Embedded test data (~395 KB) |

## Files Modified by SIGFM

### 1. `libfprint/fpi-print.h` -- Print Type Enum
Added `FPI_PRINT_SIGFM` to `FpiPrintType` enum:
```c
typedef enum {
  FPI_PRINT_UNDEFINED = 0,
  FPI_PRINT_RAW,
  FPI_PRINT_NBIS,
  FPI_PRINT_SIGFM,     // <-- NEW
} FpiPrintType;
```
Added function declaration:
```c
FpiMatchResult fpi_print_sigfm_match (FpPrint *template, FpPrint *print,
                                      gint bz3_threshold, GError **error);
```

### 2. `libfprint/fpi-print.c` -- Print Operations
- `fpi_print_set_type`: Extended to handle `FPI_PRINT_SIGFM` -- creates
  `GPtrArray` with `sigfm_free_info` as the free function (instead of `g_free`
  used for NBIS).
- `fpi_print_add_print`: Extended to accept SIGFM prints (copies via
  `sigfm_copy_info`).
- `fpi_print_add_from_image`: For SIGFM, calls `fp_image_get_sigfm_info()`
  and adds the info directly to the print array.
- `fpi_print_sigfm_match`: New function -- iterates enrolled prints, calls
  `sigfm_match_score()` for each, returns SUCCESS if any score >= threshold.

### 3. `libfprint/fpi-image-device.h` -- Algorithm Selection
Added enum and class field:
```c
typedef enum {
  FPI_DEVICE_ALGO_NBIS = FPI_PRINT_NBIS,
  FPI_DEVICE_ALGO_SIGFM = FPI_PRINT_SIGFM,
} FpiImageDeviceAlgorithm;
```
Added `algorithm` field to `FpImageDeviceClass`:
```c
struct _FpImageDeviceClass {
  FpDeviceClass           parent_class;
  gint                    bz3_threshold;
  gint                    img_width;
  gint                    img_height;
  FpiImageDeviceAlgorithm algorithm;     // <-- NEW
  ...
};
```

### 4. `libfprint/fp-image-device-private.h` -- Private State
Added `algorithm` field to `FpImageDevicePrivate`:
```c
typedef struct {
  ...
  gint                bz3_threshold;
  FpiPrintType        algorithm;       // <-- NEW
} FpImageDevicePrivate;
```

### 5. `libfprint/fp-image-device.c` -- Initialization
In `constructed()`:
```c
priv->algorithm = FPI_PRINT_NBIS;    // default
if (cls->algorithm > 0)
  priv->algorithm = cls->algorithm;   // override from driver class
```
In enroll activation: uses `priv->algorithm` instead of hardcoded `FPI_PRINT_NBIS`.

### 6. `libfprint/fpi-image-device.c` -- Capture/Match Flow
**Image capture**: When algorithm is SIGFM, calls `fp_image_extract_sigfm_info()`
instead of `fp_image_detect_minutiae()`.

**Verify**: Dispatches to `fpi_print_sigfm_match()` or `fpi_print_bz3_match()`
based on `priv->algorithm`.

**Identify**: Same dispatch logic for the identify loop.

**Enroll**: Uses `priv->algorithm` for `fpi_print_set_type()`.

### 7. `libfprint/fp-image.h` / `libfprint/fpi-image.h` -- Image Extensions
Added to FpImage struct:
```c
SigfmImgInfo *sigfm_info;
```
New public functions:
```c
void fp_image_extract_sigfm_info(FpImage *self, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data);
SigfmImgInfo *fp_image_get_sigfm_info(FpImage *self);
```

### 8. `libfprint/fp-image.c` -- SIGFM Extraction
- `ExtractSigfmData` struct for async extraction
- `fp_image_sigfm_extract_thread_func`: Runs SIFT extraction in a thread,
  rejects images with < 25 keypoints
- `fp_image_sigfm_extract_cb`: Stores result in FpImage
- `fp_image_extract_sigfm_info`: Public async entry point
- `fp_image_get_sigfm_info`: Getter for extracted info

### 9. `libfprint/fp-print.c` -- Serialization/Deserialization
Extended `fp_print_serialize` and `fp_print_deserialize` to handle
`FPI_PRINT_SIGFM` type using `sigfm_serialize_binary` / `sigfm_deserialize_binary`.
The serialized format wraps SIGFM binary data in GVariant `(a(ay))`.

### 10. `libfprint/meson.build` -- Build Integration
- Added `subdir('sigfm')` before nbis includes
- Changed `link_with: libnbis` to `link_with: [libnbis, libsigfm]`
- Added libsigfm to deps for libfprint-private

## Dependencies

| Dependency | Version | Status on System |
|-----------|---------|-----------------|
| OpenCV 4 | >= 4.5.0 | INSTALLED (4.13.0) |
| doctest | >= 2.0.0, < 3.0.0 | NOT INSTALLED (only needed for SIGFM unit tests, can skip) |

OpenCV modules used: `core`, `features2d`, `imgcodecs`, `opencv` (the meta-header).
The SIFT implementation is in `opencv2/features2d.hpp` (moved from nonfree to
main library in OpenCV 4.4+).

## How a Driver Enables SIGFM

In the driver's `class_init` function, set the `algorithm` field:

```c
static void
fpi_device_goodixtls5e0a_class_init (FpiDeviceGoodixTls5e0aClass *class)
{
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS (class);

  // Use SIGFM instead of NBIS/Bozorth3
  img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM;

  // The bz3_threshold field is reused as the SIGFM score threshold
  img_dev_class->bz3_threshold = 24;

  ...
}
```

That's it from the driver side. The framework handles everything else:
- Calls `fp_image_extract_sigfm_info()` instead of `fp_image_detect_minutiae()`
- Stores SIGFM data (keypoints + descriptors) in the print
- Uses `fpi_print_sigfm_match()` for verify/identify
- Serializes/deserializes SIGFM data for storage

## Conflict Analysis

### Files We Would Need to Modify

Our fork is based on a different (older) version of libfprint than SIGFM's fork.
The core files differ in many ways unrelated to SIGFM. We cannot just copy files
wholesale. Instead we need to surgically add the SIGFM changes to our versions.

**No conflicts (new files, just copy):**
- `libfprint/sigfm/` -- entire directory (6 source files + meson.build)

**Surgical changes needed (add SIGFM code to our existing files):**
1. `libfprint/fpi-print.h` -- Add `FPI_PRINT_SIGFM` enum value, add function decl
2. `libfprint/fpi-print.c` -- Add SIGFM handling in set_type, add_print, add_from_image; add `fpi_print_sigfm_match()`
3. `libfprint/fpi-image-device.h` -- Add `FpiImageDeviceAlgorithm` enum, add `algorithm` field to class
4. `libfprint/fp-image-device-private.h` -- Add `algorithm` field to private struct
5. `libfprint/fp-image-device.c` -- Initialize `priv->algorithm` from class in `constructed()`
6. `libfprint/fpi-image-device.c` -- Add SIGFM dispatch in capture/verify/identify
7. `libfprint/fp-image.h` -- Add `fp_image_extract_sigfm_info()` and `fp_image_get_sigfm_info()` decls
8. `libfprint/fpi-image.h` -- Add `SigfmImgInfo *sigfm_info` to FpImage struct
9. `libfprint/fp-image.c` -- Add SIGFM extraction thread, callback, public functions
10. `libfprint/fp-print.c` -- Add SIGFM serialization/deserialization
11. `libfprint/meson.build` -- Add sigfm subdir, link libsigfm
12. `libfprint/drivers/goodixtls/goodix5e0a.c` -- Set `img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM`

### Potential Issues

1. **C++ Dependency**: SIGFM is written in C++ (sigfm.cpp, binary.hpp). The libfprint
   core is C. The SIGFM headers use `extern "C"` blocks properly, so linking works.
   However, the meson build must use C++ compilation for sigfm.cpp.

2. **OpenCV Size**: OpenCV is a large dependency. Users will need it installed.
   Consider making it optional via a meson option.

3. **Threshold Tuning**: The `bz3_threshold` field is reused for SIGFM scoring.
   The appropriate threshold for 80x88 images needs empirical testing. The SIGFM
   score represents the count of geometrically consistent angle pairs, which
   scales very differently from Bozorth3 scores. Start with a low threshold
   (e.g., 10-20) and adjust based on testing.

4. **Minimum Keypoints**: SIGFM rejects images with < 25 keypoints. For our
   small 80x88 sensor, SIFT may sometimes find fewer keypoints depending on
   image quality. This threshold may need adjustment.

5. **Enrolled Print Compatibility**: Switching from NBIS to SIGFM changes the
   print data format. Previously enrolled prints (type FPI_PRINT_NBIS) will NOT
   work with SIGFM matching. Users must re-enroll after switching.

6. **Performance**: SIFT extraction uses OpenCV which spawns threads. On the
   test system this should be fine, but it's heavier than NBIS minutiae detection.

## Minimal File Set to Copy

From `libfprint-sigfm` (unstable branch):

```
libfprint/sigfm/sigfm.hpp
libfprint/sigfm/sigfm.cpp
libfprint/sigfm/binary.hpp
libfprint/sigfm/img-info.hpp
libfprint/sigfm/meson.build
```

Optional (tests only, needs doctest):
```
libfprint/sigfm/tests.cpp
libfprint/sigfm/tests-embedded.hpp
```

## Implementation Plan

### Phase 1: Copy SIGFM library
- Copy `libfprint/sigfm/` directory (5 core files, skip tests for now)
- Modify `libfprint/sigfm/meson.build` to remove doctest dependency
- Add `subdir('sigfm')` and link libsigfm in `libfprint/meson.build`

### Phase 2: Core framework changes
- Add `FPI_PRINT_SIGFM` to `fpi-print.h`
- Add `FpiImageDeviceAlgorithm` enum and `algorithm` field to `fpi-image-device.h`
- Add `algorithm` field to `fp-image-device-private.h`
- Add algorithm initialization in `fp-image-device.c`
- Add SIGFM dispatch in `fpi-image-device.c`
- Add SIGFM extraction functions to `fp-image.c` / `fp-image.h` / `fpi-image.h`
- Add SIGFM print operations to `fpi-print.c`
- Add SIGFM serialization to `fp-print.c`

### Phase 3: Enable in driver
- Set `img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM` in goodix5e0a class_init
- Tune threshold value empirically
- Test enroll/verify/identify cycles

### Phase 4: Testing and tuning
- Test with real fingerprints on the 5e0a sensor
- Adjust `bz3_threshold` for SIGFM score scale
- Adjust minimum keypoint count if needed for small sensor
- Verify serialization/deserialization works (re-enroll test)
