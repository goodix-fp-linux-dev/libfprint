# Reverse Engineering: Goodix Windows Driver Image Processing Pipeline

Reverse engineered from:
- `Wbdi.dll` (2.4MB) - main WBDI driver, contains preprocessor + algorithm library
- `GoodixEngineAdapter.dll` (1.3MB) - engine adapter, bridge to SGX enclave
- `AdapterEnclave.signed.dll` (1.9MB) - SGX enclave, matching + template management
- `WbdiEnclave.signed.dll` (2.1MB) - SGX enclave, preprocessing + sensor data

Driver version: V3.0.141.150_21H1

## 1. Overall Architecture

```
Sensor (USB)
    |
    v
Wbdi.dll (UMDF driver)
    |-- Raw data decryption (TLS from sensor)
    |-- ImageRestructMilanG / ImageRestructInterface (raw pixel decoding)
    |-- PreProcessorUnify (algmoduleunify.c) -- wrapper
    |       |-- preprocessor() export -- dispatches to:
    |           |-- preprocess() (x64\src\preprocess.c) -- main pipeline
    |               |-- auto_calibration (Kr/B computation)
    |               |-- gaussian_blur
    |               |-- quality/coverage computation (img_quality.c)
    |
    v
GoodixEngineAdapter.dll
    |-- EngineAdapterAcceptSampleData (quality/coverage check)
    |-- EngineAdapterIdentifyFeatureSet / CreateEnrollment
    |       |-- Sends preprocessed BMP to AdapterEnclave via SGX local attestation
    |
    v
AdapterEnclave.signed.dll (SGX)
    |-- enrolAddImage / identifyImage
    |       |-- image_extension_net (neural network upscaling!)
    |       |-- getFeature (feature extraction)
    |       |       |-- enhance.c (image enhancement)
    |       |       |-- gaussian_blur.c
    |       |       |-- fingerFeatureRecognition / fingerFeatureRegister
    |       |-- identifyImage (matching, returns score)
    |       |-- templateStudy (template learning/update)
    |
    v
Result (match/no-match, score)
```

## 2. PreProcessorUnify (algmoduleunify.c)

**Location**: `fcn.18003aff4` in Wbdi.dll (2866 bytes)
**Source**: `d:\project\master\winfpcode2\driver\common\sgx\algmoduleunify.c`

### Function signature (reconstructed):
```c
int PreProcessorUnify(
    RawImageData* rawInput,       // arg1: raw sensor data + header
    void* sensorConfig,           // arg2
    void* captureContext,         // arg3
    void* logContext,             // arg4
    void* outputBmpBuffer,       // arg_1f0h: receives processed BMP
    uint32_t outputBmpSize,      // arg_1f8h
    QualityCoverage* qualCov,    // arg_200h: output {quality, coverage}
    bool calibFlag1,             // arg_208h
    bool calibFlag2              // arg_210h
);
```

### What it does:
1. Allocates buffer of `rawInput->dataSize` bytes (full raw copy)
2. Allocates buffer of `rawInput->dataSize / 2` bytes (8-bit output)
3. Calls the core `preprocessor()` function (export `sym.Wbdi.dll_preprocessor`)
4. Reads quality and coverage from output struct
5. Logs: `"quality = %d, coverage = %d"`
6. Checks return value against warning codes:
   - `0xC351` (50001) - warning, continue
   - `0x29AA` (10666) - warning, continue
   - `0x7531` (30001) - warning, continue
   - Any other non-zero = error, returns `0xFF6FFFF9`
7. Copies output BMP to caller's buffer
8. Calls `preprocessor_get_CalibParam()` to get calibration parameters (Kr, B arrays)
9. Dumps: framenum, select_index, Kr, B, cali_res, data_bmp, sito_bmp

### The raw data size division by 2:
At `0x18003b1b3`: `div ecx, 2` -- divides the raw data size by 2.
This converts from 16-bit raw pixel data to 8-bit BMP output.
Input: `rows * cols * 2` bytes (16-bit per pixel)
Output: `rows * cols` bytes (8-bit per pixel)

## 3. Core Preprocessor Pipeline (preprocess.c)

**Location**: `fcn.180114320` in Wbdi.dll
**Source**: `x64\src\preprocess.c`

The preprocessing pipeline consists of these sequential steps:

