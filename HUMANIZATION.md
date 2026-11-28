# Trajectory Smoothing & Ergonomic Timing Architecture

## Overview

The trajectory smoothing subsystem (`smooth_injection.c`, `humanization_fpu.c`, `usb_hid.c`, and `lib/kmbox-commands/kmbox_commands.c`) interpolates synthetic mouse and keyboard commands sent over UART so they blend seamlessly with physical USB HID peripherals attached to the PIO-USB host port.

By default, the firmware boots into **`HUMANIZATION_FULL` (`hmode = 2`)** calibrated for **`125 Hz` (`8 ms`)** wireless office receivers (such as the Logitech MK235 / M170 `046D:C534`). No runtime configuration commands are required after power-on or reboot.

---

## Core Design Principles

### 1. 1:1 Transparent Physical Passthrough
- Reports arriving from the physical mouse and keyboard on Core 1 (`PIO-USB`) are forwarded immediately (`force_immediate`) without adding any synthetic noise or delay.
- Output-stage interpolation (`apply_output_humanization`) is gated strictly to frames containing active synthetic movement (`has_smooth || smooth_x != 0 || smooth_y != 0`).

### 2. 125 Hz (`8000 µs`) Frame Cadence Matching
- Standard wireless office receivers report movement every `~8 ms` (`125 Hz`).
- When synthetic movement is active, `hid_device_task()` paces reports at `8000 µs` with subtle Gaussian timing variation (`±350 µs`), matching the natural interval distribution of the attached physical receiver.

### 3. Zero-Drift Position Conservation
- **No Open-Loop Multiplicative Error:** `delivery_error_fp` is set to `0` so total queued displacement is never altered.
- **Zero-Drift Biomechanical Wrist Arc:** For standalone movements `>= 24 px` (`INJECT_MODE_SMOOTH`), `smooth_inject_movement_fp()` deflects the initial sub-step perpendicular to the chord by `9%..15%` of the movement magnitude. Because the final sub-step drains the exact fixed-point remainder (`x_remaining`, `y_remaining`), the lateral deflection cancels out to `0.0 px` at the target while producing a smooth curved arc.
- **Zero-Drift Output Micro-Oscillation:** Any perpendicular sub-pixel count added by `apply_output_humanization()` during movement is tracked in `g_tremor_debt_x` and `g_tremor_debt_y` and paid back during the final deceleration frames.

### 4. Minimum-Jerk Bell Velocity Profile & `3.5x` Peak Saturation Ceiling
- Each queued sub-step advances along the cumulative minimum-jerk polynomial:
  $$s(u) = 10u^3 - 15u^4 + 6u^5, \qquad \Delta s_k = s(k/N) - s((k-1)/N)$$
- In `smooth_process_frame()`, `g_smooth.max_per_frame` (`14` counts per 8 ms frame on boot) defines the reference *average* speed for frame-count calculation, while the soft saturator (`soft_saturate_fp`) uses a peak ceiling of **`3.5 * max_per_frame` (`49` counts/frame)** so the bell-curve velocity peak ($V_{\max}/V_{\text{avg}} \approx 3.0\times$) is never clipped.

### 5. Hardware TRNG Bell-Distributed Click & Key Hold Timing
- Seeded from the RP2350 hardware TRNG (`get_rand_32()`), both mouse clicks and keyboard taps sum 3 independent uniform random variables (Irwin-Hall central-limit approximation) to produce natural unimodal bell distributions:
  - **Mouse click hold (`0x02` / `km.click`):** `108..165 ms` (mean `~136.5 ms`, stddev `~10 ms`), followed by automatic button release.
  - **Keyboard tap hold (`KP` / `0x0C`):** `45..99 ms` (mean `~72 ms`), followed by automatic release via the spinlock-protected `g_kbd_queue[32]`.

---

## Operating Modes

| Mode | `hmode` | Default `max_per_frame` | Behavior |
| --- | --- | --- | --- |
| `HUMANIZATION_OFF` | `0` | `32767` | Direct digital passthrough without subdivision, velocity filtering, or output oscillation. |
| `HUMANIZATION_MICRO` | `1` | `32767` | Sub-pixel output smoothing without queue subdivision. |
| `HUMANIZATION_FULL` *(Boot Default)* | `2` | `14` (peak ceiling `49`) | Full 125 Hz smoothing: Zero-Drift wrist arc subdivision, Minimum-Jerk bell velocity curve, adaptive IIR velocity filter, and Zero-Drift output micro-oscillation. |

### Injection Modes (`0x07` `FAST_CMD_SMOOTH_MOVE` byte 5)
- **`INJECT_MODE_SMOOTH` (`1`):** Default for standalone movement commands (also used automatically by text `M<x>,<y>` and binary `0x01` when `hmode != 0`). Subdivides and arcs the movement across multiple 8 ms frames.
- **`INJECT_MODE_MICRO` (`3`):** Direct sub-pixel accumulation for host applications that stream pre-computed 125 Hz (`8 ms`) trajectory steps (such as two-phase submovement generators).
