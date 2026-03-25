#!/usr/bin/env python3
"""
Goodix SecWhiteEncrypt -- cracked implementation.

Reverse-engineered from Wbdi.dll (SecWhiteEncrypt @ 0x180005f30).
Verified by emulating the DLL code with Unicorn Engine.

Algorithm:
  1. nonce = SHA256(LE32(data_len) + "123GOODIX")[:16], with byte[15] tweaked
  2. goodix_key = 0x5cba6e25819518de2d53e96dc0347ab0  (16-byte static constant)
  3. derived = SHA256(nonce_padded_to_64 + goodix_key)
  4. aes_key = derived[:16], hmac_key = derived[:32]
  5. ct = AES-128-CBC(aes_key, iv=nonce, PKCS7_pad(plaintext))
  6. tag = HMAC-SHA256(hmac_key, ct)
  7. output = nonce(16) + ct + tag(32)

For 32-byte PSK: output is 16 + 48 + 32 = 96 bytes.

Known test vector:
  Input:  32 zero bytes
  Output: ec35ae3abb45ed3f12c4751f1e5c2cc05b3c5452e9104d9f2a3118644f37a04b
          6fd66b1d97cf80f1345f76c84f03ff30bb51bf308f2a9875c41e6592cd2a2f9e
          60809b17b5316037b69bb2fa5d4c8ac31edb3394046ec06bbdacc57da6a756c5
"""

import hashlib
import hmac
import struct
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives import padding as crypto_padding

# Static 16-byte key derived from "Goodix" string obfuscation in Wbdi.dll.
# Generated deterministically by fcn.1800085e0 via bit-rotation of "Goodix"
# characters, SHA-256 hashing of byte groups, and HMAC-SHA256 with "123456".
GOODIX_KEY = bytes.fromhex("5cba6e25819518de2d53e96dc0347ab0")

SALT = b"123GOODIX"


def _compute_nonce(data_len: int) -> bytes:
    """
    Compute the 16-byte nonce from the plaintext length.

    nonce = SHA256(LE32(data_len) || "123GOODIX")[:16]
    Then byte[15] is tweaked: low nibble = (byte15 ^ data_len_low) & 0x0f ^ byte15
    """
    nonce_hash = hashlib.sha256(struct.pack('<I', data_len) + SALT).digest()
    nonce = bytearray(nonce_hash[:16])

    # Tweak byte 15
    b15 = nonce[15]
    dl = data_len & 0xFF
    nonce[15] = ((b15 ^ dl) & 0x0F) ^ b15

    return bytes(nonce)


def _derive_keys(nonce: bytes) -> tuple:
    """
    Derive AES key (16 bytes) and HMAC key (32 bytes) from the nonce.

    derived = SHA256(nonce_padded_to_64 || GOODIX_KEY)
    aes_key = derived[:16]
    hmac_key = derived[:32]
    """
    nonce_padded = nonce + bytes(64 - len(nonce))
    derived = hashlib.sha256(nonce_padded + GOODIX_KEY).digest()
    return derived[:16], derived[:32]


def whitebox_encrypt(plaintext: bytes) -> bytes:
    """
    Encrypt plaintext using the Goodix whitebox algorithm.

    Returns: nonce(16) + ciphertext(variable) + hmac_tag(32)
    For 32-byte input: 16 + 48 + 32 = 96 bytes output.
    """
    data_len = len(plaintext)

    # Step 1: Compute nonce (also used as AES-CBC IV)
    nonce = _compute_nonce(data_len)

    # Step 2: Derive encryption and HMAC keys
    aes_key, hmac_key = _derive_keys(nonce)

    # Step 3: AES-128-CBC encrypt with PKCS7 padding
    padder = crypto_padding.PKCS7(128).padder()
    padded = padder.update(plaintext) + padder.finalize()
    cipher = Cipher(algorithms.AES(aes_key), modes.CBC(nonce))
    ct = cipher.encryptor().update(padded) + cipher.encryptor().finalize()

    # Fix: use a single encryptor instance
    cipher = Cipher(algorithms.AES(aes_key), modes.CBC(nonce))
    enc = cipher.encryptor()
    ct = enc.update(padded) + enc.finalize()

    # Step 4: HMAC-SHA256 tag over ciphertext
    tag = hmac.new(hmac_key, ct, hashlib.sha256).digest()

    # Step 5: Assemble output
    return nonce + ct + tag


