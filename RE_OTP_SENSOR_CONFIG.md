# OTP & Sensor Configuration Reverse Engineering

Source: `Wbdi.dll` from Goodix FingerPrint_V3.0.141.150_21H1_signed
Tool: radare2 disassembly of x86-64 PE
Sensor: 27c6:5e0a (Chicago HU / MilanF series)

## 1. OTP Data Structure (64 bytes, cmd 0xa6)

Our device's OTP:
```
5743433632382e00 988483aaa61ab209
0105030800003000 0000000cf1639c09
07000000e463e5fd 014ebc4300bca6a7
a6a7a6a7a6a70000 e41be51a7f1d30bc
```

### Byte-by-byte map

| Offset | Hex  | Field | Notes |
|--------|------|-------|-------|
| 0x00-0x07 | `57 43 43 36 32 38 2e 00` | Wafer ID | ASCII "WCC628.\0" |
| 0x08 | `98` | CP data | |
| 0x09 | `84` | CP data | |
| 0x0a | `83` | CP data | |
| 0x0b-0x13 | `aa a6 1a b2 09 01 05 03 08` | FT calibration (9 bytes) | Used in FT CRC |
| 0x14-0x1b | `00 00 30 00 00 00 00 0c` | MT section start (8 bytes) | |
| 0x1b | `0c` | **FDT offset byte** | Encodes fdtOffset with redundancy |
| 0x1c | `f1` | FT single byte | Used in FT CRC check |
| 0x1d-0x23 | `63 9c 09 07 00 00 00 e4` | MT section (7 bytes) | |
| 0x24-0x27 | `63 e5 fd 01` | CP section (4 bytes) | Used in CP CRC |
| 0x28 | `01` | MT flags byte | Bit fields (HV: presence flags for DAC zones) |
| 0x29 | `4e` | MT data | (HV: DACH zone 1 low byte) |
| 0x2a | `bc` | **Tcode primary** | Used to compute tcode and diff |
| 0x2b | `43` | **Tcode complement** | Must equal NOT(0x2a) for validation |
| 0x2c | `00` | | |
| 0x2d | `bc` | **Tcode backup** | Second copy for redundancy |
| 0x2e-0x31 | `a6 a7 a6 a7` | (HV: DACH zone 3 data) | |
| 0x32 | `a6` | **DAC zone 0** | FDT base value |
| 0x33 | `a7` | **DAC zone 1** | FDT base value |
| 0x34 | `a6` | **DAC zone 2** | FDT base value |
| 0x35 | `a7` | **DAC zone 3** | FDT base value |
| 0x36-0x37 | `00 00` | Padding | |
| 0x38-0x3b | `e4 1b e5 1a` | FT calibration (4 bytes) | Used in FT CRC |
| 0x3c | `7f` | **CP CRC8** | CRC of CP section |
| 0x3d | `1d` | **FT CRC8** | CRC of FT section |
| 0x3e | `30` | FT additional byte | Included in FT CRC input |
| 0x3f | `bc` | **MT CRC8** | CRC of MT section |

**IMPORTANT correction**: The byte at offset 0x2a in the GetTcodeAndDiffFromOtp function
is loaded as `imul rax, 1, 0x2a` which computes `1 * 0x2a = 0x2a = 42`. So the
tcode byte is at OTP[42] = 0xbc. Similarly, complement at OTP[43] = 0x43,
backup tcode at OTP[45] = 0xbc.

## 2. GetTcodeAndDiffFromOtp (fcn.1800582d4)

**Source file**: `chicagoh.c`
**Function name**: `GetTcodeAndDiffFromOtp`
**Address**: `0x1800582d4` (751 bytes)

### Parameters
```
arg1 (rcx) = OTP data pointer (64 bytes)
arg2 (edx) = OTP size (must be 0x40 = 64)
arg3 (r8)  = output: tcode (uint16_t*)
arg4 (r9)  = output: diff/fdt_delta (uint16_t*)
```

### Tcode byte extraction (triple-redundancy voting)

