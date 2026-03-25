# Reverse Engineering: Goodix 5E0A Windows Driver Init Sequence

## Source: Wbdi.dll (V3.0.141.150, Jun 7 2021)

Binary: `Goodix FingerPrint_V3.0.141.150_21H1_signed/Drivers/Wbdi.dll`
Key source files (from debug strings):
- `fpmcucmd.c` - MCU command wrappers
- `mcuimpl.c` - MCU implementation (higher-level)
- `mcudevloader.c` - Device loader (init sequence)
- `logicmilanfseries.c` - Logic layer (Start/Stop)
- `milanf.c` / `milanfserchipconfig.c` - Chip config
- `iohub.c` - I/O hub (command transport)
- `chicagoh.c` - OTP parsing for "Chicago H" (5E0A sensor family)

---

## Complete MCU Command Map

| Cmd Byte | Name (from DLL strings) | Direction | Description |
|----------|------------------------|-----------|-------------|
| 0x00     | NOP                    | out only  | Wake/ping sensor |
| 0x20     | GetImage               | out+in    | Capture fingerprint image |
| 0x36     | SwitchToFdtMode        | out+in    | FDT finger detection mode |
| 0x82     | ReadRegister           | out+in    | Read sensor register |
| 0x84     | WriteRegister          | out only  | Write sensor register |
| 0x90     | **DownloadChipConfig** | out only  | Upload 256-byte sensor config blob |
| 0x94     | GetLidGpioStatus       | out+in    | Read GPIO/lid status (2 bytes) |
| 0xa2     | ResetFingerPrint       | out+in    | Reset sensor, returns IRQ status |
| 0xa6     | GetOtp                 | out+in    | Read OTP (one-time programmable) data |
| 0xa8     | GetFirmwareVersion     | out+in    | Read MCU firmware version |
| 0xc4     | **SetDrvState**        | out only  | Set driver state byte (2 bytes) |
| 0xd0     | TLS handshake          | out+in    | TLS session establishment |
| 0xd2     | **GetPovImage**        | out+in    | Get Power-On-Verify image |
| 0xd6     | PovImageCheck          | out+in    | Check for POV (wake-on-finger) image |
| 0xe4     | PresetPskRead          | out+in    | Read pre-shared key data |

---

## Command 0x90: DownloadChipConfig (FpMcuDownloadChipConfig)

**Source:** `fpmcucmd.c` line 328, function `FpMcuDownloadChipConfig`
**Address:** 0x180050EB0

### What it does
Uploads the sensor configuration blob to the MCU. This is a 256-byte payload
containing register settings (image tcode, FDT tcode, FDT delta, DAC values,
NAV tcode, NAV dac, FDT offset, etc.) that configure the sensor's analog front end.

### Protocol
- Command byte: 0x90
- Sends: config data (typically up to 0x100 = 256 bytes)
- Receives: 2-byte ACK (first byte should be 0x01 for success)
- Sent via `IoHubMcuSendCmd2` (fcn.18007d6b0) with work type = 4 (sendCmd)

### How the config is built
The config is NOT a static blob. It's built dynamically by `GetChipConfig`
(in `chicagoh.c` / `milanfserchipconfig.c`) which:

1. Starts with a **default config template** (hardcoded per sensor type)
2. Modifies fields based on **OTP data** read from the sensor:
   - `_MilanFSerModifyImageTcode` - sets image capture timing code from OTP[0x16]
   - `_MilanFSerModifyFdtTcode` - sets FDT timing code
   - `_MilanFSerModifyFdtDelta` - sets FDT threshold delta
   - `_MilanFSerModifyFdtDac` - sets FDT DAC value from OTP[0x1F]/OTP[0x1A]
   - `_MilanFSerModifyNavTcode` - sets navigation timing code
   - `_MilanFSerModifyNavDac` - sets navigation DAC value
   - `_MilanFSerModifyFdtOffset` - sets FDT area offset
   - `_MilanFSerGetFdtAreaNum` - gets FDT detection area count
