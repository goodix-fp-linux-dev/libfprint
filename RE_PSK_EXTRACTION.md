# PSK Extraction & Write Mechanisms -- Wbdi.dll Reverse Engineering

Source: `Wbdi.dll` from Goodix FingerPrint V3.0.141.150 (21H1)
Debug strings reference: `pskunify.c`, `geneva.c`, `seccipher.c`

---

## Executive Summary

Three approaches were investigated for getting the PSK working on Linux without Windows:

| Approach | Feasibility | Summary |
|----------|-------------|---------|
| 1. Read whitebox (0xBB010003) | **Not possible** | The MCU only stores this blob; the Windows driver never reads it back |
| 2. Write new PSK | **Possible but requires IAP mode** | MCU rejects writes in normal APP mode; must enter IAP (firmware update) mode first |
| 3. Decrypt DPAPI blob offline | **Best option** | No entropy used; standard DPAPI with SYSTEM scope; decryptable from any Windows with the same SYSTEM DPAPI master key |

---

## Approach 1: Read Whitebox PSK (TLV 0xBB010003)

### Finding: 0xBB010003 is WRITE-ONLY

The Windows driver **never reads** type 0xBB010003 from the MCU. In the entire DLL:

- `PresetPskReadSpecDataG` (@ `0x18003e500`) is called with two TLV types:
  - `0xBB010002` -- the DPAPI-sealed PSK blob (at `0x18003da20`: `mov ecx, 0xbb010002`)
  - `0xBB020001` -- the PSK SHA-256 hash (at `0x18003dc51`: `mov ecx, 0xbb020001`)

- `PresetPskWriteKey` (@ `0x18003ec98`) **creates** a 0xBB010003 TLV (at `0x18003f022`: `mov dword [rax], 0xbb010003`) and writes it to the MCU alongside the 0xBB010002 TLV.

The whitebox-encrypted blob exists only as a transit format: the host encrypts the PSK, sends it to the MCU, and the MCU decrypts it internally using its built-in whitebox key. **The MCU firmware may or may not store 0xBB010003 for later retrieval -- but the driver never attempts to read it.**

### Why `preset_psk_read(0xBB010003, 512, 0)` fails

The MCU likely returns error because:
1. The TLV type 0xBB010003 is not stored persistently (only processed in-place during write)
2. Or the MCU treats it as a write-only type

**Conclusion: This approach is a dead end.**

---

## Approach 2: Write New PSK

### Function: `PresetPskWriteKey` @ `0x18003ec98`

Called from `ProcessPsk` (@ `0x18009a570`) at address `0x18009ac3c`.

#### Arguments (x64 calling convention):
- `rcx (arg1)` = pointer to firmware version string (e.g., "GM168", "GM168SEC", "RTSEC")
- `edx (arg2)` = 0x20 (32) -- PSK length in bytes
- `r8d (arg3)` = sealed data buffer size (from GfSealData output)
- `r9 (arg4)` = device context pointer

#### Complete Write Flow:

```
Step 0: Generate random 32-byte PSK
  -> GeneratePsk (fcn.18003cd74)
  -> Stores in var_70h (32-byte buffer)

Step 1: Seal PSK with DPAPI ("SGX" in code)
  -> GfSealData (fcn.18003d484)
  -> Allocates 0x800 (2048) byte buffer
  -> Sets TLV type = 0xBB010002 at buffer[0:4]
  -> Sets max_len = 0x800 - 8 at buffer[4:8]
  -> Calls CryptProtectData with the PSK
  -> Output: TLV { type=0xBB010002, len, DPAPI_blob }

Step 2: Encrypt PSK with whitebox crypto
  -> SecWhiteEncrypt (fcn.180005f30)
  -> Allocates 0x800 byte buffer
  -> Sets TLV type = 0xBB010003 at buffer[0:4]
  -> Sets max_len = 0x800 - 8 at buffer[4:8]
  -> AES-128-CBC encrypt with derived key
  -> Output: TLV { type=0xBB010003, len, WB_ciphertext }

Step 3: Combine and send to MCU
  -> Concatenate: TLV_sgx + TLV_wb
  -> Platform check: "GM168"/"GM168SEC" -> PresetPskWriteG (Geneva)
                      "RTSEC" -> PresetPskWriteR (Realtek)
  -> PresetPskWriteG (fcn.18009cd0c) sends via chunked protocol
```

