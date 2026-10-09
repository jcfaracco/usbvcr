/*
 * local_engine.h – Target execution engine for local Linux kernel emulation via FunctionFS.
 */

#ifndef LOCAL_ENGINE_H
#define LOCAL_ENGINE_H

#include "device.h"

/* Run USB device emulation locally in Linux kernel using FunctionFS & ConfigFS. */
int local_engine_run(usb_device_t *dev, const char *name, const char *udc);

#endif /* LOCAL_ENGINE_H */
