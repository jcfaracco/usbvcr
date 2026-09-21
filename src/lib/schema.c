/*
 * schema.c – Schema template registry (see schema.h).
 */

#include "schema.h"
#include "log.h"

#include <glib.h>

struct schema {
    GHashTable *templates;   /* "namespace.name" -> usb_node_t* (owned) */
};

schema_t *schema_new(void)
{
    schema_t *s = g_new0(schema_t, 1);
    s->templates = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free,
                                         (GDestroyNotify) usb_node_free);
    return s;
}

void schema_free(schema_t *s)
{
    if (!s)
        return;
    if (s->templates)
        g_hash_table_destroy(s->templates);
    g_free(s);
}

void schema_register(schema_t *s, usb_node_t *root, const char *ns)
{
    if (!s || !root)
        return;

    for (GList *l = root->props; l; l = l->next) {
        usb_prop_t *p = l->data;
        if (p->type != USB_PROP_NODE)
            continue;   /* skip the `namespace` scalar and other metadata */

        char *key = g_strdup_printf("%s.%s", ns ? ns : "", p->name);
        g_hash_table_insert(s->templates, key, usb_node_clone(p->val.node));
        LOG_DEBUG("[Schema] registered template %s", key);
    }
}

usb_node_t *schema_lookup(schema_t *s, const char *key)
{
    if (!s || !key)
        return NULL;
    return g_hash_table_lookup(s->templates, key);
}