3. The modified config is then sent as the 0x90 payload

### OTP-to-Config mapping (from `chicagoh.c` debug strings)
```
Tcode extraction from OTP:
  step1: aOTPData[0x16] != 0 && aOTPData[0x16] == ~aOTPData[0x17]
  step2: aOTPData[0x16] != 0 && aOTPData[0x16] == aOTPData[0x19]
  step3: aOTPData[0x19] != 0 && aOTPData[0x19] == ~aOTPData[0x17]

DAC extraction from OTP:
  step1: aOTPData[0x1F] != 0 && aOTPData[0x1F] == ~aOTPData[0x1B]
  step2: aOTPData[0x1F] != 0 && aOTPData[0x1F] == aOTPData[0x1A]
  step3: aOTPData[0x1A] != 0 && aOTPData[0x1A] == ~aOTPData[0x1B]
```

### Config Types (from `DownloadChipConfig` in `milanf.c`)
The driver supports 4 config types, each a table of (register_addr, data) pairs:
- Type 0: 7 register writes (default image config)
- Type 1: 7 register writes (alternate config)
- Type 2: 2 register writes (minimal)
- Type 3: 12 register writes (extended)

Note: The `DownloadChipConfig` function in `milanf.c` writes registers
individually via 0x84 (WriteRegister). The 0x90 command sends the whole
config blob at once. Both are used -- registers for fine-tuning, 0x90 for
the bulk config upload.

### What we need to do
We can likely send the **default config** for 5E0A without OTP modification
initially. The config should be 256 bytes. If we don't have the default
config template, we may need to capture it from a USB trace.

---

## Command 0xC4: SetDrvState (McuSetDrvState)

**Source:** `mcuimpl.c` line 1010, function `McuSetDrvState`
**Address:** 0x1800A7E40

### What it does
Tells the MCU what "driver state" the host is in. This is a simple state
notification that the MCU uses to adjust its behavior.

### Protocol
- Command byte: 0xC4
- Sends: 2 bytes -- `[state_byte, 0x00]`
- Receives: 2-byte ACK (checks first byte == 0x01)
- The `state_byte` is passed as the `dl` register parameter

### Why called twice
The Windows driver calls it twice with different state values during init:
1. First call: likely state = 0x00 or 0x01 (entering init)
2. Second call: likely state indicating "ready" or "configured"

From the caller context (`fcn.18009ae10` at `0x18009b6bd`), the state byte
comes from the logic layer and represents driver lifecycle states.

### What we need to do
Send `cmd=0xC4, payload=[state, 0x00]` where state is likely:
- 0x00 = driver initializing / idle
- 0x01 = driver ready / active
We should capture exact values from a USB trace to confirm.

---

## Command 0xD2: GetPovImage (McuGetPovImage)

**Source:** `mcuimpl.c` line 891, function `McuGetPovImage`
**Address:** 0x1800A5D30

### What it does
Retrieves a "Power-On Verify" (POV) image from the sensor. POV is Goodix's
term for wake-on-fingerprint / one-key-boot functionality. When the system
is in sleep/hibernate, the sensor can independently detect a finger touch,
capture an image, and wake the system. This command retrieves that
pre-captured image.

### Protocol
- Command byte: 0xD2
- Sends: 2 bytes (mode/flags)
- Receives: up to 0x2800 (10240) bytes = the captured image
- Sent via `IoHubMcuSendCmd2` (fcn.18007d720) -- the version with recv buffer
- The recv buffer pointer and size pointer are passed as extra arguments

### Image size analysis
10240 bytes for a 12-bit image:
- If 88x80 pixels at 12bpp packed: 88 * 80 * 1.5 = 10560 (close)
- If 80x64 pixels at 16bpp: 80 * 64 * 2 = 10240 (exact match)
- More likely: raw sensor data that needs the same reconstruction as 0x20

