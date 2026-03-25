#!/usr/bin/env python3
"""
Extract the image TLS PSK from a Goodix 27c6:5e0a (firmware 10034) sensor.

The PSK is stored on the device's MCU as a DPAPI-sealed blob. This script:
  1. Reads the sealed blob from the device via USB
  2. Saves it to sealed_psk.bin
  3. Generates a PowerShell extraction script (extract_psk.ps1) to run on Windows

The PowerShell script must be run ONCE on the Windows machine where the sensor
was enrolled, as NT AUTHORITY\SYSTEM. It outputs the 32-byte PSK in hex.

Usage:
  sudo python3 extract_psk.py              # reads blob from device
  sudo python3 extract_psk.py --ssh HOST   # also runs decryption on Windows via SSH

After extraction, put the PSK in driver_5e0a.py (IMAGE_PSK constant).
"""
import hashlib
import os
import subprocess
import sys
import time

import usb.core
import goodix
import protocol


POWERSHELL_SCRIPT = r'''
# extract_psk.ps1 — Run as NT AUTHORITY\SYSTEM (e.g. via PsExec -s or scheduled task)
# Decrypts the Goodix DPAPI-sealed PSK blob and prints the raw PSK hex.
#
# Usage (from admin PowerShell):
#   schtasks /create /tn ExtractPSK /tr "powershell -ExecutionPolicy Bypass -File C:\extract_psk.ps1" /sc once /st 00:00 /ru SYSTEM /f /rl HIGHEST
#   schtasks /run /tn ExtractPSK
#   type C:\psk_result.txt
#   schtasks /delete /tn ExtractPSK /f

Add-Type -AssemblyName System.Security
$blob = [System.IO.File]::ReadAllBytes("C:\sealed_psk.bin")
try {
    $psk = [System.Security.Cryptography.ProtectedData]::Unprotect(
        $blob, $null, [System.Security.Cryptography.DataProtectionScope]::CurrentUser)
    $hex = ($psk | ForEach-Object { $_.ToString("x2") }) -join ""
    $hash = [System.Security.Cryptography.SHA256]::Create().ComputeHash($psk)
    $hashHex = ($hash | ForEach-Object { $_.ToString("x2") }) -join ""
    $result = "PSK=$hex`nSHA256=$hashHex`nLENGTH=$($psk.Length)"
    [System.IO.File]::WriteAllText("C:\psk_result.txt", $result)
    Write-Host "PSK extracted! See C:\psk_result.txt"
    Write-Host "PSK: $hex"
} catch {
    $err = "ERROR: $($_.Exception.Message)`nMake sure you run this as SYSTEM on the original Windows."
    [System.IO.File]::WriteAllText("C:\psk_result.txt", $err)
    Write-Host $err
}
'''.strip()


def read_sealed_psk(device):
    """Read the DPAPI-sealed PSK blob from the device MCU."""
    success, flags, blob = device.preset_psk_read(0xbb010002, 332, 0)
    if not success or not blob:
        raise RuntimeError("Cannot read sealed PSK from device (TLV 0xBB010002)")
    return blob


def read_psk_hash(device):
    """Read the PSK hash from the device for verification."""
    success, flags, psk_hash = device.preset_psk_read(0xbb020001, 32, 0)
    if not success or not psk_hash:
        raise RuntimeError("Cannot read PSK hash from device (TLV 0xBB020001)")
    return psk_hash


