# Hurra for Waveshare RP2350-USB-C

This fork adapts the PIOKMbox firmware for the **Waveshare RP2350-USB-C** dual-USB board. A single Waveshare board reads a physical mouse and keyboard through its secondary PIO-USB host port (`GP12`/`GP13`) and presents their HID interfaces to the target computer through its primary native USB-C port. A 3.3 V USB-to-UART adapter (`CH340N` on `GP0`/`GP1` at `115200` baud) provides an independent serial channel for movement, click, keyboard, and diagnostic commands.

The validated hardware setup uses a **Logitech MK235** wireless receiver (`046D:C534`, pairing a **K235** keyboard and an **M170** mouse) plugged directly into the Waveshare's secondary USB-C port through a passive USB-A to USB-C adapter.

---

## Key Features

- **Safe RP2350 Hardware Profile:** Runs at **`120 MHz`** (an exact multiple of 12 MHz for PIO-USB timing, well within the 150 MHz board specification) at the nominal **`1.10 V` (`VREG_VOLTAGE_1_10`)** core voltage, with `stdio_uart` and `stdio_usb` disabled so console output never collides with the serial protocol.
- **Multi-Interface USB HID Passthrough:** Clones device and interface descriptors from the attached receiver (`MI_00` standard 6KRO keyboard and `MI_01` 12-bit mouse + Consumer/System Control), forwarding physical mouse movements, clicks, scroll wheel, and keyboard typing with 1:1 transparency.
- **125 Hz (`8 ms`) Ergonomic Trajectory Smoothing (`HUMANIZATION_FULL` default on boot):**
  - Matches the native **`125 Hz` (`8000 µs`)** report cadence of standard wireless office receivers (`046D:C534`) while keeping an immediate fast-path (`~5 µs`) for physical input events.
  - **Zero-Drift Position Conservation:** Subdivides standalone movements (`>= 24 px`) with a subtle biomechanical wrist arc (`9%..15%` lateral deflection) and compensates any sub-pixel output oscillation (`g_tremor_debt_x/y`) before movement completion so net `(dx, dy)` displacement is conserved 100%.
  - **Minimum-Jerk Bell Velocity Profile:** Uses the cumulative minimum-jerk polynomial ($10u^3 - 15u^4 + 6u^5$) with a `3.5x` peak saturation ceiling (`49 px/frame` at the default `max_per_frame = 14`), preserving natural acceleration and deceleration curves without clipping peak velocity.
  - **Automatic Routing:** All serial movement commands (`M<x>,<y>`, `0x01`, `0x07`, `0x0A`) automatically route through the smoothing pipeline when smoothing is enabled (`hmode != 0`).
- **Hardware TRNG Click & Key Hold Timing:** Uses the RP2350 hardware TRNG (`get_rand_32()`) with a 3-sample Irwin-Hall bell distribution for mouse click dwell times (`108..165 ms`, mean `~136.5 ms`) and keyboard tap durations (`45..99 ms`, mean `~72 ms`), with a spinlock-protected 32-entry keyboard report queue (`g_kbd_queue`) that guarantees every key release is delivered.

---

## Bench Wiring

| Connection | Port / Pins |
| --- | --- |
| **Target Computer** | Waveshare primary native USB-C port (next to the `BOOT` and `RUN` buttons). Also used for UF2 flashing. |
| **Mouse & Keyboard Receiver** | USB-A receiver plugged via a USB-A female to USB-C male adapter directly into the Waveshare secondary USB-C port (`PIO-USB` host: `D+` on `GP12`, `D-` on `GP13`). |
| **Control Computer (UART0)** | 3.3 V CH340N USB-to-UART adapter: CH340N `TXD` → Waveshare `GP1` (`UART0 RX`), CH340N `RXD` → Waveshare `GP0` (`UART0 TX`), and `GND` → `GND` (`115200` baud, `8N1`). |

---

## UART Protocol Reference (`115200 8N1`)

### Text Commands (`\r\n` or `\n` terminated)