Three bytes from OTP are read:
- `var_50h` = OTP[0x2a] (offset 42) -- primary tcode
- `var_53h` = OTP[0x2b] (offset 43) -- complement verification
- `var_51h` = OTP[0x2d] (offset 45) -- backup tcode

Validation priority:
1. If `OTP[0x2a] != 0` AND `OTP[0x2a] == NOT(OTP[0x2b])`: use OTP[0x2a]
2. If `OTP[0x2d] != 0` AND `OTP[0x2d] == NOT(OTP[0x2b])`: use OTP[0x2d]
3. If `OTP[0x2a] != 0` AND `OTP[0x2a] == OTP[0x2d]`: use OTP[0x2a]
4. Otherwise: fail ("no tcode and diff")

For our device: OTP[0x2a]=0xbc, NOT(0xbc)=0x43=OTP[0x2b]. Match on case 1.

### Tcode computation (from resolved byte, e.g., 0xbc)

```c
uint8_t resolved = 0xbc;
uint16_t tcode = ((resolved >> 4) + 1) * 16 + 64;
// = (11 + 1) * 16 + 64 = 192 + 64 = 256 = 0x100
```

Assembly at `0x1800584fc`:
```asm
movzx eax, byte [var_52h]  ; resolved tcode byte
sar   eax, 4               ; >> 4 (arithmetic, but byte is unsigned via movzx)
inc   eax                   ; + 1
imul  eax, eax, 0x10        ; * 16
add   eax, 0x40             ; + 64
mov   word [var_58h], ax    ; store as tcode (uint16)
```

### Diff (fdt_delta) computation

```c
uint8_t resolved = 0xbc;
uint16_t tcode = 256;  // from above
int32_t intermediate = ((resolved & 0xf) + 2) * 100;  // (12+2)*100 = 1400
intermediate = (intermediate << 8) / tcode;            // 358400 / 256 = 1400
intermediate = (intermediate / 3) >> 4;                // 466 >> 4 = 29
uint16_t diff = (uint16_t)intermediate;                // 29 = 0x1d
```

Assembly at `0x180058511`:
```asm
movzx eax, byte [var_52h]  ; resolved byte
and   eax, 0xf             ; low nibble
add   eax, 2               ; + 2
imul  eax, eax, 0x64       ; * 100
shl   eax, 8               ; * 256
movzx ecx, word [var_58h]  ; load tcode
cdq                         ; sign-extend eax->edx:eax
idiv  ecx                   ; / tcode
mov   word [var_54h], ax    ; intermediate
movzx eax, word [var_54h]  ; reload
cdq
mov   ecx, 3
idiv  ecx                   ; / 3
sar   eax, 4               ; >> 4
mov   word [var_54h], ax    ; final diff
```

**Result for our device**: tcode=256 (0x100), diff=29 (0x1d)

**Note**: The Windows log reports "from otp, tcode 256, fdt delta 31". The discrepancy
(29 vs 31) may come from the ChicagoHU_check_sensor wrapper adjusting the diff value,
or from a different code path in the actual sensor-specific logic layer. The core
`GetTcodeAndDiffFromOtp` function produces diff=29.

**Default values** (when OTP extraction fails): tcode=128 (0x80), diff=21 (0x15).

## 3. GetFdtOffsetFromOtp (fcn.1800639e0)

**Source file**: `milanfser.c`
**Function name**: `_MilanFSerGetFdtOffsetFromOtp`
**Address**: `0x1800639e0` (431 bytes)

### Parameters
```
arg1 (rcx) = OTP data pointer
arg2 (edx) = OTP size (must be >= 0x20)
arg3 (r8b) = byte offset in OTP to read (0x1b for our sensor)
arg4 (r9)  = output: fdtOffset (uint8_t*)
```

### Algorithm

Reads single byte at `OTP[0x1b]` (= 0x0c for our device).
The byte encodes the offset value with triple redundancy in bit fields:

```
Bits [1:0] = primary value
Bits [5:4] = duplicate value
Bits [3:2] = NOT of value (complement check)
```