def extract_via_ssh(host, sealed_path, user="user", password=None):
    """Extract PSK by running the PowerShell script on Windows via SSH."""
    ssh_prefix = ["sshpass", "-p", password] if password else []
    ssh_cmd = ssh_prefix + [
        "ssh", "-o", "StrictHostKeyChecking=no", f"{user}@{host}"]
    scp_cmd_base = ssh_prefix + [
        "scp", "-o", "StrictHostKeyChecking=no"]

    # Upload sealed blob
    print(f"Uploading sealed blob to {host}...")
    subprocess.run(scp_cmd_base + [sealed_path, f"{user}@{host}:C:/sealed_psk.bin"],
                   check=True, capture_output=True)

    # Upload and run extraction script as SYSTEM
    ps1_path = "/tmp/_extract_psk.ps1"
    with open(ps1_path, "w") as f:
        f.write(POWERSHELL_SCRIPT)
    subprocess.run(scp_cmd_base + [ps1_path, f"{user}@{host}:C:/extract_psk.ps1"],
                   check=True, capture_output=True)

    print("Running decryption as SYSTEM...")
    for cmd in [
        'schtasks /create /tn ExtractPSK /tr "powershell -ExecutionPolicy Bypass -File C:\\extract_psk.ps1" /sc once /st 00:00 /ru SYSTEM /f /rl HIGHEST',
        'schtasks /run /tn ExtractPSK',
    ]:
        subprocess.run(ssh_cmd + [cmd], capture_output=True)

    time.sleep(5)

    result = subprocess.run(ssh_cmd + ["type C:\\psk_result.txt"],
                            capture_output=True, text=True)
    subprocess.run(ssh_cmd + ["schtasks /delete /tn ExtractPSK /f"],
                   capture_output=True)

    output = result.stdout.strip()
    print(f"Result: {output}")

    # Parse PSK from result
    for line in output.split('\n'):
        if line.startswith('PSK='):
            return bytes.fromhex(line[4:].strip())

    raise RuntimeError(f"Failed to extract PSK: {output}")


def main():
    # Parse args
    ssh_host = None
    ssh_user = "user"
    ssh_pass = None
    i = 1
    while i < len(sys.argv):
        if sys.argv[i] == '--ssh':
            ssh_host = sys.argv[i + 1]; i += 2
        elif sys.argv[i] == '--user':
            ssh_user = sys.argv[i + 1]; i += 2
        elif sys.argv[i] == '--password':
            ssh_pass = sys.argv[i + 1]; i += 2
        else:
            i += 1

    # Connect to device
    device = goodix.Device(0x5e0a, protocol.USBProtocol)
    device.nop()
    try:
        device.enable_chip(True)
    except usb.core.USBTimeoutError:
        pass
    device.nop()

    # Read PSK hash
    psk_hash = read_psk_hash(device)
    print(f"PSK hash: {psk_hash.hex()}")

    # Read sealed blob
    print("Reading DPAPI-sealed PSK from device...")
    sealed = read_sealed_psk(device)
    sealed_path = os.path.join(os.path.dirname(__file__) or '.', "sealed_psk.bin")
    with open(sealed_path, "wb") as f:
        f.write(sealed)
    print(f"Saved {len(sealed)} bytes to {sealed_path}")

    # Generate PowerShell script
    ps1_path = os.path.join(os.path.dirname(__file__) or '.', "extract_psk.ps1")
    with open(ps1_path, "w") as f:
        f.write(POWERSHELL_SCRIPT)
    print(f"Generated {ps1_path}")

    if ssh_host:
        # Automated extraction via SSH
        psk = extract_via_ssh(ssh_host, sealed_path, ssh_user, ssh_pass)
    else:
        print(f"\n{'='*60}")
        print("To extract the PSK, copy these files to Windows and run:")
        print(f"  1. Copy sealed_psk.bin to C:\\sealed_psk.bin")
        print(f"  2. Copy extract_psk.ps1 to C:\\extract_psk.ps1")
        print(f"  3. In admin PowerShell:")
        print(f'     schtasks /create /tn ExtractPSK /tr "powershell -ExecutionPolicy Bypass -File C:\\extract_psk.ps1" /sc once /st 00:00 /ru SYSTEM /f /rl HIGHEST')
        print(f'     schtasks /run /tn ExtractPSK')
        print(f'     timeout 5')
        print(f'     type C:\\psk_result.txt')
        print(f'     schtasks /delete /tn ExtractPSK /f')
        print(f"\nOr with SSH: sudo python3 extract_psk.py --ssh <HOST> --password <PASS>")
        print(f"{'='*60}")
        return

    # Verify
    h = hashlib.sha256(psk).digest()
    if h == psk_hash:
        print(f"\nPSK VERIFIED!")
        print(f"PSK: {psk.hex()}")
        print(f"\nAdd to driver_5e0a.py:")
        print(f'IMAGE_PSK = bytes.fromhex("{psk.hex()}")')
    else:
        print(f"\nWARNING: PSK hash mismatch!")
        print(f"  Got:      {hashlib.sha256(psk).hexdigest()}")
        print(f"  Expected: {psk_hash.hex()}")


if __name__ == "__main__":
    main()