def whitebox_decrypt(wb_data: bytes) -> bytes:
    """
    Decrypt a Goodix whitebox-encrypted blob.

    Input format: nonce(16) + ciphertext(N) + hmac_tag(32)
    Returns the decrypted plaintext.
    Raises ValueError if HMAC verification fails or padding is invalid.
    """
    if len(wb_data) < 16 + 16 + 32:
        raise ValueError(f"Whitebox data too short: {len(wb_data)} bytes")

    # Parse structure
    nonce = wb_data[:16]
    tag = wb_data[-32:]
    ct = wb_data[16:-32]

    if len(ct) % 16 != 0:
        raise ValueError(f"Ciphertext length {len(ct)} is not a multiple of 16")

    # Derive keys from nonce
    aes_key, hmac_key = _derive_keys(nonce)

    # Verify HMAC tag
    expected_tag = hmac.new(hmac_key, ct, hashlib.sha256).digest()
    if not hmac.compare_digest(tag, expected_tag):
        raise ValueError("HMAC verification failed: whitebox data is corrupted or uses a different key")

    # AES-128-CBC decrypt
    cipher = Cipher(algorithms.AES(aes_key), modes.CBC(nonce))
    dec = cipher.decryptor()
    padded = dec.update(ct) + dec.finalize()

    # Remove PKCS7 padding
    unpadder = crypto_padding.PKCS7(128).unpadder()
    plaintext = unpadder.update(padded) + unpadder.finalize()

    return plaintext


# ============================================================================
# Self-test
# ============================================================================

KNOWN_PSK = bytes(32)
KNOWN_OUTPUT = bytes.fromhex(
    "ec35ae3abb45ed3f12c4751f1e5c2cc05b3c5452e9104d9f2a3118644f37a04b"
    "6fd66b1d97cf80f1345f76c84f03ff30bb51bf308f2a9875c41e6592cd2a2f9e"
    "60809b17b5316037b69bb2fa5d4c8ac31edb3394046ec06bbdacc57da6a756c5"
)


def self_test():
    """Verify the implementation against the known test vector."""
    print("Goodix SecWhiteEncrypt -- self-test")
    print("=" * 60)

    # Test encrypt
    result = whitebox_encrypt(KNOWN_PSK)
    print(f"Encrypt 32 zero bytes:")
    print(f"  Output:   {result.hex()}")
    print(f"  Expected: {KNOWN_OUTPUT.hex()}")
    assert result == KNOWN_OUTPUT, "ENCRYPT FAILED: output mismatch"
    print(f"  PASS")

    # Test decrypt
    decrypted = whitebox_decrypt(KNOWN_OUTPUT)
    print(f"\nDecrypt known whitebox blob:")
    print(f"  Output:   {decrypted.hex()}")
    print(f"  Expected: {KNOWN_PSK.hex()}")
    assert decrypted == KNOWN_PSK, "DECRYPT FAILED: output mismatch"
    print(f"  PASS")

    # Test round-trip with random data
    import os
    for psk_len in [16, 32, 48, 64]:
        test_psk = os.urandom(psk_len)
        wb = whitebox_encrypt(test_psk)
        recovered = whitebox_decrypt(wb)
        assert recovered == test_psk, f"Round-trip failed for {psk_len}-byte PSK"
        print(f"\nRound-trip test ({psk_len} bytes): PASS")
        print(f"  PSK:    {test_psk.hex()}")
        print(f"  WB:     {wb.hex()[:80]}...")
        print(f"  WB len: {len(wb)}")

    print(f"\n{'=' * 60}")
    print(f"ALL TESTS PASSED")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    self_test()
