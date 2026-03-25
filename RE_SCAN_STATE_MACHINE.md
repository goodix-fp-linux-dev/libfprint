# Scan State Machine - Milan F-Series (Wbdi.dll)

Reverse engineered from `Wbdi.dll` v3.0.141.150 (x86-64 PE32+).
Device: Goodix 27c6:5e0a.

Source files referenced in debug strings:
- `logicmilanfseries\logicmilanfseries.c` -- high-level state machine
- `fingerprint\milanfseries\milanfsermcu.c` -- MCU image/FDT commands
- `fingerprint\milanfseries\milanfser.c` -- FDT base calculation
- `fingerprint\milanfseries\milanfserspi.c` -- FDT response parsing
- `fingerprint\milanfseries\milanh.c` -- HU wrappers (USB-HID path)
- `mcu\geneva\geneva.c` -- TLS handshake
- `mcu\mcuimpl.c` -- DrvState commands
- `iohub\iohub.c` -- low-level USB command send/receive

---

## 1. Command Byte Encoding

The FDT mode command byte is computed in `HUFpMcuSwitchToFdtMode` (fcn.18006d2c0):

```c
cmd_byte = (mode << 1) | 0x30;
```

| mode | cmd_byte | Name         |
|------|----------|--------------|
| 1    | 0x32     | FDT_DOWN     |
| 2    | 0x34     | FDT_UP       |
| 3    | 0x36     | FDT_MANUAL   |

Other known commands:
| cmd_byte | Name                | Payload Size |
|----------|---------------------|-------------|
| 0x20     | GET_IMAGE           | 10 bytes    |
| 0x36     | FDT_MANUAL          | variable    |
| 0x32     | FDT_DOWN            | variable    |
| 0x34     | FDT_UP              | variable    |
| 0x90     | DOWNLOAD_CHIP_CONFIG| config blob |
| 0xC4     | SET_DRV_STATE       | 2 bytes     |
| 0x70     | (NAV/other)         | varies      |
| 0x82     | (unknown)           | varies      |
| 0xAC     | (unknown)           | varies      |
| 0x60     | (unknown)           | varies      |

---

## 2. Start() -- Entry Point (fcn.180089100)

Source: `logicmilanfseries.c`, function `Start`, ~4640 bytes.
Address: `0x180089100`

### Pseudo-code