Validation priority:
1. If `(byte & 3) == ((byte >> 4) & 3)`: use `byte & 3`
2. If `(byte & 3) == ((~byte >> 2) & 3)`: use `byte & 3`
3. If `((byte >> 4) & 3) == ((~byte >> 2) & 3)`: use `(byte >> 4) & 3`
4. Otherwise: "no fdt offset otp value"

For our device: OTP[0x1b] = 0x0c = 0b00001100
- Bits [1:0] = 0b00 = 0
- Bits [5:4] = 0b00 = 0
- Case 1 matches: fdtOffset = 0

**Result**: fdtOffset = 0 (matches Windows log "fdtOffset:0x0")

## 4. GetDacFromOtp (fcn.18005797c)

**Source file**: `chicagoh.c`
**Function name**: `GetDacFromOtp`
**Address**: `0x18005797c` (2389 bytes)

### OTP byte ranges used for CRC verification

The function assembles data from scattered OTP regions into a temp buffer, then
computes CRC8 to validate integrity:

**First CRC check (FT area)**:
- temp[0x00..0x08] = OTP[0x0b..0x13] (9 bytes)
- temp[0x09]       = OTP[0x1c]       (1 byte)
- temp[0x0a..0x0d] = OTP[0x32..0x35] (4 bytes -- the DAC values!)
- temp[0x0e..0x11] = OTP[0x38..0x3b] (4 bytes)
- temp[0x12]       = OTP[0x3e]       (1 byte)
- CRC8 over 0x13 bytes -> compare with OTP[0x3d]

**Second CRC check (CP area)**:
- temp[0x00..0x03] = OTP[0x32..0x35] (4 bytes)
- CRC8 over 4 bytes -> compare with OTP[0x3c]? (from code pattern at 0x180057b88)

### DAC value extraction (on CRC pass)

```c
// DAC zone 0 (word, zero-extended from byte)
output[0] = OTP[0x32];  // 0xa6
output[1] = OTP[0x33];  // 0xa7
output[2] = OTP[0x34];  // 0xa6
output[3] = OTP[0x35];  // 0xa7
```

These are stored as 16-bit words (byte zero-extended) in the context structure at
offset 0x70 (read DAC) and copied to offset 0x78 (FDT base values).

**Result for our device**: DAC = [0xa6, 0xa7, 0xa6, 0xa7]

These exact values appear in our FDT_DOWN_PAYLOAD at bytes [2..9] and [26..33]:
`a600 a700 a600 a700`

## 5. GetChipConfig (at ~0x1800567a0, inside fcn.1800550c4)

**Source file**: `chicagoh.c`
**Function name**: `GetChipConfig`

### Flow

1. Call `GetTcodeAndDiffFromOtp` -> get tcode, diff
2. On success: store tcode at `context[0x6a]`, diff at `context[0x68]`
3. On failure: use defaults tcode=0x80 (128), diff=0x15 (21)
4. Call `_MilanFSerGetFdtOffsetFromOtp` with offset byte 0x1b -> get fdtOffset
5. Call `GetDacFromOtp` -> get 4 DAC values into `context[0x70]` (8 bytes as words)
6. Copy DAC to `context[0x78]` (FDT base values)
7. Allocate 256 bytes, copy **default config template** from `0x18019dee0`
8. Compute checksum: `fcn.18004efcc(config, 0x7f)` -> store at config[0xfe..0xff]
9. If tcode > 0: call `_MilanFSerModifyImageTcode(config, tcode, 0)`
10. If diff > 0: compute `(diff << 8) | BIT(7)`, call `_MilanFSerModifyFdtDelta(config, value, 0)`
11. If fdtOffset > 0: call `_MilanFSerModifyFdtOffset(config, fdtOffset + 8, 0)`
12. Store config pointer and size (0x100) in output params

### Default config template (256 bytes at 0x18019dee0)

