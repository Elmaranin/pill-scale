// Console over USB Serial/JTAG.
//
// By default ESP-IDF writes to this port without a driver and cannot read
// without blocking. So we install the driver and switch stdout to it: then
// printf and console_read() work through the same buffers.

#include "board/usb_console.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

void console_begin() {
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&cfg);
    usb_serial_jtag_vfs_use_driver();
}

bool console_connected() { return usb_serial_jtag_is_connected(); }

size_t console_read(char *buf, size_t max) {
    int n = usb_serial_jtag_read_bytes(buf, max, 0);  // 0 ticks — do not wait

    return n > 0 ? (size_t)n : 0;
}