### PresetPskWriteG Wire Protocol (@ `0x18009cd0c`)

#### Pre-flags header (10 bytes, hardcoded):
```
Offset  Byte   Value
0       0x56   Magic
1       0xA5   Magic
2       0xBB   Magic
3       0x95   Magic
4       0x6B   Magic
5       0x7C   Magic
6       0x8D   Magic
7       0x9E   Magic
8       0x00   Reserved
9       0x00   Reserved
```

#### Payload construction:
```
send_buffer = PRE_FLAGS[10] + raw_TLV_data[N]
```

#### Chunked transmission (chunk_size = 0x100 = 256):
```
For each chunk:
  chunk_header[0:4]  = total_payload_length (LE32)
  chunk_header[4:8]  = this_chunk_length (LE32)
  chunk_header[8:12] = offset_in_payload (LE32)
  chunk_data[12:]    = payload[offset:offset+chunk_length]

  Send via command 0xE0 (PRESET_PSK_WRITE)
  First attempt: command 0xE0
  If that fails: retry with command 0xE4
```

#### MCU Response parsing:
```
response[0] = status byte
  0 = success
  1 = error (this is "error code 1" -- generic MCU rejection)
```

### Why Write Fails with Error 1

**Critical finding: The MCU must be in IAP (In-Application Programming) mode for writes to succeed.**

The `ProcessPsk` function (@ `0x18009a570`) has this exact flow:

```
1. Check if PSK is already valid (PresetPskIsVaildG, up to 3 retries)
   -> If valid: return success (PSK already set, no write needed)
   -> If invalid: continue to step 2

2. Check if MCU is in IAP/TESTIAP mode (fcn.18009c478)
   -> If NOT in IAP mode:
      a. Send "erase app" command (fcn.1800a51c0 with arg=0x32)
      b. Retry erase if first attempt fails
      c. Log "Send IAP mode command successfully, sleep 1000ms"
      d. Sleep 1000ms
      e. RECURSIVELY call ProcessPsk (the MCU should now be in IAP mode)
   -> If in IAP mode: continue to step 3

3. Write PSK to MCU (PresetPskWriteKey, up to 3 retries)

4. Verify write (PresetPskIsVaildG again)
   -> "4.write psk successfully, check again"
```

**Error code 1 means the MCU rejected the write because it is in APP mode (normal operation), not IAP mode.** To write a PSK:

1. Put MCU into IAP mode (this may require sending a specific firmware erase/update command)
2. Send the PSK write command
3. The MCU should then exit IAP mode and reboot into normal operation

The IAP mode entry command appears to be `fcn.1800a51c0` with parameter `0x32` (50 decimal). This likely maps to a specific USB command that triggers IAP/bootloader mode.

### Platform-Specific Write Functions

The driver checks the firmware version string to select the correct write function:

| Version String | Platform | Write Function | Address |
|---------------|----------|---------------|---------|
| "GM168" | Geneva | `PresetPskWriteG` | `fcn.18009cd0c` |
| "GM168SEC" | Geneva (secure) | `PresetPskWriteG` | `fcn.18009cd0c` |
| "RTSEC" | Realtek | `PresetPskWriteR` | `fcn.1800af104` |
| Other | Default | Falls through to Realtek path | -- |

The 5e0a sensor likely uses the Geneva path ("GM168" or similar).

---

## Approach 3: DPAPI Blob Structure

### Function: `GfSealData` @ `0x18003d484`

This is a thin wrapper around Windows `CryptProtectData`:

```c
BOOL CryptProtectData(
    DATA_BLOB *pDataIn,       // the 32-byte raw PSK
    LPCWSTR szDataDescr,      // L"This is the description string."
    DATA_BLOB *pOptionalEntropy,  // NULL (!!!)
    PVOID pvReserved,         // NULL
    CRYPTPROTECT_PROMPTSTRUCT *pPromptStruct,  // NULL
    DWORD dwFlags,            // 0
    DATA_BLOB *pDataOut       // output sealed blob
);
```

### Key findings:

1. **NO ENTROPY PARAMETER**: `pOptionalEntropy` is explicitly `xor r8d, r8d` (NULL) at address `0x18003d540`. This means **no additional entropy** is used beyond the DPAPI master key.

