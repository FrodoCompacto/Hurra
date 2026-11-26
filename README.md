# Hurra for Waveshare RP2350-USB-C

This fork adapts the PIOKMbox firmware for Simulacrum's USB proxy. A single Waveshare board reads a physical mouse and keyboard through its PIO-USB host port and presents their HID interfaces to the target PC through its native USB port. A CH340N USB-to-UART adapter gives the control PC a separate connection for commands and diagnostics.

**Passthrough status.** On the test bench, the Waveshare recognizes both HID interfaces of the Logitech MK235 receiver (`046D:C534`) through a USB hub. The physical mouse moved the cursor for about a second after reboot, then stopped. Serial mouse and keyboard commands continued to work. Hot-plugging the receiver is also unreliable. To reproduce the startup failure, leave the receiver and hub connected throughout the test.

## Bench wiring

| Connection | Where to connect it |
| --- | --- |
| Target PC | Use the Waveshare's native USB-C port next to the BOOT and RUN buttons. This is also the port used to flash UF2 files. |
| Mouse and keyboard | Plug the USB-A receiver into the hub. Connect the hub to the Waveshare's second USB-C port, the PIO-USB host (D+ on GP12, D− on GP13). |
| Control PC | Use a CH340N adapter with 3.3 V UART logic. Connect adapter TX to GP1 (RX), adapter RX to GP0 (TX), and GND to GND. |

UART0 runs at **115200 baud, 8N1**. On the Windows test bench the adapter appears as `COM3`. The `waveshare_rp2350_usb_c` target sets the system clock to 120 MHz and the core voltage to 1.10 V. [Target configuration](CMakeLists.txt) · [Board initialization](PIOKMbox.c)

## Build and flash

