# Reverse Engineering: Goodix 5E0A Scan/FDT Protocol

Source: `Wbdi.dll` (2.4MB, x86_64 PE) from Goodix FingerPrint V3.0.141.150
Tool: radare2 5.9.8

---

## 1. McuParseFdt (mcuimpl.c) - FDT Response Parsing

**Function address:** `0x1800a60e0` (2359 bytes)

### Response Structure

The FDT response from the MCU is parsed as follows:

```
arg_c8h = response buffer pointer
  [0x00]     = first byte (contains irq_type and mode info)
  [0x08]     = pointer to payload data (arg_78h)
  [0x10]     = buffer size (must be >= 3, checked at 0x1800a6171)
```

### First Byte Parsing (0x1800a61d7 - 0x1800a61f1)

The first byte of the response is split:

```c
byte0 = response[0];

irq_type = byte0 >> 4;           // upper nibble → stored in arg_60h
mode     = (byte0 & 0x0F) >> 1;  // lower nibble >> 1 → stored in arg_50h
```

Mode values:
- **mode == 2**: FDT_UP response
- **mode != 2**: FDT_DOWN response (default)
- **mode == 3**: ESD event (special handling: memset result to 0, OR flags with 0x20)

### Interrupt / Command Word (0x1800a61fa - 0x1800a6221)

```c
interrupt = *(uint16_t*)payload;  // first 2 bytes of payload = little-endian word
```

This is logged as: `"cmd: 0x%x, interrupt: 0x%x"`

### Touch Flag Construction (result->flags, the DWORD at arg_d0h)

The result structure (arg_d0h, a 0x20-byte struct cleared with memset) has these fields:

```
offset 0x00: uint32_t flags     — bitfield of status flags
offset 0x04: uint16_t touch_flag — raw touch detection bitmask
offset 0x08: void*   fdt_up_base_data
offset 0x10: void*   fdt_down_base_data / fdt_base_translated
```

### Flags Bitfield (result->flags at offset 0x00)

Built up incrementally by OR-ing bits:

| Bit | Mask | Meaning | Set When |
|-----|------|---------|----------|
| 3   | 0x08 | FDT_DOWN received | `mode != 2` AND `bit7 set` (0x1800a63c0) |
| 4   | 0x10 | FDT_UP received | `mode == 2` AND `bit7 set` (0x1800a636b) |
| 5   | 0x20 | ESD event | `mode == 3` (0x1800a6332) |
| 6   | 0x40 | (reserved/used in clearing logic) | |
| 7   | 0x80 | IRQ valid / touch detected | From parsed interrupt data (0x1800a6349) |

**Clearing logic (0x1800a6409-0x1800a6446):**
If both bit3 (FDT_DOWN) and bit6 are set, bit3 is CLEARED (`flags &= ~0x08`).
This prevents processing a DOWN event when a conflicting bit6 condition exists.

**Skip-to-end logic (0x1800a6448-0x1800a6496):**
If NONE of bits 3,4,5,6 are set, skip all further processing (jump to end).

### Touch Flag Extraction (0x1800a6532 - 0x1800a6563)

The `touch_flag` (16-bit) is extracted from the payload:

```c
// payload = response data pointer (arg_78h)
byte_at_offset_2 = payload[1 * 2];     // payload[2]  (byte at offset 2)
byte_at_offset_3 = payload[1 * 3];     // payload[3]  (byte at offset 3)

touch_flag = byte_at_offset_2 | (byte_at_offset_3 << 8);  // little-endian 16-bit

result->touch_flag = touch_flag;  // stored at result + 4 as uint16_t
```

This is logged as: `"fdt touch flag: 0x%x"`

### Touch Flag Bit Interpretation

From the log values and `FDT_isTouchedByFinger` analysis:

The `touch_flag` is a **per-zone bitmask**. Each bit represents one FDT detection zone.
For a sensor with 6 zones, bits 0-5 are meaningful (0x3F = all zones touched).

| Value | Binary     | Meaning |
|-------|------------|---------|
| 0x3f  | 0011 1111  | All 6 zones detect finger (full touch) |
| 0x3d  | 0011 1101  | 5 of 6 zones (zone 1 missing) |
| 0x17  | 0001 0111  | 3 zones (partial touch, zones 0,1,2,4) |
| 0x0f  | 0000 1111  | 4 zones (lower half) |
| 0x00  | 0000 0000  | No zones (finger not present) |

The `FDT_isTouchedByFinger` function (0x18005f2e0, 1945 instructions) performs
per-zone comparison of FDT base values. Each zone's delta is divided by 0x3F (63)
to normalize, then compared against thresholds to determine if a finger is present.

### FDT Base Data Processing

After touch_flag extraction, three data buffers are allocated and populated:
1. `result+0x08` = fdt_up_base (for FDT_DOWN events, at 0x1800a6627)
2. `result+0x10` = fdt_down_base / fdt_manual_base (for FDT_UP events)