2. **Description string**: `L"This is the description string."` (UTF-16LE) at `0x1801332a0`. This is metadata only and not needed for decryption.

3. **dwFlags = 0**: No special flags (not CRYPTPROTECT_LOCAL_MACHINE, not CRYPTPROTECT_UI_FORBIDDEN). This means the blob is encrypted with the **calling user's DPAPI master key** (CurrentUser scope). Since the driver runs as `NT AUTHORITY\SYSTEM`, it uses the SYSTEM account's master key.

### Function: `GfUnsealData` @ `0x18003d634`

Similarly wraps `CryptUnprotectData`:

```c
BOOL CryptUnprotectData(
    DATA_BLOB *pDataIn,           // the sealed blob
    LPWSTR *ppszDataDescr,        // receives description string
    DATA_BLOB *pOptionalEntropy,  // NULL (!!!)
    PVOID pvReserved,             // NULL
    CRYPTPROTECT_PROMPTSTRUCT *pPromptStruct,  // NULL
    DWORD dwFlags,                // 0
    DATA_BLOB *pDataOut           // output: 32-byte raw PSK
);
```

Again: **NO entropy**. `pOptionalEntropy` is `xor r8d, r8d` (NULL) at `0x18003d705`.

### DPAPI Blob Format (332 bytes from device)

The 332-byte blob read from TLV 0xBB010002 is a standard Windows DPAPI blob:

```
Offset  Size   Field
0x00    4      dwVersion (typically 0x01)
0x04    16     guidProvider (DPAPI provider GUID)
0x14    4      dwMasterKeyVersion
0x18    16     guidMasterKey (identifies which master key was used)
0x28    4      dwFlags
0x2C    4      cbDescription (description string length)
0x30    var    Description: L"This is the description string." (64 bytes for 31 chars + null, UTF-16)
...    var    HMAC, hash params, cipher params, encrypted data
```

### Decryption Requirements

To decrypt the DPAPI blob offline (on Linux), you need:

1. **The DPAPI master key** from the Windows SYSTEM account
   - Location: `C:\Windows\System32\Microsoft\Protect\S-1-5-18\<GUID>\<MasterKeyFile>`
   - The GUID in the blob's `guidMasterKey` field identifies which master key file to use
   - The master key file is itself encrypted with the SYSTEM account's password hash

2. **The SYSTEM account's credential** (for decrypting the master key file)
   - For SYSTEM, this is derived from the machine's DPAPI system key
   - Can be extracted from the registry: `HKLM\SECURITY\Policy\Secrets\DPAPI_SYSTEM`

3. **Tools that can do this**:
   - `mimikatz` (Windows): `dpapi::blob /in:sealed_psk.bin /unprotect` (as SYSTEM)
   - `dpapick` (Python/Linux): Can decrypt with extracted master key material
   - `impacket-dpapi` (Python/Linux): `dpapi.py unprotect -file sealed_psk.bin`
   - Manual: Extract DPAPI_SYSTEM secret + master key file, derive the key, decrypt

### Easiest Extraction Methods (ranked):

1. **Run as SYSTEM on Windows** (current approach in `extract_psk.py`):
   - Use `PsExec -s` or scheduled task to run PowerShell as SYSTEM
   - Call `[System.Security.Cryptography.ProtectedData]::Unprotect($blob, $null, ...)`
   - Works reliably, requires one-time Windows access

2. **Offline extraction from Windows disk** (new option):
   - Mount the Windows partition from Linux
   - Extract `DPAPI_SYSTEM` from `SECURITY` registry hive
   - Extract master key file from `C:\Windows\System32\Microsoft\Protect\S-1-5-18\`
   - Use `impacket-dpapi` or `dpapick` to decrypt
   - Does NOT require booting Windows!

3. **Write new PSK** (requires IAP mode):
   - Must first enter IAP/bootloader mode
   - Risk of bricking the sensor if done incorrectly
   - The IAP entry command needs further RE

---

## WhiteBox Encryption Algorithm (SecWhiteEncrypt)

Source: `seccipher.c` (`e:\git\winfpsec\winfpsec\seclibs\sourceall\sourcecode\seccipher.c`)
Function: `fcn.180005f30` (3165 bytes)

### Algorithm:

```
Input:  plaintext (32 bytes), plaintext_len (0x20)
Output: nonce(4) + iv(16) + ciphertext(48) = 68 bytes