```
Step 1:  fcn.1801112e0  -- Data setup, validate input, copy raw data
Step 2:  fcn.180118300  -- Initial processing step (likely raw pixel unpacking)
Step 3:  fcn.180113240  -- Pixel-level operation
Step 4:  fcn.18010fb30  -- Matrix operation
Step 5:  fcn.180110750  -- Processing step
Step 6:  fcn.1801120a0  -- Calls fcn.180115020 (pixel calibration core)
Step 7:  fcn.1801170b0  -- AUTO CALIBRATION (auto_calibration.c)
                           Computes Kr (gain) and B (offset) arrays
Step 8:  fcn.18010f130  -- Post-calibration adjustment
Step 9:  fcn.18011c4b0  -- GAUSSIAN BLUR (gaussian_blur.c)
Step 10: fcn.18010f730  -- Post-blur processing
Step 11: fcn.1801126e0  -- Second calibration pass (also calls auto_calibration)
Step 12: fcn.18010ff00  -- Format conversion / normalization
Step 13: fcn.18011f060  -- Final image preparation
Step 14: fcn.1800cf7a0  -- QUALITY/COVERAGE computation (img_quality.c)
Step 15: fcn.180112250  -- BMP selection (uses THRESHOLD_SELECT_BMP)
```

### Algorithm source files (from string references):
- `x64\src\preprocess.c` - main pipeline orchestrator
- `x64\src\auto_calibration.c` - Kr/B calibration computation
- `x64\src\gaussian_blur.c` - Gaussian blur (multiple variants)
- `x64\src\enhance.c` - image enhancement (used during feature extraction)
- `x64\src\img_quality.c` - quality and coverage scoring
- `x64\src\feature.c` - feature extraction
- `x64\src\recognition.c` - recognition/matching
- `x64\src\finger_goodix.c` - Goodix proprietary algorithm core
- `x64\src\bwlabel.c` - connected component labeling (for coverage)
- `x64\src\broken_level.c` - broken pixel detection
- `x64\src\mat.c` - matrix operations
- `x64\src\improcess.c` - general image processing utilities
- `x64\src\pack_finger_template.c` - template serialization

### Key findings:
- **YES Gaussian blur**: Multiple gaussian blur functions are called during preprocessing and feature extraction
- **NO explicit Gabor filtering**: No Gabor-related strings found. The `enhance.c` module likely does the ridge enhancement but using a different approach
- **YES calibration/baseline subtraction**: The Kr/B calibration is the primary preprocessing step
- **YES quality/coverage computation**: Done in `img_quality.c`, involves gaussian blur
- **NO explicit histogram equalization**: No histogram/equalization strings. Normalization is done through the Kr/B calibration
- **The image is NOT resized during preprocessing**: Resize (upscaling via neural network) happens LATER in the enclave

## 4. Calibration: Kr and B Arrays

The preprocessor calibration is the CORE of the image processing. This is NOT simple background subtraction.

### Calibration data structure (at output of preprocessor):
```
Offset 0x0000: framenum (4 bytes) - number of frames used
Offset 0x0004: B array (39200 bytes = rows*cols*sizeof(int32_t))
               Per-pixel additive offset (baseline)
Offset 0x9924: Kr array (39200 bytes)
               Per-pixel multiplicative gain correction
Offset 0x13244: cali_res (39200 bytes)
               Calibration result (intermediate)
Offset 0x1CB64: data_bmp (19600 bytes = rows*cols*sizeof(int16_t))
               Processed 16-bit image data
Offset 0x217F4: sito_bmp (19600 bytes)
               Second processed image (possibly site-optimized)
Offset 0x26484: select_index (4 bytes)
               Which frame was selected as best
```

### The calibration formula (reconstructed):
```
For each pixel i:
    corrected[i] = (raw[i] - B[i]) * Kr[i] / scale
```

Where:
- `B[i]` = per-pixel baseline offset (captures sensor fixed-pattern noise)
- `Kr[i]` = per-pixel gain correction (normalizes sensitivity variation)
- `scale` = normalization constant

### PPLIB initialization log:
```
"PPLIB : cali B result %d, framenum=%d, kr[256]=%d, kr[2048]=%d, b[256]=%d, b[2048]=%d"
```
This confirms Kr and B are per-pixel arrays indexed by pixel number.

