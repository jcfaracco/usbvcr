/*
 * qemu_engine.h – Target execution engine for QEMU / usbredir emulation.
 */

#ifndef QEMU_ENGINE_H
#define QEMU_ENGINE_H

#include "engine.h"

/* Returns the ops table for QEMU / usbredir engine */
const usb_engine_ops_t *qemu_engine_get_ops(void);

/* Backward compatibility helper: run USB device emulation over an already connected usbredir socket
 */
int qemu_engine_run(int sock, usb_device_t *dev);

#endif /* QEMU_ENGINE_H */
