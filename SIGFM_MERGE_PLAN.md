# SIGFM Merge Plan

Merging SIGFM matching algorithm from `libfprint-sigfm` into `libfprint-goodix`.

Source: `/home/carmine/dev/libfprint-sigfm/`
Target: `/home/carmine/dev/libfprint-goodix/`

---

## Overview

SIGFM is an alternative fingerprint matching algorithm (SIFT-based, using OpenCV) that
replaces NBIS/bz3 minutiae matching. The SIGFM fork modifies libfprint's core matching
pipeline to support pluggable algorithms per device class. Each `FpImageDeviceClass` can
declare `algorithm = FPI_PRINT_SIGFM` or `FPI_PRINT_NBIS`.

**New dependency:** OpenCV 4 (>=4.5.0), doctest (>=2.0.0, for tests only)

---

## Files to Copy (New — no conflicts)

These files exist only in SIGFM and must be copied verbatim:

| # | Source Path | Destination |
|---|-----------|-------------|
| 1 | `libfprint/sigfm/sigfm.cpp` (216 lines) | `libfprint/sigfm/sigfm.cpp` |
| 2 | `libfprint/sigfm/sigfm.hpp` (98 lines) | `libfprint/sigfm/sigfm.hpp` |
| 3 | `libfprint/sigfm/binary.hpp` (215 lines) | `libfprint/sigfm/binary.hpp` |
| 4 | `libfprint/sigfm/img-info.hpp` (9 lines) | `libfprint/sigfm/img-info.hpp` |
| 5 | `libfprint/sigfm/tests.cpp` (141 lines) | `libfprint/sigfm/tests.cpp` |
| 6 | `libfprint/sigfm/tests-embedded.hpp` (5471 lines) | `libfprint/sigfm/tests-embedded.hpp` |
| 7 | `libfprint/sigfm/meson.build` | `libfprint/sigfm/meson.build` |

---

## Files to Merge (Both forks modified — manual merge required)

These files differ between forks. The SIGFM changes are **additive** (new code paths
alongside existing NBIS paths), so they should merge cleanly with our goodix5e0a additions.

### Priority 1: Core Algorithm Integration

| # | File | Nature of SIGFM Changes |
|---|------|------------------------|
| 8 | `libfprint/fpi-print.h` | Add `FPI_PRINT_SIGFM` to `FpiPrintType` enum; add `fpi_print_sigfm_match()` declaration |
| 9 | `libfprint/fpi-print.c` | Add SIGFM-aware paths in `fpi_print_add_from_image()`, `fpi_print_add_print()`, `fpi_print_set_type()`, `fpi_print_clear_type()`; add new `fpi_print_sigfm_match()` function |
| 10 | `libfprint/fpi-image.h` | Add `SigfmImgInfo *sigfm_info` field to `FpImage` struct; add `#include "sigfm/sigfm.hpp"` |
| 11 | `libfprint/fp-image.c` | Add `fp_image_extract_sigfm_info()` (new async extraction function), `fp_image_sigfm_extract_thread_func()`, callback, and `fp_image_get_sigfm_info()` accessor |
| 12 | `libfprint/fp-image.h` | Declare `fp_image_extract_sigfm_info()` and `fp_image_get_sigfm_info()` |

### Priority 2: Matching Pipeline

| # | File | Nature of SIGFM Changes |
|---|------|------------------------|
| 13 | `libfprint/fpi-image-device.h` | Add `FpiImageDeviceAlgorithm` enum; add `algorithm` field to `FpImageDeviceClass`; add `#include "fpi-print.h"` |
| 14 | `libfprint/fpi-image-device.c` | Route capture/verify/identify through SIGFM or NBIS based on `priv->algorithm`; call `fp_image_extract_sigfm_info()` instead of `fp_image_detect_minutiae()` when SIGFM |
| 15 | `libfprint/fp-image-device.c` | Add `algorithm` field to private struct; add `#include "fpi-print.h"` |
| 16 | `libfprint/fp-image-device-private.h` | Add `FpiPrintType algorithm` field to private data |

### Priority 3: Print Serialization

| # | File | Nature of SIGFM Changes |
|---|------|------------------------|
| 17 | `libfprint/fp-print.c` | Add SIGFM serialization/deserialization paths (binary format via `sigfm_serialize_binary`/`sigfm_deserialize_binary`) in `fp_print_serialize()` and `fp_print_deserialize()` |

