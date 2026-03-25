# SecWhiteEncrypt (Wbdi.dll) -- Full Disassembly Analysis

**Function**: `fcn.180005f30` aka `SecWhiteEncrypt` (also logged as `GoodixDataAesEncrypt`)
**File**: `seccipher.c` (from debug strings: `e:\git\winfpsec\winfpsec\seclibs\sourceall\sourcecode\seccipher.c`)
**Size**: 3165 bytes (0x180005f30 - 0x180006b8c)
**Called from**: `PresetPskWriteKey` at `0x18003f05b`

## Function Signature

```
int SecWhiteEncrypt(void *pData, int dataLen, void *pDataEncrypted, void *pDataEncryptedLength);
```

Registers on entry:
- `rcx` (arg1 = r14/r14) = pData (plaintext input, 32 bytes for PSK)
- `edx` (arg2 = ebx) = dataLen (0x20 = 32)
- `r8`  (arg3 = r15) = pDataEncrypted (output buffer, starts at pkt+8)
- `r9`  (arg4 = rsi) = pDataEncryptedLength (pointer to dword, at pkt+4)

### Caller context (PresetPskWriteKey @ 0x18003f05b)

```
; Set output length = 0x800 - header_size
mov eax, 0x800
sub eax, dword [nmeb]         ; nmeb = 8 (the cmd header)
mov [rcx+4], eax              ; *pDataEncryptedLength = 2040

; pDataEncrypted = pkt+8, pDataEncryptedLength = pkt+4
mov r9, [var_50h+4]           ; &pkt[4] (length field)
mov r8, [var_50h+8]           ; &pkt[8] (data start)
mov edx, 0x20                 ; 32 bytes of PSK
lea rcx, [var_70h]            ; 32-byte PSK buffer
call SecWhiteEncrypt
```

## High-Level Algorithm Overview

SecWhiteEncrypt does the following:

1. **Generate 16-byte nonce** via SHA-256 HMAC-like construction
2. **Derive 32-byte key** via SHA-256 from nonce + static secret + obfuscated "Goodix" key
3. **Split derived key**: first 16 bytes = AES key, second 16 bytes = tag material
4. **AES-128-CBC encrypt** the plaintext with the derived key
5. **Append 32-byte HMAC tag** over the ciphertext
6. **Write output**: `[16B nonce] [ciphertext] [32B tag]`

## Detailed Instruction Trace

### Phase 0: Prologue & Validation (0x180005f30 - 0x180005fe3)

```asm
0x180005f30  mov r11, rsp
0x180005f33  push rbp/rbx/rdi/r13/r14
0x180005f3a  lea rbp, [r11 - 0x4f8]     ; frame pointer, huge local frame
0x180005f41  sub rsp, 0x5d0              ; 1488 bytes of stack
0x180005f48  mov rax, [0x180258f48]      ; stack canary
0x180005f4f  xor rax, rsp
0x180005f52  mov [rbp+0x4a0], rax        ; save canary

; Save arguments:
0x180005f69  mov ebx, edx               ; ebx = dataLen (0x20)
0x180005f77  mov r14, rcx               ; r14 = pData
0x180005f86  mov r15, r8                ; r15 = pDataEncrypted
; r9 -> rsi = pDataEncryptedLength

; Null checks on r15, r14, rsi -> fail with "Invalid parameters"
0x180005fc8  test r15, r15              ; pDataEncrypted
0x180005fd1  test r14, r14              ; pData
0x180005fda  test rsi, rsi              ; pDataEncryptedLength
```

### Phase 1: Cipher Setup (0x180006013 - 0x180006198)

