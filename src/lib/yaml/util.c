/*
 * yaml/util.c – Low-level libyaml document accessors (see yaml/util.h).
 */

#include "yaml/util.h"

#include <string.h>

const char *yaml_scalar(yaml_node_t *n)
{
    return (n && n->type == YAML_SCALAR_NODE)
         ? (const char *) n->data.scalar.value : NULL;
}

yaml_node_t *yaml_map_get(yaml_document_t *doc, yaml_node_t *map,
                          const char *key)
{
    if (!map || map->type != YAML_MAPPING_NODE)
        return NULL;
    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = yaml_document_get_node(doc, p->key);
        if (k && k->type == YAML_SCALAR_NODE &&
            strcmp((const char *) k->data.scalar.value, key) == 0)
            return yaml_document_get_node(doc, p->value);
    }
    return NULL;
}
