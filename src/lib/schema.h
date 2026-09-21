/*
 * schema.h – Schema template registry.
 *
 * A schema file (under schemas/) declares a `namespace:` and one or more
 * top-level descriptor nodes. Each node is registered as a reusable base
 * template under the key "<namespace>.<name>", which a device references via
 * its `schema:`/`template:` field to fold the base in before overriding fields.
 *
 * Thin wrapper over a name->node map, symmetric with codec.{c,h}.
 */

#ifndef SCHEMA_H
#define SCHEMA_H

#include "usb_node.h"

typedef struct schema schema_t;

schema_t *schema_new(void);
void      schema_free(schema_t *s);

/* Register every top-level descriptor node of `root` under "<ns>.<name>".
 * Scalar metadata (namespace, version, ...) is skipped. Nodes are cloned;
 * the caller keeps ownership of `root`. `ns` may be NULL (empty namespace). */
void schema_register(schema_t *s, usb_node_t *root, const char *ns);

/* Base template for "<ns>.<name>", or NULL. Borrowed (owned by the registry). */
usb_node_t *schema_lookup(schema_t *s, const char *key);

#endif /* SCHEMA_H */
