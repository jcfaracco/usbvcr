/*
 * yaml_loader.h – Canonical-dialect YAML -> usb_node_t descriptor tree.
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

/* Canonical file scopes. The containing directory is authoritative:
 * devices/ schemas/ flows/ codecs/ tests/ -> the scope. The scope is never
 * stated in-file. */
typedef enum {
    YAML_SCOPE_UNKNOWN = -1,
    YAML_SCOPE_DEVICE,   /* devices/ */
    YAML_SCOPE_SCHEMA,   /* schemas/ */
    YAML_SCOPE_FLOW,     /* flows/   */
    YAML_SCOPE_CODEC,    /* codecs/  */
    YAML_SCOPE_TEST,     /* tests/   */
} yaml_scope_t;

/* Scope from a file path's containing directory (authoritative source).
 * YAML_SCOPE_UNKNOWN if the parent dir is not a recognized scope dir. */
yaml_scope_t yaml_scope_from_path(const char *path);

/* Loaded device: descriptor tree plus its associated flow model.
 * flow is opaque here to avoid a hard dep; yaml_backend owns the join. */
typedef struct {
    usb_node_t *tree;   /* device/config/interface/endpoint descriptor tree */
    void       *flow;   /* Flow* (see flow.h), loaded from flows/ dir */
} yaml_device_t;

/* Load a canonical device project rooted at `dir` (contains devices/,
 * schemas/, flows/ ...). Returns NULL on error. */
yaml_device_t *yaml_loader_load(const char *dir);

void yaml_device_free(yaml_device_t *dev);

#endif /* YAML_LOADER_H */
