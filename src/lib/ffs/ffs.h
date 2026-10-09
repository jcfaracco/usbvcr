/*
 * ffs.h – FunctionFS and ConfigFS lifecycle and descriptor management.
 *
 * Provides userspace USB gadget configuration for Linux:
 *   - ConfigFS hierarchy creation and teardown
 *   - FunctionFS v2 binary descriptor and string table serialization
 *   - Mount / unmount and UDC binding
 */

#ifndef FFS_H
#define FFS_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>

#include "device.h"

/* Linux FunctionFS ABI constants and structures */
#define FUNCTIONFS_DESCRIPTORS_MAGIC_V2 2
#define FUNCTIONFS_STRINGS_MAGIC        2

#define FUNCTIONFS_HAS_FS_DESC          (1u << 0)
#define FUNCTIONFS_HAS_HS_DESC          (1u << 1)
#define FUNCTIONFS_HAS_SS_DESC          (1u << 2)

enum usb_functionfs_event_type {
    FUNCTIONFS_BIND,
    FUNCTIONFS_UNBIND,
    FUNCTIONFS_ENABLE,
    FUNCTIONFS_DISABLE,
    FUNCTIONFS_SETUP,
    FUNCTIONFS_SUSPEND,
    FUNCTIONFS_RESUME
};

struct usb_ctrlrequest {
    uint8_t  bRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_functionfs_event {
    union {
        struct usb_ctrlrequest setup;
    } __attribute__((packed));
    uint8_t type;
    uint8_t _pad[3];
} __attribute__((packed));

struct usb_functionfs_descs_head_v2 {
    uint32_t magic;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

struct usb_functionfs_strings_head {
    uint32_t magic;
    uint32_t length;
    uint32_t str_count;
    uint32_t lang_count;
} __attribute__((packed));

typedef struct {
    char *name;             /* Gadget and function instance name */
    char *udc;              /* Target UDC name (or NULL for auto-detect) */
    char *configfs_path;    /* /sys/kernel/config/usb_gadget/<name> */
    char *mount_path;       /* /dev/ffs-<name> */
    int   ep0_fd;
    GArray *ep_fds;         /* Endpoint file descriptors (int) */
} ffs_ctx_t;

/* Allocate and initialize an FFS context. */
ffs_ctx_t *ffs_ctx_new(const char *name, const char *udc);

/* Free an FFS context and its internal resources. */
void ffs_ctx_free(ffs_ctx_t *ctx);

/* Build FunctionFS v2 descriptors block from a usb_device_t. */
GByteArray *ffs_build_descriptors(usb_device_t *dev);

/* Build FunctionFS strings block from a usb_device_t. */
GByteArray *ffs_build_strings(usb_device_t *dev);

/* Detect the first available UDC in /sys/class/udc/. Returns g_malloc'd string. */
char *ffs_detect_udc(void);

/* Setup ConfigFS hierarchy, mount FunctionFS, write descriptors, and bind UDC. */
int ffs_setup(ffs_ctx_t *ctx, usb_device_t *dev);

/* Unbind UDC, close endpoints, unmount, and remove ConfigFS hierarchy. */
void ffs_teardown(ffs_ctx_t *ctx);

#endif /* FFS_H */
