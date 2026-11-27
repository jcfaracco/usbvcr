/*
 * engine.c – Pluggable USB execution engine factory and registry.
 */

#include "engine.h"
#include "qemu_engine.h"
#ifdef USBVCR_HAVE_LOCAL_ENGINE
#include "local_engine.h"
#endif
#include "log.h"
#include <string.h>

typedef struct {
    const char *name;
    const usb_engine_ops_t *(*get_ops)(void);
} engine_entry_t;

static const engine_entry_t engine_registry[] = {
    { "qemu", qemu_engine_get_ops },
    { "usbredir", qemu_engine_get_ops },
#ifdef USBVCR_HAVE_LOCAL_ENGINE
    { "local", local_engine_get_ops },
#endif
};

usb_engine_t *
usb_engine_create(const char *name, const usb_engine_config_t *config)
{
    const char *target = (name && *name) ? name : "qemu";
    const usb_engine_ops_t *ops = NULL;

    for (size_t i = 0; i < G_N_ELEMENTS(engine_registry); i++)
    {
        if (g_ascii_strcasecmp(engine_registry[i].name, target) == 0)
        {
            ops = engine_registry[i].get_ops();
            break;
        }
    }

    if (!ops)
    {
#ifndef USBVCR_HAVE_LOCAL_ENGINE
        if (g_ascii_strcasecmp(target, "local") == 0)
        {
            LOG_ERR("local engine not built in this binary (configure with "
                    "-Denable_local_engine=true on Linux)");
            return NULL;
        }
#endif
        LOG_ERR("unknown execution engine: %s", target);
        return NULL;
    }

    usb_engine_t *engine = g_new0(usb_engine_t, 1);
    engine->ops = ops;

    if (ops->init)
    {
        if (ops->init(engine, config) < 0)
        {
            LOG_ERR("failed to initialize engine: %s", target);
            usb_engine_destroy(engine);
            return NULL;
        }
    }

    return engine;
}

int
usb_engine_run(usb_engine_t *engine, usb_device_t *dev)
{
    if (!engine || !engine->ops || !engine->ops->run)
    {
        LOG_ERR("invalid engine or run operation");
        return -1;
    }
    return engine->ops->run(engine, dev);
}

void
usb_engine_stop(usb_engine_t *engine)
{
    if (engine && engine->ops && engine->ops->stop)
        engine->ops->stop(engine);
}

void
usb_engine_destroy(usb_engine_t *engine)
{
    if (!engine)
        return;
    if (engine->ops && engine->ops->destroy)
        engine->ops->destroy(engine);
    g_free(engine);
}