### Calibration data persistence:
- Calibration data (Kr/B) is computed during `preprocessor_init` and saved
- Functions: `preprocess_init_calidata`, `preprocess_load_calidata`, `preprocess_save_calidata`
- CRC-checked: `"preprocessor: cali data crc error"`
- Version-tracked: `"preprocess version error, current:%s, save:%s"`

## 5. Baseline Subtraction: The 2 Extra Images

### How baseline images are used:

From the capture flow in `logicmilanhvseries.c` (`fcn.180090630`):

1. **Capture loop** processes multiple images (controlled by `frame_count`)
2. Each image goes through `PreProcessorUnify` independently
3. The preprocessor internally handles multi-frame calibration:
   - The B array IS the baseline - computed from baseline frames
   - Frame 0 = finger image, subsequent frames = baseline
   - `preprocessor_init` takes the baseline raw data for B computation

### Multi-image selection logic (`fcn.18009365c`):

When 2+ preprocessed images exist, the driver selects the best one:

```c
int SelectBestImage(ImageResult* img0, ImageResult* img1) {
    int quality_diff = abs(img0->quality - img1->quality);

    if (quality_diff >= 20) {
        // Large quality difference: pick higher quality
        return (img0->quality > img1->quality) ? 0 : 1;
    } else {
        // Similar quality: pick higher coverage
        return (img0->coverage >= img1->coverage) ? 0 : 1;
    }
}
```

**Threshold = 20**: If quality difference >= 20, pick the higher-quality image.
Otherwise, pick the higher-coverage image.

### The dual-quality log format:
```
quality0 = %d, coverage0 = %d          // First preprocessed image
quality1 = %d, coverage1 = %d, select = %d   // Second image + selection
```

## 6. Image Format Transformation

### Input format:
- **16-bit per pixel, 1 channel** (confirmed by: `"bits = %d channels = %d frame_count = %d"`)
- The raw USB data arrives encrypted (TLS), decrypted by the driver
- Then parsed by `ImageRestructMilanG` / `ImageRestructInterface` in WbdiEnclave
- Our 4-byte and 6-byte packing (as implemented in our driver) decodes to the same 16-bit pixel values

### Preprocessing output:
- **8-bit per pixel grayscale BMP** (rawSize / 2)
- The preprocessor converts 16-bit calibrated pixels to 8-bit via normalization
- Output stored as `data_bmp` and `sito_bmp` (site-optimized BMP)

### Output to matching engine:
The `ConvertFormat` function (`reqimpl.c`) packs the data for the engine adapter:
- Offset 0x24: format marker `0x696D6773` ("sgmi" = Goodix image)
- Offset 0x02: 12-byte sensor header
- Offset 0x20: 2-byte quality/coverage
- Offset 0x28: image data (8-bit BMP, rows * cols bytes)
- Offset 0xEC18: second image buffer (for SITO data)
- Offset 0x16210: additional data block (0x4C90 bytes)
- Offset 0x1AEA0+: metadata (liveness flag, match info, etc.)

### BMP filename format (debug dump):
```
%s\image_%2d-%2d-%2d.%d_q-%d_c-%d.bmp
```
(path\image_HH-MM-SS.ms_q-quality_c-coverage.bmp)

## 7. Quality and Coverage Computation

### Quality computation (`img_quality.c`):
- The `getQuality` export calls `fcn.1800cf7a0` (img_quality.c)
- It operates on the 8-bit preprocessed BMP
- Uses **Gaussian blur** as part of quality assessment
- Quality is stored as **byte** value (0-100 scale)
- Coverage is stored as **byte** value (0-100 scale)
- Both are written to `result->quality` (offset 0x28) and `result->coverage` (offset 0x29)

### Acceptance thresholds (from registry defaults):

| Parameter | Registry Key | Default | Address |
|-----------|-------------|---------|---------|
| MinImageQuality | `Software\Goodix\FP\MinImageQuality` | **25** | 0x180246419 |
| MinImageCoverage | `Software\Goodix\FP\MinImageCoverage` | **65** | 0x18024641a |
| MaxOverlayRatio | `Software\Goodix\FP\MaxOverlayRatio` | **100** | 0x18024641b |
| SamplesNumPerTemplate | `Software\Goodix\FP\SamplesNumPerTemplate` | **12** | 0x180246418 |