| Command | Description |
| --- | --- |
| `P` or `KMBOX_PING` | Connectivity check (`KMBOX_PONG`). |
| `KMBOX_INFO` | Prints cloned `VID:PID`, manufacturer/product strings, HID mount status, smoothing configuration (`hmode=2,max=14`), and `KMBOX_PIO` / `KMBOX_ITF` / `KMBOX_RX` telemetry counters in a single DMA transfer. |
| `M<dx>,<dy>` | Moves the mouse by `(dx, dy)` counts through the active smoothing pipeline (e.g. `M80,0`). |
| `W<delta>` | Scrolls the vertical wheel (`int8`, positive = up, negative = down, e.g. `W-3`). |
| `KP<keycode>[,<mod>[,<hold_ms>]]` | Taps a USB HID keycode (`0..255`) with optional modifier bitmask (`1`=LCtrl, `2`=LShift, `4`=LAlt, `8`=LGUI). When `hold_ms` is omitted or `0`, holds for a bell-distributed `45..99 ms` (mean `~72 ms`) and automatically releases (e.g. `KP4` types `'a'`, `KP43,1` presses `Ctrl+Tab`). |
| `KD<keycode>[,<mod>]` | Presses and holds a key and/or modifier mask. |
| `KU<keycode>[,<mod>]` | Releases a specific key and/or modifier mask. |
| `KR` or `KRA` | Releases all synthetic keys and modifiers immediately. |
| `REBOOT` or `KMBOX_REBOOT` | Cleanly releases all HID states, disconnects USB, and reboots the board via watchdog. |
| `KMBOX_BOOTSEL` | Cleanly disconnects USB, resets Core 1, and enters the RP2350 UF2 mass-storage bootloader without pressing physical buttons. |

### 8-Byte Binary Fast Commands

| Opcode | Layout (8 bytes) | Description |
| --- | --- | --- |
| `0x01` (`FAST_CMD_MOUSE_MOVE`) | `[0x01, dx_lo, dx_hi, dy_lo, dy_hi, buttons, wheel, pan]` | Moves mouse (routed through `INJECT_MODE_SMOOTH` when smoothing is active) and updates buttons/wheel. |
| `0x02` (`FAST_CMD_MOUSE_CLICK`) | `[0x02, button, count, 0, 0, 0, 0, 0]` | Executes a timed button click (`1`=Left, `2`=Right, `3`=Middle) with TRNG bell-distributed hold (`108..165 ms`, mean `~136.5 ms`) and automatic release. |
| `0x07` (`FAST_CMD_SMOOTH_MOVE`) | `[0x07, dx_lo, dx_hi, dy_lo, dy_hi, mode, 0, 0]` | Queues a smoothed movement (`int16` little-endian). Use `mode = 1` (`INJECT_MODE_SMOOTH`) for standalone moves with automatic subdivision and wrist arc, or `mode = 3` (`INJECT_MODE_MICRO`) when streaming pre-computed 125 Hz curve steps from the host. |
| `0x08` (`FAST_CMD_SMOOTH_CONFIG`) | `[0x08, max_per_frame, vel_match, 0, 0, 0, 0, 0]` | Optional runtime override for target average counts per 8 ms frame (defaults to `14` on boot in `HUMANIZATION_FULL`, with `3.5x` peak saturation ceiling). |
| `0x09` (`FAST_CMD_SMOOTH_CLEAR`) | `[0x09, 0, 0, 0, 0, 0, 0, 0]` | Clears pending movement queue and accumulators. |
| `0x0C` (`FAST_CMD_KEY_COMBO`) | `[0x0C, modifiers, keycode, k1, k2, k3, k4, flags]` | Keyboard shortcut/tap (`flags = 0`: timed tap `45..99 ms` with guaranteed release; `flags = 1`: press & hold; `flags = 2`: release all). |
| `0x0F` (`FAST_CMD_CYCLE_HUMAN`) | `[0x0F, 0, 0, 0, 0, 0, 0, 0]` | Cycles smoothing mode (`0 = OFF` → `1 = MICRO` → `2 = FULL`). |

---

## Build and Flash

To use a GitHub Actions CI build, open the latest workflow run, download the `PIOKMbox-WaveshareRP2350USBC-<sha>` artifact, and extract `PIOKMbox.uf2`. [Build CI runs](https://github.com/FrodoCompacto/Hurra/actions/workflows/build-ci.yml)

To build locally with Pico SDK 2.2.0, Arm GNU Toolchain 14.2.Rel1, and CMake:

```bash
git submodule update --init --recursive
bash build.sh waveshare
```

The build outputs `build-waveshare/PIOKMbox.uf2`. Send `KMBOX_BOOTSEL` over UART (`COM3`) or hold `BOOT` while pressing `RUN`, then copy `PIOKMbox.uf2` to the `RP2350` USB drive.

---

## Dependencies and Credits

This firmware builds upon the Raspberry Pi Pico SDK, TinyUSB, and Pico-PIO-USB. See each submodule and header for license terms. [Submodules](.gitmodules) · [Pico-PIO-USB](lib/Pico-PIO-USB) · [Trajectory Smoothing Architecture](HUMANIZATION.md)