### Priority 4: Build System

| # | File | Nature of SIGFM Changes |
|---|------|------------------------|
| 18 | `libfprint/meson.build` | Add `subdir('sigfm')`, link `libsigfm`, add to `priv_deps` |

### Priority 5: Minor / Cosmetic

| # | File | Nature of SIGFM Changes |
|---|------|------------------------|
| 19 | `libfprint/fprint-list-udev-hwdb.c` | Adds USB ID `0x27c6:0x521d` (for 52xd device — we can skip this) |

---

## Files with CONFLICTING Changes (Driver Layer)

The SIGFM fork has a **completely different codebase** for the goodixtls driver layer.
These are NOT simple merges — the SIGFM fork reformatted all code, removed copyright
headers, restructured the TLS implementation, and does not have our goodix5e0a driver.

| # | File | Conflict Type |
|---|------|--------------|
| — | `drivers/goodixtls/goodix.c` | Reformatted + restructured TLS (socket-based vs our pipe-based) |
| — | `drivers/goodixtls/goodix.h` | Reformatted + removed typedefs |
| — | `drivers/goodixtls/goodixtls.c` | Completely different TLS server implementation |
| — | `drivers/goodixtls/goodixtls.h` | Restructured TLS server struct |
| — | `drivers/goodixtls/goodix_proto.c` | Reformatted only |
| — | `drivers/goodixtls/goodix_proto.h` | Reformatted + removed `GOODIX_CMD_POV_IMAGE_CHECK` |
| — | `drivers/goodixtls/goodix511.c` | Different parent type (no goodix5xx base), different frame handling |
| — | `drivers/goodixtls/goodix511.h` | Reformatted |

**Recommendation:** Do NOT merge driver-layer files. Our fork's driver code is specific
to the 5e0a device and already works. The SIGFM driver changes are for 52xd/53xd devices
and are incompatible with our goodix5xx abstraction layer.

---

## Merge Order (Recommended)

```
Phase 1 — Copy SIGFM library (no conflicts):
  cp -r libfprint-sigfm/libfprint/sigfm/ libfprint-goodix/libfprint/sigfm/

Phase 2 — Core types (small, foundational changes):
  Merge: fpi-print.h        (add enum value + declaration)
  Merge: fpi-image.h        (add sigfm_info field)
  Merge: fp-image.h         (add declarations)
  Merge: fpi-image-device.h (add algorithm enum + field)
  Merge: fp-image-device-private.h (add algorithm field)

Phase 3 — Implementation files:
  Merge: fpi-print.c        (add SIGFM match + print handling)
  Merge: fp-image.c         (add SIGFM extraction functions)
  Merge: fpi-image-device.c (add SIGFM routing in capture/verify/identify)
  Merge: fp-image-device.c  (add algorithm to private init)
  Merge: fp-print.c         (add SIGFM serialization)

Phase 4 — Build system:
  Merge: libfprint/meson.build (add sigfm subdir + link)

Phase 5 — Configure our driver to use SIGFM (optional):
  In goodix5e0a.c class_init: set algorithm = FPI_PRINT_SIGFM
  (or keep FPI_PRINT_NBIS if NBIS works well enough for 5e0a)
```

---

## Key Considerations

1. **OpenCV dependency**: SIGFM requires `opencv4 >= 4.5.0`. This is a heavy dependency.
   Ensure it is available on the target system or make it optional.

2. **C++ in build**: `sigfm.cpp` is C++ code. The meson.build creates a static library
   that is linked into the C codebase. The header uses `extern "C"` for compatibility.

3. **Our driver files are safe**: The goodix5e0a.c/h and goodix5xx.c/h files only exist
   in our fork. No SIGFM changes touch them.

4. **Algorithm selection is per-device-class**: Each driver chooses its algorithm in its
   class init. Default is NBIS (value 0), so existing drivers are unaffected.

5. **Print format incompatibility**: SIGFM prints use a different serialization format.
   Prints enrolled with NBIS cannot be verified with SIGFM and vice versa. Switching
   algorithm requires re-enrollment.

6. **The SIGFM fork's driver reformatting is a trap**: Nearly every driver file shows as
   "different" but most changes are just code reformatting (brace style, pointer spacing).
   Do not merge these — they provide no functional value and would destroy our code style.
