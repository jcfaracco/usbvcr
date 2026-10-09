/*
 * qemu_engine.h – Target execution engine for QEMU / usbredir emulation.
 */

#ifndef QEMU_ENGINE_H
#define QEMU_ENGINE_H

#include "device.h"

/* Run USB device emulation against QEMU over a connected usbredir socket. */
int qemu_engine_run(int sock, usb_device_t *dev);

#endif /* QEMU_ENGINE_H */