```asm
; Initialize mbedtls_cipher_context
0x180006015  lea rcx, [rbp+0x70]        ; cipher context at rbp+0x70
0x180006021  call fcn.1800085e0         ; ** Obfuscated nonce generator (see below)

; Search cipher definition table at 0x18022e660 for type=5 (AES-128-CBC)
0x18000603f  cmp [0x18022e668], r12     ; linked list of cipher defs
0x180006066  cmp dword [rax], 5         ; looking for MBEDTLS_CIPHER_AES_128_CBC = 5
0x18000606b  add rax, 0x10              ; each entry is 16 bytes: {type, ptr}
; Found at 0x18022e690: type=5, info_ptr -> 0x18022e6d0

; AES-128-CBC cipher_info at 0x18022e6d0:
;   type = 5 (MBEDTLS_CIPHER_AES_128_CBC)
;   mode = 2 (CBC)
;   key_bitlen = 0x80 (128 bits)
;   name -> "AES-128-CBC" (0x1802337a8)
;   block_size = 0x10 (16)
;   iv_size = 0x10 (16)

; mbedtls_cipher_setup: allocate context (calloc 0x6c = 108 bytes)
0x180006123  call fcn.180009d70         ; mbedtls_md_setup -> calloc(1, 0x6c)

; Allocate HMAC key buffer: calloc(2, 0x40) = 128 bytes
0x180006134  mov edx, 0x40              ; 64
0x180006139  lea ecx, [rdx - 0x3e]      ; 2
0x18000613c  call calloc                ; 128 bytes for ipad+opad
0x180006141  mov [var_sp_40h], rax      ; hmac_ctx_buf
0x180006146  mov [rbp-0x40], rax

; md_info (SHA-256) pointer stored at 0x18022e590:
;   type = 6 (MBEDTLS_MD_SHA256)
;   name -> "SHA256" (0x180230678)
;   output_size = 0x20 (32)
;   block_size = 0x40 (64)
;   starts  -> fcn.180009e30 (SHA256_init with standard IV)
;   update  -> fcn.180009d40 -> fcn.18000b950
;   finish  -> fcn.180009d50 -> fcn.18000ba30
```

### Phase 2: Nonce Generation (0x180006198 - 0x1800062a3)

```asm
; Initialize SHA-256 state at r14 (reusing pData pointer temporarily as hash context)
; Actually r14 here is the md_context allocated at 0x180006123

; The nonce source is ebx (dataLen=0x20=32 decimal)
; Convert dataLen to 4-byte big-endian in [rbp+0x31..0x34]:
0x1800061e0  lea ecx, [r9*8]            ; shift amount
0x1800061e8  mov eax, ebx               ; eax = dataLen
0x1800061ea  shr eax, cl                ; extract byte
0x1800061f0  mov [rdx-1], al            ; store byte
; This loop (0x1800061e0-0x18000621f) writes dataLen as big-endian 4 bytes

; SHA-256 starts
0x180006224  call fcn.180009e30         ; sha256_starts(ctx) - sets standard SHA-256 IV

; SHA-256 update with 4-byte big-endian dataLen
0x18000622f  lea rdx, [rbp+0x30]       ; 4 bytes of encoded dataLen
0x180006236  call fcn.180009d40         ; sha256_update(ctx, encoded_len, 4)

; SHA-256 update with "123GOODIX" (9 bytes)
0x18000623b  mov r8d, 9                 ; length = 9
0x180006241  lea rdx, str.123GOODIX     ; 0x1802302f8
0x18000624b  call fcn.180009d40         ; sha256_update(ctx, "123GOODIX", 9)

; SHA-256 finish -> output to [rbp+0x30] (32 bytes)
0x180006257  call fcn.180009d50         ; sha256_finish(ctx, hash_out)
```

**Nonce = SHA-256(big_endian_32(dataLen) || "123GOODIX")**

For dataLen=0x20: `SHA-256(00 00 00 20 || "123GOODIX")`

```asm
; Extract the 16-byte nonce from the 32-byte hash
0x18000625c  movaps xmm0, [rbp+0x30]   ; first 16 bytes of SHA-256 output
0x180006263  movups [var_5b8h], xmm0    ; save nonce
0x180006267  psrldq xmm0, 8            ; shift right by 8 bytes
0x18000626c  movq rax, xmm0            ; rax = bytes 8-15 of hash

; Compute "tweak byte" from high byte of nonce:
0x18000627c  shr rcx, 0x38             ; cl = byte[15] of hash
0x180006280  xor cl, bl                ; xor with dataLen low byte
0x180006282  shr rax, 0x38             ; al = same byte[15]
0x180006286  and cl, 0x0f              ; mask to low nibble
0x18000628e  xor cl, al                ; xor back with original
0x180006295  mov [rbp-0x59], cl        ; save tweak byte (unused later?)

; Copy 16-byte nonce to output buffer:
0x18000629b  movups xmm2, [var_5b8h]   ; load nonce
0x18000629f  movups [r15], xmm2        ; ** WRITE NONCE to pDataEncrypted[0..15] **
0x1800062a3  movaps [rbp+0x30], xmm2   ; keep copy in local var
```

### Phase 3: Key Derivation via SHA-256 HMAC (0x1800062a7 - 0x1800062ef)

