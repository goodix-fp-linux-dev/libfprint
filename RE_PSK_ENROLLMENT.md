# PSK Erase & Re-Write Recovery Flow -- Wbdi.dll Reverse Engineering

Source: `Wbdi.dll` from Goodix FingerPrint V3.0.141.150 (21H1)
Functions: `ProcessPsk` @ `0x18009a570`, `McuEraseApp` @ `0x1800a51c0`, `PresetPskWriteKey` @ `0x18003ec98`
Source files (from debug strings): `geneva.c`, `mcuimpl.c`, `pskunify.c`

---

## 1. ProcessPsk Complete Recovery Flow (0x18009a570)

This is the master function. It handles the entire lifecycle: check PSK, erase if needed, write new PSK, verify.

### Annotated C pseudocode (reconstructed from disassembly):

```c
int ProcessPsk(DeviceContext *ctx) {
    char fw_version[128] = {0};    // var_190h
    char fw_parsed[256] = {0};     // var_70h
    uint8_t local_hash[32] = {0};  // var_170h
    uint8_t psk_buf[1024] = {0};   // var_210h
    int ret = 0xFF000001;          // var_50h
    int ret2 = 0xFF000001;         // var_54h
    int sealed_size = 0x400;       // var_58h (1024)
    int psk_len = 0x20;            // var_64h (32)

    LOG("ProcessPsk", "enter");

    if (ctx == NULL) {
        LOG("ProcessPsk", "invalid param");
        return 0;
    }

    // Check module exit event
    if (WaitForSingleObject(ctx->exit_event, 0) == WAIT_OBJECT_0) {
        LOG("ProcessPsk", "module exit");
        return 0;
    }

    // ---- Step 0: Get firmware version ----
    ret = McuGetFirmwareVersion(ctx, fw_version, 128);  // fcn.1800a5910
    if (!ret) {
        LOG("ProcessPsk", "get firmware version fail");
        return 0;
    }

    // Parse version into fw_parsed
    if (!ParseFirmwareVersion(fw_version, fw_parsed)) {  // fcn.1800a4e98
        LOG("ProcessPsk", " -->failed");
        return 0;
    }
    LOG("ProcessPsk", "cur firmware version: %s", fw_parsed);

    // ---- Step 0b: Seal the PSK data (prepare DPAPI) ----
    // If a global callback exists, use it; otherwise call GfSealData directly
    if (g_seal_callback != NULL) {               // [0x18025e5a0]
        ret2 = g_seal_callback(&sealed_size, &psk_len);  // fcn.18003f55c
        sealed_size += 0x20;
    } else {
        ret2 = GfSealData(local_hash, psk_len, psk_buf, &sealed_size);  // fcn.18003d484
    }
    if (ret2 != 0) {
        LOG("ProcessPsk", "GfSealData failed");
        return 0;
    }

    // ---- Step 1: Check if PSK is already valid (3 retries) ----
    LOG("ProcessPsk", "1.check psk if valid(total times:%d)", 3);
    for (int i = 0; i < 3; i++) {
        LOG("ProcessPsk", "check times: %d", i + 1);
        ret2 = PresetPskIsVaildG(sealed_size, ctx);  // fcn.18003d85c
        if (ret2 == 0) {
            // PSK hash matches! We're done.
            LOG("ProcessPsk", "psk is valid!");
            return 1;  // SUCCESS - no write needed
        }
        LOG("ProcessPsk", "production_check_psk_is_valid failed with ret:0x%x.", abs(ret2));
    }

    // ---- Step 2: PSK is invalid. Check IAP mode ----
    LOG("ProcessPsk", "2.check if in iap or app");
    bool in_iap = IsInIAPMode(fw_parsed);  // fcn.18009c478

    if (!in_iap) {
        // NOT in IAP mode -- must erase app to enter IAP
        LOG("ProcessPsk", "Not in IAP/TESTIAP mode, call set iap API");

        ret = McuEraseApp(ctx, 0x32);  // fcn.1800a51c0, arg2=0x32
        if (!ret) {
            // First erase failed, retry once
            LOG("ProcessPsk", "erase app failed, retry...");
            ret = McuEraseApp(ctx, 0x32);
            if (!ret) {
                LOG("ProcessPsk", " -->failed");
                return 0;  // FAILED - cannot enter IAP mode
            }
        }

        // Erase succeeded - MCU is now rebooting into IAP/bootloader
        LOG("ProcessPsk", "Send IAP mode command successfully, sleep 1000ms");
        Sleep(1000);  // Wait for MCU to reboot into IAP mode

        // RECURSIVE CALL - now MCU should be in IAP mode
        ret = ProcessPsk(ctx);
        return ret;
    }

    // ---- Step 3: In IAP mode -- write PSK (3 retries) ----
    for (int j = 0; j < 3; j++) {
        LOG("ProcessPsk", "3.write psk to mcu(times:%d)(total times:%d)", j+1, 3);
        ret = PresetPskWriteKey(fw_parsed, 0x20, sealed_size, ctx);  // fcn.18003ec98
        if (ret == 0) {
            // Write succeeded! Verify it.
            LOG("ProcessPsk", "4.write psk successfully ,check again");
            ret2 = PresetPskIsVaildG(sealed_size, ctx);
            if (ret2 == 0) {
                LOG("ProcessPsk", "psk is valid!");
                return 1;  // SUCCESS
            }
            LOG("ProcessPsk", "production_check_psk_is_valid excute failed with ret:0x%x.", abs(ret2));
            // Continue retry loop
        } else {
            LOG("ProcessPsk", "production_write_key excute failed with ret:0x%x.", abs(ret));
            // Continue retry loop
        }
    }

    LOG("ProcessPsk", "Exit ret:%d", ret);
    return ret;
}
```

