# FDT_DOWN Payload Construction - Complete Reverse Engineering

Source: `Wbdi.dll` from Goodix FingerPrint V3.0.141.150_21H1_signed
Function: `SwitchToFdtMode` at `0x1800585c4` (chicagoh.c)
Sensor: 27c6:5e0a (ChicagoHU / MilanF series)

---

## 1. Payload Layout (35 bytes / 0x23)

```
Offset  Size  Field                    Source
------  ----  -----                    ------
[0]     1     prefix byte              STATIC: 0x1C | (speed_flag << 4)
[1]     1     has_base_data flag       1 if threshold data provided, else 0
[2..9]  8     DAC base values          OTP[0x32..0x35] zero-extended to 4 LE words
[10..21] 12   FDT thresholds           DYNAMIC: 6 x uint16_t LE, computed from runtime FDT_MANUAL read
[22..25] 4    padding                  zeros (unused threshold slots)
[26..33] 8    DAC base values (copy)   Same as [2..9], second copy for FDT_DOWN only
[34]    1     trailing zero            zero byte (allocation artifact)
```

Total size calculation: `threshold_size + base_size*2 + 7 = 12 + 8*2 + 7 = 35`

---

## 2. Byte-by-Byte Breakdown of Our Current Payload

```
1c 01  a6 00 a7 00 a6 00 a7 00  80 b1 80 c7 80 a8 80 be 80 b0 80 c1  00 00 00 00  a6 00 a7 00 a6 00 a7 00  00
│  │   └──────DAC base──────┘   └────────────thresholds───────────┘   └──pad──┘   └──────DAC copy──────┘   │
│  │   from OTP[0x32..0x35]     DYNAMIC from FDT_MANUAL + convert    zeros        same as [2..9]           zero
│  has_base=1
prefix=0x1C
```

---

## 3. Where Each Byte Comes From

### Byte [0]: Prefix (STATIC per mode)

Assembly at `0x1800586d3` and `0x180058943`:
```c
// Mode selection (0x1800586ca)
if (global_flag[0x18024686c] != 0)    // HV sensor flag, always 1 for 5e0a
    prefix = 0x1C;                      // FDT_DOWN with base data support
else
    prefix = 0x0C;                      // FDT_DOWN without base data

// Speed flag merge (0x180058948)
byte0 = prefix | (global_speed_flag[0x18025bff2] << 4);
// For 5e0a: speed_flag = 0, so byte0 = 0x1C | 0 = 0x1C
```

Prefix values by mode:
| Mode | Prefix | Constant |
|------|--------|----------|
| FDT_DOWN (mode=1) | 0x1C | HV sensor with base |
| FDT_DOWN (mode=1) | 0x0C | non-HV sensor |
| FDT_UP (mode=2) | 0x0E | always |
| FDT_MANUAL (mode=3) | 0x0D | always |

### Byte [1]: has_base_data (DYNAMIC but predictable)

Assembly at `0x18005897e`:
```c
byte1 = (arg3_threshold_ptr != NULL && arg4_threshold_size != 0) ? 1 : 0;
```

During normal FDT_DOWN after calibration: always 1.
During initial FDT_DOWN without calibration: 0.

### Bytes [2..9]: DAC Base Values (STATIC from OTP)

Assembly at `0x180058904`:
```c
// Copy 8 bytes from context+0x78 to payload+2
memcpy(payload + 2, context->dac_base, 8);
```

Source: OTP bytes [0x32..0x35], each zero-extended to uint16_t LE:
```
OTP[0x32] = 0xa6 -> 0x00a6 -> bytes: a6 00
OTP[0x33] = 0xa7 -> 0x00a7 -> bytes: a7 00
OTP[0x34] = 0xa6 -> 0x00a6 -> bytes: a6 00
OTP[0x35] = 0xa7 -> 0x00a7 -> bytes: a7 00
```

These are stored at `context+0x78` during `GetDacFromOtp` (`0x18005797c`).

### Bytes [10..21]: FDT Thresholds - THIS IS THE CRITICAL DYNAMIC SECTION

