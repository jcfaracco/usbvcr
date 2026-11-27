/*
 * local_engine.h – Target execution engine for local Linux kernel emulation via FunctionFS.
 */

#ifndef LOCAL_ENGINE_H
#define LOCAL_ENGINE_H

#include "engine.h"

/* Returns the ops table for local FunctionFS / ConfigFS engine */
const usb_engine_ops_t *local_engine_get_ops(void);

/* Run USB device emulation locally in Linux kernel using FunctionFS & ConfigFS. */
int local_engine_run(usb_device_t *dev, const char *name, const char *udc);

#endif /* LOCAL_ENGINE_H */
