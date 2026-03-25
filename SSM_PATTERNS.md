# FpiSsm (Sequential State Machine) Patterns for libfprint Drivers

## 1. FpiSsm API Reference

Source: `libfprint/fpi-ssm.h` and `libfprint/fpi-ssm.c`

### Core Concept

FpiSsm is a sequential state machine for async driver code. States are numbered
0..N-1 (matching C enum values). The machine walks through states linearly, but
can also jump to any state or complete early.

State flow: `S0 -> S1 -> S2 -> ... -> SN-1 -> completion callback`

### Lifecycle

```
fpi_ssm_new(dev, handler, NR_STATES)   // allocate (NR_STATES is an enum sentinel)
fpi_ssm_start(ssm, completion_cb)      // start running; takes ownership
  -> handler called for state 0
  -> ... each state calls fpi_ssm_next_state() when done ...
  -> completion_cb called, ssm auto-freed
```

### Key Functions

| Function | Purpose |
|---|---|
| `fpi_ssm_new(dev, handler, NR_STATES)` | Create SSM. NR_STATES is typically an enum sentinel. |
| `fpi_ssm_start(ssm, callback)` | Start the SSM. Takes ownership (auto-frees after callback). |
| `fpi_ssm_next_state(ssm)` | Advance to the next state. If past the last state, marks completed. |
| `fpi_ssm_jump_to_state(ssm, state)` | Jump to any state (can go backward for loops). |
| `fpi_ssm_mark_completed(ssm)` | Complete successfully (skips remaining states). |
| `fpi_ssm_mark_failed(ssm, error)` | Complete with error. Error is propagated to the completion callback. |
| `fpi_ssm_start_subsm(parent, child)` | Start a child SSM. On child success -> parent advances. On child failure -> parent fails with same error. |
| `fpi_ssm_get_cur_state(ssm)` | Returns current state number (for switch statement). |
| `fpi_ssm_set_data(ssm, data, destroy)` | Attach arbitrary data to the SSM. |
| `fpi_ssm_get_data(ssm)` | Retrieve attached data. |
| `fpi_ssm_next_state_delayed(ssm, ms)` | Advance after a delay (milliseconds). |
| `fpi_ssm_jump_to_state_delayed(ssm, state, ms)` | Jump after a delay. |

### Error Propagation Rules

1. **In the handler**: Call `fpi_ssm_mark_failed(ssm, error)` to abort.
2. **mark_failed** stores the error, then calls `fpi_ssm_mark_completed()` internally.
3. **In the completion callback**: `error` parameter is non-NULL on failure, NULL on success.
   The error is `(transfer full)` -- the callback owns it.
4. **Sub-SSM errors**: If a child SSM fails, the parent automatically fails with the same error.
   If the child succeeds, the parent automatically advances to the next state.
5. **Double failure**: Calling `mark_failed` twice (outside cleanup) logs a warning and ignores
   the second error.

### Sub-State Machine Internals

```c
void fpi_ssm_start_subsm(FpiSsm *parent, FpiSsm *child)
{
    child->parentsm = parent;
    fpi_ssm_start(child, __subsm_complete);   // internal callback
}

static void __subsm_complete(FpiSsm *ssm, FpDevice *_dev, GError *error)
{
    FpiSsm *parent = ssm->parentsm;
    if (error)
        fpi_ssm_mark_failed(parent, error);   // propagate failure
    else
        fpi_ssm_next_state(parent);           // advance parent
}
```

The child SSM is auto-freed. The parent SSM is "paused" during child execution --
neither the parent handler nor completion callback is called until the child finishes.

---

## 2. Pattern: The Async Callback Contract

Every state in the handler does ONE of:
1. Fires an async operation whose callback calls `fpi_ssm_next_state(ssm)` or `fpi_ssm_mark_failed(ssm, err)`
2. Starts a sub-SSM with `fpi_ssm_start_subsm(ssm, child)`
3. Calls `fpi_ssm_next_state(ssm)` directly (synchronous state)

