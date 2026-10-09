/*
 * ffs.c – FunctionFS and ConfigFS lifecycle and descriptor management.
 */

#include "ffs.h"
#include "log.h"
#include "bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <glib.h>

#define CONFIGFS_BASE "/sys/kernel/config/usb_gadget"

ffs_ctx_t *ffs_ctx_new(const char *name, const char *udc)
{
    ffs_ctx_t *ctx;
    const char *gadget_name;

    ctx = g_new0(ffs_ctx_t, 1);
    gadget_name = (name != NULL) ? name : "usbvcr";
    ctx->name = g_strdup(gadget_name);
    ctx->udc = (udc != NULL) ? g_strdup(udc) : NULL;
    ctx->configfs_path = g_strdup_printf("%s/%s", CONFIGFS_BASE, ctx->name);
    ctx->mount_path = g_strdup_printf("/dev/ffs-%s", ctx->name);
    ctx->ep0_fd = -1;
    ctx->ep_fds = g_array_new(FALSE, FALSE, sizeof(int));
    return ctx;
}

void ffs_ctx_free(ffs_ctx_t *ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    if (ctx->ep0_fd >= 0)
    {
        close(ctx->ep0_fd);
    }
    if (ctx->ep_fds != NULL)
    {
        for (guint i = 0; i < ctx->ep_fds->len; i++)
        {
            int fd = g_array_index(ctx->ep_fds, int, i);
            if (fd >= 0)
            {
                close(fd);
            }
        }
        g_array_free(ctx->ep_fds, TRUE);
    }

    g_free(ctx->name);
    g_free(ctx->udc);
    g_free(ctx->configfs_path);
    g_free(ctx->mount_path);
    g_free(ctx);
}

static uint32_t count_config_subdescriptors(const uint8_t *cd, uint16_t cdlen)
{
    uint32_t count;
    size_t off;

    count = 0;
    off = 9;   /* skip 9-byte Configuration Descriptor header */
    while (off + 2 <= cdlen)
    {
        uint8_t sub_len = cd[off];
        if (sub_len == 0 || off + sub_len > cdlen)
        {
            break;
        }
        count++;
        off += sub_len;
    }
    return count;
}

GByteArray *ffs_build_descriptors(usb_device_t *dev)
{
    const uint8_t *cd;
    uint16_t cdlen;
    uint32_t sub_count;
    GByteArray *arr;
    struct usb_functionfs_descs_head_v2 head;
    const uint8_t *sub_descs;
    guint sub_bytes;

    if (dev == NULL || dev->ops == NULL || dev->ops->get_descriptor == NULL)
    {
        return NULL;
    }

    cd = NULL;
    cdlen = 0;
    if (dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &cd, &cdlen) < 0 || cdlen < 9)
    {
        LOG_ERR("ffs: unable to retrieve configuration descriptor");
        return NULL;
    }

    sub_count = count_config_subdescriptors(cd, cdlen);
    sub_descs = cd + 9;
    sub_bytes = (guint)(cdlen - 9);

    arr = g_byte_array_new();
    memset(&head, 0, sizeof(head));
    head.magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2;
    head.flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC;
    head.length = (uint32_t)(sizeof(head) + sizeof(uint32_t) + sub_bytes +
                             sizeof(uint32_t) + sub_bytes);

    g_byte_array_append(arr, (const guint8 *)&head, sizeof(head));

    /* Full-Speed descriptors */
    g_byte_array_append(arr, (const guint8 *)&sub_count, sizeof(sub_count));
    g_byte_array_append(arr, sub_descs, sub_bytes);

    /* High-Speed descriptors */
    g_byte_array_append(arr, (const guint8 *)&sub_count, sizeof(sub_count));
    g_byte_array_append(arr, sub_descs, sub_bytes);

    return arr;
}

GByteArray *ffs_build_strings(usb_device_t *dev)
{
    GByteArray *arr;
    struct usb_functionfs_strings_head head;
    uint16_t lang;
    const char *str;
    guint str_len;

    (void)dev;
    arr = g_byte_array_new();
    str = "USB VCR Emulated Device";
    str_len = (guint)strlen(str) + 1;

    memset(&head, 0, sizeof(head));
    head.magic = FUNCTIONFS_STRINGS_MAGIC;
    head.length = (uint32_t)(sizeof(head) + sizeof(lang) + str_len);
    head.str_count = 1;
    head.lang_count = 1;

    lang = 0x0409;   /* en-US */

    g_byte_array_append(arr, (const guint8 *)&head, sizeof(head));
    g_byte_array_append(arr, (const guint8 *)&lang, sizeof(lang));
    g_byte_array_append(arr, (const guint8 *)str, str_len);

    return arr;
}

static int write_str_file(const char *path, const char *val)
{
    FILE *fp;

    fp = fopen(path, "w");
    if (fp == NULL)
    {
        LOG_ERR("ffs: failed to open %s for writing: %s", path, g_strerror(errno));
        return -1;
    }
    fputs(val, fp);
    fclose(fp);
    return 0;
}

char *ffs_detect_udc(void)
{
    DIR *dir;
    struct dirent *ent;
    char *udc;

    dir = opendir("/sys/class/udc");
    if (dir == NULL)
    {
        return NULL;
    }

    udc = NULL;
    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_name[0] != '.')
        {
            udc = g_strdup(ent->d_name);
            break;
        }
    }
    closedir(dir);
    return udc;
}