```c
void Start(ctx) {
    // ---- Phase 1: OneKeyBoot check ----
    // Read OneKeyBoot status from device (offset 0xD0 in vtable)
    // The status byte is at response[1]
    status = ReadOneKeyBootStatus(ctx);
    image_status = response[1];  // byte at index 1
    LOG("OneKeyBoot image status:%d", image_status);  // line 579

    // image_status == 255 (0xFF) means NO OneKeyBoot image present
    // image_status == 0xAA means OneKeyBoot image IS present

    if (status == 1 && response[0] == 0xAA && sgx_flag) {
        LOG("Has OneKeyBoot image, EC From low to high Or resume from MS!");
        // Device has firmware image in OneKeyBoot partition
        // This means EC power was lost or resuming from modern standby
    }

    // ---- Phase 2: Check if TLS reconnect needed ----
    // Check flags: SgxLost, McuLostPower, TlsConnected
    LOG("fetch psk, SgxLost:%d McuLostPower:%d TlsConnected:%d");  // line 607

    if (McuLostPower || sgx_e0_flag) {
        // PSK needs to be re-fetched
        FetchPsk(ctx);           // vtable offset 0x90
    }

    // Connect TLS (try + retry)
    FetchPskIfNeeded(ctx);       // vtable offset 0x80

    // ---- Phase 3: TLS Handshake ----
    if (need_tls) {
        LOG("start tls...");           // line 627
        StartTls(ctx);                 // vtable offset 0x88 --> GenevaStartTls
        if (failed && sgx_e0_flag) {
            LOG("fetch psk...");
            FetchPsk(ctx);
        }
        LOG("retry start tls...");     // line 640
        StartTls(ctx);                 // retry
        if (failed && reconnect_flag) {
            ResetConnection(ctx, 1);   // vtable offset 0x28
            ctx->e0 = 1;
            ctx->retryFlag = 1;
            Sleep(500);
            goto restart;              // jump back to top of loop
        }
    }

    // ---- Phase 4: Download Chip Config (cmd 0x90) ----
    LOG("download chip config...");    // line 659
    LogicDownloadChipConfig(ctx);      // fcn.1800827e4
    // This sends cmd 0x90 with the chip configuration blob.
    // The config contains DAC values, FDT thresholds, image parameters.

    // ---- Phase 5: DrvState and FDT Init ----
    // Clear 14 bytes + 20 bytes buffers for FDT base storage
    memset(fdt_base0_buf, 0, 14);   // 14 bytes = 7 words (FDT area count)
    memset(fdt_base1_buf, 0, 20);   // 20 bytes = for FDT data

    // Get FDT base0 (sends FDT_MANUAL 0x36)
    GetFdtBase0(ctx->mcu, fdt_base0_buf);     // vtable offset 0x58/0x58
    // Get FDT base1 (sends FDT_MANUAL 0x36)
    GetFdtBase1(ctx->mcu, fdt_base1_buf);     // vtable offset 0x58/0x60

    // Calculate FDT parameters from bases
    CalcFdtParams(fdt_base0_buf, fdt_base1_buf.word0, fdt_base1_buf.word2);

    // Initialize FDT module
    FdtInit(ctx->fp_context);

    // Set DrvState (sends cmd 0xC4)
    SetDrvState(ctx->fp_context[0]);

    // ---- Phase 6: Update All Bases ----
    // (only if first run or not using cached bases)
    LOG("update all base...");         // line 727
    UpdateAllBase(ctx);                // fcn.18008ccc4
    // Sends FDT_MANUAL (0x36) to get fresh FDT baseline data
    // Then calculates FDT_DOWN base and FDT_UP base from it

    // ---- Phase 7: Algorithm Init ----
    LOG("algorithm preprocess init");  // line 740
    AlgorithmPreprocessInit(ctx);

    // ---- Phase 8: Register scan callback and wait ----
    // Register LogicSubmitPovCb as the callback for scan events
    RegisterCallback(LogicSubmitPovCb, ctx);
    LOG("request processed");          // line 747

    // ---- Phase 9: Pre-image / SSO handling ----
    if (sso_flag && pre_image_needed) {
        LOG("Get SSO | Pre image");    // line 754
        // Queue a pre-capture or SSO identification
    }

    // ---- Phase 10: Enter critical section, start FDT_DOWN ----
    EnterCriticalSection(&ctx->cs);

    if (has_pending_request && ctx->image_buffer) {
        // Direct image path (already have buffer)
        SendGetImage(...);
    } else {
        // Set up FDT_DOWN wait structure
        fdt_params.field_0 = 0;
        fdt_params.field_4 = 0;
        fdt_params.image_buffer = ctx->image_buffer;
        fdt_params.image_size = ctx->fp_context->image_size;
        SwitchToFdtDown(ctx, &fdt_params);  // vtable offset 0xB8
        // This sends cmd 0x32 (FDT_DOWN) to enter finger-detect mode
    }

    LeaveCriticalSection(&ctx->cs);

    ctx->d4 = 1;  // mark "scan started"
    SetEvent(ctx->event);
}
```

### "image status:255" Meaning

`image_status` is the byte at index 1 of the OneKeyBoot response.
- **255 (0xFF)** = No OneKeyBoot firmware image present (normal state)
- **0xAA** = OneKeyBoot image IS present (device recovering from power loss)

When status is 255, the driver proceeds normally. When 0xAA, it indicates
the MCU lost power and needs re-initialization (PSK re-fetch, TLS re-handshake).

---

## 3. UpdateAllBase (fcn.18008ccc4) -- The FDT Baseline Acquisition

This function is called during `Start()` to acquire fresh baseline readings
from the sensor. It uses FDT_MANUAL (0x36) to do this.

### Pseudo-code

