/*
 * qemu_engine.c – Target execution engine for QEMU / usbredir emulation.
 */

#include "qemu_engine.h"
#include "usb_core.h"
#include "transport.h"
#include "log.h"
#include <unistd.h>
#include <glib.h>

typedef struct {
    char *host;
    char *port;
    int sock;
} qemu_engine_priv_t;

int
qemu_engine_run(int sock, usb_device_t *dev)
{
    return usb_core_run(sock, dev);
}

static int
qemu_engine_init(usb_engine_t *engine, const usb_engine_config_t *config)
{
    if (!engine)
        return -1;

    if (config && config->port)
    {
        int p = atoi(config->port);
        if (p <= 0 || p > 65535)
        {
            LOG_ERR("invalid port '%s' for QEMU engine", config->port);
            return -1;
        }
    }

    qemu_engine_priv_t *priv = g_new0(qemu_engine_priv_t, 1);
    priv->host = g_strdup(config && config->host ? config->host : "localhost");
    priv->port = g_strdup(config && config->port ? config->port : "1337");
    priv->sock = -1;
    engine->priv = priv;
    return 0;
}

static int
qemu_engine_run_impl(usb_engine_t *engine, usb_device_t *dev)
{
    if (!engine || !engine->priv || !dev)
        return -1;

    qemu_engine_priv_t *priv = engine->priv;

    LOG_DEBUG("Connecting to %s:%s ...", priv->host, priv->port);
    priv->sock = connect_to_server(priv->host, priv->port);
    if (priv->sock < 0)
    {
        LOG_ERR("Connection failed to %s:%s", priv->host, priv->port);
        return -1;
    }
    LOG_INFO("Connected to %s:%s", priv->host, priv->port);

    int rc = usb_core_run(priv->sock, dev);

    close(priv->sock);
    priv->sock = -1;
    return rc;
}

static void
qemu_engine_destroy_impl(usb_engine_t *engine)
{
    if (!engine || !engine->priv)
        return;

    qemu_engine_priv_t *priv = engine->priv;
    if (priv->sock >= 0)
    {
        close(priv->sock);
        priv->sock = -1;
    }
    g_free(priv->host);
    g_free(priv->port);
    g_free(priv);
    engine->priv = NULL;
}

static const usb_engine_ops_t qemu_engine_ops = {
    .name = "qemu",
    .description = "QEMU / usbredir TCP client emulation engine",
    .init = qemu_engine_init,
    .run = qemu_engine_run_impl,
    .stop = NULL,
    .destroy = qemu_engine_destroy_impl,
};

const usb_engine_ops_t *
qemu_engine_get_ops(void)
{
    return &qemu_engine_ops;
}