**CRITICAL RULE**: Every state must eventually call exactly one of:
- `fpi_ssm_next_state(ssm)`
- `fpi_ssm_jump_to_state(ssm, N)`
- `fpi_ssm_mark_completed(ssm)`
- `fpi_ssm_mark_failed(ssm, error)`
- `fpi_ssm_start_subsm(ssm, child)` (which defers the decision to the child)

Failing to call any of these will **hang** the SSM forever.
Calling more than one will trigger a BUG_ON assertion.

---

## 3. Goodix 511 Activation SSM (Simple Linear)

Source: `libfprint/drivers/goodixtls/goodix511.c`

### States

```c
enum activate_states {
  ACTIVATE_READ_AND_NOP,                   // Start read loop + send NOP
  ACTIVATE_ENABLE_CHIP,                    // Enable chip
  ACTIVATE_NOP,                            // Another NOP
  ACTIVATE_CHECK_FW_VER,                   // Query firmware version, validate
  ACTIVATE_CHECK_PSK,                      // Read PSK, validate
  ACTIVATE_RESET,                          // Reset device, validate reset number
  ACTIVATE_SET_MCU_IDLE,                   // Switch MCU to idle mode
  ACTIVATE_READ_ODP,                       // Read OTP, then sub-SSM writes registers
  ACTIVATE_UPLOAD_MCU_CONFIG,              // Upload MCU configuration
  ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY,   // Set scan frequency
  ACTIVATE_NUM_STATES,                     // Sentinel
};
```

### Pattern Used

Each state sends a command and passes `ssm` as the user_data to the callback.
The callback either calls `fpi_ssm_next_state(ssm)` on success or
`fpi_ssm_mark_failed(ssm, error)` on failure.

### Sub-SSM Example (OTP Write)

In `ACTIVATE_READ_ODP`, the callback `read_otp_callback` creates a child SSM:

```c
static void read_otp_callback(FpDevice *dev, guint8 *data, guint16 len,
                               gpointer ssm, GError *err) {
    if (err) { fpi_ssm_mark_failed(ssm, err); return; }
    // ... store OTP data ...
    FpiSsm *otp_ssm = fpi_ssm_new(dev, otp_write_run, OTP_WRITE_NUM);
    fpi_ssm_start_subsm(ssm, otp_ssm);    // child writes 4 registers sequentially
}
```

The child SSM `otp_write_run` has 4 states, each writing one sensor register.
When all 4 complete, the parent automatically advances past ACTIVATE_READ_ODP.

### Completion Callback

```c
static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
    if (!error)
        goodixtls5xx_init_tls(dev);   // start TLS handshake
    else
        fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
}
```

---

## 4. Goodix 5xx Scan SSM (With Sub-SSM Calibration)

Source: `libfprint/drivers/goodixtls/goodix5xx.c`

### States

```c
enum SCAN_STAGES {
  SCAN_STAGE_QUERY_MCU,           // Query MCU state
  SCAN_STAGE_SWITCH_TO_FDT_MODE,  // Switch to FDT mode
  SCAN_STAGE_CALIBRATE,           // Sub-SSM: calibrate (3 states)
  SCAN_STAGE_SWITCH_TO_FDT_DOWN,  // Switch to finger-detect-down
  SCAN_STAGE_GET_IMG,             // Read & process image
  SCAN_STAGE_SWITCH_TO_FTD_UP,   // Switch to finger-detect-up
  SCAN_STAGE_SWITCH_TO_FTD_DONE, // Report finger-off, advance
  SCAN_STAGE_NUM,
};
```

### Calibration Sub-SSM