```asm
; SHA-256 starts (second hash computation for key derivation)
0x1800062a7  call fcn.180009e30         ; sha256_starts(ctx)

; Update with 64-byte "Goodix" key from global at [rbp-0x50] = 0x18022e590
; This is the SHA-256 md_info block_size (0x40) worth of key data
0x1800062ac  mov r8d, 0x40              ; 64 bytes
0x1800062b2  lea rdx, [rbp+0x30]       ; nonce (16 bytes, but padded context)
0x1800062b9  call fcn.180009d40         ; sha256_update(ctx, nonce_block, 64)

; Update with the obfuscated "nonce" data (from Phase 2 output at [rbp+0x70])
; edi = 0x58 (88 bytes); this is the obfuscated Goodix key material
0x1800062be  mov r8, rdi               ; r8 = 0x58 = 88
0x1800062c1  lea rdx, [rbp+0x70]       ; the "Goodix" obfuscated key
0x1800062c8  call fcn.180009d40         ; sha256_update(ctx, goodix_key, 88)

; SHA-256 finish -> 32-byte derived key in [rbp+0x30]
0x1800062d4  call fcn.180009d50         ; sha256_finish(ctx, derived_key)
```

### Phase 3b: HMAC Key Setup (0x1800062d9 - 0x1800062ef)

```asm
; Setup HMAC with the first 0x20 (32) bytes of derived_key as the HMAC key
0x1800062df  lea rcx, [rbp-0x50]       ; hmac context
0x1800062e3  mov [rbp+0x70], rax       ; clear
0x1800062eb  lea r8d, [rax+0x20]       ; r8 = 0x20 (32)
0x1800062ef  call fcn.180007420        ; ** mbedtls_md_hmac_starts(md_ctx, derived_key, 32) **
```

**fcn.180007420 = `mbedtls_md_hmac_starts`**: This function:
1. If key > block_size (64), hashes the key first
2. Fills ipad with 0x36, opad with 0x5c
3. XORs key bytes into both ipad and opad
4. Calls `sha256_starts` then `sha256_update(ctx, ipad, block_size)`
   -> prepares the inner hash state for HMAC

### Phase 4: AES-128-CBC Key Setup & Encryption (0x1800062f4 - 0x18000665e)

```asm
; mbedtls_cipher_setkey:
; cipher_info at [rbp-0x30] (the AES-128-CBC cipher_info)
; key_bitlen from cipher_info+8 = 0x80 (128)
; direction from [var_48h] = 1 (ENCRYPT)
0x1800062f4  mov rax, [rbp-0x30]       ; cipher_info
0x1800062f8  mov r8d, [rsi+8]          ; key_bitlen = 128
0x18000632c  call [rax+0x28][+0x18]    ; mbedtls_cipher_setkey(ctx, derived_key, 128, ENCRYPT)
```

The **AES key is the first 16 bytes of the 32-byte derived key** (derived_key[0..15]).

```asm
; mbedtls_cipher_set_iv: uses the nonce as IV
; IV stored at [rbp+0x08] via memcpy from [var_5b8h], length = edi (16)
0x180006359  mov r8, rdi               ; 16
0x18000635c  lea rdx, [var_5b8h]       ; the 16-byte nonce
0x180006360  lea rcx, [rbp+8]          ; IV dest
0x180006364  call memcpy               ; copy nonce to IV buffer

; The main encryption loop:
; Processes plaintext in cipher block_size (16 byte) chunks
; For 32 bytes input: 2 iterations
0x1800063d0  ; loop start: process blocks via mbedtls_cipher_update
0x180006452  call [rax+8]              ; single-block cipher_update (mode=1, one full block)
0x180006502  call [r10+0x10]           ; multi-block cipher_update (mode=2, CBC chaining)
```

The encryption loop handles:
- **mode 1** (single block): direct encrypt call at `[rax+8]` with block_size data
- **mode 2** (CBC): XOR with previous ciphertext, then encrypt at `[r10+0x10]`

### Phase 5: Cipher Finish & HMAC Tag (0x1800066a3 - 0x180006972)

