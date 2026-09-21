/*
 * yaml_tree.h – Canonical-dialect YAML -> usb_node_t descriptor tree.
 *
 * The one cohesive engine: walk a libyaml document and fold it into a
 * usb_node_t tree, resolving schema/template merges and codec blocks along
 * the way. No project/manifest knowledge — callers pass the shared tables.
 *
 * Pure lib module: no socket/engine deps (README §5 layering rule).
 */

#ifndef YAML_TREE_H
#define YAML_TREE_H

#include <glib.h>
#include <yaml.h>

#include "usb_node.h"
#include "schema.h"

/* Build a usb_node tree from the canonical-dialect YAML file at `path`.
 *
 *   schemas: schema base template registry (may be NULL).
 *   codecs:  "codec name" -> codec_t* tables (may be NULL).
 *
 * On success returns the root node (caller owns it) and, for schema files,
 * sets *ns_out to the file's `namespace:` (caller frees). NULL on error. */
usb_node_t *yaml_tree_load(const char *path,
                           schema_t *schemas,
                           GHashTable *codecs,
                           char **ns_out);

/* Shared libyaml accessors (used here and by the manifest reader). */

/* Scalar text of a yaml node, or NULL. */
const char *yaml_tree_scalar(yaml_node_t *n);

/* Value node of `key` in mapping `map`, or NULL. */
yaml_node_t *yaml_tree_map_get(yaml_document_t *doc, yaml_node_t *map,
                               const char *key);

#endif /* YAML_TREE_H */
