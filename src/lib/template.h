/*
 * template.h – Prototype descriptor template registry.
 *
 * A template file (under templates/) declares a `namespace:` and one or more
 * top-level descriptor nodes. Each node is registered as a reusable base
 * template under the key "<namespace>.<name>", which a device references via
 * its `extends:` field to fold the base in before overriding fields.
 *
 * Thin wrapper over a name->node map, symmetric with codec.{c,h}.
 */

#ifndef TEMPLATE_H
#define TEMPLATE_H

#include "usb_node.h"

typedef struct template_registry template_registry_t;
typedef struct template_registry schema_t; /* Backward-compatibility alias */

template_registry_t *template_registry_new(void);
void                 template_registry_free(template_registry_t *t);

/* Register every top-level descriptor node of `root` under "<ns>.<name>".
 * Scalar metadata (namespace, version, ...) is skipped. Nodes are cloned;
 * the caller keeps ownership of `root`. `ns` may be NULL (empty namespace). */
void template_register(template_registry_t *t, usb_node_t *root, const char *ns);

/* Base template for "<ns>.<name>", or NULL. Borrowed (owned by the registry). */
usb_node_t *template_lookup(template_registry_t *t, const char *key);

/* Backward compatibility macros */
#define schema_new()          template_registry_new()
#define schema_free(s)        template_registry_free(s)
#define schema_register(s, r, ns) template_register((s), (r), (ns))
#define schema_lookup(s, k)   template_lookup((s), (k))

#endif /* TEMPLATE_H */