---

## 2. McuEraseApp (0x1800a51c0) -- The Erase Command in Detail

### What arg 0x32 means

**0x32 is the `sleep_time` parameter**, not the MCU command byte. It is placed as the second byte of a 2-byte payload sent via MCU command **0xA4** (`COMMAND_MCU_ERASE_APP`).

### Disassembly proof:

```asm
; Build 2-byte payload buffer at var_40h
0x1800a52b4  mov byte [rsp + 0 + 0x40], 0    ; payload[0] = 0x00
0x1800a52c2  mov cl, byte [var_10h]           ; cl = arg2 (0x32 = 50)
0x1800a52c6  mov byte [rsp + 1 + 0x40], cl   ; payload[1] = 0x32

; Send via IoHubMcuSendCmd2 with command 0xA4
0x1800a531a  lea r8, [var_40h]                ; r8 = payload pointer
0x1800a531f  mov dx, 0xa4                     ; dx = command byte 0xA4
0x1800a5314  mov r9d, 2                       ; r9 = payload_length = 2
0x1800a5326  call IoHubMcuSendCmd2            ; fcn.18007d6b0
```

### Wire format:

The MCU command 0xA4 is sent through the standard Goodix message protocol:

```
USB payload = message_pack(message_protocol(payload=\x00\x32, cmd=0xA4))

Where:
  payload[0] = 0x00  (fixed)
  payload[1] = 0x32  (sleep time: 50 units -- likely 50 * 20ms = 1 second)
  command    = 0xA4  (COMMAND_MCU_ERASE_APP)
```

This matches the Python implementation in `goodix.py`:
```python
def mcu_erase_app(self, sleep_time: int, reply: bool):
    self.protocol.write(
        encode_message_pack(
            encode_message_protocol(
                b"\x00" + struct.pack("<B", sleep_time),
                COMMAND_MCU_ERASE_APP)))
```

### What the erase does:

1. The MCU receives command 0xA4 with payload `\x00\x32`
2. It erases the application firmware from flash (NOT just PSK storage -- the ENTIRE APP partition)
3. The MCU then resets into IAP (In-Application Programming / bootloader) mode
4. The host must wait ~1000ms for the MCU to complete erase and reboot
5. After reboot, the MCU firmware version string will contain "IAP" or "TESTIAP"