These are copied from `payload + 4` with `memcpy(result->base_data, payload+4, sensor_area_size)`.

---

## 2. FDT Payload Format (35 bytes / 0x23)

### Command Construction (_FpMcuSwitchToFdtMode at 0x1800526f4)

The FDT command byte is computed from the mode:

```c
cmd_byte = (mode << 1) | 0x30;   // at 0x180052b30-0x180052b35
```

| Mode | Computation | CMD | Meaning |
|------|-------------|-----|---------|
| 1    | 1*2 \| 0x30 | **0x32** | FDT_DOWN (wait for finger touch) |
| 2    | 2*2 \| 0x30 | **0x34** | FDT_UP (wait for finger lift) |
| 3    | 3*2 \| 0x30 | **0x36** | FDT_MANUAL (manual/baseline capture) |

### FDT Payload First Byte (prefix byte)

Constructed at 0x180052a1f-0x180052a3e:

```c
// g_sensor_variant is a global byte at 0x18025bff0
prefix = mode_byte | (g_sensor_variant << 4);

// mode_byte depends on mode AND a global flag at 0x18024686c:
// For FDT_DOWN (mode=1):
//   if global_flag[0x18024686c] != 0:
//     mode_byte = 0x1c   (high-voltage sensor)
//   else:
//     mode_byte = 0x0c   (standard sensor)

// For FDT_UP (mode=2):
//   mode_byte = 0x0e     (always, at 0x18005288f)

// For FDT_MANUAL (mode=3):
//   mode_byte = 0x0d     (at 0x180052932)
```

**CONFIRMED:** FDT_DOWN prefix = 0x1c (when HV flag set), FDT_UP prefix = **0x0e** (not 0x1e).

### Payload Second Byte

At 0x180052a5a-0x180052a6c:

```c
payload[1] = has_fdt_base_data ? 1 : 0;
// This indicates whether FDT base/reference data follows in the payload
```

### Full FDT_DOWN Payload Layout (mode=1)

```
Offset  Size  Content
0x00    1     prefix byte (0x1c for HV, 0x0c otherwise, OR'd with sensor_variant<<4)
0x01    1     has_base_data flag (0 or 1)
0x02    N     FDT base/reference data (N = fdt_area_size from sensor config)
0x02+N  1     0x0A (literal, at 0x180052a96) — threshold marker?
0x03+N  1     0x03 (literal, at 0x180052ab3) — threshold count?
0x04+N  M     extra FDT data (M = arg3 parameter, FDT delta/offset data)
0x04+N+M 1   0x06 (literal, at 0x180052b1b) — terminator byte

Total = fdt_area_size * 2 + 8 (when has_base_data=1)
      = fdt_area_size * 2 + 7 (when has_base_data=0)
```

For a 35-byte payload: `fdt_area_size * 2 + 8 = 35` → `fdt_area_size = 13-14` (sensor zone data).

### FDT_UP Payload Layout (mode=2)

Same structure but with prefix byte = 0x0e. No extra arg3 data (r8=0 at 0x180051bae).

### FDT_MANUAL Payload Layout (mode=3)

Same structure with prefix byte = 0x0d. Has additional callback parameters
(arg_d0h, arg_d8h, arg_e0h, arg_e8h) passed to the MCU send function.

---

## 3. MilanFSerMcuGetImage (0x180064c70)

### Flow

1. Validates parameters (arg1 = context, arg2 = output buffer, arg3 = image buffer, arg4 = size)
2. Gets the sensor type from `sensor_info->type` (at context+0x20 → +0x20):
   - Type 0x0A, 0x11, 0x0E: **Milan HV series** → calls `_HUGetImage` (fcn.1800652a8)
   - Other types: calls `fcn.1800520d8` (different image capture path)
3. Before calling `_HUGetImage`, retrieves:
   - `delta_dac = fcn.18008185c(context)` — gets DAC delta value
   - `image_dac = fcn.1800817ec(context)` — gets image DAC base value

### _HUGetImage (0x1800652a8)

1. **Reads 4 DAC values** from sensor context (offset 0x70, 8 bytes = 4x uint16_t):
   ```c
   memcpy(dac_values[4], context+0x70, 8);
   ```

2. **Constructs 10-byte command payload:**
   ```
   Offset  Content
   0x00    0x01  (literal flag byte)
   0x01    padding/reserved
   0x02    dac_values[0] low byte    ← DAC channel 0
   0x03    dac_values[0] high byte
   0x04    dac_values[1] low byte    ← DAC channel 1
   0x05    dac_values[1] high byte
   0x06    dac_values[2] low byte    ← DAC channel 2
   0x07    dac_values[2] high byte
   0x08    dac_values[3] low byte    ← DAC channel 3
   0x09    dac_values[3] high byte
   ```