```c
enum CALIBRATION_STAGES {
  CALIBRATION_STAGE_FDT_UP,    // Switch to FDT up mode
  CALIBRATION_STAGE_NAV0,      // Send nav_0 command
  CALIBRATION_STAGE_GET_IMG,   // Read calibration image
  CALIBRATION_STAGE_NUM,
};

static void do_calibration(FpDevice *dev, FpiSsm *parent) {
    fpi_ssm_start_subsm(parent, fpi_ssm_new(dev, calibrate_run, CALIBRATION_STAGE_NUM));
}
```

When `SCAN_STAGE_CALIBRATE` is reached, it starts the calibration sub-SSM.
The parent scan SSM is paused until calibration completes.

---

## 5. Goodix 5e0a Activation SSM (Dual TLS Sessions)

Source: `libfprint/drivers/goodixtls/goodix5e0a.c`

This is the most complex pattern -- it establishes TWO separate TLS sessions
in a single linear SSM.

### States

```c
enum activate_5e0a_states {
  ACTIVATE_READ_AND_NOP1,    // Start USB read loop + NOP
  ACTIVATE_ENABLE_CHIP,      // Enable chip
  ACTIVATE_NOP2,             // Second NOP
  ACTIVATE_CHECK_PSK,        // Read PSK hash (informational only)
  ACTIVATE_CMD_TLS,          // ** TLS session 1: command channel (PSK = zeros) **
  ACTIVATE_POV_IMAGE_CHECK,  // POV image check command (over cmd TLS)
  ACTIVATE_IMG_TLS,          // ** TLS session 2: image channel (PSK = device-specific) **
  ACTIVATE_DONE,             // Terminal state, just advances
  ACTIVATE_5E0A_NUM_STATES,
};
```

### Dual TLS Flow

```
State 0-3: Standard initialization (NOP, enable chip, check PSK hash)
State 4 (ACTIVATE_CMD_TLS):
    goodix_tls_init(dev, on_cmd_tls_complete, ssm);
    -> Establishes command TLS with 32 zero-byte PSK
    -> on_cmd_tls_complete calls fpi_ssm_next_state(ssm)

State 5 (ACTIVATE_POV_IMAGE_CHECK):
    goodix_send_pov_image_check(dev, on_pov_image_check_done, ssm);
    -> Sent OVER the command TLS channel
    -> Failure is non-fatal (logs warning, continues)

State 6 (ACTIVATE_IMG_TLS):
    goodix_tls_init_image(dev, psk, psk_len, on_img_tls_complete, ssm);
    -> Establishes SECOND TLS session with device-specific PSK
    -> on_img_tls_complete calls fpi_ssm_next_state(ssm)

State 7 (ACTIVATE_DONE):
    fpi_ssm_next_state(ssm);
    -> Falls through to completion callback
```

### Completion

```c
static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
    fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
}
```

### Scan SSM (5e0a)

```c
enum scan_5e0a_states {
  SCAN_QUERY_MCU,      // Query MCU state
  SCAN_FDT_DOWN,       // Switch to finger-detect-down (wait for finger)
  SCAN_GET_IMAGE,      // Read image via image TLS channel
  SCAN_DONE,           // Report finger-off, advance
  SCAN_5E0A_NUM_STATES,
};
```

Image reading uses `goodix_tls_read_image_5e0a()` which decrypts via the
image TLS session (`priv->image_tls_hop`).

### Deactivation (Dual Shutdown)

```c
static void dev_deactivate(FpImageDevice *img_dev) {
    goodix_reset_state(dev);
    goodix_shutdown_tls(dev, &error);        // shutdown command TLS
    goodix_shutdown_image_tls(dev, &error);  // shutdown image TLS
    fpi_image_device_deactivate_complete(img_dev, error);
}
```

---

## 6. Common Callback Patterns

### Simple "check and advance" callback

```c
static void check_none(FpDevice *dev, gpointer user_data, GError *error) {
    if (error) {
        fpi_ssm_mark_failed(user_data, error);
        return;
    }
    fpi_ssm_next_state(user_data);
}
```

### Validation callback (check data, then advance or fail)

