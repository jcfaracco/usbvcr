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

/* Canonical file kinds, discriminated by the top-level `kind:` value.
 * DEVICE/SCHEMA/CODEC carry no `kind:` — detect them by presence of
 * `name`+`imports` / `namespace` / `codec` respectively. */
typedef enum {
    YAML_KIND_UNKNOWN = -1,
    YAML_KIND_FLOW    = 0,   /* kind: "FlowLogic"      */
    YAML_KIND_TEST,          /* kind: "ValidationSuite" */
} yaml_kind_t;

/* Map a `kind:` string to yaml_kind_t (YAML_KIND_UNKNOWN if unmatched). */
yaml_kind_t yaml_kind_from_str(const char *kind);

/* Loaded device: descriptor tree plus its associated flow model.
 * flow is opaque here to avoid a hard dep; yaml_backend owns the join. */
typedef struct {
    UsbNode *tree;   /* device/config/interface/endpoint descriptor tree */
    void    *flow;   /* Flow* (see flow.h), loaded from flows/ dir */
} yaml_device_t;

/* Load a canonical device project rooted at `dir` (contains devices/,
 * schemas/, flows/ ...). Returns NULL on error. */
yaml_device_t *yaml_loader_load(const char *dir);

void yaml_device_free(yaml_device_t *dev);

#endif /* YAML_LOADER_H */