### Quality/coverage check in engine adapter:
```
"now quality: %d minImageQuality: %d"
"now coverage : %d minImageCoverage: %d"
```
- If quality < 25: `"[ALG] Poor quality"` -> reject
- If coverage < 65: `"[ALG] Poor coverage"` -> reject
- If quality/coverage OK but no match: `"[ALG] Good quality and coverage, but not match"`

### GOODIX_THRESHOLD_SELECT_BMP:
Per-sensor-type threshold for BMP selection. Used in `fcn.180113d30` to determine
which frame's BMP data to use. Values range from 300-800 depending on sensor type.

## 8. Matching Algorithm (AdapterEnclave)

### Architecture:
- Matching happens **inside the SGX enclave** (`AdapterEnclave.signed.dll`)
- The enclave receives preprocessed BMP via SGX local attestation (encrypted channel)
- ALL template storage uses `sgx_seal_data` / `sgx_unseal_data`

### Algorithm: **Goodix proprietary** (NOT NBIS/bozorth3)
- Source files: `finger_goodix.c`, `recognition.c`, `feature.c`, `enhance.c`
- No NBIS, bozorth3, mindtct, or ISO 19794 strings found
- Algorithm has version string: `"enrolStart: Algorithm version %s"`
- Uses Intel IPP (Intel Performance Primitives) for crypto and math

### Neural Network Image Upscaling:
The enclave contains a **neural network for image extension/upscaling**:
```
"image_extension_net_core"
"image_extension_net_chicagoHs"    (for Chicago HS sensor)
"image_extension_net_milan_n"       (for Milan N sensor)
"_ppp_extension_init success"
"_ppp_extension_init2 GX_ALGO_BAD_PARAM"
"PPLIB: stImExtInfo.nOutRows %d, stImExtInfo.nOutCols=%d, gChipTypeGrow=%d"
"PPLIB: nRowsNew %d, nColsNew=%d, nChipType=%d"
```

This neural network (`extern_edge_net_int.c`, `extern_edge_net_int_milan_n.c`)
**upscales the small sensor image** before feature extraction. The name
"edge_net" suggests it's an edge-aware upscaling network.

Flow:
```
1. enrolAddImage / identifyImage receives 8-bit BMP
2. gChipNeedGrowAfterPreprocess flag checked
3. If grow needed: image_extension_net() upscales image
4. getFeature() extracts minutiae from upscaled image
5. fingerFeatureRecognition() matches against templates
```

### Matching flow:
```c
// Enrollment:
enrolStart()       -> allocate session, set max templates
enrolAddImage()    -> image_extension_net() -> getFeature() -> fingerFeatureRegister()
enrolGetTemplate() -> extract packed template
templatePack()     -> seal with SGX

// Identification:
identifyImage()    -> image_extension_net() -> getFeature() -> fingerFeatureRecognition()
                   -> returns: result, score, quality, coverage
templateStudy()    -> update template if match (learning)
```

### Score and result format:
```
"identifyImage result: %d, score: %d, quality: %d, coverage: %d"
"ret=%d, result=%d, score=%d"
"match(1-match,0-not match): %d"
```

## 9. Sensor Parameter Table

The preprocessor library contains a lookup table of 13 sensor types at `0x18022b190`:

| Type | ISFLOATING | PIXEL_CANCEL | IS_COATING | THRESH_BMP | COL | ROW | Pixels |
|------|-----------|-------------|-----------|-----------|-----|-----|--------|
| 0 | 1 | 0 | 4 | 800 | 88 | 108 | 9504 |
| 1 | 1 | 0 | 4 | 800 | 64 | 176 | 11264 |
| 2 | 1 | 0 | 4 | 800 | 54 | 176 | 9504 |
| 3 | 1 | 0 | 4 | 400 | 112 | 132 | 14784 |
| 4 | 1 | 0 | 0 | 600 | 60 | 128 | 7680 |
| 5 | 1 | 0 | 0 | 600 | 88 | 108 | 9504 |
| 6 | 1 | 0 | 0 | 600 | 64 | 176 | 11264 |
| 7 | 1 | 0 | 0 | 600 | 68 | 118 | 8024 |
| 8 | 1 | 0 | 0 | 300 | 96 | 96 | 9216 |
| 9 | 1 | 0 | 0 | 800 | 88 | 108 | 9504 |
| 10 | 1 | 0 | 4 | 800 | 64 | 80 | 5120 |
| 11 | 1 | 0 | 4 | 800 | 88 | 108 | 9504 |
| 12 | 1 | 0 | 4 | 600 | 64 | 80 | 5120 |