```c
static void check_firmware(FpDevice *dev, gchar *firmware,
                           gpointer user_data, GError *error) {
    if (error) { fpi_ssm_mark_failed(user_data, error); return; }
    if (strcmp(firmware, EXPECTED_VERSION)) {
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Bad firmware: %s", firmware);
        fpi_ssm_mark_failed(user_data, error);
        return;
    }
    fpi_ssm_next_state(user_data);
}
```

### Non-fatal callback (log warning, continue anyway)

```c
static void on_pov_image_check_done(FpDevice *dev, guint8 *data, guint16 len,
                                     gpointer ssm, GError *err) {
    if (err) {
        fp_warn("POV image check failed: %s -- continuing anyway", err->message);
        g_error_free(err);   // consume the error
    }
    fpi_ssm_next_state(ssm);  // always continue
}
```

---

## 7. Example: Complete 5e0a Activation + Scan Implementation

This is the exact pattern used in `goodix5e0a.c` for reference when writing
or modifying the driver.

### Activation SSM

```c
// ---- STATE ENUM ----
enum activate_5e0a_states {
  ACTIVATE_READ_AND_NOP1,     // 0: Start read loop + NOP
  ACTIVATE_ENABLE_CHIP,       // 1: Enable chip
  ACTIVATE_NOP2,              // 2: NOP
  ACTIVATE_CHECK_PSK,         // 3: Read PSK hash
  ACTIVATE_CMD_TLS,           // 4: TLS session 1 (cmd channel, PSK=zeros)
  ACTIVATE_POV_IMAGE_CHECK,   // 5: POV image check over cmd TLS
  ACTIVATE_IMG_TLS,           // 6: TLS session 2 (image channel, PSK=custom)
  ACTIVATE_DONE,              // 7: Terminal state
  ACTIVATE_5E0A_NUM_STATES,
};

// ---- CALLBACKS ----

static void check_none_5e0a(FpDevice *dev, gpointer ssm, GError *error)
{
    if (error) { fpi_ssm_mark_failed(ssm, error); return; }
    fpi_ssm_next_state(ssm);
}

static void on_cmd_tls_complete(FpDevice *dev, gpointer ssm, GError *error)
{
    if (error) { fpi_ssm_mark_failed(ssm, error); return; }
    fp_dbg("Command TLS established");
    fpi_ssm_next_state(ssm);
}

static void on_pov_image_check_done(FpDevice *dev, guint8 *data, guint16 len,
                                     gpointer ssm, GError *err)
{
    if (err) {
        fp_warn("POV image check failed: %s -- continuing", err->message);
        g_error_free(err);
    }
    fpi_ssm_next_state(ssm);
}

static void on_img_tls_complete(FpDevice *dev, gpointer ssm, GError *error)
{
    if (error) { fpi_ssm_mark_failed(ssm, error); return; }
    fp_dbg("Image TLS established");
    fpi_ssm_next_state(ssm);
}

// ---- STATE HANDLER ----

static void activate_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpiDeviceGoodixTls5e0a *self = FPI_DEVICE_GOODIXTLS5E0A(dev);

    switch (fpi_ssm_get_cur_state(ssm)) {
    case ACTIVATE_READ_AND_NOP1:
        goodix_start_read_loop(dev);
        goodix_send_nop(dev, check_none_5e0a, ssm);
        break;

    case ACTIVATE_ENABLE_CHIP:
        goodix_send_enable_chip(dev, TRUE, check_none_5e0a, ssm);
        break;

    case ACTIVATE_NOP2:
        goodix_send_nop(dev, check_none_5e0a, ssm);
        break;

    case ACTIVATE_CHECK_PSK:
        goodix_send_preset_psk_read(dev, GOODIX_5E0A_PSK_FLAGS, 0,
                                    on_psk_read_5e0a, ssm);
        break;

    case ACTIVATE_CMD_TLS:
        // Command TLS: PSK = 32 zero bytes (default)
        goodix_tls_init(dev, on_cmd_tls_complete, ssm);
        break;

    case ACTIVATE_POV_IMAGE_CHECK:
        // Sent over the now-established command TLS channel
        goodix_send_pov_image_check(dev, on_pov_image_check_done, ssm);
        break;

    case ACTIVATE_IMG_TLS:
        {
            // Image TLS: device-specific PSK
            const guint8 *psk = self->image_psk;
            guint psk_len = 32;
            goodix_tls_init_image(dev, psk, psk_len, on_img_tls_complete, ssm);
        }
        break;

    case ACTIVATE_DONE:
        // Nothing to do, just advance to trigger completion
        fpi_ssm_next_state(ssm);
        break;
    }
}

// ---- COMPLETION ----

static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error)
{
    if (error)
        fp_err("5e0a activation failed: %s", error->message);
    fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
}

// ---- ENTRY POINT ----

static void dev_activate(FpImageDevice *img_dev)
{
    FpDevice *dev = FP_DEVICE(img_dev);
    fpi_ssm_start(
        fpi_ssm_new(dev, activate_run_state, ACTIVATE_5E0A_NUM_STATES),
        activate_complete
    );
}
```