```c
int UpdateAllBase(ctx) {
    LOG("UpdateAllBase start");

    // Check if we can use cached FDT bases from file
    if (cached_bases_valid) {
        LOG("use fdt up down & fdt down base from file");
        // Load from saved file and skip re-acquisition
        return;
    }

    // Step 1: Get FDT base0
    LOG("get fdt base0 ...");
    // Calls HUMilanFSerMcuGetFdtManualBase which sends cmd 0x36
    ret = GetFdtManualBase(ctx, base0_buf, base0_size, fdt_down_dest, ...);

    // Step 2: Get nav base
    LOG("get nav base...");
    GetNavBase(ctx);

    // Step 3: Get FDT base1
    LOG("get fdt base1 ...");
    ret = GetFdtManualBase(ctx, base1_buf, base1_size, fdt_up_dest, ...);

    // Step 4: Get FDT Delta
    LOG("get fdt Delta ...");
    ret = GetFdtDelta(ctx);
    LOG("fdt delta: 0x%x", delta);

    // Step 5: Validate and update bases
    if (!new_base_valid) {
        LOG("new base invalid, using existing base");
    }

    // Step 6: Calculate FDT_DOWN base from FDT_MANUAL base
    // MilanFSerCalcFdtDownBase: copies the FDT_MANUAL response
    // and applies the "down threshold" offset
    CalcFdtDownBase(ctx, fdt_manual_base, base_size, fdt_down_base);

    // Step 7: Calculate FDT_UP base similarly
    CalcFdtUpBase(ctx, fdt_manual_base, base_size, fdt_up_base);

    LOG("current fdt up base & fdt down base");
    // Dumps both bases for debug

    // Step 8: Validate image base
    // Get image via GET_IMAGE (0x20) to check if it's valid
    LOG("imageRet: %s");
    if (image_valid) {
        LOG("update image base, set IsImageBaseValid: TRUE");
    } else {
        LOG("invalid image base(%s)");
    }

    // Step 9: Update nav base
    LOG("navRet: %s");
    if (nav_valid) {
        LOG("update nav base");
    }

    LOG("isBaseValid = %d, isImageBaseValid = %d");
    LOG("UpdateAllBase end");
}
```

---

## 4. HUFpMcuSwitchToFdtDown (fcn.18006c500) -- FDT_DOWN Command

Called to put the sensor into finger-detect-down mode.

### Pseudo-code

```c
int HUFpMcuSwitchToFdtDown(ctx, fdt_base_data, fdt_base_size) {
    LOG("enter");  // HUFpMcuSwitchToFdtDown, line 854

    if (ctx == NULL) {
        LOG("invalid param");
        return 0;
    }

    if (!use_sgx_path) {
        // Standard path
        ackTimeout = GetAckTimeout(ctx);

        // Build FDT command payload:
        // mode=1 (FDT_DOWN), fdt_base_data, fdt_base_size
        ret = HUFpMcuSwitchToFdtMode(ctx,
                mode=1,           // FDT_DOWN
                fdt_base_data,    // pointer to FDT base data
                fdt_base_size,    // size of base data
                ackTimeout,       // from GetAckTimeout
                0                 // no extra params for FDT_DOWN
        );
    }
    // SGX path skipped (different codepath)

    LOG("exit");
    return ret;
}
```

---

## 5. HUFpMcuSwitchToFdtMode (fcn.18006d2c0) -- FDT Command Builder

This is the core function that builds and sends all FDT commands (0x32, 0x34, 0x36).

### Pseudo-code

