/*
 * qemu_engine.c – Target execution engine for QEMU / usbredir emulation.
 */

#include "qemu_engine.h"
#include "usb_core.h"

int qemu_engine_run(int sock, usb_device_t *dev)
{
    return usb_core_run(sock, dev);
}