### Scan SSM

```c
enum scan_5e0a_states {
  SCAN_QUERY_MCU,
  SCAN_FDT_DOWN,
  SCAN_GET_IMAGE,
  SCAN_DONE,
  SCAN_5E0A_NUM_STATES,
};

static void scan_run_state(FpiSsm *ssm, FpDevice *dev)
{
    FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev);

    switch (fpi_ssm_get_cur_state(ssm)) {
    case SCAN_QUERY_MCU:
        goodix_send_query_mcu_state(dev, check_none_cmd_5e0a, ssm);
        break;

    case SCAN_FDT_DOWN:
        goodix_send_mcu_switch_to_fdt_down(dev,
            goodix_5e0a_fdt_down_mode, sizeof(goodix_5e0a_fdt_down_mode),
            NULL, check_none_cmd_5e0a, ssm);
        break;

    case SCAN_GET_IMAGE:
        fpi_image_device_report_finger_status(img_dev, TRUE);
        goodix_tls_read_image_5e0a(dev, scan_on_read_img_5e0a, ssm);
        break;

    case SCAN_DONE:
        fpi_image_device_report_finger_status(img_dev, FALSE);
        fpi_ssm_next_state(ssm);
        break;
    }
}

static void scan_complete(FpiSsm *ssm, FpDevice *dev, GError *error)
{
    if (error) {
        fpi_image_device_session_error(FP_IMAGE_DEVICE(dev), error);
        return;
    }
    fp_dbg("5e0a scan finished");
}

// Triggered when libfprint enters AWAIT_FINGER_ON state:
static void dev_change_state(FpImageDevice *img_dev, FpiImageDeviceState state)
{
    if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
        fpi_ssm_start(
            fpi_ssm_new(FP_DEVICE(img_dev), scan_run_state, SCAN_5E0A_NUM_STATES),
            scan_complete
        );
}
```

---

## 8. Checklist for Writing a New SSM

1. Define an `enum` with state names. Last entry is the `_NUM_STATES` sentinel.
2. Write a handler function with a `switch(fpi_ssm_get_cur_state(ssm))`.
3. For each state: fire ONE async operation, passing `ssm` as user_data.
4. Each async callback must call exactly one of: `fpi_ssm_next_state`,
   `fpi_ssm_mark_failed`, `fpi_ssm_mark_completed`, or `fpi_ssm_start_subsm`.
5. Write a completion callback that handles both success (error==NULL) and
   failure (error!=NULL).
6. Start with `fpi_ssm_start(fpi_ssm_new(dev, handler, SENTINEL), completion_cb)`.
7. The SSM is auto-freed after the completion callback runs. Never free it manually.
8. For nested operations, use `fpi_ssm_start_subsm()` -- the parent is automatically
   advanced on child success, or failed on child failure.
