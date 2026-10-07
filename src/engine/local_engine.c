/*
 * local_engine.c – Target execution engine for local Linux kernel emulation via FunctionFS.
 */

#include "local_engine.h"
#include "ffs/ffs.h"
#include "log.h"
#include "bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <glib.h>

typedef struct {
    uint8_t  ep_addr;
    int      fd;
    gboolean is_in;
} local_ep_t;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static GArray *open_endpoints(ffs_ctx_t *ctx, usb_device_t *dev)
{
    const uint8_t *cd;
    uint16_t cdlen;
    GArray *eps;
    size_t off;
    unsigned ep_idx;

    cd = NULL;
    cdlen = 0;
    if (dev->ops->get_descriptor(dev, USB_DESC_CONFIG, 0, 0, &cd, &cdlen) < 0 || cdlen < 9)
    {
        return NULL;
    }

    eps = g_array_new(FALSE, FALSE, sizeof(local_ep_t));
    ep_idx = 1;
    off = 9;

    while (off + 2 <= cdlen)
    {
        uint8_t sub_len = cd[off];
        uint8_t sub_type = cd[off + 1];

        if (sub_len == 0 || off + sub_len > cdlen)
        {
            break;
        }

        if (sub_type == 0x05 && sub_len >= 7)  /* Endpoint descriptor */
        {
            local_ep_t ep;
            g_autofree char *path = NULL;

            memset(&ep, 0, sizeof(ep));
            ep.ep_addr = cd[off + 2];
            ep.is_in = (ep.ep_addr & 0x80) != 0;

            path = g_strdup_printf("%s/ep%u", ctx->mount_path, ep_idx);
            ep.fd = open(path, O_RDWR | O_NONBLOCK);
            if (ep.fd >= 0)
            {
                g_array_append_val(eps, ep);
                LOG_INFO("local_engine: opened %s for ep 0x%02x (%s)",
                         path, ep.ep_addr, ep.is_in ? "IN" : "OUT");
            }
            else
            {
                LOG_WARN("local_engine: failed to open %s: %s", path, g_strerror(errno));
            }
            ep_idx++;
        }
        off += sub_len;
    }

    return eps;
}

static void close_endpoints(GArray *eps)
{
    if (eps == NULL)
    {
        return;
    }

    for (guint i = 0; i < eps->len; i++)
    {
        local_ep_t *ep = &g_array_index(eps, local_ep_t, i);
        if (ep->fd >= 0)
        {
            close(ep->fd);
            ep->fd = -1;
        }
    }
    g_array_free(eps, TRUE);
}

static void handle_control_event(ffs_ctx_t *ctx,
                                 usb_device_t *dev,
                                 const struct usb_ctrlrequest *req)
{
    usb_control_packet_header_t setup;
    uint8_t data[USB_VCR_BUF_SIZE];
    uint16_t n;
    int rc;

    memset(&setup, 0, sizeof(setup));
    setup.requesttype = req->bRequestType;
    setup.request     = req->bRequest;
    setup.value       = get_u16le((const uint8_t *)&req->wValue);
    setup.index       = get_u16le((const uint8_t *)&req->wIndex);
    setup.length      = get_u16le((const uint8_t *)&req->wLength);

    n = 0;
    rc = -1;
    if (dev->ops->control != NULL)
    {
        rc = dev->ops->control(dev, &setup, data, &n);
    }

    if (rc == 0)
    {
        if (setup.requesttype & 0x80)
        {
            write(ctx->ep0_fd, data, n);
        }
        else
        {
            write(ctx->ep0_fd, NULL, 0);
        }
    }
    else
    {
        uint8_t dummy;
        if (setup.requesttype & 0x80)
        {
            read(ctx->ep0_fd, &dummy, 0);   /* Stall IN */
        }
        else
        {
            write(ctx->ep0_fd, NULL, 0);
        }
    }
}

static void handle_ep0(ffs_ctx_t *ctx, usb_device_t *dev)
{
    struct usb_functionfs_event ev;
    ssize_t ret;

    ret = read(ctx->ep0_fd, &ev, sizeof(ev));
    if (ret != (ssize_t)sizeof(ev))
    {
        return;
    }

    switch (ev.type)
    {
    case FUNCTIONFS_SETUP:
        handle_control_event(ctx, dev, &ev.setup);
        break;
    case FUNCTIONFS_ENABLE:
        LOG_INFO("local_engine: FunctionFS enabled by host");
        break;
    case FUNCTIONFS_DISABLE:
        LOG_INFO("local_engine: FunctionFS disabled by host");
        break;
    case FUNCTIONFS_BIND:
        LOG_DEBUG("local_engine: FunctionFS bound");
        break;
    case FUNCTIONFS_UNBIND:
        LOG_DEBUG("local_engine: FunctionFS unbound");
        break;
    default:
        break;
    }
}

static void service_endpoints(usb_device_t *dev, GArray *eps)
{
    uint8_t buf[USB_VCR_BUF_SIZE];

    if (eps == NULL)
    {
        return;
    }

    for (guint i = 0; i < eps->len; i++)
    {
        local_ep_t *ep = &g_array_index(eps, local_ep_t, i);
        if (ep->fd < 0)
        {
            continue;
        }

        if (ep->is_in && dev->ops->ep_in != NULL)
        {
            int n = dev->ops->ep_in(dev, ep->ep_addr, buf, sizeof(buf));
            if (n > 0)
            {
                write(ep->fd, buf, (size_t)n);
            }
        }
        else if (!ep->is_in && dev->ops->ep_out != NULL)
        {
            ssize_t r = read(ep->fd, buf, sizeof(buf));
            if (r > 0)
            {
                dev->ops->ep_out(dev, ep->ep_addr, buf, (size_t)r);
            }
        }
    }
}

int local_engine_run(usb_device_t *dev, const char *name, const char *udc)
{
    ffs_ctx_t *ctx;
    GArray *eps;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    g_stop = 0;

    ctx = ffs_ctx_new(name, udc);
    if (ffs_setup(ctx, dev) < 0)
    {
        LOG_ERR("local_engine: failed to setup FunctionFS / ConfigFS");
        ffs_ctx_free(ctx);
        return -1;
    }

    eps = open_endpoints(ctx, dev);
    LOG_INFO("local_engine: emulation running locally (press Ctrl+C to stop)");

    while (!g_stop)
    {
        struct pollfd pfd;
        int r;

        pfd.fd = ctx->ep0_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        r = poll(&pfd, 1, 10);  /* 10ms timeout for periodic ticks and endpoint polls */
        if (r > 0 && (pfd.revents & POLLIN))
        {
            handle_ep0(ctx, dev);
        }

        service_endpoints(dev, eps);

        if (dev->ops->tick != NULL)
        {
            dev->ops->tick(dev);
        }
    }

    LOG_INFO("local_engine: stopping and tearing down...");
    close_endpoints(eps);
    ffs_teardown(ctx);
    ffs_ctx_free(ctx);
    return 0;
}
