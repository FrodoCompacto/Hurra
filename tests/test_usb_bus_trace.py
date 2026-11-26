"""Run the bus recorder against a deterministic clock, with no board attached."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TEST = r"""
#include "usb_trace.h"
#include <assert.h>
#include <string.h>
unsigned clock_ms = 1;
static char row[1024], frozen[1024];

static void poll(unsigned slot, usb_trace_kind_t result) {
    usb_bus_sample_t s = {.result=result, .us=UINT32_MAX, .frame=UINT32_MAX,
        .frame_gap_us=UINT32_MAX, .elapsed_us=UINT32_MAX, .edge_pc=31, .rx_pc=30,
        .pid_byte=0x5a, .rx_enabled=1, .edge_enabled=1, .tx_pc=31};
    usb_trace_in(slot, &s);
}
int main(void) {
    usb_trace_init();
    usb_trace_bus_index(row, sizeof row,123,7,"test");
    assert(strstr(row, "frame=123 core1_beats=7 pages=\r\n"));
    usb_trace_open(2, 5, 0x81);
    uint8_t request[8]={0xa3,0,0,0,3,0,4,0}, port[4]={3,1,1,0};
    usb_trace_setup(5,request);
    usb_trace_control_in(5,port,4);
    usb_trace_open(3, 1, 0x82);
    poll(2,TRACE_NAK); poll(3,TRACE_NO_START); poll(3,TRACE_CRC);
    poll(3,TRACE_NAK);
    poll(3,TRACE_NO_START); poll(3,TRACE_NO_EOP);
    usb_trace_bus_index(row,sizeof row,124,7,"test");
    assert(strstr(row,"pages= 4 6\r\n"));
    // Even a receiver that never delivers its first data packet gets a fault page.
    poll(3,TRACE_NO_START);
    usb_trace_bus_index(row,sizeof row,125,7,"test");
    assert(strstr(row,"pages= 4 6 7\r\n"));
    usb_trace_bus_page(7,frozen,sizeof frozen);
    assert(strstr(frozen,"fault slot=3 dev=1 ep=82"));
    assert(strstr(frozen,"good nak")); assert(strstr(frozen,"first_bad no_start"));
    assert(strstr(frozen,"hub ms=1 dev=5 port=3 status=0103 change=0001"));
    assert(strstr(frozen,"last no_start")); assert(strstr(frozen,"KMBOX_BUS_END\r\n"));
    usb_trace_close(3); usb_trace_bus_page(6,row,sizeof row); assert(strstr(row,"open=0"));
    usb_trace_open(3,2,0x81); poll(3,TRACE_DATA); poll(3,TRACE_STALL);
    usb_trace_bus_page(6,row,sizeof row); assert(strstr(row,"dev=2 ep=81 open=1 data=1"));
    usb_trace_bus_page(7,row,sizeof row); assert(!strcmp(row,frozen));
    usb_trace_bus_page(4,row,sizeof row); assert(strstr(row,"data=0 nak=1 err=0"));
    usb_trace_bus_page(99,row,sizeof row); assert(strstr(row,"invalid page"));
    return 0;
}
"""

with tempfile.TemporaryDirectory() as temp:
    d = Path(temp)
    (d / "pico").mkdir()
    (d / "pico/time.h").write_text("extern unsigned clock_ms;\n#define get_absolute_time() clock_ms\n#define to_ms_since_boot(t) (t)\n")
    (d / "pico/critical_section.h").write_text("typedef int critical_section_t;\n" + "".join(
        f"#define critical_section_{name}(p) ((void)(p))\n" for name in ("init", "enter_blocking", "exit")))
    (d / "tusb.h").write_text("#define CFG_TUH_HID 4\n#define CFG_TUH_DEVICE_MAX 4\n")
    src = d / "test.c"
    src.write_text(TEST)
    exe = d / ("test.exe" if os.name == "nt" else "test")
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + temp, "-I.", "-Ilib/Pico-PIO-USB/src", "usb_trace.c", str(src),
                    "-o", str(exe)], cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)
    print("PASS: independent endpoints, no-data failures, hub status, paging and slot reuse")
