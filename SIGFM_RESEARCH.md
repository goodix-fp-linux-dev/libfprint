# SIGFM Research Notes

Research date: 2026-03-22

## What is SIGFM?

SIGFM stands for **"SIFT Is Good For Matching"**. It is a fingerprint matching algorithm
designed specifically for **low-resolution sensors** (such as Goodix area sensors), intended
to replace or complement libfprint's default NBIS/bozorth3 matching algorithm.

- **Author**: Matthieu CHARETTE (@mpi3d)
- **License**: MIT (standalone library); LGPL 2.1 (libfprint integration)
- **Language**: C++ (100%)
- **Repositories**:
  - Standalone library: https://github.com/goodix-fp-linux-dev/sigfm
  - libfprint fork (bertin0): https://github.com/bertin0/libfprint-sigfm
  - Upstream MR: https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/418
  - Related issue: https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/485

## MR !418 Status

- **Title**: "add sigfm algorithm implementation"
- **Created**: Nov 24, 2022 by Natasha England-Elbro
- **Last updated**: Jun 24, 2024
- **Status**: **Draft / Open** (NOT merged as of 2026-03-22)
- **Stats**: 99 discussion threads, 18 commits, 37 file changes
- The CS9711 fork regularly rebases onto new libfprint releases + this MR,
  suggesting it remains pending upstream integration.

## Algorithm Details

SIGFM uses **OpenCV's SIFT** (Scale-Invariant Feature Transform) for keypoint detection
and descriptor extraction, then applies a multi-stage geometric validation pipeline.

### Processing Pipeline

1. **Image normalization**: Subtract a "clear" (background) image from the fingerprint
   image, then min-max normalize pixel intensities to 0-255.

2. **SIFT feature extraction**: `cv::SIFT::create()->detectAndCompute()` extracts
   keypoints and floating-point descriptors. Requires a minimum of 5 keypoints.

3. **Descriptor matching**: BFMatcher (Brute Force) with k-NN (k=2) finds candidate
   correspondences between enrolled and verification images.

4. **Lowe's ratio test**: Filter matches where `match[0].distance < 0.75 * match[1].distance`
   (DISTANCE_MATCH = 0.75).

5. **Vector length consistency**: For matched point pairs, verify that relative distances
   are proportional: `1 - min(len1,len2)/max(len1,len2) <= 0.05` (LENGTH_MATCH = 0.05).

6. **Angle consistency**: Using dot/cross products, verify that angles between vector
   pairs align within tolerance (ANGLE_MATCH = 0.05 for both sin and cos components).

7. **Final decision**: Returns match=true if >= 5 angle pairs satisfy both geometric
   conditions (MIN_MATCH = 5).

### Key Constants (from standalone library)

```
DISTANCE_MATCH = 0.75   # Lowe's ratio threshold
LENGTH_MATCH   = 0.05   # Vector length variance tolerance
ANGLE_MATCH    = 0.05   # Angle variance tolerance
MIN_MATCH      = 5      # Minimum matches required at each stage
```

### Data Structures

- `SigfmImgInfo` struct: Stores extracted SIFT keypoints and descriptors.
  Uses C++ `std::vector` internally but exposes a C-compatible API via opaque pointer.
- Supports binary serialization/deserialization (needed by libfprint to store enrolled prints).
- `structs::match`: Two `cv::Point2i` points representing a matched feature pair.
- `structs::angle`: Rotation info (cos, sin) plus two correlated matches.

## Image Format Requirements

| Property      | Value                                     |
|---------------|-------------------------------------------|
| Resolution    | **64x80 pixels** (primary target)         |
| Color depth   | **Grayscale** (single channel, 8-bit)     |
| Orientation   | Width x Height = 64 x 80                  |
| Format        | PGM or any format OpenCV can read         |
| Notes         | "May work with other resolutions"         |

For the Goodix 5110 sensor, the raw image is larger and needs cropping:
```
mogrify -crop 64x80+0+0 -format jpg ./fingerprint.pgm
```

**Important**: The algorithm loads images with `cv::IMREAD_GRAYSCALE`. The normalization
step (`(256 - clear) - image`) assumes 8-bit unsigned pixel values.

## Build Dependencies

SIGFM requires **OpenCV** with the **contrib modules** (for SIFT, which was patented
until 2020 and lives in `opencv_contrib/xfeatures2d`).

### Required packages

**Arch Linux**:
```
pacman -S opencv
```

**Ubuntu/Debian**:
```
apt install libopencv-dev python3-opencv libopencv-contrib406t64 doctest-dev
```

### Full libfprint build dependencies (with SIGFM)
```
# Core libfprint deps
meson libgusb-dev libcairo2-dev libgudev-1.0-dev libgirepository1.0-dev
libnss3-dev libssl-dev gtk-doc-tools

# SIGFM-specific deps
libopencv-dev doctest-dev cmake
```