```
b011 6071 2c9d 2cc9 1ce5 18fd 00fd 00fd
03ba 0001 80ca 0004 0084 0015 b386 0000
c488 0000 ba8a 0000 b28c 0000 aa8e 0000
c190 00bb bb92 00b1 b194 0000 a896 0000
b698 0000 009a 0000 00d2 0000 00d4 0000
00d6 0000 00d8 0000 0050 0001 05d0 0000
0070 0000 0072 0078 5674 0034 1220 0010
402a 0102 0422 0001 2024 0032 0080 0001
005c 0080 0056 0024 2058 0003 0232 000c
0266 0003 007c 0000 5882 0080 152a 0182
0322 0001 2024 0014 0080 0001 005c 0000
0156 0004 2058 0003 0232 000c 0266 0003
007c 0000 5882 0080 152a 0108 005c 0080
0054 0010 0162 0004 0364 0019 0066 0003
007c 0001 582a 0108 005c 0000 0152 0008
0054 0000 0166 0003 007c 0001 5800 bcff
```

The config is structured as sections of big-endian register address/value pairs
(2 bytes reg + 2 bytes value), separated by end markers (reg 0x007c).

Notable registers in the config:
- `0x005c` = Image tcode register (default value: 0x0080 = 128)
- `0x0056` = FDT offset register (default value: 0x0024)
- `0x0082` (at 0x5882 entries) = FDT delta (default value: 0x0080 then 0x0015)
- `0x002a` = Nav tcode

## 6. MCU Config Upload (cmd 0x90)

**Source file**: `fpmcucmd.c`
**Function name**: `FpMcuDownloadChipConfig`
**Address**: ~`0x180050ec0`

### Flow

```
FpMcuDownloadChipConfig(device, config_ptr, config_size):
  1. validate(config_ptr, config_size)     // fcn.1800c7940 with arg=2
  2. get_spi_handle(device)                // fcn.1800818cc
  3. send_command(spi, 0x90, config_ptr, config_size, ...)  // fcn.18007d6b0
     // Command byte 0x90 is loaded at 0x180050fd8: mov dx, 0x90
  4. Check ACK byte == 0x01
```

The `0x90` command sends the **entire 256-byte config** to the MCU. This config
is derived from the default template with OTP-based modifications applied to
tcode, fdt_delta, and fdtOffset register values.

**This is necessary for FDT to work correctly.** The config tells the MCU how
to configure the sensor's analog front-end for finger detection.

### Config modification functions

**_MilanFSerModifyImageTcode** (fcn.18006433c):
- Target register: 0x005c
- Walks config buffer looking for reg 0x5c entries
- Replaces the value with the new tcode
- Recomputes config checksum

**_MilanFSerModifyFdtDelta** (fcn.180063f8c):
- Target register: 0x0082
- Replaces value with `(delta << 8) | BIT(7)`
- Recomputes config checksum

**_MilanFSerModifyFdtOffset** (fcn.180064090):
- Target register: 0x0056
- Reads current value, preserves high byte, replaces low byte with `fdtOffset + 8`
- Recomputes config checksum

### Config register write mechanism (fcn.18004ee7c)

The generic register setter walks the config in 4-byte steps:
```c
for (ptr = config + start; ptr + 4 <= config + end; ptr += 4) {
    uint16_t reg = *(uint16_t*)ptr;       // register address (BE)
    if (reg == target_register) {
        *(uint16_t*)(ptr + 2) = new_value; // write value
        recompute_checksum(config, 0x7f);
        config[0xfe] = checksum;
        return 1;
    }
}
```

## 7. FDT Payload Structure (cmd 0x36 data)

### SwitchToFdtMode (fcn.1800585c4)

The FDT payload sent with mcu_switch_to_fdt_down (cmd 0x36) has this structure:

```
Byte [0]:   Mode byte = mode_type | (touch_enhance_flag << 4)
            mode_type: 0x0c (fdt_down, normal)
                       0x1c (fdt_down, touch_enhance=1)
                       0x0e (fdt_up)
                       0x0d (fdt_manual)
            touch_enhance_flag: read from global at 0x18025bff2

Byte [1]:   Base flag (0x01 if FDT base data is provided, 0x00 if not)

Bytes [2..9]:  DAC base values from context+0x78 (4 x uint16_t LE)
               = the OTP DAC values zero-extended to words
               For our device: a6 00 a7 00 a6 00 a7 00

Bytes [10..N]: FDT threshold data (if base_flag=1, copied from caller)
               For fdt_down: this is the actual FDT detection thresholds
               Contains 6 x uint16_t threshold values

Bytes [N+1..]: Second copy of DAC values (for fdt_down mode only)

Trailing [0]:  Zero byte
```

### Our FDT_DOWN_PAYLOAD decoded

```
1c              mode=0x0c | (0x01 << 4) = 0x1c (fdt_down, touch_enhance=1)
01              base_flag=1 (threshold data follows)
a600 a700       DAC zones 0,1 (from OTP[0x32..0x33])
a600 a700       DAC zones 2,3 (from OTP[0x34..0x35])
80b1 80c7       FDT thresholds zone pair 0 (hi=threshold, lo=0x80 flag)
80a8 80be       FDT thresholds zone pair 1
80b0 80c1       FDT thresholds zone pair 2
0000 0000       Padding/unused thresholds
a600 a700       DAC zones 0,1 (second copy for fdt_down)
a600 a700       DAC zones 2,3 (second copy for fdt_down)
00              Trailing zero
```

Total: 35 bytes

### FDT Threshold Values (the 80xx bytes)

The 6 threshold values `0xb1, 0xc7, 0xa8, 0xbe, 0xb0, 0xc1` with `0x80` prefix:

The `0x80` byte appears to be a flag/type indicator. The actual threshold values
represent finger-detection sensitivity levels for different sensor zones. These
values are NOT directly computed from OTP in the GetChipConfig path -- they come
from a runtime calibration step (likely `pov_image_check` / finger detection
calibration that reads the sensor and computes appropriate thresholds).

The threshold format is: `[flag_byte] [threshold_byte]` as 16-bit LE words.
- Flag 0x80 likely means "valid/enabled threshold"
- The threshold byte is the actual detection level

These threshold values are zone-specific and represent the signal level difference
between "no finger" and "finger present" states. They are stored in the driver's
context and reused across FDT operations.

## 8. OTP CRC Verification (MilanHU_OTPCRCCheck, fcn.18006585c)

Three independent CRC checks validate different OTP regions:

### MT (Mass Test) CRC
- Temp buffer built from:
  - OTP[0x16..0x1b] (6 bytes, offset `1*0x16`)
  - OTP[0x1d..0x23] (7 bytes, offset `1*0x1d`)
  - OTP[0x28..0x31] (10 bytes, offset `1*0x28`)
- CRC8 over 23 (0x17) bytes
- Compare with OTP[0x3f]

### FT (Final Test) CRC
- Temp buffer built from:
  - OTP[0x0b..0x15] (11 bytes, offset `1*0x0b`)
  - OTP[0x1c] (1 byte, offset `1*0x1c`)
  - OTP[0x32..0x3b] (10 bytes, offset `1*0x32`)
  - OTP[0x3e] (1 byte, offset `1*0x3e`)
- CRC8 over 23 (0x17) bytes
- Compare with OTP[0x3d]

### CP (Chip Probe) CRC
- Temp buffer built from:
  - OTP[0x00..0x0a] (11 bytes)
  - OTP[0x24..0x27] (4 bytes, offset `1*0x24`)
- CRC8 over 15 (0x0f) bytes
- Compare with OTP[0x3c]

CRC8 computation: `fcn.1800c7910(data, length)` -- standard CRC-8 algorithm.

## 9. MilanHV Series OTP (for reference, fcn.180079878)

The MilanHV series (different from our Chicago/MilanF sensor) uses a more complex
OTP layout with bit-field packing. Key differences:

| Field | MilanF offset | MilanHV offset | MilanHV encoding |
|-------|--------------|----------------|------------------|
| Tcode | OTP[0x2a] (full byte) | OTP[0x17] & 0x3e | Shifted, 5-bit field |
| FDT tcode | same as tcode | OTP[0x1b] * 2 | Separate field |
| Touch diff | N/A | OTP[0x20] | Direct byte |
| DAC H zone 0 | OTP[0x32] | (OTP[0x11]&1)<<8 \| OTP[0x16] | 9-bit value |
| DAC L zone 0 | N/A | (OTP[0x11]&0x40)<<2 \| OTP[0x1f] | 9-bit value |
| DAC H zone 1 | OTP[0x33] | (OTP[0x28]&1)<<8 \| OTP[0x29] | 9-bit value |
| DAC H zone 2 | OTP[0x34] | (OTP[0x28]&2)<<7 \| OTP[0x2a] | 9-bit value |
| DAC H zone 3 | OTP[0x35] | (OTP[0x28]&4)<<6 \| OTP[0x2b] | 9-bit value |

The MilanHV variant uses OTP[0x17] bit 7 as a flag for `context[0xa4]`
(likely high-voltage mode indicator).

## 10. Summary: What the 5e0a Driver Needs

### Minimum viable configuration

1. **Read OTP** (cmd 0xa6) -> 64 bytes
2. **Extract tcode** from OTP[0x2a] with complement validation
3. **Compute tcode** = `((byte >> 4) + 1) * 16 + 64`
4. **Compute diff** = `(((byte & 0xf) + 2) * 100 * 256 / tcode) / 3) >> 4`
5. **Extract fdtOffset** from OTP[0x1b] with redundancy voting
6. **Extract DAC** values from OTP[0x32..0x35]
7. **Build MCU config** (256 bytes) from template, patch registers:
   - Reg 0x005c = tcode
   - Reg 0x0082 = `(diff << 8) | 0x80`
   - Reg 0x0056 low byte = `fdtOffset + 8` (if fdtOffset > 0)
   - Recompute checksum at config[0xfe..0xff]
8. **Upload config** via cmd 0x90 (256 bytes)
9. **Build FDT payload** with DAC values and thresholds
10. **Send FDT** via cmd 0x36

### For our specific device (computed values)

| Parameter | Value | Source |
|-----------|-------|--------|
| tcode | 256 (0x100) | OTP[0x2a]=0xbc |
| diff/fdt_delta | 29 (0x1d) | Computed from OTP[0x2a] |
| fdtOffset | 0 | OTP[0x1b]=0x0c |
| DAC zones | [0xa6, 0xa7, 0xa6, 0xa7] | OTP[0x32..0x35] |
| mode byte | 0x1c | fdt_down + touch_enhance=1 |

### Register addresses

| Register | Purpose | Default | Modified |
|----------|---------|---------|----------|
| 0x005c | Image tcode | 0x0080 (128) | 0x0100 (256) |
| 0x0082 | FDT delta | 0x0015 | `(diff<<8)\|0x80` = 0x1d80 |
| 0x0056 | FDT offset | 0x0024 | unchanged (fdtOffset=0) |

### Is config upload (0x90) required?

**Yes, almost certainly.** The config tells the MCU how to configure the sensor's
analog parameters. Without it, the MCU uses its own defaults which may not match
the OTP-calibrated values. This could explain why FDT works sometimes but
verification fails -- the sensor may use uncalibrated tcode/delta values.

### FDT threshold origin

The threshold values (0xb1, 0xc7, 0xa8, 0xbe, 0xb0, 0xc1 in our payload) appear
to come from a runtime calibration step rather than directly from OTP. The Windows
driver likely performs a baseline read during `pov_image_check` and computes
per-zone thresholds from the measured values. The exact computation is in the
caller of SwitchToFdtMode, not in the OTP processing path.

### Differences from 511 driver

The 511d sensor (MilanHV series) writes to registers 0x0220, 0x0236, 0x0238, 0x023a
based on OTP. The 5e0a (MilanF/Chicago series) uses a completely different register
set (0x005c, 0x0056, 0x0082) and a different OTP byte layout. The 5e0a uses simpler
8-bit DAC values while the 511d uses 9-bit packed values.