Assembly at `0x1800588bb`:
```c
// Copy threshold data to payload + (base_size + 2)
// base_size = var_54h = 8, so offset = 10
memcpy(payload + 10, arg3_threshold_data, arg4_threshold_size);
// arg4_threshold_size = 12 bytes = 6 words
```

**The thresholds are DYNAMIC.** They are computed at runtime from FDT_MANUAL sensor readings.

#### Threshold Computation Algorithm (CalcFdtDownBase / `ConvertRawBaseToThreshold`)

The CalcFdtDownBase function (`0x1800632a0`) does:
1. Copies raw FDT base data from sensor response
2. Calls `ConvertRawBaseToThreshold` (`0x18006384c`)

ConvertRawBaseToThreshold pseudo-code:
```c
void ConvertRawBaseToThreshold(uint16_t* data, int size_bytes) {
    for (int i = 0; i < size_bytes / 2; i++) {
        uint16_t raw = data[i];
        // Arithmetic shift right by 1 (preserves sign)
        int16_t shifted = (int16_t)raw >> 1;
        // Shift to high byte, set bit 7 in low byte
        data[i] = (shifted << 8) | 0x80;
    }
}
```

**Formula: `threshold[i] = ((raw_base[i] >> 1) << 8) | 0x80`**

#### Worked Example from Windows Log

Raw FDT base from sensor (FDT_MANUAL response):
```
0x015e, 0x017f, 0x0147, 0x016f, 0x014f, 0x016d
```

Computation:
| Zone | Raw Base | >> 1 | << 8 | \| 0x80 | Result (LE bytes) |
|------|----------|------|------|---------|-------------------|
| 0 | 0x015e (350) | 0xAF (175) | 0xAF00 | 0xAF80 | 80 AF |
| 1 | 0x017f (383) | 0xBF (191) | 0xBF00 | 0xBF80 | 80 BF |
| 2 | 0x0147 (327) | 0xA3 (163) | 0xA300 | 0xA380 | 80 A3 |
| 3 | 0x016f (367) | 0xB7 (183) | 0xB700 | 0xB780 | 80 B7 |
| 4 | 0x014f (335) | 0xA7 (167) | 0xA700 | 0xA780 | 80 A7 |
| 5 | 0x016d (365) | 0xB6 (182) | 0xB600 | 0xB680 | 80 B6 |

Windows log confirms: `fdt_downbase[0]:0xaf80, [1]:0xbf80, [2]:0xa380, [3]:0xb780, [4]:0xa780, [5]:0xb680`

### Bytes [22..25]: Padding Zeros

These are zeros because the threshold data is only 12 bytes (6 zones) but the
buffer may have been allocated larger. The remaining 4 bytes of the "threshold
region" are zeros.

### Bytes [26..33]: DAC Base Values (Second Copy)

Assembly at `0x1800589b2`:
```c
// Only for FDT_DOWN (mode == 1):
// Copy DAC base again at offset (base_size + threshold_size + 6)
// = 8 + 12 + 6 = 26
memcpy(payload + 26, context->dac_base, 8);
```

Same data as bytes [2..9].

### Byte [34]: Trailing Zero

Artifact of the allocation size (`35 = 12 + 16 + 7`). The last byte is
uninitialized or zero from the allocator.

---

## 4. CRITICAL: Thresholds Are DYNAMIC, Not Static

**The threshold values change with every FDT_MANUAL read.** They reflect the
current sensor capacitance baseline without a finger present.

### Why Our Current Static Payload Works Sometimes

Our hardcoded values `0xb180, 0xc780, 0xa880, 0xbe80, 0xb080, 0xc180` are from
a specific sensor reading at a specific time/temperature. They happen to be
close enough to the actual baseline that the sensor firmware can detect large
finger-touch deltas. But they will be WRONG when:

1. Temperature changes (sensor capacitance drifts)
2. Different hardware unit (different sensor baseline)
3. After power cycle (sensor state changes)

### What Windows Does (Complete Flow)

From the Windows WBDI.log, the init sequence is:

```
1. CheckTcodeAndDiff: extract tcode=256, fdt_delta=31 from OTP
2. Reset MCU
3. Set mode to IDLE (Mode 7)
4. Write 4 registers (config modifications for tcode/delta/offset)
5. Upload 256-byte config (cmd 0x90)
6. Set powerdown FDT scan frequency
7. TLS handshake
8. gf_update_all_base:
   a. gf_get_fdtbase 0: FDT_MANUAL -> read raw base -> compute fdt_downbase
   b. gf_get_navbase:  Mode 5 (NAV baseline)
   c. gf_get_fdtbase 1: FDT_MANUAL -> read raw base -> compute fdt_downbase
                         (second reading for stability)
   d. Check fdt_delta = 0x1f (31)
   e. gf_get_fdtbase 2: FDT_MANUAL -> third reading -> final base
   f. Nav_isTouchedByFinger check
   g. Image_isTouchedByFinger check
9. device_check_imagebase_exist: "Set FDT normal base and to FDT DOWN"
10. ChicagoHUSetMode: Mode 1 = FDT DOWN with computed thresholds
```

**Windows reads the FDT base THREE TIMES before switching to FDT_DOWN.**

---

## 5. CalcFdtDownBase vs CalcFdtUpBase

### CalcFdtDownBase (`0x1800632a0`) - simpler

```c
bool MilanFSerCalcFdtDownBase(ctx, raw_base, base_size, output_buf) {
    // 1. Validate base_size matches sensor config (context->fdt_base_size at offset 0x44)
    if (base_size != ctx->sensor_info->base_size) return false;

    // 2. Copy raw base data
    memcpy(output_buf, raw_base, base_size);

    // 3. Convert: threshold[i] = ((raw[i] >> 1) << 8) | 0x80
    ConvertRawBaseToThreshold(output_buf, base_size);

    return true;
}
```

**Does NOT add fdt_delta.** The DOWN thresholds are pure sensor baseline.

### CalcFdtUpBase (`0x1800633e0`) - adds delta

```c
bool MilanFSerCalcFdtUpBase(ctx, raw_base, base_size, area_bits, output_buf) {
    // 1. Validate base_size
    if (base_size != ctx->sensor_info->base_size) return false;

    // 2. Check fdt_diff is set
    uint16_t diff = ctx->fdt_diff;  // at context+0x68
    if (diff == 0) { log("Fdt diff not set"); return false; }

    // 3. Copy raw base
    memcpy(output_buf, raw_base, base_size);

    // 4. Apply delta: threshold[i] = ((raw[i] >> 1) + diff) << 8 | 0x80
    CoreCalcBase(output_buf, base_size, area_bits, diff);

    return true;
}
```

**Adds fdt_delta to each threshold.** The UP thresholds are higher (need bigger
change to trigger "finger lifted").

### CoreCalcBase (`0x1800638b8`)

```c
void CoreCalcBase(uint16_t* data, int size, uint16_t area_bits, uint16_t diff) {
    // Phase 1: Apply diff to each word
    for (int i = 0; i < size; i += 2) {
        uint16_t val = data[i/2];
        if (diff != 0) {
            val = (((int16_t)val >> 1) + diff) << 8 | 0x80;
        } else {
            val = (((int16_t)val >> 1) + 0x15) << 8 | 0x80;  // default diff=21
        }
        data[i/2] = val;
    }

    // Phase 2: Zero out disabled areas
    int bit = 0;
    for (int i = 0; i < size; i += 2) {
        if (!((area_bits >> bit) & 1)) {
            // Area disabled
            if (diff != 0)
                data[i/2] = ((diff - 2) << 8) | 0x80;
            else
                data[i/2] = 0x1380;  // default minimum
        }
        bit++;
    }
}
```

---

## 6. Threshold Format Explained

Each threshold word has the format: `0xTT80` (stored as LE bytes `80 TT`)

- **High byte (TT)**: The actual threshold value (0x00-0xFF range)
  - Represents the capacitance baseline reading for that zone, divided by 2
  - Higher value = higher baseline capacitance (varies with sensor/temp)