### Relationship to init
This is NOT a calibration frame. It's checking if the sensor captured a
finger image during the power-off state. In the init sequence, it comes
AFTER TLS setup and config download. The flow is:
1. PovImageCheck (0xD6) -- asks "did sensor capture a POV image?"
2. If yes, GetPovImage (0xD2) -- retrieves the image for matching
3. The driver can then do a verify against enrolled templates

### What we need to do
For basic init, **we may not need this command at all**. POV is a
power-management optimization feature. If we don't care about
wake-on-fingerprint, we can skip 0xD2 entirely.

However, if PovImageCheck (0xD6) returns "image available", we may
need to clear it with GetPovImage to avoid the sensor being stuck
in POV mode.

---

## Command 0xA2: ResetFingerPrint (FpMcuResetFingerPrint)

**Source:** `fpmcucmd.c` line 294, function `FpMcuResetFingerPrint`
**Address:** 0x180051380

### What it does
Resets the fingerprint sensor module on the MCU. Returns an IRQ status
word indicating the sensor's state after reset.

### Protocol
- Command byte: 0xA2
- Sends: 2 bytes `[0x05, 0x14]` (hardcoded at 0x18005145D-0x18005146B)
  - 0x05 = reset type/command
  - 0x14 = timeout/parameter (decimal 20)
- Receives: 4 bytes (IRQ status as uint32)
- Logs: "irq status: 0x%x"

### IRQ Status 0x80001
From the init log, `irq status = 0x80001` after reset:
- Bit 0 (0x1): Reset complete / sensor ready
- Bit 19 (0x80000): Likely "power on" or "clean state" flag
- This appears to be the normal/expected status after a successful reset

### What we need to do
Send `cmd=0xA2, payload=[0x05, 0x14]`, expect 4 bytes back.
Verify that bit 0 is set in the response (reset complete).

---

## Complete Windows Init Sequence (from logicmilanfseries.c "Start")

### Phase 1: Device Loading (mcudevloader.c "Load")
```
1. WakeUp (NOP / enable chip)
2. PSK process (preset_psk_read, 0xE4)
3. Update firmware check
4. Dump FW version (GetFirmwareVersion, 0xA8)
5. DumpDeviceInfo
6. Reset fingerprint (0xA2) -> check IRQ status
7. Get chipid (ReadRegister, 0x82)
8. Find fingerprint module for chipid
9. Create FP context
```

### Phase 2: Logic Start (logicmilanfseries.c "Start")
```
1.  Check sensor (ESD check, register validation)
2.  OneKeyBoot image status check (PovImageCheck, 0xD6)
3.  If POV image exists: GetPovImage (0xD2) + process it
4.  Fetch PSK (preset_psk_read, 0xE4)
5.  Start TLS handshake (0xD0)
6.  If TLS fails: retry with PSK re-fetch
7.  Download chip config (0x90) -- sensor register blob
8.  [Several internal config steps]
9.  SetDrvState (0xC4) -- notify MCU of driver state
10. SetDrvState (0xC4) -- second state update
11. Update all base (capture baseline frames for FDT/NAV/image)
12. Algorithm preprocess init
13. Get SSO / Pre image (if wake-on-finger active)
14. -> Ready for FDT_DOWN
```

### Phase 3: Normal Operation
```
1. FDT_DOWN (0x36) -- finger detection in down mode
2. [Wait for finger interrupt]
3. GET_IMAGE (0x20) -- capture fingerprint
4. Process / match image
5. Return to FDT_DOWN
```

---

## Gap Analysis: What Our Driver Is Missing

### Current sequence:
```
NOP -> enable_chip -> NOP -> preset_psk_read -> TLS_CMD -> pov_image_check -> TLS_IMG -> FDT_DOWN
```

