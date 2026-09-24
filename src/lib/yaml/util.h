/*
 * yaml/util.h – Low-level libyaml document accessors.
 *
 * The tiny read helpers every YAML consumer needs (tree builder, codec loader,
 * flow loader, manifest reader). Depends only on libyaml so any lib module can
 * use it without pulling in the descriptor-tree layer.
 */

#ifndef YAML_UTIL_H
#define YAML_UTIL_H

#include <yaml.h>

/* Scalar text of a yaml node, or NULL. */
const char *yaml_scalar(yaml_node_t *n);

/* Value node of `key` in mapping `map`, or NULL. */
yaml_node_t *yaml_map_get(yaml_document_t *doc, yaml_node_t *map,
                          const char *key);

#endif /* YAML_UTIL_H */
