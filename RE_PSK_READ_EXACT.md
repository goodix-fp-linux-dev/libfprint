# PSK READ (cmd 0xE4) -- Exact Wire Bytes from Windows Driver RE

Source: `Wbdi.dll` from Goodix FingerPrint V3.0.141.150
Functions: `PresetPskReadSpecDataG` @ `0x18003e500`, `PresetPskReadG` @ `0x18009c6f8`
Source file (from debug strings): `pskunify.c` and `geneva.c`

## 1. Payload Format (16 bytes, little-endian)

```
Offset  Size  Field       Description
------  ----  ----------  ------------------------------------------
0x00    4     length      Bytes to read this chunk (max 0x100 = 256)
0x04    4     offset      Cumulative byte offset into data
0x08    4     type        Data type (e.g., 0xBB010002)
0x0C    4     reserved    Always 0x00000000
```

Total payload size: **16 bytes** (0x10), sent with command **0xE4** (PRESET_PSK_READ).

## 2. Exact Hex Bytes for Type 0xBB010002 (DPAPI-sealed PSK blob, 332 bytes)

The Windows driver pages reads at 256 bytes per chunk. For 332 bytes of PSK data,
the MCU response includes a 9-byte header (1 status + 4 flags + 4 data_length),
so total response = 332 + 9 = 341 bytes. ceil(341 / 256) = **2 iterations**.

### First read (iteration 0): request 256 bytes at offset 0

```
cmd: 0xE4
payload (16 bytes): 00 01 00 00  00 00 00 00  02 00 01 BB  00 00 00 00
                    |length=256| |offset=0  | |type      | |reserved  |
recvBufSize: 0x200 (512, initial value)
```

MCU responds with 265 bytes (0x109): 1 status + 4 flags + 4 data_len + 256 data.

### Second read (iteration 1): request remaining 76 bytes at offset 256

```
var_60h = 341 - 256 - 9 = 76 (0x4C)

cmd: 0xE4
payload (16 bytes): 4C 00 00 00  00 01 00 00  02 00 01 BB  00 00 00 00
                    |length=76 | |offset=256| |type      | |reserved  |
recvBufSize: 0x109 (265, carried over from first call's actual recv count)
```

MCU responds with 85 bytes: 1 status + 4 flags + 4 data_len + 76 data.

## 3. Exact Hex Bytes for Type 0xBB020001 (PSK SHA-256 hash, 32 bytes)

Total response = 32 + 9 = 41 bytes. Fits in one chunk (41 <= 256).
ceil(41 / 256) = 1 iteration. Since it is the last (only) iteration:
  `length = 41 - 0 - 9 = 32`

```
cmd: 0xE4
payload (16 bytes): 20 00 00 00  00 00 00 00  01 00 02 BB  00 00 00 00
                    |length=32 | |offset=0  | |type      | |reserved  |
recvBufSize: 0x200 (512)
```

## 4. Type 0xBB010003 (Whitebox-encrypted PSK)

**0xBB010003 is NEVER read from the device.** It appears only once in the binary,
inside `PresetPskWriteKey` (@ `0x18003f022`), where it is used as the type tag
for **writing** the whitebox-encrypted PSK blob to the MCU. The driver never calls
`PresetPskReadG` with type 0xBB010003.

The flow in the Windows driver is:
1. Read DPAPI-sealed blob: `PresetPskReadSpecDataG(0xBB010002)` -> `PresetPskReadG`
2. Unseal with DPAPI on host
3. Re-encrypt with whitebox crypto
4. Write whitebox blob to MCU: `PresetPskWriteKey` with type 0xBB010003

## 5. Why Two Reads in the Log (Pagination Loop)

The log shows:
```
PresetPskReadG: 1.sendDataToDeviceEx
  sendCmd, cmd: 0xe4, outDataSize: 0x10, recvBufSize: 0x200
PresetPskReadG: 2.parase return data
PresetPskReadG: 1.sendDataToDeviceEx
  sendCmd, cmd: 0xe4, outDataSize: 0x10, recvBufSize: 0x109
```

