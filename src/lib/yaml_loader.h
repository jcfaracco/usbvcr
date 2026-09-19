/*
 * yaml_loader.h – Canonical-dialect YAML -> UsbNode descriptor tree.
 *
 * Parses the canonical `schemas/ devices/ flows/` dialect (README §3):
 * resolves `import`, applies `template`, overrides fields, and fills the
 * null-valued auto-computed length/count fields (README §3.1).
 *
 * Pure lib module: no socket/engine deps (README §5 layering rule).
 */

#ifndef YAML_LOADER_H
#define YAML_LOADER_H

#include "usb_node.h"

/* Loaded device: descriptor tree plus its associated flow model.
 * flow is opaque here to avoid a hard dep; yaml_backend owns the join. */
typedef struct {
    UsbNode *tree;   /* device/config/interface/endpoint descriptor tree */
    void    *flow;   /* Flow* (see flow.h), loaded from flows/ dir */
} YamlDevice;

/* Load a canonical device project rooted at `dir` (contains devices/,
 * schemas/, flows/ ...). Returns NULL on error. */
YamlDevice *yaml_loader_load(const char *dir);

void yaml_device_free(YamlDevice *dev);

#endif /* YAML_LOADER_H */