1. Generate 4-byte nonce from plaintext_len:
   nonce = struct.pack('<I', plaintext_len)  # = 20 00 00 00 for 32 bytes

2. Derive AES-128 key:
   hash = HMAC-SHA256(key=nonce, data="123GOODIX")
   aes_key = hash[0:16]

3. Derive IV:
   hash2 = HMAC-SHA256(key=aes_key_padded_to_64, data=plaintext)
   iv = hash2[0:16]
   (Some XOR post-processing with hash2 high bytes)

4. Encrypt:
   cipher = AES-128-CBC(key=aes_key, iv=iv)
   ciphertext = cipher.encrypt(PKCS7_pad(plaintext))

5. Output: nonce + iv + ciphertext
```

**Important**: The cipher is deterministic -- no randomness. Same plaintext always produces the same ciphertext. The "whitebox" name is misleading; it's really just HMAC-derived AES-CBC.

The salt string `"123GOODIX"` is hardcoded at `0x1802302f8` in the DLL.

**Correction to `write_psk.py`**: The nonce is NOT derived from the plaintext bytes via XOR -- it is simply the 4-byte little-endian encoding of the plaintext LENGTH. The key derivation uses HMAC-SHA256, not plain SHA256.

---

## TLV Types Summary

| TLV Type | Name | Direction | Content |
|----------|------|-----------|---------|
| 0xBB010002 | DPAPI Sealed PSK | Read/Write | CryptProtectData output (~324 bytes data) |
| 0xBB010003 | WhiteBox Encrypted PSK | Write-only | AES-128-CBC encrypted PSK (~68 bytes) |
| 0xBB020001 | PSK SHA-256 Hash | Read-only | SHA-256 of the raw 32-byte PSK |

---

## Error Codes

| Return Value | Meaning |
|-------------|---------|
| 0 | Success |
| 1 | MCU execution failed (generic error, often "not in IAP mode") |
| 0xffefffff | Invalid input parameter |
| 0xffeffffb | Memory allocation failed |
| 0xffdffffc | SendDataToDeviceEx failed |
| 0xffdffffd | Read from MCU failed |
| 0xff000001 | Initial/unset return value |

---

## Recommended Action Plan

### Option A: DPAPI extraction (recommended, no risk)

1. Read sealed blob: `preset_psk_read(0xBB010002, 332, 0)` -- already works
2. Read PSK hash: `preset_psk_read(0xBB020001, 32, 0)` -- already works
3. Decrypt the sealed blob:
   - **Easy**: Run PowerShell as SYSTEM on Windows (existing `extract_psk.py`)
   - **Offline**: Extract DPAPI master key material from Windows partition, decrypt on Linux with `impacket`
4. Verify: SHA-256(decrypted_psk) should match the hash from step 2

### Option B: Write new PSK (advanced, some risk)

1. Reverse engineer the IAP mode entry command (`fcn.1800a51c0` with arg `0x32`)
2. Send IAP mode command to MCU
3. Wait 1000ms
4. Fix `write_psk.py`:
   - Fix whitebox encryption (nonce = `pack('<I', len)`, use HMAC-SHA256 not SHA256)
   - Send correct pre-flags header (already correct: `56 a5 bb 95 6b 7c 8d 9e 00 00`)
   - Use correct chunked format matching PresetPskWriteG
5. Send write command
6. Verify with PSK hash read

### Option C: Offline DPAPI decryption (no Windows boot needed)

```bash
# 1. Mount Windows partition
sudo mount /dev/sdXY /mnt/windows

# 2. Extract DPAPI system key from registry
impacket-dpapi masterkey \
  -system /mnt/windows/Windows/System32/config/SYSTEM \
  -security /mnt/windows/Windows/System32/config/SECURITY

# 3. Find the right master key file (GUID from blob header bytes 0x18-0x28)
# Master keys in: /mnt/windows/Windows/System32/Microsoft/Protect/S-1-5-18/<GUID>/

# 4. Decrypt the master key
impacket-dpapi masterkey -file <master_key_file> -key <dpapi_system_key>

# 5. Decrypt the sealed PSK blob
impacket-dpapi blob -file sealed_psk.bin -key <decrypted_master_key>
```

This approach lets you extract the PSK entirely from Linux, as long as you have access to the Windows partition (even from a mounted disk image).