### Windows sequence (with our gaps marked):
```
NOP
GetFirmwareVersion (0xA8)        <- MISSING (informational, may not be required)
PresetPskRead (0xE4) x3          <- We do this
GetFirmwareVersion (0xA8)        <- MISSING (informational)
ResetFingerPrint (0xA2)          <- MISSING ** CRITICAL **
  -> sends [0x05, 0x14], expects IRQ status
ReadRegister (0x82)              <- MISSING (chipid check, may not be required)
GetOtp (0xA6)                    <- MISSING ** IMPORTANT for config **
PovImageCheck (0xD6)             <- We do this
--- TLS Handshake ---
TLS (0xD0)                       <- We do this
DownloadChipConfig (0x90)        <- MISSING ** CRITICAL **
  -> 256-byte sensor config derived from OTP
SetDrvState (0xC4)               <- MISSING (may be important)
  -> [state, 0x00]
SetDrvState (0xC4)               <- MISSING (may be important)
  -> [state, 0x00]
GetPovImage (0xD2)               <- MISSING (only needed if POV image exists)
  -> or "update all base" baseline capture
FDT_DOWN (0x36)                  <- We do this
GET_IMAGE (0x20)                 <- We do this
```

### Priority fixes:
1. **ResetFingerPrint (0xA2)** - Send `[0x05, 0x14]` before TLS. The sensor
   may need a clean reset to properly initialize its state machine for FDT.

2. **GetOtp (0xA6)** + **DownloadChipConfig (0x90)** - The sensor needs its
   analog front-end configured (tcode, DAC, FDT thresholds) before FDT will
   work reliably. Without this, FDT may use wrong detection thresholds.

3. **SetDrvState (0xC4)** - May be needed to tell the MCU the host driver
   is ready. Without it, the MCU might not enter the right operational mode.

4. GetFirmwareVersion (0xA8), ReadRegister (0x82) - Informational, lower priority.

5. GetPovImage (0xD2) - Only needed for wake-on-fingerprint, skip for now.

---

## Implementation Notes

### Helper functions used by all MCU commands
Three helper functions are called before every MCU command send:
- `fcn.18008185c` - Gets some session/context ID
- `fcn.1800817ec` - Gets a second context parameter
- `fcn.1800818cc` - Gets a third context parameter (returns the IoHub)

These appear to retrieve the IoHub context needed for command dispatch.

### Command struct layout (offset from arg_10h)
From `_IoHubExec` (fcn.18007e798):
- +0x00: work type (0=readRaw, 1=readReg, 2=writeReg, 3=writeReg2, 4=sendCmd, 5=writeTls)
- +0x08: command byte (word, e.g., 0x90)
- +0x0A: flags (word)
- +0x10: send data pointer
- +0x18: send data size
- +0x20: recv data pointer
- +0x28: recv buffer size
- +0x30: recv actual size pointer
- +0x40: ack event handle
- +0x60: status code

### TLS wrapping
Commands sent after TLS handshake (0x90, 0xC4, 0xD2, 0x36, 0x20) are
encrypted within the TLS session. The IoHub has work type 5 (writeTls)
for this purpose. Our driver already handles TLS wrapping.

---

## Summary: Recommended Init Sequence for Our Driver

```
1.  NOP (0x00)                          -- wake sensor
2.  GetFirmwareVersion (0xA8)           -- optional, log version
3.  PresetPskRead (0xE4)               -- get PSK for TLS
4.  ResetFingerPrint (0xA2)            -- SEND [0x05, 0x14], verify IRQ
5.  ReadRegister (0x82)                 -- optional, verify chipid
6.  GetOtp (0xA6)                       -- read OTP for config building
7.  PovImageCheck (0xD6)               -- check/clear POV state
8.  TLS Handshake (0xD0)              -- establish encrypted channel
9.  DownloadChipConfig (0x90)          -- send 256-byte sensor config
10. SetDrvState (0xC4) [0x01, 0x00]   -- notify MCU: driver ready
11. FDT_DOWN (0x36)                    -- start finger detection
12. GET_IMAGE (0x20)                   -- capture on finger detect
```

Steps 9-10 are the most likely cause of our FDT verify failures.
The sensor's analog front-end needs proper configuration before
FDT thresholds and image capture will work correctly.