```c
int HUFpMcuSwitchToFdtMode(ctx, mode, fdt_base_ptr, fdt_base_size,
                            ackTimeout, extra_params...) {
    // mode: 1=FDT_DOWN, 2=FDT_UP, 3=FDT_MANUAL
    int reg_size = 8;  // 8 bytes of register data from context+0x70

    switch (mode) {
    case 1:  // FDT_DOWN
        // Format the base data for logging
        hex_str = FormatHex(fdt_base_ptr, fdt_base_size, 16);
        LOG("switch to fdt down");
        LOG("%s[0x%x]:\r\n%s", "switch to fdt down", fdt_base_size, hex_str);
        FreeHexStr(hex_str);

        // Set sub-mode byte based on global flag
        if (global_flag_18024686c) {
            sub_mode = 0x1C;   // 28 -- extended mode
        } else {
            sub_mode = 0x0C;   // 12 -- standard mode
        }
        break;

    case 2:  // FDT_UP
        hex_str = FormatHex(fdt_base_ptr, fdt_base_size, 16);
        LOG("switch to fdt up");
        FreeHexStr(hex_str);
        sub_mode = 0x0E;   // 14
        break;

    case 3:  // FDT_MANUAL
        hex_str = FormatHex(fdt_base_ptr, fdt_base_size, 16);
        LOG("switch to fdt manual");
        FreeHexStr(hex_str);
        sub_mode = 0x0D;   // 13
        break;

    default:
        LOG("invalid param");
        return 0;
    }

    // ---- Build the payload ----

    bool has_base_data = (fdt_base_ptr != NULL && fdt_base_size > 0);

    if (has_base_data) {
        // Total size: reg_size*2 + fdt_base_size + 7
        total_size = fdt_base_size + reg_size * 2 + 7;
    } else {
        // Total size: reg_size*2 + 6  (no base data appended)
        total_size = reg_size * 2 + 6;
    }

    payload = allocate(total_size);

    // ---- Copy register data from context struct ----
    // Source: ctx + 0x70, length: reg_size (8 bytes)
    // These are the DAC register values (4 x 16-bit words)
    // Destination: payload + 2  (after the 2-byte header)
    memcpy(payload + 2, ctx + 0x70, reg_size);

    // ---- Copy base data if present ----
    if (has_base_data) {
        // Base data goes after: reg_size + 2 bytes header
        memcpy(payload + reg_size + 2, fdt_base_ptr, fdt_base_size);
    }

    // ---- For FDT_DOWN (mode==1): duplicate register data ----
    if (mode == 1 && (ctx+0x70) != NULL && reg_size > 0) {
        // Copy reg data AGAIN at offset: reg_size + fdt_base_size + 6
        // This is the "second copy" of DAC values for the DOWN detect
        memcpy(payload + reg_size + fdt_base_size + 6,
               ctx + 0x70, reg_size);
    }

    // ---- Set byte 0: sub_mode | (sleep_flag << 4) ----
    byte0 = sub_mode | (global_sleep_flag_25c6f9 << 4);
    payload[0] = byte0;

    // ---- Set byte 1: has_base_data flag ----
    payload[1] = has_base_data ? 1 : 0;

    // ---- Compute command byte ----
    // For mode != 3 (FDT_MANUAL):
    if (mode != 3) {
        cmd = (mode << 1) | 0x30;
        // mode=1 -> 0x32 (FDT_DOWN)
        // mode=2 -> 0x34 (FDT_UP)

        ioHandle = GetIoHandle(ctx);
        ret = IoHubMcuSendCmd2(ioHandle,
                cmd,           // 0x32 or 0x34
                payload,       // built payload
                total_size,    // payload length
                ackTimeout,    // ack timeout
                0, 0);         // no response expected inline
    } else {
        // FDT_MANUAL (mode=3, cmd=0x36)
        cmd = 0x36;
        ioHandle = GetIoHandle(ctx);
        ret = IoHubMcuSendCmd2(ioHandle,
                cmd,
                payload,
                total_size,
                extra_params...);  // includes response buffer
    }

    free(payload);
    return ret;
}
```

### FDT_DOWN (0x32) Payload Layout

For a sensor with 8 bytes of DAC registers and N bytes of FDT base data:

```
Offset  Size   Content
------  ----   -------
0       1      mode_byte = sub_mode | (sleep_flag << 4)
               sub_mode: 0x0C (standard) or 0x1C (extended) for FDT_DOWN
               sleep_flag: from global at 0x18025C6F9
1       1      has_base_data: 1 if base data present, 0 otherwise
2       8      DAC register values (from ctx+0x70, 4 x 16-bit words)
10      N      FDT base data (the FDT_MANUAL response, typically sensor_size/2 bytes)
10+N    1      (padding/alignment byte)
11+N    8      DAC register values AGAIN (second copy, FDT_DOWN only)
------
Total: N + 8*2 + 7 = N + 23 bytes (when has_base_data=1)
```

For our 5e0a sensor (88x108 = 9504 pixels, but FDT uses zone data):
- The FDT base size from chip config is typically `ctx->fp_context->image_size`
  stored at `ctx->fp_context[0x20][0x20] + 0x44`
- For Milan F-series, this is typically 7-12 words (14-24 bytes) of zone averages.

### Context Struct (ctx + 0x70): DAC Register Block

The 8 bytes at `ctx + 0x70` are the current DAC (Digital-to-Analog Converter)
register values. These control the sensor sensitivity:

```
ctx + 0x70:  DAC[0] (16-bit)  -- image DAC word 0
ctx + 0x72:  DAC[1] (16-bit)  -- image DAC word 1
ctx + 0x74:  DAC[2] (16-bit)  -- image DAC word 2
ctx + 0x76:  DAC[3] (16-bit)  -- image DAC word 3
```