static int ffs_init_configfs_dirs(ffs_ctx_t *ctx, usb_device_t *dev)
{
    const uint8_t *dd;
    uint16_t ddlen;
    g_autofree char *str_dir = NULL;
    g_autofree char *cfg_dir = NULL;
    g_autofree char *fn_dir = NULL;
    g_autofree char *link_dst = NULL;
    g_autofree char *val = NULL;
    g_autofree char *file_path = NULL;

    dd = NULL;
    ddlen = 0;
    if (dev->ops->get_descriptor(dev, USB_DESC_DEVICE, 0, 0, &dd, &ddlen) < 0 || ddlen < 18)
    {
        LOG_ERR("ffs: unable to retrieve device descriptor");
        return -1;
    }

    g_mkdir_with_parents(ctx->configfs_path, 0755);

    /* Write VID and PID */
    val = g_strdup_printf("0x%04x", (unsigned)get_u16le(dd + 8));
    file_path = g_strdup_printf("%s/idVendor", ctx->configfs_path);
    write_str_file(file_path, val);

    g_free(val);
    g_free(file_path);
    val = g_strdup_printf("0x%04x", (unsigned)get_u16le(dd + 10));
    file_path = g_strdup_printf("%s/idProduct", ctx->configfs_path);
    write_str_file(file_path, val);

    /* Strings and configurations */
    str_dir = g_strdup_printf("%s/strings/0x409", ctx->configfs_path);
    g_mkdir_with_parents(str_dir, 0755);
    g_free(file_path);
    file_path = g_strdup_printf("%s/product", str_dir);
    write_str_file(file_path, "USB VCR Device");

    cfg_dir = g_strdup_printf("%s/configs/c.1", ctx->configfs_path);
    g_mkdir_with_parents(cfg_dir, 0755);

    fn_dir = g_strdup_printf("%s/functions/ffs.%s", ctx->configfs_path, ctx->name);
    g_mkdir_with_parents(fn_dir, 0755);

    link_dst = g_strdup_printf("%s/ffs.%s", cfg_dir, ctx->name);
    symlink(fn_dir, link_dst);
    return 0;
}

int ffs_setup(ffs_ctx_t *ctx, usb_device_t *dev)
{
    g_autoptr(GByteArray) descs = NULL;
    g_autoptr(GByteArray) strings = NULL;
    g_autofree char *ep0_path = NULL;
    g_autofree char *detected_udc = NULL;
    g_autofree char *udc_file = NULL;

    if (ffs_init_configfs_dirs(ctx, dev) < 0)
    {
        return -1;
    }

    g_mkdir_with_parents(ctx->mount_path, 0755);
    if (mount(ctx->name, ctx->mount_path, "functionfs", 0, NULL) < 0)
    {
        LOG_ERR("ffs: mount %s -> %s failed: %s", ctx->name, ctx->mount_path, g_strerror(errno));
        return -1;
    }

    ep0_path = g_strdup_printf("%s/ep0", ctx->mount_path);
    ctx->ep0_fd = open(ep0_path, O_RDWR);
    if (ctx->ep0_fd < 0)
    {
        LOG_ERR("ffs: failed to open %s: %s", ep0_path, g_strerror(errno));
        return -1;
    }

    descs = ffs_build_descriptors(dev);
    strings = ffs_build_strings(dev);
    if (descs == NULL || strings == NULL)
    {
        return -1;
    }

    if (write(ctx->ep0_fd, descs->data, descs->len) != (ssize_t)descs->len ||
        write(ctx->ep0_fd, strings->data, strings->len) != (ssize_t)strings->len)
    {
        LOG_ERR("ffs: failed writing descriptors/strings to ep0: %s", g_strerror(errno));
        return -1;
    }

    if (ctx->udc == NULL)
    {
        detected_udc = ffs_detect_udc();
        ctx->udc = g_strdup(detected_udc);
    }

    if (ctx->udc != NULL)
    {
        udc_file = g_strdup_printf("%s/UDC", ctx->configfs_path);
        write_str_file(udc_file, ctx->udc);
        LOG_INFO("ffs: bound gadget to UDC %s", ctx->udc);
    }

    return 0;
}

void ffs_teardown(ffs_ctx_t *ctx)
{
    g_autofree char *udc_file = NULL;
    g_autofree char *link_dst = NULL;
    g_autofree char *fn_dir = NULL;
    g_autofree char *cfg_dir = NULL;
    g_autofree char *str_dir = NULL;

    if (ctx == NULL)
    {
        return;
    }

    udc_file = g_strdup_printf("%s/UDC", ctx->configfs_path);
    write_str_file(udc_file, "");

    if (ctx->ep0_fd >= 0)
    {
        close(ctx->ep0_fd);
        ctx->ep0_fd = -1;
    }

    umount(ctx->mount_path);
    rmdir(ctx->mount_path);

    link_dst = g_strdup_printf("%s/configs/c.1/ffs.%s", ctx->configfs_path, ctx->name);
    unlink(link_dst);

    fn_dir = g_strdup_printf("%s/functions/ffs.%s", ctx->configfs_path, ctx->name);
    rmdir(fn_dir);

    cfg_dir = g_strdup_printf("%s/configs/c.1", ctx->configfs_path);
    rmdir(cfg_dir);

    str_dir = g_strdup_printf("%s/strings/0x409", ctx->configfs_path);
    rmdir(str_dir);

    rmdir(ctx->configfs_path);
}
