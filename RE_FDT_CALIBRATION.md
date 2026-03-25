# FDT Calibration Reverse Engineering - Goodix Wbdi.dll

Source: `Wbdi.dll` from Goodix FingerPrint_V3.0.141.150_21H1_signed

## Table of Contents
1. [OTP Tcode/Diff Extraction](#1-otp-tcodediff-extraction)
2. [FDT Offset from OTP](#2-fdt-offset-from-otp)
3. [Config Register Modification Functions](#3-config-register-modification-functions)
4. [FDT Base Data (GetFdtManualBase)](#4-fdt-base-data-getfdtmanualbase)
5. [CalcFdtBase - Computing Thresholds from Base Data](#5-calcfdtbase---computing-thresholds-from-base-data)
6. [FpMcuSwitchToFdtMode - Payload Construction](#6-fpmcuswitchtofdt---payload-construction)
7. [FDT_InitParameter - Sensor-Specific FDT Setup](#7-fdt_initparameter---sensor-specific-fdt-setup)
8. [CheckSensorOtp - OTP Validation](#8-checksensorotp---otp-validation)
9. [Complete FDT Calibration Algorithm](#9-complete-fdt-calibration-algorithm)
10. [Command 0xd2](#10-command-0xd2)

---

## 1. OTP Tcode/Diff Extraction

### _MilanFSerGetTcodeAndDiffFromOtp (0x180063b90)
**For Milan F-Series (32-byte OTP)**

Arguments:
- rcx = OTP data pointer
- edx = OTP size (must be >= 0x20)
- r8 = pointer to output tcode (uint16_t*)
- r9 = pointer to output diff (uint16_t*)

Algorithm:
```c
// otp_byte = otp_data[0x16]
uint8_t otp_byte = otp_data[0x16];

// Validate: otp_data[0x16] != 0 AND otp_data[0x16] + otp_data[0x17] == 0xFF
if (otp_byte == 0 || (otp_byte + otp_data[0x17]) != 0xFF) {
    // "no tcode and diff"
    return 0;
}

// tcode = ((otp_byte >> 4) + 1) << 4
uint16_t tcode = ((otp_byte >> 4) + 1) << 4;

// diff = (((otp_byte & 0xF) + 2) * 100 * 256 / tcode) / 3 >> 4
uint16_t diff = ((((otp_byte & 0x0F) + 2) * 0x64) << 8) / tcode;
diff = (diff / 3) >> 4;

*out_tcode = tcode;
*out_diff = diff;
```

### _MilanHUSerGetTcodeAndDiffFromOtp (0x18006d848)
**For Milan HU (64-byte OTP, e.g. sensor 5e0a)**

OTP bytes checked at offsets 0x2A, 0x2B, 0x2D (note: `imul rax, 1, 0x2a` = offset 0x2A)

Tcode validation (triple redundancy check with priority):
```c
uint8_t otp[0x40];
memcpy(otp, otp_data, 0x40);
uint8_t tcode_byte = 0;

// Step 1: otp[0x2A] != 0 AND otp[0x2A] == ~otp[0x2B]
if (otp[0x2A] != 0 && (otp[0x2A] - (uint8_t)(~otp[0x2B])) == 0) {
    tcode_byte = otp[0x2A];
}
// Step 2: otp[0x2D] != 0 AND otp[0x2D] == ~otp[0x2B]
else if (otp[0x2D] != 0 && (otp[0x2D] - (uint8_t)(~otp[0x2B])) == 0) {
    tcode_byte = otp[0x2D];
}
// Step 3: otp[0x2A] != 0 AND otp[0x2D] == otp[0x2A]
else if (otp[0x2A] != 0 && (otp[0x2D] - otp[0x2A]) == 0) {
    tcode_byte = otp[0x2A];
}
else {
    // "byteArray Tcode and threshold is wrong"
}

if (tcode_byte != 0) {
    // diff = ((tcode_byte & 0x0F) + 2) * 0x64
    uint16_t diff_raw = ((tcode_byte & 0x0F) + 2) * 0x64;

    // tcode = ((tcode_byte >> 4) & 0x0F + 1) << 4
    uint16_t tcode = (((tcode_byte >> 4) & 0x0F) + 1) << 4;

    // diff = (diff_raw * 0x100 / tcode / 3) >> 4
    // Note: truncated to uint8_t via movzx al
    uint16_t diff = ((diff_raw * 0x100 / tcode) / 3) >> 4;
    diff = (uint8_t)diff;  // truncated to byte!

    *out_tcode = tcode;
    *out_diff = diff;
}
```

**Key OTP offsets for HU sensors:**
| Offset | Description |
|--------|-------------|
| 0x16   | Tcode byte (F-series) |
| 0x17   | ~Tcode byte (complement, F-series) |
| 0x19   | Tcode byte copy (F-series) |
| 0x2A   | Tcode byte (HU, primary) |
| 0x2B   | ~Tcode byte (HU, complement) |
| 0x2D   | Tcode byte (HU, backup) |

### Tcode/Diff Computation Example
For `tcode_byte = 0x__` (e.g. 0x1F from logs with tcode=256=0x100):
- If tcode_byte = 0x1F: upper nibble = 1, lower nibble = F = 15
  - tcode = (1 + 1) << 4 = 0x20 = 32 ... doesn't match 256
- If tcode_byte = 0xFF: upper nibble = F = 15, lower nibble = F = 15
  - tcode = (15 + 1) << 4 = 0x100 = 256 -- MATCHES!
  - diff_raw = (15 + 2) * 100 = 1700
  - diff = (1700 * 256 / 256) / 3 >> 4 = 1700 / 3 >> 4 = 566 >> 4 = 35
  - But log says fdt delta = 31...

The exact tcode_byte value depends on the actual OTP data read from the sensor.

---

## 2. FDT Offset from OTP

### _MilanFSerGetFdtOffsetFromOtp (0x1800639e0)

Arguments:
- rcx = OTP data pointer
- edx = OTP size (must be >= 0x20)
- r8b = byte index into OTP (which byte to read)
- r9 = pointer to output fdt_offset (uint8_t*)

Algorithm:
```c
uint8_t otp_byte = otp_data[byte_index];

// Triple-redundant encoding in one byte:
// bits[1:0] = value_low
// bits[5:4] = value_high
// bits[3:2] = ~value (complement, shifted)

uint8_t val_low = otp_byte & 0x03;
uint8_t val_high = (otp_byte >> 4) & 0x03;
uint8_t val_comp = (~otp_byte >> 2) & 0x03;

uint8_t fdt_offset;

if (val_low == val_high) {
    fdt_offset = val_low;    // Both agree
} else if (val_low == val_comp) {
    fdt_offset = val_low;    // Low matches complement
} else if (val_high == val_comp) {
    fdt_offset = val_high;   // High matches complement
} else {
    // "no fdt offset otp value"
    fdt_offset = 0;
    return 0;  // failure
}

*out_fdt_offset = fdt_offset;
return 1;  // success
```

The FDT offset is a 2-bit value (0-3) used to adjust the FDT sensitivity.

---

## 3. Config Register Modification Functions

These functions modify register values within the sensor's configuration blob.
The config blob is a linear array of `(register_id: uint16_t, value: uint16_t)` pairs.

### Config Structure
Each config entry is 4 bytes: `[reg_id_lo, reg_id_hi, value_lo, value_hi]`

### fcn.18004ee7c - Write Config Register
```c
// Scans config blob for register matching target_reg_id, replaces its value
// Also recalculates checksum at offset 0xFE
bool WriteConfigRegister(
    uint8_t* config,     // Full config blob
    uint8_t start_off,   // Start scan offset
    uint8_t total_len,   // Total length = start_off + range
    uint16_t target_reg, // Register ID to find (r9w)
    uint16_t new_value,  // New value to write (arg_80h)
    uint16_t* old_value  // Optional: output previous value (arg_88h)
);
```

### fcn.18004ed98 - Read Config Register
```c
bool ReadConfigRegister(
    uint8_t* config,     // Full config blob
    uint8_t start_off,
    uint8_t total_len,
    uint16_t target_reg, // r9w
    uint16_t* out_value  // arg_50h
);
```

### _MilanFSerModifyFdtTcode (0x180064238)
Modifies register **0x005C** in config with the tcode value.
```c
// config[5] = start offset, config[6] = end offset region
uint8_t total = config[5] + config[6];
WriteConfigRegister(config, config[5], total, 0x005C, tcode, callback);
```

### _MilanFSerModifyFdtDelta (0x180063f8c)
Modifies register **0x0082** in config with the diff/delta value.
```c
uint8_t total = config[5] + config[6];
WriteConfigRegister(config, config[5], total, 0x0082, diff, callback);
```

### _MilanFSerModifyFdtOffset (0x180064090)
Modifies register **0x0056** in config, merging fdt_offset into low byte.
```c
// First read current value of reg 0x0056
uint16_t current;
ReadConfigRegister(config, config[5], total, 0x0056, &current);

// Preserve high byte, replace low byte with fdt_offset
uint16_t new_val = (current & 0xFF00) | fdt_offset;

// Write back
WriteConfigRegister(config, config[5], total, 0x0056, new_val, callback);
```

### _MilanFSerModifyFdtDac (0x180063e88)
Modifies register **0x0220** with DAC value.

### _MilanFSerGetFdtAreaNum (0x180063d94)
Reads register **0x00CA** to get FDT area number.
```c
ReadConfigRegister(config, config[1], config[1]+config[2], 0x00CA, out_area_num);
```

---

## 4. FDT Base Data (GetFdtManualBase)

### GetFdtManualBase (Chicago H variant: 0x1800573bf)
### HUMilanFSerMcuGetFdtManualBase (around 0x18006c88f area)

This function sends an **FDT_MANUAL command (0x36)** to the sensor and reads back the base calibration data.

The flow:
1. Calls `fcn.1800585c4` (SwitchToFdtMode) with mode=3 (MANUAL), which builds and sends command **0x36**
2. The sensor responds with raw FDT base data
3. Base data is copied to output buffer
4. If `rawBaseOut` buffer provided, also converts raw data using `fcn.18006384c`

### fcn.18006384c - Convert Raw Base Data
```c
// Converts raw base words: for each pair of bytes (word):
// new_value = (old_value >> 1) << 8 | 0x80
// (i.e., shift right by 1, move to high byte, set bit 7)
void ConvertRawBase(uint16_t* data, int size_bytes) {
    for (int i = 0; i < size_bytes; i += 2) {
        uint16_t val = data[i/2];
        val = ((val >> 1) << 8) | 0x80;  // sar 1, shl 8, bts 7
        data[i/2] = val;
    }
}
```

### Key parameters:
- `baseSize` (arg_c0h): checked against `config->base_size` at offset 0x44 within the config structure
- Maximum base size: 0x18 (24 bytes = 12 words)
- The base data is `base_size` bytes (typically 12 bytes = 6 words for a 2x5 or similar area config)
- **Stored at structure offset 0x70** from the device context (`device_ctx + 0x70`)

### HU Variant Notes (0x18006d2c0 HUFpMcuSwitchToFdtMode):
- In the HU variant, base data is stored at `context + 0x70`
- `var_54h` = 8 (initial "FDT base data length" for HU)
- The payload copies base data from `context+0x70` into the FDT command payload

---

## 5. CalcFdtBase - Computing Thresholds from Base Data

### MilanFSerCalcFdtUpBase (around 0x1800633e7)
### MilanFSerCalcFdtDownBase (around 0x180063303)

Both call the same core function: `fcn.1800638b8`

### fcn.1800638b8 - Core CalcBase Algorithm
Arguments:
- rcx = base_data buffer (uint16_t array, modified in place)
- edx = base_size (in bytes)
- r8w = area_bits (bitmask of which FDT areas to adjust)
- r9w = fdt_diff (the calibrated delta threshold)

**Phase 1: Apply diff to base values**
```c
// For each word in the base data (step by 2 bytes):
for (int i = 0; i < base_size; i += 2) {
    uint16_t base_val = data[i/2];

    if (fdt_diff != 0) {
        // base_val = ((base_val >> 1) + fdt_diff) << 8 | 0x80
        uint16_t result = (int16_t)base_val >> 1;  // arithmetic shift right
        result = (result + fdt_diff) << 8;
        result |= 0x80;  // set bit 7
        data[i/2] = result;
    } else {
        // No diff: default threshold
        // base_val = ((base_val >> 1) + 0x15) << 8 | 0x80
        uint16_t result = (int16_t)base_val >> 1;
        result = (result + 0x15) << 8;
        result |= 0x80;  // set bit 7 (bts eax, 7)
        data[i/2] = result;
    }
}
```
Note: `0x15 = 21` is the default diff when no OTP diff is available.

**Phase 2: Zero out disabled areas**
```c
// For each word, check if its corresponding area bit is set
int bit_index = 0;
for (int i = 0; i < base_size; i += 2) {
    uint16_t area_bit = (area_bits >> bit_index) & 1;

    if (area_bit == 0) {
        // Area disabled - set to minimum threshold or default
        if (fdt_diff != 0) {
            // threshold = (fdt_diff - 2) << 8 | 0x80
            data[i/2] = ((fdt_diff - 2) << 8) | 0x80;
        } else {
            // Default minimum: 0x1380
            data[i/2] = 0x1380;
        }
    }

    bit_index++;
}
```

### CalcFdtUpBase vs CalcFdtDownBase
Both call `fcn.1800638b8` with:
- **base data**: copied from the raw base read from sensor
- **fdt_diff**: the calibrated diff value from OTP (stored at `context + 0x68` as uint16_t)
- **area_bits**: the FDT area bitmask (from `r8w` / config register 0xCA)

The "Up" vs "Down" distinction is in how the result is used:
- **FDT_DOWN**: base + diff = threshold for detecting finger placement
- **FDT_UP**: base + diff = threshold for detecting finger removal

---

## 6. FpMcuSwitchToFdtMode - Payload Construction

### _FpMcuSwitchToFdtMode (0x1800526f4)
### HUFpMcuSwitchToFdtMode (0x18006d2c0)

Arguments:
- rcx (arg1) = device context pointer
- edx (arg2) = mode: 1=DOWN, 2=UP, 3=MANUAL
- r8 (arg3) = extra base data pointer (optional)
- r9d (arg4) = extra base data size
- stack: config data pointer, config data size, callback pointers

### Mode Selection (byte 0 prefix):
```c
uint8_t prefix;
switch (mode) {
    case 1: // FDT_DOWN
        if (global_flag[0x18024686c] != 0)
            prefix = 0x1C;  // Standard FDT_DOWN
        else
            prefix = 0x0C;  // Alternative FDT_DOWN
        break;
    case 2: // FDT_UP
        prefix = 0x0E;
        break;
    case 3: // FDT_MANUAL
        prefix = 0x0D;
        break;
}
```

### Payload Structure:
```
Byte 0: prefix | (speed_flag << 4)
        - speed_flag from global [0x18025bff0] (non-HU) or [0x18025c6f9] (HU)
        - For DOWN: prefix = 0x1C (with base) or 0x0C (without)
        - For UP:   prefix = 0x0E
        - For MANUAL: prefix = 0x0D

Byte 1: has_base_data flag (1 if config data provided, 0 otherwise)

If mode == DOWN (mode 1) and base data provided:
    Bytes [base_len+cfg_sz+4]: 0x0A  (touch detect sub-command?)
    Bytes [base_len+cfg_sz+5]: 0x03  (touch detect parameter?)

    // Copy extra base data if provided:
    Bytes [base_len+cfg_sz+6 .. +6+base_len-1]: extra base data

    // Terminator:
    Bytes [cfg_sz + base_len*2 + 6]: 0x06

For HU variant (0x18006d2c0):
    var_54h = 8 (FDT base size for HU)

    Byte 0: prefix | (speed_flag << 4)
    Byte 1: has_base_data flag

    // Starting at offset 2, copy base data from context+0x70:
    Bytes [2 .. 2+base_len-1]: base data from device_ctx->fdt_base (at ctx+0x70)

    // If has_base_data and mode==DOWN:
    // Additional base data appended after config data
```

### USB Command Construction:
```c
uint16_t cmd_id = (mode << 1) | 0x30;
// mode 1 (DOWN): cmd = 0x32
// mode 2 (UP):   cmd = 0x34
// mode 3 (MANUAL): cmd = 0x36
```

The payload is sent via `fcn.18007d6b0` which wraps the actual USB transfer:
```c
SendMcuCommand(device, cmd_id, payload, payload_size, ...);
```

### Full HU Payload Layout for FDT_DOWN:
```
[0]:     prefix | (speed << 4)     e.g. 0x1C | (0 << 4) = 0x1C
[1]:     has_base_data              1 if base data included
[2..2+N-1]: FDT base data          N = base_len (8 for HU), from ctx+0x70
[2+N .. 2+N+cfg_sz-1]: config FDT data (if has_base_data)
[2+N+cfg_sz .. end]: footer with 0x0A, 0x03, more base, 0x06
```

When no base data (has_base_data=0):
```
[0]:     prefix | (speed << 4)
[1]:     0x00
[2..2+N-1]: FDT base data from ctx+0x70
```
Total payload size without config: `base_len * 2 + 6`
Total payload size with config: `config_size + base_len * 2 + 7`

---

## 7. FDT_InitParameter - Sensor-Specific FDT Setup

### FDT_InitParameter (0x18005e33c)

This is a large function (3796 bytes) that initializes FDT parameters based on sensor type.

Arguments:
- cl = sensor_code (0x40=176row, 0x36=?, 0x2A=?, 0x50=?, 0x58=?, 0x70=?, 0xA0=?)
- dx = fdt_delta
- r8b = sensor_sub_type
- r9w = additional param
- stack: more params including fdt_offset (arg_b8h)

### Sensor type dispatch (byte at var_54h):
| Code | Sensor Size | Rows | Cols |
|------|------------|------|------|
| 0x40 | 0xB0 (176) | 5    | 2    |
| 0x40 | 0x78 (120) | 5    | 2    |
| 0x40 | 0x50 (80)  | 3    | 2    |
| 0x58 | (various)  | -    | -    |
| 0x36 | (various)  | -    | -    |
| 0x2A | (various)  | -    | -    |
| 0x50 | (various)  | -    | -    |
| 0x70 | (various)  | -    | -    |
| 0xA0 | (various)  | -    | -    |

### For sensor type 0x40 with sub-type 0xB0 (our likely case):
```c
global_fdt_mode = 0;           // [0x18025c680]
global_fdt_row_count = 2;      // [0x18025c689] (down rows)
global_fdt_col_count = 5;      // [0x18025c68a] (down cols)
global_fdt_up_row_count = 2;   // [0x18025c6a1]
global_fdt_up_col_count = 5;   // [0x18025c6a0]

// FDT area boundary pointers set to static tables:
// down_row_boundaries -> [0x18019e520]
// down_col_boundaries -> [0x18019e524]
// (these are arrays of boundary pixel values)

// FDT threshold pointer -> [0x1802470e8]
// Threshold buffer is 5 bytes, computed from fdt_offset:
// thresh[0] = 0x00 (always)
// thresh[1] = (fdt_offset + 3) / 4     = fdt_offset/4 rounded
// thresh[2] = (fdt_offset * 2 + 3) / 4 = fdt_offset*2/4 rounded
// thresh[3] = (fdt_offset * 3 + 3) / 4 = fdt_offset*3/4 rounded
// thresh[4] = fdt_offset - 2           = fdt_offset - global_subtract
```

The global `[0x18025c6ec]` is set to 2 (the subtract constant).

### For sensor type 0x40 with sub-type 0x78:
Same structure but with different table pointers:
- threshold buffer at [0x180247118]
- Different boundary table addresses

### For sensor type 0x40 with sub-type 0x50:
- rows = 2, cols = 3 (for down); rows = 2, cols = 3 (for up)
- threshold buffer at [0x180247124]
- Different boundary calculation (division by 2 instead of 4):
  - thresh[1] = (fdt_offset + 1) / 2

---

## 8. CheckSensorOtp - OTP Validation

### MilanFSerCheckSensorOtp (0x1800655f0)
For 32-byte OTP (F-series):
```c
bool MilanFSerCheckSensorOtp(uint8_t* otp_data, int otp_size) {
    if (otp_size != 0x20) return false;

    bool valid = CheckOtpCRC(otp_data);  // calls fcn.180065e3c

    if (valid) {
        if (global_sensor_variant == 1) {  // [0x18025c6f8]
            // Clear certain OTP fields
            otp_data[0x1C] = 0;
            otp_data[0x1B] = 0;
            otp_data[0x1A] = 0;
        }
        return true;
    }
    return false;
}
```

### MilanHUCheckSensorOTP (0x180065764)
For 64-byte OTP (HU series, e.g. 5e0a):
```c
bool MilanHUCheckSensorOTP(uint8_t* otp_data, int otp_size) {
    if (otp_size != 0x40) return false;  // Must be exactly 64 bytes

    bool valid = CheckOtpCRC(otp_data);  // calls fcn.18006585c

    if (valid) {
        return true;
    }
    return false;
}
```

### MilanG_TcodeCheck (at 0x18006b4f0 area)
Triple-redundancy check for tcode validity using OTP bytes:
```c
// For F-series style OTP (offsets 0x16, 0x17, 0x19):
uint8_t tcode_val = otp[0x16];
uint8_t tcode_inv = ~otp[0x17];  // complement
uint8_t tcode_dup = otp[0x19];

// Step 1: tcode_val != 0 && tcode_val == tcode_inv
// Step 2: tcode_val != 0 && tcode_val == tcode_dup
// Step 3: tcode_dup != 0 && tcode_dup == tcode_inv
// If all fail: "Tcode Check All Fail"
```

---

## 9. Complete FDT Calibration Algorithm

### Step-by-step process during init:

#### A. Read OTP data
1. Send `FpMcuGetOtp` command to read 32 bytes (F-series) or 64 bytes (HU) of OTP
2. Validate OTP with `CheckSensorOTP`

#### B. Extract calibration parameters from OTP
3. Call `GetTcodeAndDiffFromOtp`:
   - Extract `tcode_byte` from OTP[0x2A] (HU) or OTP[0x16] (F-series)
   - Compute `tcode = ((tcode_byte >> 4) + 1) << 4`
   - Compute `diff = (((tcode_byte & 0xF) + 2) * 100 * 256 / tcode) / 3 >> 4`
4. Call `GetFdtOffsetFromOtp`:
   - Read FDT offset byte from OTP (2-bit value, 0-3)
   - Uses triple-redundancy decoding

#### C. Apply calibration to sensor config
5. Call `ModifyFdtTcode`: Write `tcode` to config register **0x005C**
6. Call `ModifyFdtDelta`: Write `diff` to config register **0x0082**
7. Call `ModifyFdtOffset`: Merge `fdt_offset` into config register **0x0056** (low byte)

#### D. Initialize FDT parameters
8. Call `FDT_InitParameter` to set up:
   - Row/column counts for FDT area grid
   - Threshold buffer computed from `fdt_offset`
   - FDT area boundary tables
   - Buffer pointers for base data storage

#### E. Get FDT base data
9. Call `GetFdtManualBase`:
   - Sends command **0x36** (FDT_MANUAL) to sensor
   - Reads back raw FDT base data (up to 24 bytes = 12 words)
   - Converts to threshold format: `(raw_val >> 1) << 8 | 0x80`

#### F. Compute FDT thresholds
10. Call `CalcFdtDownBase` / `CalcFdtUpBase`:
    - For each base word: `threshold = ((base >> 1) + diff) << 8 | 0x80`
    - If diff == 0: `threshold = ((base >> 1) + 0x15) << 8 | 0x80`
    - Disabled areas get: `(diff - 2) << 8 | 0x80` or `0x1380`

#### G. Switch to FDT_DOWN for finger detection
11. Call `FpMcuSwitchToFdtMode(mode=1)`:
    - Build payload with prefix 0x1C (or 0x0C)
    - Include computed base/threshold data
    - Send command **0x32** (FDT_DOWN)

---

## 10. Command 0xd2

No direct string reference to "0xd2" or "command d2" was found in the DLL's string table. The command byte `0xd2` used before FDT in the Windows init trace is likely the **McuSetDrvState** command, which sets the driver state on the MCU side. The relevant strings found are:
- `McuSetDrvState` at 0x180204a28
- `McuSetDrvStateStub` at 0x1801a1a98

This is a state transition command telling the MCU firmware to enter a specific operational mode. It is likely necessary before FDT operations to ensure the MCU is in the correct state, but the exact payload format was not fully resolved from the disassembly (it's likely a simple 1-2 byte command with a state code).

---

## Key Constants Summary

| Constant | Value | Description |
|----------|-------|-------------|
| FDT_DOWN prefix | 0x1C | With base data flag set globally |
| FDT_DOWN prefix (alt) | 0x0C | Without base data flag |
| FDT_UP prefix | 0x0E | |
| FDT_MANUAL prefix | 0x0D | |
| CMD_FDT_DOWN | 0x32 | `(1 << 1) \| 0x30` |
| CMD_FDT_UP | 0x34 | `(2 << 1) \| 0x30` |
| CMD_FDT_MANUAL | 0x36 | `(3 << 1) \| 0x30` |
| Default diff | 0x15 (21) | Used when no OTP diff |
| Default disabled-area thresh | 0x1380 | Used when diff==0 and area disabled |
| Config reg: tcode | 0x005C | FDT tcode register |
| Config reg: delta/diff | 0x0082 | FDT delta register |
| Config reg: offset | 0x0056 | FDT offset register (low byte) |
| Config reg: DAC | 0x0220 | FDT DAC register |
| Config reg: area_num | 0x00CA | FDT area number register |
| Max base size | 0x18 (24) | 12 words max |
| HU base size | 0x08 (8) | 4 words for HU |
| OTP tcode offset (F-ser) | 0x16 | |
| OTP tcode offset (HU) | 0x2A | |
| OTP tcode complement (HU) | 0x2B | |
| OTP tcode backup (HU) | 0x2D | |
| HU OTP size | 0x40 (64) | |
| F-series OTP size | 0x20 (32) | |
| Bit 7 set constant | 0x80 | BTS instruction sets bit 7 |

---

## Function Address Map

| Address (VMA) | Function Name | Size |
|---------------|---------------|------|
| 0x1800526f4 | _FpMcuSwitchToFdtMode | 1366 |
| 0x1800573bf | GetFdtManualBase (ChicagoH) | 969 |
| 0x18005e33c | FDT_InitParameter | 3796 |
| 0x1800614c8 | (Caller: init chain for F-series) | - |
| 0x1800633e7 | MilanFSerCalcFdtUpBase | ~412 |
| 0x180063303 | MilanFSerCalcFdtDownBase (approx) | - |
| 0x18006384c | ConvertRawBaseToThreshold | 105 |
| 0x1800638b8 | CoreCalcBase (threshold computation) | 293 |
| 0x1800639e0 | _MilanFSerGetFdtOffsetFromOtp | 431 |
| 0x180063b90 | _MilanFSerGetTcodeAndDiffFromOtp | 516 |
| 0x180063d94 | _MilanFSerGetFdtAreaNum | 243 |
| 0x180063e88 | _MilanFSerModifyFdtDac | 258 |
| 0x180063f8c | _MilanFSerModifyFdtDelta | 258 |
| 0x180064090 | _MilanFSerModifyFdtOffset | 423 |
| 0x180064238 | _MilanFSerModifyFdtTcode | 258 |
| 0x1800655f0 | MilanFSerCheckSensorOtp | 370 |
| 0x180065764 | MilanHUCheckSensorOTP | 246 |
| 0x18006b4f0 | MilanG_TcodeCheck | 674 |
| 0x18006c88f | HUMilanFSerMcuGetFdtManualBase (area) | - |
| 0x18006d2c0 | HUFpMcuSwitchToFdtMode | 1414 |
| 0x18006d848 | _MilanHUSerGetTcodeAndDiffFromOtp | 856 |
| 0x18004ed98 | ReadConfigRegister | 226 |
| 0x18004ee7c | WriteConfigRegister | 333 |
| 0x18007d6b0 | SendMcuCommand (wrapper) | 111 |

---

## Critical Implementation Notes for libfprint Driver

1. **The FDT base data MUST be read via command 0x36 (FDT_MANUAL) before FDT_DOWN works.**
   Without base data, the sensor cannot compute proper touch thresholds.

2. **The threshold formula is: `((raw_base_val >> 1) + diff) << 8 | 0x80`**
   Where `diff` comes from OTP. If OTP is unavailable, default diff = 0x15 (21).

3. **For HU sensors (5e0a), the base data size is 8 bytes (4 words).**

4. **The FDT_DOWN command (0x32) payload byte[0] must include the prefix 0x1C, not just the mode.**
   Byte[0] = `0x1C | (speed_flag << 4)`

5. **The FDT_DOWN command byte[1] = 1 when base data is included in the payload.**

6. **Config registers 0x005C (tcode), 0x0082 (delta), 0x0056 (offset) MUST be set from OTP before FDT works correctly.** These are set during the CheckSensorOtp phase via the Modify* functions.

7. **The `bts eax, 7` instruction (bit test and set) sets bit 7 of eax, equivalent to `eax |= 0x80`.**
   This appears throughout the threshold calculations and is part of the hardware protocol format.