**Note**: Our 5e0a sensor (80x88 = 7040 pixels) is closest to **Type 10 or 12**
(64x80 = 5120) or could be a sensor type configured at runtime via `ppp_param_init()`.
The sensor type is determined from the chip ID during initialization.

- `ISFLOATING`: Always 1 (floating-point calibration)
- `PIXEL_CANCEL`: Always 0 (no dead pixel cancellation by default)
- `IS_COATING`: 0 or 4 (coating type affects calibration behavior)
- `THRESH_BMP`: 300-800 (threshold for BMP frame selection)

## 10. Additional Configuration (Registry)

All configurable via `HKLM\Software\Goodix\FP\`:

| Key | Purpose |
|-----|---------|
| MinImageQuality | Min quality to accept (default: 25) |
| MinImageCoverage | Min coverage to accept (default: 65) |
| MaxOverlayRatio | Max template overlay ratio (default: 100) |
| MaxPreoverlayRatio | Pre-overlay ratio limit |
| SamplesNumPerTemplate | Enrollment samples needed (default: 12) |
| LivenessSwitch | Anti-spoof toggle |
| SensorBrokenCheckSwitch | Dead pixel detection |
| HVDacAdjustSwitch | DAC dynamic adjustment |
| IdentifyRetryNumber | Match retry count |
| RetrySwitch | Enable match retry |
| RejectDetailForQuality | Quality rejection detail |
| RejectDetailForCoverage | Coverage rejection detail |
| CaptureMatchSwitch | Log capture/match timing |

## 11. Implications for Our libfprint Driver

### What we MUST do:
1. **Kr/B calibration**: This is the most critical preprocessing step. Without it,
   the raw image has fixed-pattern noise and sensitivity variation. We need to:
   - Capture baseline frames (after finger lift)
   - Compute B (per-pixel offset) and Kr (per-pixel gain)
   - Apply: `output[i] = (raw[i] - B[i]) * Kr[i] / scale`
   - Persist calibration data across sessions

2. **Quality/coverage scoring**: Implement quality assessment to reject bad captures.
   Minimum thresholds: quality >= 25, coverage >= 65.

3. **Multi-image selection**: When quality difference >= 20, pick higher quality;
   otherwise pick higher coverage.

### What we CAN skip:
1. **Neural network upscaling**: This is proprietary and inside the SGX enclave.
   We use NBIS/libfprint's matching which works on the native resolution.

2. **SGX enclave**: We do matching in user space (libfprint handles this).

3. **Gaussian blur for quality**: Simple quality metrics (variance, gradient-based)
   can substitute for the proprietary quality computation.

### What our current driver already does:
- Raw pixel decoding (4/6 byte packing) - matches Windows approach
- Basic image output to libfprint

### What we're missing:
- **Kr/B calibration** (the most impactful improvement)
- **Baseline frame capture** (need to capture after finger lift)
- **Quality scoring** (for enrollment sample selection)
- **8-bit normalization** (our 16-bit to 8-bit conversion may differ)

## 12. Data Flow Summary

```
USB sensor
    |
    | (encrypted TLS frames, 16-bit pixels in packed format)
    v
Raw decode (ImageRestructMilanG)
    |
    | (16-bit grayscale, rows x cols)
    v
Preprocessor (preprocess.c):
    |-- B baseline subtraction (per-pixel offset)
    |-- Kr gain correction (per-pixel multiply)
    |-- Gaussian blur (smoothing)
    |-- 16-bit to 8-bit conversion
    |-- Quality/coverage scoring
    v
8-bit grayscale BMP (rows x cols)
    |
    v
Engine Adapter -> SGX Enclave:
    |-- Neural net upscaling (image_extension_net)
    |-- Feature extraction (getFeature)
    |-- Enhancement (enhance.c with gaussian blur)
    |-- Recognition (identifyImage with score)
    v
Match result (score, template update)
```