To use a CI build, open the run for the commit you want. Download the artifact whose name starts with `PIOKMbox-WaveshareRP2350USBC` and extract `PIOKMbox.uf2`. [Build CI runs](https://github.com/FrodoCompacto/Hurra/actions/workflows/build-ci.yml)

To build locally, use Bash with Pico SDK 2.2.0, an Arm toolchain, and CMake. Set `PICO_SDK_PATH` to the SDK directory and run the following commands from the root of this fork. Specify the **waveshare** target explicitly because the script defaults to `flash-metros`. [Build script](build.sh)

```bash
git submodule update --init --recursive
bash build.sh waveshare
```

The build produces `build-waveshare/PIOKMbox.uf2`. To enter BOOTSEL, send `KMBOX_BOOTSEL` over UART or hold BOOT, press and release RUN, then release BOOT. Copy the UF2 file to the **RP2350** drive on the PC connected to the native USB port. The board restarts when flashing finishes.

## Test physical passthrough on Windows

From the root of the **Simulacrum** checkout, run `.\proxy\waveshare-console.ps1`. The console defaults to COM3 and lets you select another port. You can also use a serial terminal and send each command followed by a newline.

| Command | What it does |
| --- | --- |
| `KMBOX_PING` | Checks that the board responds through the CH340N. |
| `KMBOX_REBOOT` | Restarts the firmware. Leave the hub and receiver attached for the startup test. |
| `KMBOX_INFO` | Shows the receiver's VID/PID, detected interfaces, and USB and HID receive counters. |
| `KMBOX_TRACE` | Reads the current boot's trace from RAM. You can read it again until the next reset. |
| `KMBOX_BOOTSEL` | Restarts the board in UF2 flashing mode. |

1. With the receiver in the hub and the hub connected to the Waveshare, reboot the board and move the **physical mouse** during the first few seconds. Console option 3 generates input over UART and exercises the output path to the PC.
2. If the cursor stops, select **4** (`KMBOX_INFO`). For the MK235, look for `046D:C534` and `mouse=1,kbd=1`. The `m` field in `KMBOX_RX` counts physical mouse reports received.
3. Before resetting the board, select **5** (`KMBOX_TRACE`) and save the output. Record the firmware commit, the hub model, and whether the cursor moved before the failure.

### Read the trace

A `live` trace is still recording. It becomes `frozen` when an interrupt IN endpoint on a mounted HID device delivers data and later fails three consecutive polls. A data packet or a normal `NAK` breaks the streak. Timestamps are milliseconds since boot. [Trace implementation](usb_trace.c)

In one capture from firmware `d6fba79`, the MK235 interfaces mounted between 1962 and 1993 ms. From 3112 to 3117 ms the host recorded six `no_start` events, alternating between receiver endpoints `0x82` and `0x81`. The Waveshare stopped detecting the start of a response. The cause is still under investigation. Other trace results include `no_eop` for a timeout before the end of a packet and `crc` for a data packet that failed its CRC check.

Starting with `8aab855`, compare the first recorded error with the PC-side USB and UART DMA timestamps. The firmware marks its requests to disconnect and reconnect as `pc_disconnect` and `pc_connect`. It records `pc_mount` when the PC configures the device. The UART marks are `uart_dma_init` (initialization entered), `uart_dma_rx` (RX running), and `uart_dma_tx_ready` (TX ready). For each mark, `before_ms` is the latest occurrence while the trace is live, or the last one before it froze. `after_ms` is the first occurrence after it froze. Zero means the event has yet to be observed in that interval.

### Capture a host-side USB failure

Keep the receiver in the hub and the hub connected to the board. Once the fault appears,
run this from the root of the Hurra checkout. Close programs using the same COM port first.

```powershell
.\tools\capture_usb_bus.ps1 -Port COM3
```

The collector saves a timestamped text file in the current directory. It reads the boot trace
and takes two samples of the endpoint counters at least 1.2 seconds apart. It issues only UART
queries. The firmware observes transfers already scheduled by the USB stack, including normal
NAKs. Capture files omit raw HID payloads.

The `KMBOX_BUS` header includes the source revision (`build`), frame counter, Core1 heartbeat
counter, and available page numbers. Across samples, a progressing frame counter shows that
the host frame callback is running. The heartbeat gives a separate indication of Core1's
foreground progress; interpret it over more than its configured heartbeat interval. `KMBOX_BUS N` reads one page with an explicit
end marker. Each interrupt IN endpoint has live counters and its own first persistent failure
(three consecutive errors, even before the first data report). A successful packet or NAK
resets that endpoint's streak. The fault page preserves the last good poll, first bad poll,
and third failed poll until reset, even if its slot is reused. Live pages are read sequentially.

| Evidence in the two samples | What to investigate |
| --- | --- |
| Frame counter advances; hub NAK or data counts advance; both receiver endpoints accumulate errors | The host can still exchange packets with the hub. Inspect the receiver's downstream port. If present, `hub` is the last observed successful hub port status reply; check its port and age before interpreting it. |
| Frames advance but both hub and receiver stop returning even NAKs | Compare root-bus signalling, TX pin ownership, and the PIO receiver across good and bad samples. |
| Frames stop advancing while UART still responds | Inspect the host frame timer and Core1 execution before adjusting the HID parser. |
| Receiver data counts advance but the mouse/keyboard counters in `KMBOX_INFO` stay flat | Follow the report from its interrupt endpoint into the HID callback and interface parser. |

`sof` reports whether local frame-token transmission completed; `gap_us` measures the interval
between host frame callbacks. Each sample includes poll duration, speed and PRE settings,
D+/D? levels, TX output-enable bits (bit 0 for D+, bit 1 for D?), RX IRQ flags, FIFO depth,
and the relative program counters of the TX, RX and edge-detector state machines. `rx_on`
and `edge_on` are the respective enable bits. The PID buffer may retain an old value after a
timeout. Sample times in microseconds wrap about every 72 minutes; compare within the same boot.
`setup` identifies the latest acknowledged SETUP before the error streak. A status reply and
a SETUP can belong to different transfers; their addresses and timestamps are printed separately.

These are **digital snapshots taken after each transaction**. If the next capture still leaves
a silent receiver and a missed start indistinguishable, capture USB traffic between the board
and the hub with a USB analyzer. Check whether an IN token addressed to the failing endpoint
is followed on the wire by DATA, NAK, or silence. That measurement can use the same firmware.

## Other targets and dependencies

The fork retains support code for other boards, a second board acting as a bridge, and a display. The Simulacrum setup uses one Waveshare and the CH340N. For the other configurations, see the [bridge guide](bridge/README.md) and the [earlier fork README](https://github.com/FrodoCompacto/Hurra/blob/8aab855/README.md).

The firmware uses the Pico SDK, TinyUSB, and Pico-PIO-USB. See each dependency for its license. [Submodules](.gitmodules) · [Pico-PIO-USB](lib/Pico-PIO-USB)