These are populated by:
1. OTP (One-Time-Programmable) data read from the sensor during init
2. Modified by `_MilanFSerModifyFdtDac` based on chip config adjustments
3. Further adjusted by `_MilanFSerModifyFdtTcode` and `_MilanFSerModifyFdtDelta`

---

## 6. HUMilanFSerMcuGetFdtManualBase (fcn.18006c860)

Gets the FDT baseline by sending FDT_MANUAL (0x36) command.

### Pseudo-code

```c
int HUMilanFSerMcuGetFdtManualBase(ctx, fdt_base_data_in,
        base_size, dest_raw, dest_processed, ...) {
    LOG("enter");  // line 919

    if (ctx == NULL) return 0;
    if (base_size > 24) {
        LOG("not supported");  // max 24 bytes = 12 zones
        return 0;
    }

    chip_info = ctx->info->chip_info;  // ctx[0x20][0x20]
    expected_size = chip_info->base_data_size;  // at offset 0x44

    // Validate sizes match
    if (base_size != 0 && base_size != expected_size) {
        LOG("invalid param, baseSize: %d", base_size);
        return 0;
    }

    // Prepare input data: extract high bytes with bit 7 set
    if (fdt_base_data_in != NULL) {
        for (i = 0; i < base_size / 2; i++) {
            prepared[i] = (fdt_base_data_in[i] & 0xFF00) | 0x0080;
            // Take high byte, set bit 7 in low byte
        }
    }

    // Get timeouts
    dataTimeout = GetDataInTimeout(ctx);
    ackTimeout = GetAckTimeout(ctx);

    // Send FDT_MANUAL command
    // This calls HUFpMcuSwitchToFdtMode with mode=3
    ret = HUFpMcuSwitchToFdtMode(ctx,
            mode=3,               // FDT_MANUAL -> cmd 0x36
            prepared,             // prepared base data
            base_size,            // size
            response_buf,         // output: raw FDT response
            ackTimeout,
            dataTimeout);

    if (!ret) {
        LOG("-->failed");
        return 0;
    }

    // Copy response to dest_raw (raw FDT manual base)
    if (dest_raw != NULL) {
        memcpy(dest_raw, response_buf, expected_size);
    }

    // Copy to dest_processed and apply post-processing
    if (dest_processed != NULL) {
        memcpy(dest_processed, response_buf, expected_size);
        // Apply MilanFSerSpiParseFdt processing
        ParseFdtResponse(dest_processed, expected_size);
    }

    LOG("exit");
    return 1;
}
```

---

## 7. _HUGetImage (fcn.1800652a8) -- GET_IMAGE (0x20) Command

### Pseudo-code

```c
int _HUGetImage(ctx, image_status, image_buffer, image_buf_size,
                ackTimeout, dataTimeout) {
    LOG("enter");  // _HUGetImage, line 17

    if (ctx == NULL || image_buffer == NULL || image_status == NULL) {
        LOG("invalid param");
        return 0;
    }

    // Copy 8 bytes of DAC register data from ctx+0x70
    memcpy(dac_local, ctx + 0x70, 8);  // 4 x 16-bit DAC words

    // Set command flags byte
    cmd_flags[0] = 0x01;  // base flag

    // Check if "no touch" mode (image_status[0] == 0 means finger present)
    if (image_status[0] == 0) {
        // Finger present: clear bit 2
        cmd_flags[0] &= ~0x04;  // = 0x01
    } else {
        // No finger: set bit 2
        cmd_flags[0] |= 0x04;   // = 0x05
    }

    // Build 10-byte payload for GET_IMAGE (0x20)
    // payload[0] = cmd_flags[0]  (0x01 or 0x05)
    // payload[1] = reserved (0)
    // payload[2..9] = DAC values (split into individual bytes)

    int write_pos = 2;
    for (i = 0; i < 4; i++) {
        // Log each DAC word
        LOG("image set dac[%d]: 0x%x", i, dac_local[i]);

        // Write low byte of DAC[i]
        payload[write_pos + i] = dac_local[i] & 0xFF;
        write_pos++;

        // Write high byte of DAC[i]
        payload[write_pos + i] = (dac_local[i] >> 8) & 0xFF;
    }

    // Send GET_IMAGE command
    ioHandle = GetIoHandle(ctx);
    ret = IoHubMcuSendCmd2(ioHandle,
            0x20,                  // GET_IMAGE command
            payload,               // 10-byte payload
            10,                    // payload size
            image_buffer,          // response goes here
            image_buf_size,        // expected response size
            ackTimeout,
            dataTimeout);

    if (!ret) {
        LOG("-->failed");
        return 0;
    }

    LOG("exit");
    return ret;
}
```