- **Low byte (0x80)**: Flag byte
  - Bit 7 is always set (`bts eax, 7` instruction = `|= 0x80`)
  - This is a hardware protocol requirement, not a threshold value
  - All 6 threshold words have low byte = 0x80

The sensor firmware compares incoming capacitance readings against these
thresholds. When the reading exceeds the threshold (finger present), it
generates an interrupt.

---

## 7. FDT Base Size for 5e0a

From Windows log: raw FDT base is 12 bytes (6 uint16_t words).
```
received fdt base::0x5e017f0147016f014f016d01
```
= `015e 017f 0147 016f 014f 016d` = 6 words

This means:
- `base_size` (at context->sensor_info+0x44) = 12
- Number of FDT zones = 6
- Threshold data = 12 bytes
- DAC base size (`var_54h`) = 8 bytes (4 words from OTP DAC values)

These are DIFFERENT sizes:
- FDT threshold zone count: 6 (from sensor capability)
- DAC channel count: 4 (from OTP)

---

## 8. Complete SwitchToFdtMode Pseudo-code (`0x1800585c4`)

```c
// chicagoh.c line ~612
int SwitchToFdtMode(
    void* context,            // rcx (arg1)
    int mode,                 // edx (arg2): 1=DOWN, 2=UP, 3=MANUAL
    void* threshold_data,     // r8  (arg3): computed FDT thresholds
    int threshold_size,       // r9d (arg4): size of threshold data
    // stack args:
    void* arg_e0h,            // callback related
    int arg_e8h,              // callback related
    int arg_f0h,              // dac_value from fcn.1800817ec
    int arg_f8h               // extra param
) {
    const int BASE_SIZE = 8;  // var_54h, DAC base size for ChicagoHU
    uint8_t prefix;
    bool has_base;
    uint8_t* payload;
    int payload_size;

    // 1. Select prefix byte
    switch (mode) {
        case 1:  // FDT_DOWN
            if (g_hv_flag != 0)
                prefix = 0x1C;
            else
                prefix = 0x0C;
            break;
        case 2:  // FDT_UP
            prefix = 0x0E;
            break;
        case 3:  // FDT_MANUAL
            prefix = 0x0D;
            break;
        default:
            log("invalid param");
            return 0;
    }

    // 2. Determine has_base_data
    has_base = (threshold_data != NULL && threshold_size != 0);

    // 3. Calculate payload size and allocate
    if (has_base) {
        payload_size = threshold_size + BASE_SIZE * 2 + 7;
    } else {
        payload_size = BASE_SIZE * 2 + 6;
    }
    payload = malloc(payload_size);

    // 4. Copy threshold data (if has_base) to payload[BASE_SIZE + 2]
    if (has_base) {
        memcpy(payload + BASE_SIZE + 2, threshold_data, threshold_size);
    }

    // 5. Copy DAC base from context+0x78 to payload[2]
    memcpy(payload + 2, &context->dac_base[0], BASE_SIZE);

    // 6. Set prefix byte
    payload[0] = prefix | (g_speed_flag << 4);

    // 7. Set has_base flag
    payload[1] = has_base ? 1 : 0;

    // 8. For FDT_DOWN: copy DAC base again at end
    if (mode == 1 && context->dac_base != NULL && BASE_SIZE > 0) {
        int end_offset = BASE_SIZE + threshold_size + 6;
        memcpy(payload + end_offset, &context->dac_base[0], BASE_SIZE);
    }

    // 9. Compute command byte and send
    uint16_t cmd = (mode << 1) | 0x30;
    // mode 1 -> cmd 0x32 (FDT_DOWN)
    // mode 2 -> cmd 0x34 (FDT_UP)
    // mode 3 -> cmd 0x36 (FDT_MANUAL)
    int ret = SendMcuCommand(spi_handle, cmd, payload, payload_size, ...);

    free(payload);
    return ret;
}
```

---

## 9. The fdt_delta Discrepancy: 29 vs 31

The `GetTcodeAndDiffFromOtp` function at `0x1800582d4` computes diff=29 from
our OTP byte 0xBC. But Windows logs `fdt delta 31`.