### Build system
- Meson + Ninja
- C++17 standard (deliberately avoids C++20 for compiler compatibility)
- SIGFM is built as a peer library alongside NBIS within libfprint

```bash
meson setup build --prefix=/usr -Ddoc=false -Dgtk-examples=false
ninja -C build
sudo ninja -C build install
```

## Goodix Sensor Compatibility

### Tested sensors

| Sensor          | Resolution | Status                         |
|-----------------|------------|--------------------------------|
| Goodix 5110     | 80x64      | Tested, works (MR !418 ref)   |
| Goodix 55b4     | ~80x64     | Community tested               |
| Goodix 521d     | ~80x64     | Community tested               |
| Goodix 5117     | ~80x64     | Community tested               |
| **Goodix 5e0a** | 64x80      | **Not explicitly tested**      |

The algorithm was specifically designed for the Goodix line of low-resolution area
sensors. The 5e0a sensor at 64x80 resolution is within the target specification.

### Key advantage for Goodix sensors
- Works with **single capture images** (no swipe/stitching required)
- Designed for the exact resolution range these sensors produce
- NBIS/bozorth3 (libfprint default) performs poorly at these resolutions because
  minutiae extraction needs higher resolution images

## libfprint Integration Architecture

### Driver-side changes (minimal)

A driver only needs to set one field:
```c
img_class->algorithm = FPI_DEVICE_ALGO_SIGFM;
```

libfprint handles the rest internally.

### Internal changes (MR !418)

1. **New enum value**: `FPI_DEVICE_ALGO_SIGFM` added to `FpImageDeviceClass.algorithm`
   (default remains `FPI_DEVICE_ALGO_NBIS` for backward compatibility).

2. **FpImage extended**: Now stores `SigfmImgInfo*` alongside existing minutiae data.

3. **Threshold unification**: `bz3_threshold` renamed to `score_threshold` to serve
   both NBIS and SIGFM algorithms. This is a **breaking change** for existing drivers
   that reference `bz3_threshold`.

4. **Verification flow**: Runtime algorithm check determines whether to invoke
   bozorth3 or SIGFM matching.

5. **Serialization**: Enrolled fingerprint data includes SIGFM descriptors when that
   algorithm is active, stored in binary format.

## Comparison: SIGFM vs NBIS (bozorth3)

| Aspect               | NBIS/bozorth3              | SIGFM                        |
|----------------------|----------------------------|------------------------------|
| Algorithm type       | Minutiae-based             | SIFT keypoint-based          |
| Min resolution       | ~200+ DPI effective        | Works at 64x80 pixels        |
| External deps        | None (bundled)             | OpenCV + contrib             |
| Language             | C                          | C++17                        |
| Single-image match   | Needs good minutiae        | Designed for single capture  |
| Upstream status      | Included in libfprint      | Draft MR, not merged         |
| Maturity             | Decades of use             | Experimental                 |

## Implications for Goodix 5e0a Driver

1. **Resolution match**: The 5e0a produces 64x80 images, exactly SIGFM's target.
2. **NBIS likely fails**: At 64x80, bozorth3 minutiae extraction will be unreliable.
3. **Integration is simple**: One line change in the driver class.
4. **Dependency cost**: Adds OpenCV as a required dependency (significant).
5. **Not upstream yet**: MR !418 is still draft; using SIGFM means maintaining a
   fork or cherry-picking the SIGFM patches.
6. **bertin0's fork**: https://github.com/bertin0/libfprint-sigfm already has the
   integration and is the recommended base for experimental Goodix drivers.

## Open Questions

- What is the False Accept Rate (FAR) and False Reject Rate (FRR) for SIGFM?
  No formal accuracy metrics have been published.
- Does the "clear image" subtraction step require a separate calibration capture?
  The standalone demo uses one, but the libfprint integration may handle this differently.
- Will MR !418 ever be merged upstream, or will SIGFM remain a community fork?
- How does the `score_threshold` map to security levels in practice?

## Sources

- [SIGFM standalone library](https://github.com/goodix-fp-linux-dev/sigfm)
- [libfprint MR !418](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/418)
- [bertin0/libfprint-sigfm fork](https://github.com/bertin0/libfprint-sigfm)
- [libfprint issue #485](https://gitlab.freedesktop.org/libfprint/libfprint/-/issues/485)
- [Goodix 55b4 install gist](https://gist.github.com/d-k-bo/15e53eab53e2845e97746f5f8661b224)
- [Goodix FP Linux Dev org](https://github.com/goodix-fp-linux-dev)
- [OpenCV SIFT-FLANN fingerprint matching](https://opencv.org/blog/fingerprint-matching-using-opencv/)