### GET_IMAGE (0x20) Payload Layout

```
Offset  Size   Content
------  ----   -------
0       1      flags: 0x01 (finger present) or 0x05 (no finger/base capture)
1       1      reserved (0x00)
2       1      DAC[0] low byte
3       1      DAC[0] high byte
4       1      DAC[1] low byte
5       1      DAC[1] high byte
6       1      DAC[2] low byte
7       1      DAC[2] high byte
8       1      DAC[3] low byte
9       1      DAC[3] high byte
------
Total: 10 bytes
```

---

## 8. McuSetDrvState (fcn.1800a7e40) -- SET_DRV_STATE (0xC4)

### Pseudo-code

```c
int McuSetDrvState(ctx, state_byte) {
    LOG("enter");  // McuSetDrvState, line 1010

    if (!ValidateCtx(ctx, 1))
        return 0;

    // Build 2-byte payload
    payload[0] = state_byte;
    payload[1] = 0;  // (zeroed by memset)

    ackTimeout = GetAckTimeout(ctx);
    ioHandle = GetIoHandle(ctx);

    ret = IoHubMcuSendCmd2(ioHandle,
            0xC4,            // SET_DRV_STATE
            payload,         // 2 bytes
            2,               // size
            ackTimeout,
            0, 0, 0);       // no response expected

    LOG("exit");
    return ret;
}
```

---

## 9. Complete Scan Flow: Start() to HandleFdtDown

### Sequence Diagram

```
Start()
  |
  v
[1] OneKeyBoot check (image_status:255 = no FW image = normal)
  |
  v
[2] Check SgxLost / McuLostPower / TlsConnected
  |  If power was lost, re-fetch PSK
  |
  v
[3] "start tls..." --> GenevaStartTls (TLS handshake)
  |  If fails, retry once with Sleep(500)
  |
  v
[4] "download chip config..." --> cmd 0x90
  |  Uploads sensor configuration (DAC, FDT params, thresholds)
  |  Populates ctx->fp_context with chip config data
  |
  v
[5] DrvState setup --> cmd 0xC4 (x2: once for init, once after config)
  |  Sets MCU driver state
  |
  v
[6] "update all base..." --> UpdateAllBase()
  |  |
  |  v
  |  [6a] "get fdt base0 ..." --> FDT_MANUAL (cmd 0x36)
  |  |    First FDT_MANUAL: gets raw sensor baseline zone averages
  |  |    Response: N bytes of zone data (N = chip_config.base_data_size)
  |  |    This data populates the FDT_DOWN threshold computation
  |  |
  |  v
  |  [6b] "get nav base..." --> (navigation baseline)
  |  |
  |  v
  |  [6c] "get fdt base1 ..." --> FDT_MANUAL (cmd 0x36) again
  |  |    Second manual read for FDT_UP base computation
  |  |
  |  v
  |  [6d] "get fdt Delta ..." --> computes temperature/drift compensation
  |  |
  |  v
  |  [6e] MilanFSerCalcFdtDownBase(manual_base, size, fdt_down_base)
  |  |    Copies FDT_MANUAL response, applies down-threshold offset
  |  |    This becomes the payload for cmd 0x32
  |  |
  |  v
  |  [6f] MilanFSerCalcFdtUpBase(manual_base, size, fdt_up_base)
  |  |    Similarly computes the up-threshold base
  |  |
  |  v
  |  [6g] GET_IMAGE (cmd 0x20) to validate image baseline
  |
  v
[7] "algorithm preprocess init" --> Initialize matching algorithm
  |
  v
[8] Register LogicSubmitPovCb callback
  |
  v
[9] EnterCriticalSection
  |  Send FDT_DOWN (cmd 0x32) with computed fdt_down_base
  |  LeaveCriticalSection
  |  SetEvent -- signals "scan is active"
  |
  v
[WAITING FOR FINGER] -- MCU is in FDT_DOWN mode, IRQ when finger detected
  |
  v
HandleFdtDown() triggered by IRQ
  |
  v
[10] "HandleFdtDown start"
  |  |
  |  v
  |  [10a] "update fdt up base" -- recalculate FDT_UP base
  |  |
  |  v
  |  [10b] Check: is there a pending request? (enrollment/verify/identify)
  |  |     If "waiting fdt up, skip" -- wrong state
  |  |     If "no pending request, skip" -- no one asked for a scan
  |  |     If "image base not valid, wait fdt up" -- need recalibration
  |  |     If "display(screen) is off, skip" -- power saving
  |  |
  |  v
  |  [10c] "start capture data for identify"
  |  |     GET_IMAGE (cmd 0x20) -- capture the actual fingerprint image
  |  |
  |  v
  |  [10d] "Image_isTouchedByFinger: %s" -- software touch validation
  |  |     If temperature drift detected: "detected temperature drift"
  |  |     If image invalid: "image is invalid" / "void_image" / "image_base"
  |  |
  |  v
  |  "HandleFdtDown end"
  |
  v
HandleFdtUp() -- After image capture, switch to FDT_UP to detect finger lift
  |
  v
[11] "HandleFdtUp start"
  |  |
  |  v
  |  [11a] "update fdt down base" -- recalculate FDT_DOWN base for next cycle
  |  |
  |  v
  |  [11b] "update image base" -- update reference image for next comparison
  |  |
  |  v
  |  [11c] "broken check init in fdt up" / "broken check in fdt up"
  |  |     Pixel defect check during FDT_UP phase
  |  |
  |  v
  |  Send FDT_UP (cmd 0x34) -- wait for finger to lift
  |  |
  |  "HandleFdtUp end"
  |
  v
[12] SubmitPendingRequest() -- process the captured image
  |  |
  |  v
  |  Run preprocessor on image
  |  Compute quality and coverage
  |  LOG("quality = %d, coverage = %d")
  |  Submit result to WBF framework
  |
  v
[13] Loop back: Send FDT_DOWN (cmd 0x32) again for next scan
```