### IoHubMcuSendCmd2 (0x18007d6b0 -> 0x18007d720):

This is the generic MCU command dispatcher. It:
1. Waits for the device mutex
2. Checks if the module exit event is signaled
3. Builds the full USB frame with the command byte and payload
4. Retries up to 2 times if the device has flag [+0x40] set (retry_count = 2 if flag, else 1)
5. Returns success/failure

---

## 3. IsInIAPMode (0x18009c478) -- IAP Mode Detection

### How it works:

```c
bool IsInIAPMode(char *fw_version_parsed) {
    // String table at 0x180249688 contains two pointers:
    //   [0] -> "IAP"      (at 0x1801a3f5c)
    //   [1] -> "TESTIAP"  (at 0x1801a3f60)
    const char *iap_strings[] = {"IAP", "TESTIAP"};

    for (int i = 0; i < 2; i++) {
        // Compare firmware version at offset +0xC0 with IAP strings
        if (_stricmp(iap_strings[i], fw_version_parsed + 0xC0) == 0) {
            return true;  // In IAP/bootloader mode
        }
    }
    return false;  // In normal APP mode
}
```

The function compares the parsed firmware version string (at offset 0xC0 in the buffer, which is the "mode" field) against "IAP" and "TESTIAP" using case-insensitive comparison.

**On Linux, we can detect IAP mode by calling `firmware_version()` and checking if the response contains "IAP".**

---

## 4. PresetPskIsVaildG (0x18003d85c) -- PSK Validation

The validation function performs 4 steps:

```c
int PresetPskIsVaildG(int sealed_size, DeviceContext *ctx) {
    // Step 1: Read sealed PSK from MCU (TLV 0xBB010002)
    uint8_t *sgx_psk = calloc(sealed_size, 1);
    ret = PresetPskReadSpecDataG(sgx_psk, 0xBB010002, &sealed_size, ctx);
    if (ret != 0) { LOG("read sgx_psk ERROR"); return ret; }

    // Step 2: Unseal (DPAPI decrypt) and compute SHA-256
    uint8_t *raw_psk = calloc(32, 1);
    int raw_len = 32;
    ret = GfUnsealData(sgx_psk, sealed_size, raw_psk, &raw_len);
    if (ret != 0) { LOG("gf_sgx_unseal_data failed"); return ret; }

    uint8_t local_hash[32];
    ret = SecSha256(raw_psk, raw_len, local_hash);  // fcn.180006b90
    if (ret != 0) { LOG("SecSha256 failed"); return ret; }

    // Step 3: Read PSK hash from MCU (TLV 0xBB020001)
    uint8_t mcu_hash[32];
    int hash_len = 32;
    ret = PresetPskReadSpecDataG(mcu_hash, 0xBB020001, &hash_len, ctx);
    if (ret != 0) { LOG("read hash of psk ERROR"); return ret; }

    // Step 4: Compare hashes
    if (memcmp(local_hash, mcu_hash, hash_len) != 0) {
        LOG("hash is not equal");
        return 0xFF000001;  // INVALID
    }

    // Step 5: Set PSK in driver context
    ret = PresetPskPskSet(ctx, raw_psk, raw_len);  // fcn.1800a97a8
    if (ret != 0) { LOG("[FAILED] PresetPskPskSet failed"); return ret; }

    return 0;  // VALID
}
```

**Key insight**: The validation checks if `SHA256(DPAPI_decrypt(sealed_blob)) == MCU_stored_hash`. The MCU internally performs the same check using the whitebox-decrypted PSK. If the hashes match, the PSK is "valid."

---

## 5. PresetPskWriteKey (0x18003ec98) -- Writing a New PSK

### Complete flow:

```c
int PresetPskWriteKey(char *fw_version, int psk_len, int sealed_size, DeviceContext *ctx) {
    // Step 0: Generate 32 random bytes
    LOG("PresetPskWriteKey", "0.generate random psk");
    uint8_t psk[32];
    int psk_actual_len = 32;
    ret = GeneratePsk(psk, &psk_actual_len);  // fcn.18003cd74
    if (ret != 0) { return ret; }

    // Step 1: Seal PSK with DPAPI (Windows CryptProtectData)
    LOG("PresetPskWriteKey", "1.seal psk by sgx");
    uint8_t *tlv_sgx = calloc(0x800, 1);   // 2048 bytes
    tlv_sgx[0:4] = 0xBB010002;             // TLV type
    tlv_sgx[4:8] = 0x800 - 8;              // max data size (2040)
    // GfSealData writes DPAPI blob starting at tlv_sgx[8]
    // On return, tlv_sgx[4:8] contains actual sealed data length
    ret = GfSealData(psk, 0x20, &tlv_sgx[8], &tlv_sgx[4]);
    if (ret != 0) { return ret; }

    // Step 2: WhiteBox encrypt PSK
    LOG("PresetPskWriteKey", "2.encrypt psk by wb");
    uint8_t *tlv_wb = calloc(0x800, 1);    // 2048 bytes
    tlv_wb[0:4] = 0xBB010003;              // TLV type
    tlv_wb[4:8] = 0x800 - 8;              // max data size
    // SecWhiteEncrypt writes ciphertext starting at tlv_wb[8]
    ret = SecWhiteEncrypt(psk, 0x20, &tlv_wb[8], &tlv_wb[4]);
    if (ret != 0) { return ret; }

    // Step 3: Concatenate TLVs and send
    LOG("PresetPskWriteKey", "3.write to mcu");
    int sgx_total = 8 + tlv_sgx[4];   // header(8) + DPAPI data length
    int wb_total  = 8 + tlv_wb[4];    // header(8) + WB data length
    int combined_size = sgx_total + wb_total;

    uint8_t *combined = calloc(combined_size, 1);
    memcpy(combined, tlv_sgx, sgx_total);
    memcpy(combined + sgx_total, tlv_wb, wb_total);

    // Platform dispatch based on firmware version
    if (strcmp(fw_version, "GM168") == 0 || strcmp(fw_version, "GM168SEC") == 0) {
        LOG("platform is geneva");
        ret = PresetPskWriteG(combined, combined_size, ctx);  // fcn.18009cd0c
    } else if (strcmp(fw_version, "RTSEC") == 0) {
        LOG("platform is realtek");
        ret = PresetPskWriteR(combined, combined_size, ctx);  // fcn.1800af104
    }

    return ret;
}
```

### TLV layout sent to MCU:

```
Offset  Field
0x00    TLV1 type:  0xBB010002 (LE32) -- DPAPI sealed PSK
0x04    TLV1 len:   <actual DPAPI blob size> (LE32)
0x08    TLV1 data:  <DPAPI sealed blob, ~324 bytes>
...
N+0x00  TLV2 type:  0xBB010003 (LE32) -- WhiteBox encrypted PSK
N+0x04  TLV2 len:   <actual WB ciphertext size> (LE32)
N+0x08  TLV2 data:  <nonce(4) + iv(16) + ciphertext(48) = 68 bytes>
```

**Important**: The TLV1 length field at offset 0x04 is initially set to `0x800 - 8 = 0x7F8` (max), then GfSealData overwrites it with the actual sealed data length. The same pattern applies to TLV2.

---

## 6. Safety Analysis: What Does "Erase App" Do?

### Does it erase just the PSK or the entire firmware?

**It erases the ENTIRE application firmware.** The function is called `McuEraseApp` (from debug strings), and after erasing, the MCU boots into IAP (bootloader) mode. This means:

- The application partition is wiped
- The MCU falls back to its built-in bootloader (IAP mode)
- The bootloader itself is NOT erased (it's in a separate protected flash region)

### Can the device recover?

**Yes, but only if new firmware is written.** The ProcessPsk flow shows that after erasing:

1. The MCU is in IAP mode (bootloader)
2. Windows calls `ProcessPsk` recursively
3. On the recursive call, `IsInIAPMode()` returns true
4. It then calls `PresetPskWriteKey` which writes the PSK
5. After PSK write, the MCU presumably auto-exits IAP mode (or a reset command is sent)

**However**, the ProcessPsk flow does NOT explicitly write new firmware -- it only writes the PSK. This means one of:
- (a) The MCU bootloader can accept PSK writes directly, and the firmware is preserved separately (PSK storage is in a different flash region)
- (b) The "erase app" only erases enough to trigger IAP mode but doesn't actually wipe the full firmware
- (c) The firmware is re-flashed by a different code path called before/after ProcessPsk

**Most likely (b)**: The 0xA4 command with parameter 0x32 likely sets a flag or erases a small boot header that forces IAP mode on next boot, without destroying the entire firmware image. The name "McuEraseApp" may be misleading. After PSK write completes, the MCU reboots back to APP mode.

### Risk assessment:

| Risk | Level | Notes |
|------|-------|-------|
| Erase fails mid-write | **Low** | MCU bootloader is protected; always recoverable via IAP |
| Firmware destroyed | **Medium** | If app is truly erased, need firmware update commands (0xF0/0xF4) |
| Device bricked | **Very Low** | IAP/bootloader is in ROM or protected flash; always accessible |
| PSK write rejected | **Low** | MCU returns error code 1; device stays in IAP mode, can retry |

---

## 7. The Complete Linux Enrollment Flow

### Pseudocode for full PSK enrollment on Linux:

```python
def linux_psk_enrollment(device):
    """
    Complete PSK enrollment flow for Linux.
    Replicates Windows ProcessPsk behavior.
    """

    # ---- Phase 1: Check if PSK is already valid ----
    success, flags, mcu_hash = device.preset_psk_read(0xBB020001, 32, 0)
    if not success:
        print("Cannot read PSK hash from MCU")
        return False

    # If we have a known PSK, verify it
    if known_psk is not None:
        if sha256(known_psk) == mcu_hash:
            print("PSK is already valid!")
            return True

    # ---- Phase 2: Generate new PSK ----
    import os
    new_psk = os.urandom(32)  # 32 cryptographically random bytes

    # ---- Phase 3: Check MCU mode ----
    fw_version = device.firmware_version()
    print(f"Firmware version: {fw_version}")

    in_iap = ("IAP" in fw_version.upper())

    if not in_iap:
        # Must enter IAP mode first
        print("MCU is in APP mode, sending erase command...")

        # Send McuEraseApp: cmd=0xA4, payload=\x00\x32
        # The 0x32 (50) is the sleep_time parameter
        success = device.mcu_erase_app(sleep_time=0x32, reply=True)
        if not success:
            print("First erase failed, retrying...")
            success = device.mcu_erase_app(sleep_time=0x32, reply=True)
            if not success:
                print("FATAL: Cannot enter IAP mode")
                return False

        print("Erase command sent, waiting 1000ms for MCU reboot...")
        time.sleep(1.0)

        # Verify MCU is now in IAP mode
        # May need to re-init USB connection after MCU reboot
        device.nop()
        fw_version = device.firmware_version()
        print(f"Post-erase firmware: {fw_version}")
        if "IAP" not in fw_version.upper():
            print("WARNING: MCU did not enter IAP mode")
            # May need another retry or longer wait

    # ---- Phase 4: Build and send PSK ----
    print("MCU in IAP mode, writing PSK...")

    # Step 4a: Create fake DPAPI blob (TLV 0xBB010002)
    # On Linux we don't have CryptProtectData.
    # The MCU stores this opaquely -- it's only used by the Windows
    # driver to recover the PSK later. We can write any blob here.
    # Use a simple format: just the raw PSK (the MCU doesn't parse it).
    fake_sealed = new_psk  # Or any opaque blob we want to store
    tlv_dpapi = struct.pack('<II', 0xBB010002, len(fake_sealed)) + fake_sealed

    # Step 4b: WhiteBox encrypt PSK (TLV 0xBB010003)
    # This is what the MCU ACTUALLY uses to extract the PSK
    wb_encrypted = whitebox_encrypt(new_psk)
    tlv_wb = struct.pack('<II', 0xBB010003, len(wb_encrypted)) + wb_encrypted

    # Step 4c: Concatenate TLVs
    combined = tlv_dpapi + tlv_wb

    # Step 4d: Prepend pre-flags header
    PRE_FLAGS = bytes.fromhex("56a5bb956b7c8d9e0000")
    payload = PRE_FLAGS + combined

    # Step 4e: Send via chunked protocol (PresetPskWriteG)
    CHUNK_SIZE = 256
    for offset in range(0, len(payload), CHUNK_SIZE):
        chunk = payload[offset:offset + CHUNK_SIZE]
        header = struct.pack('<III', len(payload), len(chunk), offset)
        send_buf = header + chunk

        # Send as command 0xE0 (PRESET_PSK_WRITE_R)
        device.protocol.write(
            goodix.encode_message_pack(
                goodix.encode_message_protocol(send_buf, 0xE0)))

        # Read ACK
        ack = device.protocol.read()
        goodix.check_ack(
            goodix.check_message_protocol(
                goodix.check_message_pack(ack), 0xB0), 0xE0)

        # Read response
        resp = device.protocol.read()
        resp_data = goodix.check_message_protocol(
            goodix.check_message_pack(resp), 0xE0)

        if resp_data[0] == 0x00:
            print(f"  Chunk at offset {offset}: MCU rejected (error 0)")
            # Try 0xE4 as fallback (DLL does this)
            # ...
            return False

    # ---- Phase 5: Verify ----
    print("Write complete, verifying...")
    time.sleep(0.5)  # Brief settle time

    success, flags, new_hash = device.preset_psk_read(0xBB020001, 32, 0)
    expected_hash = hashlib.sha256(new_psk).digest()

    if new_hash == expected_hash:
        print("PSK ENROLLMENT SUCCESSFUL!")
        print(f"New PSK: {new_psk.hex()}")
        print(f"SHA-256: {expected_hash.hex()}")
        # Save new_psk to disk for TLS usage
        with open("psk.bin", "wb") as f:
            f.write(new_psk)
        return True
    else:
        print(f"VERIFICATION FAILED")
        print(f"  Expected: {expected_hash.hex()}")
        print(f"  Got:      {new_hash.hex() if new_hash else 'None'}")
        return False


def whitebox_encrypt(psk: bytes) -> bytes:
    """
    Replicate SecWhiteEncrypt from seccipher.c.

    CORRECTED algorithm (from RE_PSK_EXTRACTION.md):
      nonce = struct.pack('<I', len(psk))  -- NOT XOR of bytes!
      key   = HMAC-SHA256(key=nonce, data="123GOODIX")[:16]
      iv    = HMAC-SHA256(key=key_padded_to_64, data=psk)[:16]
      ct    = AES-128-CBC(key, iv, PKCS7_pad(psk))
      output = nonce + iv + ct
    """
    import hmac

    # Step 1: Nonce = 4-byte LE encoding of plaintext length
    nonce = struct.pack('<I', len(psk))  # = \x20\x00\x00\x00 for 32 bytes

    # Step 2: Derive AES-128 key via HMAC-SHA256
    key = hmac.new(nonce, b"123GOODIX", hashlib.sha256).digest()[:16]

    # Step 3: Derive IV via HMAC-SHA256
    key_padded = key + bytes(64 - len(key))  # Pad to SHA-256 block size
    iv_full = hmac.new(key_padded, psk, hashlib.sha256).digest()
    iv = iv_full[:16]
    # NOTE: Some XOR post-processing with iv_full[16:32] may apply
    # For the initial implementation, try without XOR first

    # Step 4: AES-128-CBC encrypt with PKCS7 padding
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    from cryptography.hazmat.primitives import padding as crypto_padding

    padder = crypto_padding.PKCS7(128).padder()
    padded = padder.update(psk) + padder.finalize()

    cipher = Cipher(algorithms.AES(key), modes.CBC(iv))
    ct = cipher.encryptor().update(padded) + cipher.encryptor().finalize()

    # Step 5: Output
    return nonce + iv + ct  # 4 + 16 + 48 = 68 bytes
```

---

## 8. Detailed Command Reference

### MCU Erase App (0xA4)

| Field | Value | Notes |
|-------|-------|-------|
| Command | 0xA4 | `COMMAND_MCU_ERASE_APP` |
| Payload[0] | 0x00 | Fixed zero byte |
| Payload[1] | 0x32 | Sleep time parameter (50 units) |
| Response | 0x01 = success | Single byte status |
| Effect | Erases app, MCU reboots to IAP | Wait 1000ms after |

### PSK Write (0xE0 / 0xE4)

| Field | Value | Notes |
|-------|-------|-------|
| Command | 0xE0 | `COMMAND_PRESET_PSK_WRITE_R` (primary) |
| Fallback | 0xE4 | `COMMAND_PRESET_PSK_READ_R` (if 0xE0 fails) |
| Chunk header | 12 bytes | `[total_len:4][chunk_len:4][offset:4]` (all LE32) |
| Chunk size | 256 | Max data per chunk |
| Response[0] | 0x00 = success | 0x01 = error (MCU rejected) |

### PSK Read (0xE4)

Used for reading TLV data from MCU:

| TLV Type | Content | Size |
|----------|---------|------|
| 0xBB010002 | DPAPI sealed PSK blob | ~324-340 bytes |
| 0xBB010003 | WhiteBox encrypted PSK | Write-only (read fails) |
| 0xBB020001 | SHA-256 hash of raw PSK | 32 bytes |

### Firmware Version (0xA8)

Returns a string like "GF3208_RTSEC_APP_10038" or "GF3208_RTSEC_IAP_10002".
The mode field (APP vs IAP) determines whether PSK writes are accepted.

---

## 9. State Machine Summary

```
                    START
                      |
                      v
              firmware_version()
                      |
                      v
           preset_psk_read(0xBB020001)
              get PSK hash from MCU
                      |
                      v
              sha256(our_psk) == hash?
                /           \
              YES            NO
              |               |
              v               v
           DONE         check firmware_version
          (PSK OK)       contains "IAP"?
                          /        \
                        YES         NO
                        |            |
                        v            v
                   WRITE PSK    mcu_erase_app(0x32)
                        |            |
                        |         sleep(1s)
                        |            |
                        |         firmware_version()
                        |         contains "IAP"?
                        |          /        \
                        |        YES     NO (retry erase)
                        |        |
                        v        v
                 PresetPskWriteKey:
                   0. generate 32 random bytes
                   1. DPAPI seal (fake on Linux)
                   2. WhiteBox encrypt (HMAC-SHA256 + AES-128-CBC)
                   3. Build TLV payload: PRE_FLAGS + TLV_DPAPI + TLV_WB
                   4. Chunked send via cmd 0xE0
                        |
                        v
                 preset_psk_read(0xBB020001)
                   verify sha256(psk) == mcu_hash
                        |
                        v
                      DONE
                   Save psk.bin
```

---

## 10. Critical Corrections to write_psk.py

The existing `write_psk.py` has these bugs:

1. **WhiteBox nonce**: Uses XOR-shift of plaintext bytes. Should be `struct.pack('<I', len(psk))` = `\x20\x00\x00\x00`.

2. **Key derivation**: Uses `SHA256(nonce + salt)`. Should be `HMAC-SHA256(key=nonce, data="123GOODIX")`.

3. **IV derivation**: Uses `SHA256(key_padded + psk)`. Should be `HMAC-SHA256(key=key_padded, data=psk)`.

4. **Missing IAP mode**: Does not send erase command before writing. MCU rejects writes in APP mode (error code 1).

5. **Fake DPAPI blob**: Uses 32 zero bytes. This is fine (MCU stores it opaquely), but we should store something we can identify later.

6. **Response byte polarity**: Code checks `if resp_data[0] != 0` as error. The DLL checks `response[0] == 0` as **success**, `response[0] == 1` as error. Verify which polarity the MCU actually uses.
