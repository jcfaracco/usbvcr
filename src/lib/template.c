/*
 * template.c – Prototype descriptor template registry (see template.h).
 */

#include "template.h"
#include "log.h"

#include <glib.h>

struct template_registry {
    GHashTable *templates;   /* "namespace.name" -> usb_node_t* (owned) */
};

template_registry_t *template_registry_new(void)
{
    template_registry_t *t = g_new0(template_registry_t, 1);
    t->templates = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free,
                                         (GDestroyNotify) usb_node_free);
    return t;
}

void template_registry_free(template_registry_t *t)
{
    if (!t)
        return;
    if (t->templates)
        g_hash_table_destroy(t->templates);
    g_free(t);
}

void template_register(template_registry_t *t, usb_node_t *root, const char *ns)
{
    if (!t || !root)
        return;

    for (GList *l = root->props; l; l = l->next) {
        char *key = NULL;
        usb_prop_t *p = l->data;

        if (p->type != USB_PROP_NODE)
            continue;   /* skip the `namespace` scalar and other metadata */

        key = g_strdup_printf("%s.%s", ns ? ns : "", p->name);

        g_hash_table_insert(t->templates, key, usb_node_clone(p->val.node));

        LOG_DEBUG("[Template] registered template %s", key);
    }
}

usb_node_t *template_lookup(template_registry_t *t, const char *key)
{
    if (!t || !key)
        return NULL;

    return g_hash_table_lookup(t->templates, key);
}