---

## 10. Why FDT_MANUAL (0x36) Before FDT_DOWN (0x32)?

The 0x36 FDT_MANUAL command is sent BEFORE 0x32 FDT_DOWN because:

1. **FDT_MANUAL reads the raw sensor zone averages** without triggering an
   interrupt. It's a one-shot "read current state" command.

2. **The response from 0x36 becomes the baseline** for the FDT_DOWN detection.
   The MCU compares future readings against this baseline to detect a finger.

3. **MilanFSerCalcFdtDownBase** takes the 0x36 response and applies a
   threshold offset (the "delta") to compute the FDT_DOWN trigger level.
   This adjusted baseline is then sent as part of the 0x32 payload.

4. **Without a fresh 0x36 reading**, the 0x32 command would use stale
   baseline data, leading to false triggers or missed touches.

The flow is:
```
0x36 (FDT_MANUAL) --> response = current_baseline
CalcFdtDownBase(current_baseline) --> fdt_down_threshold
0x32 (FDT_DOWN, payload includes fdt_down_threshold) --> MCU monitors
```

---

## 11. The Second FDT After GET_IMAGE

Looking at `HandleFdtDown` and `HandleFdtUp`:

- **First FDT before scan**: FDT_MANUAL (0x36) during `UpdateAllBase` to get
  the baseline, then FDT_DOWN (0x32) to enter detection mode.

- **After GET_IMAGE (0x20)**: The driver sends **FDT_UP (0x34)**, not another
  FDT_MANUAL. This is handled in `HandleFdtUp`.

### Differences between first and second FDT:

| Aspect | First FDT (before scan) | Second FDT (after image) |
|--------|------------------------|-------------------------|
| Command | 0x36 (FDT_MANUAL) then 0x32 (FDT_DOWN) | 0x34 (FDT_UP) |
| Purpose | Acquire baseline, enter detect mode | Wait for finger lift |
| sub_mode byte | 0x0D (manual) / 0x0C or 0x1C (down) | 0x0E (up) |
| Payload byte[1] | 1 (has base data) | 1 (has base data) |
| Base data source | Fresh from 0x36 response | Recalculated from latest data |
| Triggers on | Finger touching sensor | Finger leaving sensor |
| Next state | HandleFdtDown (capture) | Back to FDT_DOWN for next cycle |

The FDT_UP (0x34) after image capture uses an updated FDT_UP base that was
recalculated in `HandleFdtUp` -> "update fdt down base". After the finger
lifts (FDT_UP triggers), the driver loops back to FDT_DOWN (0x32) for the
next scan cycle.

