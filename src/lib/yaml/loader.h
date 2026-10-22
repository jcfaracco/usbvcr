/*
 * yaml/loader.h – Canonical-dialect YAML -> usb_node_t descriptor tree.
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
#include "schema.h"

/* Canonical file scopes. The containing directory is authoritative:
 * devices/ schemas/ flows/ codecs/ tests/ -> the scope. The scope is never
 * stated in-file. */
typedef enum {
    YAML_SCOPE_UNKNOWN = -1,
    YAML_SCOPE_SCHEMAS,  /* schemas/ */
    YAML_SCOPE_CODECS,   /* codecs/  */
    YAML_SCOPE_DEVICES,  /* devices/ */
    YAML_SCOPE_FLOWS,    /* flows/   */
    YAML_SCOPE_TESTS,    /* tests/   */
} yaml_scope_t;

/* Scope from a file path's containing directory (authoritative source).
 * YAML_SCOPE_UNKNOWN if the parent dir is not a recognized scope dir. */
yaml_scope_t yaml_scope_from_path(const char *path);

/* One entry of the project's `devices:` manifest: a device descriptor tree
 * paired with its flow. flow is opaque here to avoid a hard dep on flow.h. */
typedef struct {
    char       *name;       /* device file path from the manifest (identity) */
    char       *flow_path;  /* resolved flows/ path from the manifest, or NULL */
    usb_node_t *tree;       /* device/config/interface/endpoint descriptor tree */
    void       *flow;       /* Flow* (see flow.h), built from flow_path (TODO) */
} yaml_dev_entry_t;

/* Loaded project: the `devices:` manifest (device+flow pairs) plus the shared
 * schema templates and codec tables they resolve against. */
typedef struct {
    char       *name;       /* project `name:` field, or NULL if unset */
    GList      *devices;    /* yaml_dev_entry_t* — all manifest entries */
    schema_t   *schemas;    /* schema base templates (schemas/ registry) */
    GHashTable *codecs;     /* "codec name" -> codec_t* (codecs/ tables) */

    /* Optional `connection:` manifest block (README §3, run config): lets a
     * project pin how `usbvcr --yaml <file>` runs without CLI flags. Any
     * field left unset is NULL; main.c's CLI flags take precedence when
     * explicitly passed. */
    char       *engine;     /* "local" or "qemu", or NULL if unset */
    char       *udc;        /* local engine: UDC name, or NULL (auto-detect) */
    char       *host;       /* qemu engine: remote host, or NULL (default) */
    char       *port;       /* qemu engine: remote port, or NULL (default) */

    /* Convenience aliases to the first manifest entry (not owned), so single-
     * device callers keep working. yaml_backend owns the flow join. */
    usb_node_t *tree;
    void       *flow;
} yaml_device_t;

/* Load a canonical device project rooted at `dir` (contains devices/,
 * schemas/, flows/ ...). Returns NULL on error. */
yaml_device_t *yaml_parse_from_file(const char *dir);

void yaml_device_free(yaml_device_t *dev);

#endif /* YAML_LOADER_H */