This is a **pagination loop** in `PresetPskReadG`. The driver reads at most 256 (0x100)
bytes per chunk, with the number of iterations = `ceil(total_response_size / 256)`.

Loop variable tracking:
- `var_60h` = chunk length (initially 0x100; recalculated on last iteration as `total - accumulated - 9`)
- `var_58h` = offset accumulator (starts at 0; incremented by `var_54h` each iteration)
- `var_54h` = data bytes received this chunk (from MCU response bytes [5:9])
- `var_70h` = recv buffer size (initially 0x200; updated by sendCmd, NOT reset between iterations)

Data reassembly into the output buffer:
- Iteration 0: copies `var_54h + 8` bytes from response[1], including the flags+data_len header
- Iteration N>0: copies `var_54h` bytes from response[9], just the data portion, at output[offset+8]

The "2.parase return data" log line appears AFTER the full loop completes (line 1018
in geneva.c), not between reads. The loop body only logs "1.sendDataToDeviceEx".

Note: the code at `0x18009c97d` also has a second sendCmd call (`0x18009ca73`) that serves
as a retry if the first call fails (but NOT a separate read for different data).

## 6. TLV Head Construction in PresetPskReadSpecDataG

The TLV head is a 12-byte structure allocated with `calloc(0xc, 1)`:

```
Offset  Size  Value           Description
------  ----  --------------- -----------
0x00    4     data_type       e.g., 0xBB010002 (from arg1/ecx)
0x04    4     0x00000000      Hardcoded zero
0x08    4     0x00000000      From calloc zero-init (not used)
```

Only the first 8 bytes are passed to `PresetPskReadG` (var_48h = 8), which copies them
into payload bytes [8:16]. The output buffer is sized as `data_size + 9` (to accommodate
the 9-byte MCU response header: 1 status + 4 flags + 4 data_length).

## 7. Comparison with Python Code

Python `goodix.py` (line 753-758) sends:

```python
payload = struct.pack("<I", length) +    # [0:4]  length
          struct.pack("<I", offset) +    # [4:8]  offset
          struct.pack("<I", flags) +     # [8:12] type/flags
          struct.pack("<I", 0)           # [12:16] reserved
```

For `preset_psk_read(0xbb010002, 332, 0)`, Python sends:

```
4C 01 00 00  00 00 00 00  02 00 01 BB  00 00 00 00
|length=332| |offset=0  | |type      | |reserved  |
```

**Key difference**: Python requests all 332 bytes in ONE call. Windows pages 256 bytes
at a time. Both approaches work -- the MCU firmware handles large single reads fine.

The payload structure is IDENTICAL between Python and Windows:
`[length(4LE), offset(4LE), type(4LE), reserved(4LE)]`

The user's original question mentioned "Python sends 8 bytes, Windows sends 16" -- this
was incorrect. The Python code in goodix.py lines 756-758 constructs a full 16-byte payload
when both `length` and `offset` are provided. Only when they are `None` does it send 8 bytes
(just type + reserved), but this is for a different usage pattern (e.g., `preset_psk_read(0xbb020003)`
without length/offset on older devices).

## 8. Summary Table

| Type         | Hex type     | Data | First payload (hex)                                    | Second payload (hex)                                  |
|------------- |------------- |------|-------------------------------------------------------|-------------------------------------------------------|
| Sealed PSK   | 0xBB010002   | 332B | `00 01 00 00 00 00 00 00 02 00 01 bb 00 00 00 00`    | `4c 00 00 00 00 01 00 00 02 00 01 bb 00 00 00 00`    |
| PSK hash     | 0xBB020001   | 32B  | `20 00 00 00 00 00 00 00 01 00 02 bb 00 00 00 00`    | (none, fits in one chunk)                              |
| Whitebox PSK | 0xBB010003   | --   | Not read; only written via PresetPskWriteKey           | --                                                     |