The Windows log shows:
```
ChicagoHU_check_sensor:01430 >>> read 64 bytes from base file
modify_fdt_delta:00485 >>> data index: 0xc7, before update: 0x80 0x15 after update:0x80 0x1f, delta:31
ChicagoHU_check_sensor:01476 >>> from otp, tcode 256, fdt delta 31
```

The "from base file" suggests Windows reads a cached OTP file that may have
slightly different values, or the `ChicagoHU_check_sensor` wrapper applies an
adjustment. The `modify_fdt_delta` shows delta=31=0x1f being written to config
register 0x0082 as bytes `0x80 0x1f` (LE: value `0x1f80`).

**For the FDT_DOWN thresholds, fdt_delta is NOT USED.** Only CalcFdtUpBase uses
fdt_delta. The FDT_DOWN thresholds are pure baseline conversions.

---

## 10. What Must Change in Our libfprint Driver

### Current Problem

The file `goodix5e0a.h` has STATIC hardcoded thresholds:
```c
static const guint8 goodix_5e0a_fdt_down_mode[] = {
  0x1c, 0x01,
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00,
  0x80, 0xb1, 0x80, 0xc7, 0x80, 0xa8, 0x80, 0xbe,
  0x80, 0xb0, 0x80, 0xc1,  // <-- these are WRONG/stale
  0x00, 0x00, 0x00, 0x00,
  0xa6, 0x00, 0xa7, 0x00, 0xa6, 0x00, 0xa7, 0x00,
  0x00
};
```

### Required Fix

1. **Send FDT_MANUAL (cmd 0x36) to read current sensor baseline**
2. **Parse the 12-byte raw base from the response** (6 x uint16_t LE)
3. **Compute thresholds dynamically:**
   ```c
   for (int i = 0; i < 6; i++) {
       uint16_t raw = le16_read(base_data + i*2);
       uint16_t threshold = ((raw >> 1) << 8) | 0x80;
       le16_write(fdt_payload + 10 + i*2, threshold);
   }
   ```
4. **Build the 35-byte FDT_DOWN payload with fresh thresholds**
5. **Send FDT_DOWN (cmd 0x32)**

### Complete Flow (Matching Windows)

```
1. Read OTP (cmd 0xa6)
2. Extract tcode, diff, fdtOffset, DAC values from OTP
3. Build and upload 256-byte config with tcode/diff/offset patches (cmd 0x90)
4. Read FDT base 3x via FDT_MANUAL (cmd 0x36) - for stability
5. Compute FDT_DOWN thresholds from final base reading
6. Build FDT_DOWN payload with dynamic thresholds
7. Send FDT_DOWN (cmd 0x32)
8. Wait for touch interrupt
```

### FDT_MANUAL Payload (14 bytes for cmd 0x36)

```
[0] = 0x0D          (MANUAL prefix)
[1] = 0x01          (has_base=1)
[2..9] = DAC base   (a6 00 a7 00 a6 00 a7 00)
[10..13] = zeros    (no threshold data for MANUAL)
```

FDT_MANUAL total size: `0 + 8*2 + 6 = 22 bytes` (when has_base=0)
or `0 + 8*2 + 7 = 23 bytes` (when has_base=1 with 0 threshold bytes)

Actually from the log: `cmd0-cmd1-Len-ackt:0x3-3-0xe-1000` shows Len=0xe=14.
This is the HAL-level payload, not the MCU-level. The HAL adds its own header.

The FDT_MANUAL response contains:
```
[0..1] = interrupt/status word
[2..3] = touch_flag (should be 0x0000 for manual)
[4..15] = raw FDT base data (12 bytes = 6 words)
```

Total response: 17 bytes (from log: `cmd0-cmd1-cmd2-len: 0x3-0x3-0x0-17`)

---

## 11. FDT_UP Thresholds (for completeness)

For FDT_UP, the thresholds use a DIFFERENT formula that includes fdt_delta:

```c
threshold_up[i] = (((raw_base[i] >> 1) + fdt_delta) << 8) | 0x80;
```

With fdt_delta=31 and raw_base[0]=0x015e:
```
(350 >> 1) + 31 = 175 + 31 = 206 = 0xCE
threshold = 0xCE80
```

