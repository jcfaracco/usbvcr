/*
 * yaml_loader.c – Canonical-dialect loader.  STUB (README §9 build step 2).
 *
 * TODO:
 *   - parse devices/*.yaml, resolve `import` of schemas/, apply `template`
 *   - override named fields; support raw-mode (hex / raw_file) payloads
 *   - autofill null length/count fields: bLength, wTotalLength,
 *     bNumInterfaces, bNumEndpoints, wDescriptorLength (README §3.1)
 *   - load flows/ into Flow model (flow.h)
 */

#include "yaml_loader.h"

#include <stdlib.h>

YamlDevice *yaml_loader_load(const char *dir)
{
    (void)dir;
    return NULL; /* not implemented */
}

void yaml_device_free(YamlDevice *dev)
{
    if (!dev)
        return;
    if (dev->tree)
        usb_node_free(dev->tree);
    /* TODO: flow_free(dev->flow); */
    free(dev);
}