---

## 12. IoHubMcuSendCmd2 (fcn.18007d720) -- USB Command Transport

All commands go through this function at the iohub layer.

```c
int IoHubMcuSendCmd2(io_handle, cmd, payload, payload_size,
                     response_buf, response_buf_ptr,
                     ack_timeout, data_timeout) {
    // Determines retry count based on TLS state
    retry_count = io_handle->tls_active ? 2 : 1;

    for (attempt = 0; attempt < retry_count; attempt++) {
        // Check for module exit
        WaitForSingleObject(io_handle->exit_event, 0);

        // Build command packet (fcn.180080f58)
        // Format: [pack_type=4][cmd_word][flags][payload...]
        packet = BuildPacket(4, cmd, 0, payload, payload_size,
                            response_buf_ptr, ack_timeout, data_timeout);

        // Send via TLS-encrypted USB
        ret = SendAndReceive(io_handle, packet);

        // Check response status
        if (ret && packet->status != 0xC0000000...) {
            success = 1;
        }

        // Handle retry on error 0xE0000004
        if (!success && packet->status == 0xE0000004) {
            need_retry = 1;
        }

        // Cleanup
        FreePacket(packet);
    }

    return success;
}
```

---

## 13. Key Data Structures

### Sensor Context (ctx)

```
Offset  Type       Field
------  ----       -----
0x00    HANDLE     event
0x08    HANDLE     event2
0x38    void*      mcu_handle (passed to vtable calls)
0x40    vtable*    function_vtable
0x50    void*      mcu_context
0x58    void*      fp_context (fingerprint processing context)
0x68    void*      chip_info_context
0x70    uint8[8]   dac_registers (4 x 16-bit DAC values)
0x88    void*      image_buffer
0x90    CRITICAL_SECTION  cs
0xC0    uint32     has_pending_request
0xD4    uint32     scan_started_flag
0xE0    uint32     sgx_power_flag
0xF8    void*      image_data_ptr
0x100   uint8[]    scratch buffer
```

### Chip Info (from ctx->fp_context[0x20][0x20])

```
Offset  Type       Field
------  ----       -----
0x00    uint32     sensor_type (0x0A, 0x0E, 0x11, etc.)
0x44    uint32     base_data_size (FDT zone data size in bytes)
```

### Function Vtable (ctx->func_vtable at ctx+0x40)

```
Offset  Function
------  --------
0x18    CheckOneKeyBoot / read status
0x20    (unknown)
0x28    ResetConnection
0x40    GetMcuInfo
0x80    FetchPsk
0x88    StartTls (GenevaStartTls)
0x90    FetchPskFromMcu
0xA0    SendDrvStateKeepAlive
0xB8    SwitchToFdtDown
0xD0    ReadOneKeyBootStatus
```

---

## 14. Summary: Full Command Sequence (from logs)

```
1.  OneKeyBoot read           --> internal status check
2.  GenevaStartTls            --> TLS handshake over USB
3.  cmd 0x90 (CONFIG)         --> upload chip configuration
4.  cmd 0xC4 (DRV_STATE)     --> set driver state (x2)
5.  cmd 0x36 (FDT_MANUAL)    --> get baseline reading #0 (fdt base0)
6.  cmd 0x36 (FDT_MANUAL)    --> get baseline reading #1 (fdt base1)
7.  CalcFdtDownBase()         --> compute down-threshold from 0x36 response
8.  cmd 0x20 (GET_IMAGE)     --> validate image baseline
9.  cmd 0x36 (FDT_MANUAL)    --> (if needed, refresh baseline)
10. cmd 0x32 (FDT_DOWN)      --> enter finger-detect mode (WAITING)
    --- finger touch detected by MCU IRQ ---
11. cmd 0x20 (GET_IMAGE)     --> capture fingerprint image
12. cmd 0x34 (FDT_UP)        --> wait for finger lift
    --- finger lift detected ---
13. cmd 0xC4 (DRV_STATE)     --> update state
14. Process image (quality/coverage check, matching)
15. Loop to step 10 (or re-calibrate if drift detected)
```

Additional commands seen in logs (0x70, 0x82, 0xAC, 0x60) are for:
- Navigation baseline acquisition
- Sensor self-test / broken pixel detection
- Power management
- ESD (Electrostatic Discharge) recovery