The UP thresholds are HIGHER than DOWN thresholds, meaning the sensor needs a
LARGER signal change to trigger "finger lifted" than "finger placed."

---

## 12. Summary of Data Sources

| Payload Bytes | Source | Static/Dynamic | Changes When |
|---------------|--------|----------------|--------------|
| [0] prefix    | Hardcoded per sensor type | STATIC | Never |
| [1] has_base  | Runtime flag | STATIC (always 1 after init) | - |
| [2..9] DAC    | OTP[0x32..0x35] | STATIC per device | Different sensor unit |
| [10..21] thresh | FDT_MANUAL reading | **DYNAMIC** | Temperature, power cycle, time |
| [22..25] pad  | zeros | STATIC | Never |
| [26..33] DAC copy | Same as [2..9] | STATIC per device | Different sensor unit |
| [34] trail    | zero | STATIC | Never |

**The thresholds (bytes 10-21) are the ONLY dynamic part.** Everything else is
static for a given sensor unit.

---

## 13. Windows Log Confirmation of Exact Payloads

### FDT_MANUAL sends (3 rounds during init)

**Round 0** (no previous base, uses OTP-derived data):
```
base data sent: 0xafafbfbfa4a4b8b8a8a8b7b7
HAL payload: [0D 01] [af af bf bf a4 a4 b8 b8 a8 a8 b7 b7]
```

**Round 1** (uses converted base from round 0):
```
base data sent: 0x80af80bf80a380b780a780b6
HAL payload: [0D 01] [80 af 80 bf 80 a3 80 b7 80 a7 80 b6]
```

**Round 2** (uses converted base from round 1):
```
base data sent: 0x80af80bf80a480b880a780b7
HAL payload: [0D 01] [80 af 80 bf 80 a4 80 b8 80 a7 80 b7]
```

### FDT_DOWN send (after all 3 manual rounds)

```
base data sent: 0x80af80bf80a480b880a880b7
HAL payload: [1C 01] [80 af 80 bf 80 a4 80 b8 80 a8 80 b7]
cmd 0x32 confirmed: "get ack for cmd: 0x32"
```

Note: The FDT_DOWN thresholds use the THIRD (final) FDT_MANUAL reading's base,
re-converted. They differ slightly from round 2 because a new FDT_MANUAL read
was done between round 2 and the FDT_DOWN send.

### FDT base readings (raw from sensor)

```
Round 0: 0x5e017f0147016f014f016d01
Round 1: 0x5e017f01490170014f016f01
Round 2: (similar, from third FDT_MANUAL)
```

6 zones, 12 bytes, LE uint16_t:
`[0x015e, 0x017f, 0x0147/0x0149, 0x016f/0x0170, 0x014f, 0x016d/0x016f]`

### Threshold computation verification

Raw 0x015e -> (350 >> 1) = 175 = 0xAF -> (0xAF << 8) | 0x80 = 0xAF80 -> LE bytes: 80 AF
Matches log: fdt_downbase[0] = 0xaf80

---

## 14. Key Function Addresses

| Address | Function | Source File |
|---------|----------|-------------|
| 0x1800585c4 | SwitchToFdtMode (ChicagoH) | chicagoh.c |
| 0x180057020 | SwitchToFdtDown (wrapper) | chicagoh.c |
| 0x1800571a0 | SwitchToFdtUp (wrapper) | chicagoh.c |
| 0x180057380 | GetFdtManualBase | chicagoh.c |
| 0x1800632a0 | MilanFSerCalcFdtDownBase | milanfser.c |
| 0x1800633e0 | MilanFSerCalcFdtUpBase | milanfser.c |
| 0x18006384c | ConvertRawBaseToThreshold | milanfser.c |
| 0x1800638b8 | CoreCalcBase (adds diff) | milanfser.c |
| 0x1800582d4 | GetTcodeAndDiffFromOtp | chicagoh.c |
| 0x18005797c | GetDacFromOtp | chicagoh.c |
| 0x18007d6b0 | SendMcuCommand | fpmcucmd.c |
