/*
 * yaml_loader.c – Canonical-dialect loader.  STUB (README §9 build step 2).
 *
 * TODO:
 *   - parse devices/\*.yaml, resolve `import` of schemas/, apply `template`
 *   - override named fields; support raw-mode (hex / raw_file) payloads
 *   - autofill null length/count fields: bLength, wTotalLength,
 *     bNumInterfaces, bNumEndpoints, wDescriptorLength (README §3.1)
 *   - load flows/ into Flow model (flow.h)
 */

#include "yaml_loader.h"
#include "utils.h"

#include <stdlib.h>
#include <glib.h>

/* Index order must match yaml_kind_t enumerators. */
static const char *const yaml_kind_names[] = {
    "FlowLogic",       /* YAML_KIND_FLOW */
    "ValidationSuite", /* YAML_KIND_TEST */
};

yaml_kind_t yaml_kind_from_str(const char *kind)
{
    int i = str_index_in_set(kind, yaml_kind_names,
                             G_N_ELEMENTS(yaml_kind_names));
    return i < 0 ? YAML_KIND_UNKNOWN : (yaml_kind_t)i;
}

yaml_device_t *yaml_loader_load(const char *dir)
{
    (void)dir;
    return NULL; /* not implemented */
}

void yaml_device_free(yaml_device_t *dev)
{
    if (!dev)
        return;
    if (dev->tree)
        usb_node_free(dev->tree);
    /* TODO: flow_free(dev->flow); */
    g_free(dev);
}