```asm
; mbedtls_cipher_finish (PKCS7 padding for CBC)
0x180006750  cmp r9d, 2                ; cipher mode == 2 (CBC)
0x18000675e  cmp r10d, 1               ; direction == ENCRYPT
; For encrypt mode: applies PKCS7 padding if needed
; Calls the padding function at [rbp-0x20] = 0x180007e70
0x18000679f  call r9                   ; pkcs7_pad(block, block_size)

; After cipher_finish, compute HMAC tag over ciphertext:
0x1800068ea  movsxd rdi, [r13+0x14]    ; md digest size = 0x20 (32)
0x1800068f5  mov rcx, r14              ; hmac ctx
0x1800068fb  call [r13+0x28]           ; hmac_finish -> sha256_finish (inner hash)
0x180006902  call [r13+0x18]           ; sha256_starts (prepare outer hash)
0x18000690a  movsxd r8, [r13+0x14]     ; block_size
0x180006910  call [r13+0x20]           ; sha256_update(ctx, opad+ciphertext_hash, block_size)
0x180006918  lea rdx, [rbp+0x80]       ; inner hash result
0x180006922  call [r13+0x20]           ; sha256_update(ctx, inner_hash, md_size)
0x180006926  lea rdx, [rbp+0x30]       ; output for final HMAC
0x18000692d  call [r13+0x28]           ; sha256_finish -> 32-byte HMAC tag
```

### Phase 6: Write Tag to Output (0x180006936 - 0x180006972)

```asm
; Check output buffer space: r15 + 0x20 <= *pDataEncryptedLength
0x18000693a  add r15, 0x20             ; need 32 more bytes for HMAC tag
0x18000693e  cmp r15, rax
0x180006941  jbe write_tag

; Write 32-byte HMAC tag:
0x18000695a  movups xmm0, [rbp+0x30]   ; first 16 bytes of HMAC
0x180006962  movups [rdi], xmm0        ; write to output
0x180006965  movups xmm1, [rbp+0x40]   ; second 16 bytes of HMAC
0x180006969  movups [rdi+0x10], xmm1   ; write to output

; Update *pDataEncryptedLength with final size
0x180006972  mov [r12], eax            ; total_size = nonce(16) + ciphertext + tag(32)
```

### Phase 7: Cleanup (0x180006a6a - 0x180006b8c)

```asm
; Zero out 1024-byte work buffer at [rbp+0xa0]
0x180006a9d  mov r8d, 0x400            ; 1024
0x180006aa3  call memset(rbp+0xa0, 0, 1024)

; Zero cipher context (88 bytes at rbp-0x30..rbp+0x28)
0x180006b04  mov ecx, 0x58             ; 88
0x180006b10  mov byte [rax], 0         ; zeroing loop

; Free HMAC key material (ipad/opad, 2*digest_size bytes)
0x180006b33  mov eax, [r13+0x14]       ; digest_size
0x180006b37  add eax, eax              ; 2 * 32 = 64
0x180006b41  mov byte [rax], 0         ; zero loop
0x180006b51  call free                 ; free hmac buffer

; Zero md context (24 bytes at rbp-0x50)
0x180006b60  mov byte [rcx], 0         ; zeroing loop

; Return ebx (0 on success, negative error code on failure)
0x180006b6d  mov eax, ebx
; Stack canary check, epilogue, ret
```

## Output Buffer Layout

For 32 bytes of plaintext PSK encrypted with AES-128-CBC + PKCS7:

```
Offset  Size  Content
------  ----  -------
0x00    16    Nonce (= IV for AES-CBC) = SHA-256(BE32(dataLen) || "123GOODIX")[0:16]
0x10    48    AES-128-CBC ciphertext (32 bytes data + 16 bytes PKCS7 padding)
0x40    32    HMAC-SHA-256 tag over ciphertext
------  ----
Total:  96 bytes (0x60)
```

The `*pDataEncryptedLength` is updated to 0x60 (96) on return.

## Key Derivation Summary

```
nonce_hash = SHA-256( big_endian_32(data_len) || "123GOODIX" )
nonce = nonce_hash[0:16]   # written to output[0:16], also used as AES IV

obfuscated_key = generate_goodix_key()  # fcn.1800085e0, 88 bytes at [rbp+0x70]

derived = SHA-256( nonce_padded_to_64 || obfuscated_key_88bytes )
         # Actually: sha256_starts(); sha256_update(nonce, 64); sha256_update(obf_key, 88); sha256_finish()

aes_key  = derived[0:16]
hmac_key = derived[0:32]   # full 32 bytes used as HMAC key via mbedtls_md_hmac_starts
```

## Obfuscated Key Generator: fcn.1800085e0

This function generates a "Goodix" key at runtime using bitwise obfuscation.
It operates on the ASCII values of "Goodix" characters:

```
'G' = 0x47, 'o' = 0x6f, 'o' = 0x6f, 'd' = 0x64, 'i' = 0x69, 'x' = 0x78
```

