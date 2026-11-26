# Hurra for Waveshare RP2350-USB-C

This fork adapts the PIOKMbox firmware for Simulacrum's USB proxy. A single Waveshare board reads a physical mouse and keyboard through its PIO-USB host port and presents their HID interfaces to the target PC through its native USB port. A CH340N USB-to-UART adapter gives the control PC a separate connection for commands and diagnostics.

The tested setup uses a Logitech MK235 receiver (`046D:C534`) plugged directly into the Waveshare through a USB-A to USB-C adapter. Both the physical keyboard and mouse worked, as did movement and typing commands sent over UART. With the hub used on the bench, physical input stopped shortly after boot.

## Bench wiring

| Connection | Where to connect it |
| --- | --- |
| Target PC | Use the Waveshare's native USB-C port next to the BOOT and RUN buttons. This is also the port used to flash UF2 files. |
| Mouse and keyboard | Connect the USB-A receiver through a USB-C adapter directly to the Waveshare's second USB-C port, the PIO-USB host (D+ on GP12, D− on GP13). |
| Control PC | Use a CH340N adapter with 3.3 V UART logic. Connect adapter TX to GP1 (RX), adapter RX to GP0 (TX), and GND to GND. |

UART0 runs at **115200 baud, 8N1**. On the Windows test bench the adapter appears as `COM3`. The `waveshare_rp2350_usb_c` target sets the system clock to 120 MHz and the core voltage to 1.10 V. [Target configuration](CMakeLists.txt) · [Board initialization](PIOKMbox.c)

## Build and flash

To use a CI build, open the run for the commit you want. Download the artifact whose name starts with `PIOKMbox-WaveshareRP2350USBC` and extract `PIOKMbox.uf2`. [Build CI runs](https://github.com/FrodoCompacto/Hurra/actions/workflows/build-ci.yml)

To build locally, use Bash with Pico SDK 2.2.0, an Arm toolchain, and CMake. Set `PICO_SDK_PATH` to the SDK directory and run the following commands from the root of this fork. Specify the **waveshare** target explicitly because the script defaults to `flash-metros`. [Build script](build.sh)

```bash
git submodule update --init --recursive
bash build.sh waveshare
```

The Waveshare target uses the production firmware profile by default. The build produces `build-waveshare/PIOKMbox.uf2`. To enter BOOTSEL, send `KMBOX_BOOTSEL` over UART or hold BOOT, press and release RUN, then release BOOT. Copy the UF2 file to the **RP2350** drive on the PC connected to the native USB port. The board restarts when flashing finishes.

## Test physical passthrough on Windows

From the root of the **Simulacrum** checkout, run `.\proxy\waveshare-console.ps1`. The console defaults to COM3 and lets you select another port. You can also use a serial terminal and send each command followed by a newline.

| Command | What it does |
| --- | --- |
| `KMBOX_PING` | Checks that the board responds through the CH340N. |
| `KMBOX_REBOOT` | Restarts the firmware. Leave the receiver attached during reboot. |
| `KMBOX_INFO` | Shows the receiver's VID/PID, detected interfaces, and USB and HID receive counters. |
| `KMBOX_BOOTSEL` | Restarts the board in UF2 flashing mode. |

1. Attach the MK235 through the adapter before boot. Select **4** (`KMBOX_INFO`) and check for VID `046D`, PID `C534`, and `ready=1,mouse=1,kbd=1,itfs=2`.
2. Move the physical mouse and type in a text field on the target PC. Run **4** again. The `m` and `k` counters in `KMBOX_RX` should increase. The direct-connection bench test recorded `err=0` in `KMBOX_PIO` and `drp=0` in `KMBOX_RX`.
3. To check the UART path, select **3** and focus a text field on the target PC during the three-second countdown. The cursor should move right and the letter `a` should appear.

## Other targets and dependencies

The fork retains support code for other boards, a second board acting as a bridge, and a display. The Simulacrum setup uses one Waveshare and the CH340N. For the other configurations, see the [bridge guide](bridge/README.md) and the [earlier fork README](https://github.com/FrodoCompacto/Hurra/blob/8aab855/README.md).

The firmware uses the Pico SDK, TinyUSB, and Pico-PIO-USB. See each dependency for its license. [Submodules](.gitmodules) · [Pico-PIO-USB](lib/Pico-PIO-USB)