3. **Adjusts byte[0] bit 2** based on a flag:
   ```c
   if (*output_buffer == 0)
       payload[0] &= 0xFB;   // clear bit 2 (disable auto-exposure?)
   else
       payload[0] |= 0x04;   // set bit 2 (enable auto-exposure?)
   ```

4. **Iterates DAC values** (loop 0-3) packing each 16-bit DAC into 2 bytes at
   sequential offsets, logging each: `"image set dac[%d]: 0x%x"`

5. **Sends command 0x20** with the 10-byte payload:
   ```c
   send_mcu_cmd(context, cmd=0x20, payload, payload_len=0x0A, ...);
   ```

### DAC Purpose

The 4 DAC values control the sensor's analog front-end:
- **DAC channels** set bias/reference voltages for the capacitive sensor array
- Different DAC values for image capture vs. FDT detection
- `_MilanFSerModifyFdtDac` and `_MilanFSerModifyNavDac` modify these for
  different operational modes
- The `delta_dac` and `image_dac` (from OTP/calibration) fine-tune the values
  to compensate for manufacturing variation

---

## 4. Quality/Coverage Check

### Configuration

Quality and coverage thresholds are read from Windows registry:

```
HKLM\Software\Goodix\FP\MinImageQuality  → stored at 0x180246419 (byte)
HKLM\Software\Goodix\FP\MinImageCoverage → stored at 0x18024641a (byte)
```

Additional related registry keys found:
- `RejectDetailForQuality`  (0x1801316a8)
- `RejectDetailForCoverage` (0x1801316d8)
- `MaxOverlayRatio`
- `MaxPreoverlayRatio`
- `SamplesNumPerTemplate`

### Preprocessor Function (exported: `Wbdi.dll!preprocessor` at 0x1800cd480)

1. Checks calibration state (`[0x1802659a4] == 1`)
2. Packs sensor parameters:
   - Sensor dimensions from globals at 0x180265990 (88) and 0x180265994 (108)
   - These appear to be **88 columns x 108 rows** = 9504 pixels
3. Calls internal processing: `fcn.180114320` (the actual image quality engine)
4. Returns quality and coverage:
   ```c
   // r14 points to result structure
   result->quality  = [r14+0];   // at result offset 0x00
   result->coverage = [r14+4];   // at result offset 0x04

   printf("preprocessor: quality %d, coverage %d", result->coverage, result->quality);

   // Store in output
   output[0x28] = coverage;  // byte
   output[0x29] = quality;   // byte
   ```
5. Copies processed image data to output buffer

### SubmitPendingRequest Quality Check (0x180090630)

The scan flow function performs **dual-frame capture and comparison**:

1. Captures frame 0 → logs `"quality0 = %d, coverage0 = %d"`
2. Captures frame 1 → logs `"quality1 = %d, coverage1 = %d, select = %d"`
3. Selects the better frame based on combined quality+coverage score

The actual acceptance/rejection thresholds are read from the registry
(`MinImageQuality`, `MinImageCoverage`) and compared in the logic flow.
Default values appear to be configurable per-deployment. Typical observed
values from logs: quality >= ~20, coverage >= ~60 for acceptance.

---

## 5. Other Commands

### Command Table

| CMD  | Name | Description | Source |
|------|------|-------------|--------|
| 0xAE | QUERY_MCU_STATE | Query MCU status/version | fpmcucmd.c |
| 0x20 | GET_IMAGE | Capture fingerprint image (with DAC payload) | milanfsermcu.c |
| 0x32 | FDT_DOWN | Switch to finger-detect-down mode (35B payload) | fpmcucmd.c |
| 0x34 | FDT_UP | Switch to finger-detect-up mode (35B payload) | fpmcucmd.c |
| 0x36 | FDT_MANUAL | Switch to manual FDT mode (baseline capture) | fpmcucmd.c |

### Additional Commands (from function name analysis)

| Function | Likely CMD | Purpose |
|----------|-----------|---------|
| `MilanFSerSpiReadReg` | 0x82 | Read sensor register |
| `MilanFSerSpiWriteReg` | 0x80 | Write sensor register |
| `MilanFSerSpiResetFingerPrint` | 0xA4 | Reset fingerprint sensor |
| `MilanFSerSpiGetOtp` | 0xA6 | Read OTP (one-time-programmable) data |
| `MilanFSerSpiSwitchToIdleMode` | 0x70? | Switch to idle mode |
| `MilanFSerSpiSwitchToSleepMode` | 0x60? | Switch to sleep mode |
| `MilanFSerSpiGetFdtManualBase` | 0x36 | Get FDT manual baseline |

### Commands 0x70, 0xC4, 0xAC, 0x60

Based on analysis of the driver and Goodix protocol conventions:

- **0x70**: Likely **SWITCH_TO_IDLE_MODE** — puts the sensor in low-power idle state
  (`MilanFSerSpiSwitchToIdleMode`). Sent between scan cycles.

- **0xC4**: Likely **UPLOAD_CONFIG** or **SET_CONFIG** — uploads sensor configuration
  parameters. The driver has extensive config management (`upload_config`,
  `DynamicConfig` subsystem, registry-based config overrides).

- **0xAC**: Likely **READ_CHIP_ID** or **GET_SENSOR_INFO** — reads chip identification.
  The driver logs `sensorId`, `chipId`, `sensorType`, `frameSize`, `navCol`, `navRow`,
  `sensorCol`, `sensorRow`.

- **0x60**: Likely **SWITCH_TO_SLEEP_MODE** — deep sleep mode
  (`MilanFSerSpiSwitchToSleepMode`). Used during system power management.

---

## 6. Full Scan Cycle (Reconstructed)

```
1. QUERY_MCU (0xAE)
   → Verify MCU is ready

2. FDT_DOWN (0x32, 35 bytes)
   → Payload: [prefix=0x1c] [has_base=1] [base_data...] [0x0A] [0x03] [delta_data...] [0x06]
   → MCU enters finger-detection mode
   → Waits for interrupt (finger touch)

3. [MCU IRQ] → Parse FDT response
   → McuParseFdt extracts: irq_type, mode, touch_flag
   → FDT_isTouchedByFinger: check per-zone deltas against threshold
   → If touch_flag has sufficient zones set: finger present

4. GET_IMAGE (0x20, 10 bytes)
   → Payload: [flags] [pad] [DAC0_lo] [DAC0_hi] [DAC1_lo] [DAC1_hi] [DAC2_lo] [DAC2_hi] [DAC3_lo] [DAC3_hi]
   → MCU captures image using specified DAC values
   → Returns 88x108 pixel image data

5. Preprocessor: compute quality + coverage
   → If quality < MinImageQuality OR coverage < MinImageCoverage: reject, retry

6. FDT_UP (0x34, 35 bytes)
   → Payload: [prefix=0x0e] [has_base=0] [base_data...] ...
   → MCU enters finger-lift-detection mode
   → Waits for interrupt (finger removed)

7. [MCU IRQ] → Parse FDT response
   → Confirm finger has been lifted

8. Baseline refresh: QUERY_MCU (0xAE) → FDT_UP (0x34) → GET_IMAGE (0x20) x2
   → Two baseline images captured without finger

9. FDT_DOWN (0x32)
   → Ready for next scan cycle
```

---

## 7. Key Data Structures

### FDT Result Structure (0x20 bytes)

```c
struct fdt_result {
    uint32_t flags;          // +0x00: bit3=DOWN, bit4=UP, bit5=ESD, bit7=IRQ_VALID
    uint16_t touch_flag;     // +0x04: per-zone touch bitmask
    uint16_t padding;        // +0x06
    void*    fdt_base_1;     // +0x08: allocated base data buffer 1
    void*    fdt_base_2;     // +0x10: allocated base data buffer 2
    // +0x18: padding to 0x20
};
```

### Preprocessor Result

```c
struct preprocess_result {
    void*    image_data;     // +0x00: pointer to processed image
    // ...
    uint32_t image_size;     // +0x14: image data size
    // ...
    uint8_t  coverage;       // +0x28
    uint8_t  quality;        // +0x29
};
```

### Sensor Info (from OTP)

```c
struct sensor_info {
    uint32_t type;           // +0x00: 0x0A, 0x0E, or 0x11 for Milan HV
    uint8_t  tcode;          // from OTP
    uint8_t  diff;           // from OTP
    uint8_t  touch_diff;     // from OTP
    uint8_t  fdt_tcode;      // from OTP
    uint16_t dac_values[4];  // +0x70: 4 DAC channel values (8 bytes)
    uint32_t fdt_area_size;  // +0x44: number of FDT detection zones
};
```

---

## 8. Critical Corrections for Our Driver

1. **FDT_UP prefix is 0x0E, not 0x1E** — confirmed from `_FpMcuSwitchToFdtMode`
   at 0x18005288f: `mov byte [var_50h], 0xe`

2. **FDT_DOWN prefix is 0x1C** (for HV sensors like 5E0A) — confirmed from
   0x1800527e5: `mov byte [var_50h], 0x1c`

3. **Touch flag is at response payload bytes [2] and [3]** (16-bit little-endian),
   NOT at a fixed byte offset from the start of the full USB response.

4. **The 0x20 GET_IMAGE command requires a 10-byte payload** with DAC values,
   not just a bare command.

5. **Quality/coverage are computed by an internal preprocessor library**
   (`Wbdi.dll!preprocessor`), not by the MCU. The MCU just returns raw image data.