The algorithm:
1. Iterates with `r10 = 1, 3, 5, ...` (shift_right) and `r9 = 7, 5, 3, ...` (shift_left)
2. For each character: `byte = (char >> shift_right) | (char << shift_left)`
3. Produces 6 bytes per iteration, 5 iterations (r9 goes 7 down to -1)
4. Result: 30 obfuscated bytes at [rbp+0x70] initially
5. Then calls `fcn.180008110` and `fcn.180008210` for further key expansion
6. Generates CRC32 table at 0x1803143e0 (polynomial 0x04C11DB7)
7. Final CRC-based scramble produces the full key material

This is a **deterministic** key derivation -- no randomness. The "Goodix" string is
the seed, bitrotated and CRC-expanded into the key schedule.

## Sub-Function Reference

| Address | Name / Purpose |
|---------|---------------|
| `0x180005f30` | **SecWhiteEncrypt** - main function |
| `0x180007190` | Debug logging (varargs) |
| `0x1800085e0` | Obfuscated "Goodix" key generator |
| `0x180008110` | Key expansion sub-function (called from 0x1800085e0) |
| `0x180008210` | Key expansion sub-function (called from 0x1800085e0) |
| `0x180008430` | Key expansion sub-function (called from 0x1800085e0) |
| `0x180009d70` | `mbedtls_md_setup` - allocate SHA-256 context (calloc 1x108) |
| `0x180009db0` | `mbedtls_md_free` |
| `0x180009e30` | `mbedtls_sha256_starts` - init with standard SHA-256 IV |
| `0x180009d40` | `mbedtls_sha256_update` (jumps to 0x18000b950) |
| `0x180009d50` | `mbedtls_sha256_finish` (jumps to 0x18000ba30) |
| `0x180007420` | `mbedtls_md_hmac_starts` - HMAC key setup (ipad/opad XOR) |
| `0x18012ab30` | `memcpy` |
| `0x1800c54f0` | `free` |

## Key Constants

| Address | Value |
|---------|-------|
| `0x1802302f8` | `"123GOODIX"` - nonce derivation salt |
| `0x18022e590` | SHA-256 `md_info` struct (type=6, out=32, block=64) |
| `0x18022e660` | Cipher definition linked list head |
| `0x18022e6d0` | AES-128-CBC `cipher_info` (type=5, mode=2, key=128bit, block=16, iv=16) |
| `0x180007e70` | PKCS7 padding function pointer |
| `0x180007ea0` | PKCS7 unpadding function pointer |
| `0x1803143e0` | CRC32 lookup table (256 entries, polynomial 0x04C11DB7) |

## Critical Finding: Deterministic Nonce

The nonce is **entirely deterministic** -- it depends only on `dataLen`:

```python
import hashlib
nonce = hashlib.sha256(dataLen.to_bytes(4, 'big') + b"123GOODIX").digest()[:16]
```

For the PSK write case where dataLen=32 (0x20), the nonce is always:
```python
hashlib.sha256(b"\x00\x00\x00\x20" + b"123GOODIX").digest()[:16]
```

This means the nonce, IV, and therefore the AES key are all **static** for a given
message length. The encryption is deterministic -- same plaintext always produces
the same ciphertext.

## Pseudocode

```python
def SecWhiteEncrypt(plaintext: bytes) -> bytes:
    data_len = len(plaintext)

    # Phase 1: Deterministic nonce from data length
    nonce_full = sha256(data_len.to_bytes(4, 'big') + b"123GOODIX")
    nonce = nonce_full[:16]  # also used as AES-CBC IV

    # Phase 2: Generate obfuscated Goodix key (deterministic, ~88 bytes)
    goodix_key = generate_goodix_key()  # bit-rotations of "Goodix" + CRC expansion

    # Phase 3: Derive AES key via SHA-256
    derived = sha256(nonce.ljust(64, b'\x00') + goodix_key)
    aes_key = derived[:16]
    hmac_key = derived[:32]

    # Phase 4: AES-128-CBC encrypt with PKCS7 padding
    cipher = AES_128_CBC(key=aes_key, iv=nonce)
    ciphertext = cipher.encrypt(pkcs7_pad(plaintext, 16))

    # Phase 5: HMAC-SHA-256 tag
    tag = hmac_sha256(hmac_key, ciphertext)

    # Phase 6: Assemble output
    return nonce + ciphertext + tag
```
