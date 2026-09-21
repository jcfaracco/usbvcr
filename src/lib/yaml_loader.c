/*
 * yaml_loader.c – Canonical-dialect loader.  STUB (README §9 build step 2).
 *
 * TODO:
 *   - parse devices/\*.yaml, resolve `import` of schemas/, apply `template`
 *   - override named fields; support raw-mode (hex / raw_file) payloads
 *   - autofill null length/count fields: bLength, wTotalLength,
 *     bNumInterfaces, bNumEndpoints, wDescriptorLength (README §3.1)
 *   - load flows/ into Flow model (flow.h)
 */

#include "yaml_loader.h"
#include "codec.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <yaml.h>

/* Input bounds. These files describe USB descriptors (a few KB) and may be
 * untrusted (fuzzer harness), so reject pathological input up front rather
 * than let a giant or deeply-nested document exhaust memory / the C stack. */
#define YAML_MAX_FILE_BYTES (1u * 1024u * 1024u)   /* 1 MiB */
#define YAML_MAX_DEPTH      32                      /* dev>cfg>iface>ep is shallow */


typedef struct {
    const char *name;
    yaml_scope_t scope;
} scope_entry;

/* Directory name -> scope. The directory is authoritative; the scope is
 * never stated in-file. */
static const scope_entry scope_by_dir[] = {
    { "schemas", YAML_SCOPE_SCHEMAS },
    { "codecs",  YAML_SCOPE_CODECS  },
    { "devices", YAML_SCOPE_DEVICES },
    { "flows",   YAML_SCOPE_FLOWS   },
    { "tests",   YAML_SCOPE_TESTS   },
};

/* USB descriptor field widths follow the field-name prefix convention:
 * "bcd" and "id" prefixes are 2 bytes, a leading 'w' is 2 bytes, everything
 * else ('b', 'i', ...) is 1 byte. */
static size_t width_for_key(const char *key)
{
    if (g_str_has_prefix(key, "bcd")) return 2;
    if (g_str_has_prefix(key, "id"))  return 2;
    if (key[0] == 'w')                return 2;
    return 1;
}

/* True (and *out set) when a scalar is a *whole* integer literal (decimal or
 * 0x-hex). Anything with trailing characters ("1.0", "Data,Var,Abs", metadata
 * like version/name) is not a descriptor field and returns FALSE. */
static gboolean parse_uint(const char *val, uint32_t *out)
{
    if (!val || !g_ascii_isdigit(val[0]))
        return FALSE;

    char *end = NULL;
    unsigned long long v = g_ascii_strtoull(val, &end, 0);
    if (!end || *end != '\0')   /* trailing junk => not a plain integer */
        return FALSE;

    *out = (uint32_t) v;
    return TRUE;
}

static gboolean scalar_is_null(const char *val)
{
    return !val || val[0] == '\0' ||
           strcmp(val, "null") == 0 || strcmp(val, "~") == 0;
}

/* First property named `name` in `node`, or NULL. */
static usb_prop_t *find_prop(usb_node_t *node, const char *name)
{
    for (GList *l = node->props; l; l = l->next) {
        usb_prop_t *p = l->data;
        if (strcmp(p->name, name) == 0)
            return p;
    }
    return NULL;
}

/* Scalar text of a yaml node, or NULL. */
static const char *ynode_scalar(yaml_node_t *n)
{
    return (n && n->type == YAML_SCALAR_NODE)
         ? (const char *) n->data.scalar.value : NULL;
}

/* Value node of `key` in mapping `map`, or NULL. */
static yaml_node_t *ymap_get(yaml_document_t *doc, yaml_node_t *map,
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

/* Handle a codec block:
 *   report_descriptor:
 *     codec: "hid_report"
 *     items: [ {usage_page: ...}, {usage: ...}, ... ]
 * Encode the ordered items via the named table into one USB_PROP_ARRAY under
 * `key`. Returns TRUE if `map` was a codec block (handled or failed-and-
 * consumed), FALSE if it has no `codec:` field (caller falls back to generic). */
static gboolean build_codec_block(yaml_document_t *doc, yaml_node_t *map,
                                  yaml_device_t *dev, usb_node_t *out,
                                  const char *key)
{
    const char *cname = ynode_scalar(ymap_get(doc, map, "codec"));
    if (!cname)
        return FALSE;   /* not a codec block */

    codec_t *c = dev->codecs ? g_hash_table_lookup(dev->codecs, cname) : NULL;
    if (!c) {
        LOG_WARN("yaml_loader: unknown codec '%s' for '%s'", cname, key);
        return TRUE;    /* consumed: don't expand items generically */
    }

    /* Build the ordered item list; codec_item_t borrows the yaml scalar strings
     * (valid until the document is deleted, well after codec_encode). */
    GList *items = NULL;
    yaml_node_t *seq = ymap_get(doc, map, "items");
    if (seq && seq->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *i = seq->data.sequence.items.start;
             i < seq->data.sequence.items.top; i++) {
            yaml_node_t *im = yaml_document_get_node(doc, *i);
            if (!im || im->type != YAML_MAPPING_NODE)
                continue;
            for (yaml_node_pair_t *p = im->data.mapping.pairs.start;
                 p < im->data.mapping.pairs.top; p++) {
                const char *k = ynode_scalar(yaml_document_get_node(doc, p->key));
                const char *v = ynode_scalar(yaml_document_get_node(doc, p->value));
                if (!k)
                    continue;
                codec_item_t *ci = g_new0(codec_item_t, 1);
                ci->key   = k;
                ci->value = v ? v : "";
                items = g_list_append(items, ci);
            }
        }
    }

    GByteArray *bytes = g_byte_array_new();
    if (codec_encode(c, items, bytes) == 0 && bytes->len > 0)
        usb_node_set_prop(out, create_prop_array(key, bytes->data, bytes->len));
    else
        LOG_WARN("yaml_loader: codec '%s' failed to encode '%s'", cname, key);

    g_byte_array_free(bytes, TRUE);
    g_list_free_full(items, g_free);
    return TRUE;
}

/* ── Recursive tree builder (libyaml document API) ──────────────────────
 * The whole file is loaded into a yaml_document_t, then walked recursively:
 * mapping -> usb_node, sequence -> repeated props under the key, scalar ->
 * leaf property. `ns` collects a schema's `namespace:`. */
static void build_map(yaml_document_t *doc, yaml_node_t *map,
                      yaml_device_t *dev, usb_node_t *out, char **ns,
                      int depth);

/* Attach a scalar `key: val` onto `out`, choosing the property kind from the
 * value (namespace / template-ref / null / integer / ignored string). */
static void build_scalar(yaml_device_t *dev, usb_node_t *out,
                         const char *key, const char *val, char **ns)
{
    if (strcmp(key, "namespace") == 0) {
        g_free(*ns);
        *ns = g_strdup(val);
        return;
    }

    /* A device node's base: fold the named schema template in first, so the
     * device's own later fields override it. */
    if (strcmp(key, "schema") == 0 || strcmp(key, "template") == 0) {
        usb_node_t *base = dev->templates
                         ? g_hash_table_lookup(dev->templates, val) : NULL;
        if (base)
            usb_node_merge(out, base);
        else
            LOG_WARN("yaml_loader: unknown schema/template '%s'", val);
        return;
    }

    if (scalar_is_null(val)) {
        usb_node_set_prop(out, create_prop_null(key));
        return;
    }

    uint32_t n;
    if (parse_uint(val, &n)) {
        if (width_for_key(key) == 2)
            usb_node_set_prop(out, create_prop_u16(key, (uint16_t) n));
        else
            usb_node_set_prop(out, create_prop_u8(key, (uint8_t) n));
        return;
    }

    /* Non-numeric scalar (HID report item, raw_file, enum name): not a
     * descriptor field. Left for a later encoding pass. */
    LOG_DEBUG("[Builder] skip non-numeric %s: %s", key, val);
}

/* Fold the value node of `key` into `out`. `depth` guards against a
 * maliciously deep document exhausting the C stack. */
static void build_value(yaml_document_t *doc, const char *key,
                        yaml_node_t *v, yaml_device_t *dev,
                        usb_node_t *out, char **ns, int depth)
{
    if (depth > YAML_MAX_DEPTH) {
        LOG_WARN("yaml_loader: nesting past %d levels, truncating '%s'",
                 YAML_MAX_DEPTH, key);
        return;
    }

    switch (v->type) {
    case YAML_SCALAR_NODE:
        build_scalar(dev, out, key, (const char *) v->data.scalar.value, ns);
        break;

    case YAML_MAPPING_NODE: {
        /* A `codec:`-tagged block encodes to a single byte array, not a
         * subtree of per-item nodes. */
        if (build_codec_block(doc, v, dev, out, key))
            break;

        /* Reuse an existing same-named node (e.g. one folded in from a schema
         * template) so nested fields override into it rather than discarding
         * the template's other fields. */
        usb_prop_t *ex = find_prop(out, key);
        usb_node_t *child;
        if (ex && ex->type == USB_PROP_NODE) {
            child = ex->val.node;
        } else {
            child = usb_node_create();
            usb_node_set_prop(out, create_prop_node(key, child));
        }
        build_map(doc, v, dev, child, ns, depth + 1);
        break;
    }

    case YAML_SEQUENCE_NODE:
        /* Each item becomes a distinct sibling under the same key. */
        for (yaml_node_item_t *i = v->data.sequence.items.start;
             i < v->data.sequence.items.top; i++) {
            yaml_node_t *item = yaml_document_get_node(doc, *i);
            if (!item)
                continue;
            if (item->type == YAML_MAPPING_NODE) {
                usb_node_t *child = usb_node_create();
                out->props = g_list_append(out->props,
                                           create_prop_node(key, child));
                build_map(doc, item, dev, child, ns, depth + 1);
            } else {
                build_value(doc, key, item, dev, out, ns, depth + 1);
            }
        }
        break;

    default:
        break;
    }
}

static void build_map(yaml_document_t *doc, yaml_node_t *map,
                      yaml_device_t *dev, usb_node_t *out, char **ns,
                      int depth)
{
    if (map->type != YAML_MAPPING_NODE)
        return;   /* depth is gated in build_value before recursing here */

    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = yaml_document_get_node(doc, p->key);
        yaml_node_t *v = yaml_document_get_node(doc, p->value);
        if (!k || !v || k->type != YAML_SCALAR_NODE)
            continue;
        build_value(doc, (const char *) k->data.scalar.value, v, dev, out, ns,
                    depth);
    }
}

/* Load `path` into a usb_node tree. On success returns the root node (caller
 * owns it) and, for schemas, sets *ns to the file's namespace. NULL on error. */
static usb_node_t *load_tree(const char *path, yaml_device_t *dev, char **ns)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_ERR("Unable to open file '%s'", path);
        return NULL;
    }

    /* Reject oversized input before handing it to libyaml (bounds memory even
     * for alias-expanded documents). */
    if (fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        if (sz > (long) YAML_MAX_FILE_BYTES) {
            LOG_ERR("yaml_loader: '%s' is %ld bytes, over %u limit",
                    path, sz, YAML_MAX_FILE_BYTES);
            fclose(fp);
            return NULL;
        }
        rewind(fp);
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(fp);
        return NULL;
    }
    yaml_parser_set_input_file(&parser, fp);

    yaml_document_t doc;
    if (!yaml_parser_load(&parser, &doc)) {
        LOG_ERR("YAML error(%lu, %lu): %s",
                (unsigned long) parser.problem_mark.line + 1,
                (unsigned long) parser.problem_mark.column + 1,
                parser.problem);
        yaml_parser_delete(&parser);
        fclose(fp);
        return NULL;
    }

    usb_node_t *root = usb_node_create();
    yaml_node_t *top = yaml_document_get_root_node(&doc);
    if (top)
        build_map(&doc, top, dev, root, ns, 0);

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    return root;
}


yaml_scope_t yaml_scope_from_path(const char *path)
{
    if (!path)
        return YAML_SCOPE_UNKNOWN;

    /* Scope is named by the file's immediate parent directory. */
    char *dir  = g_path_get_dirname(path);
    char *base = g_path_get_basename(dir);
    yaml_scope_t scope = YAML_SCOPE_UNKNOWN;
    for (size_t i = 0; i < G_N_ELEMENTS(scope_by_dir); i++)
        if (strcmp(base, scope_by_dir[i].name) == 0) {
            scope = scope_by_dir[i].scope;
            break;
        }
    g_free(base);
    g_free(dir);
    return scope;
}


/* ── per-scope parsers ──────────────────────────────────────────────────
 * Each consumes one file and folds it into `dev`. Return 0 on success, -1 on
 * parse error. */

/* Schemas: build the tree, then register each top-level descriptor node as a
 * template under "<namespace>.<name>" for devices to import. */
static int parse_schemas(const char *path, yaml_device_t *dev)
{
    char *ns = NULL;
    usb_node_t *root = load_tree(path, dev, &ns);
    if (!root) {
        g_free(ns);
        return -1;
    }

    for (GList *l = root->props; l; l = l->next) {
        usb_prop_t *p = l->data;
        if (p->type != USB_PROP_NODE)
            continue;   /* skip the `namespace` scalar and other metadata */

        char *key = g_strdup_printf("%s.%s", ns ? ns : "", p->name);
        g_hash_table_insert(dev->templates, key, usb_node_clone(p->val.node));
        LOG_DEBUG("[Schema] registered template %s", key);
    }

    /* Registry owns clones; drop the working tree. */
    usb_node_free(root);
    g_free(ns);
    return 0;
}

/* Codecs: parse each table (codecs/<name>.yaml) and register it under its
 * `codec:` name for devices to reference from a `report_descriptor`
 * (README §6). */
static int parse_codecs(const char *path, yaml_device_t *dev)
{
    codec_t *c = codec_load(path);
    if (!c)
        return -1;

    const char *name = codec_name(c);   /* borrowed */
    g_hash_table_insert(dev->codecs, g_strdup(name ? name : ""), c);
    LOG_DEBUG("[Codec] registered '%s' from %s", name ? name : "", path);
    return 0;
}

static int parse_tests(const char *path, yaml_device_t *dev)
{
    (void)path; (void)dev;
    return 0; /* TODO: build ValidationSuite (README §3.4) */
}

static void dev_entry_free(gpointer p)
{
    yaml_dev_entry_t *e = p;
    if (!e)
        return;
    g_free(e->name);
    g_free(e->flow_path);
    usb_node_free(e->tree);
    /* TODO (step 4): flow_free(e->flow); */
    g_free(e);
}

/* Project file: read the `devices:` manifest — a sequence of {device, flow}
 * pairs — and load each device tree (schema templates and codec tables must be
 * loaded first). Paths are resolved relative to the project dir. The flow is
 * recorded by path only; building the Flow model is step 4. A missing project
 * file is not an error (no devices). */
static int parse_main(const char *path, yaml_device_t *dev)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        LOG_DEBUG("[Manifest] no project file '%s'", path);
        return 0;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(fp);
        return -1;
    }
    yaml_parser_set_input_file(&parser, fp);

    yaml_document_t doc;
    if (!yaml_parser_load(&parser, &doc)) {
        LOG_ERR("YAML error(%lu, %lu): %s",
                (unsigned long) parser.problem_mark.line + 1,
                (unsigned long) parser.problem_mark.column + 1,
                parser.problem);
        yaml_parser_delete(&parser);
        fclose(fp);
        return -1;
    }

    int   rc  = 0;
    char *dir = g_path_get_dirname(path);
    yaml_node_t *root = yaml_document_get_root_node(&doc);
    yaml_node_t *devs = ymap_get(&doc, root, "devices");

    if (!devs || devs->type != YAML_SEQUENCE_NODE) {
        LOG_WARN("yaml_loader: project '%s' has no devices: manifest", path);
    } else {
        for (yaml_node_item_t *i = devs->data.sequence.items.start;
             rc == 0 && i < devs->data.sequence.items.top; i++) {
            yaml_node_t *item = yaml_document_get_node(&doc, *i);
            if (!item || item->type != YAML_MAPPING_NODE)
                continue;

            const char *dref = ynode_scalar(ymap_get(&doc, item, "device"));
            const char *fref = ynode_scalar(ymap_get(&doc, item, "flow"));
            if (!dref) {
                LOG_WARN("yaml_loader: manifest entry with no device:, skipping");
                continue;
            }

            char *dpath = g_build_filename(dir, dref, NULL);
            char *ns = NULL;
            usb_node_t *tree = load_tree(dpath, dev, &ns);
            g_free(ns);
            if (!tree) {
                LOG_ERR("yaml_loader: failed to load device '%s'", dpath);
                g_free(dpath);
                rc = -1;
                break;
            }

            yaml_dev_entry_t *e = g_new0(yaml_dev_entry_t, 1);
            e->name      = g_strdup(dref);
            e->flow_path = fref ? g_build_filename(dir, fref, NULL) : NULL;
            e->tree      = tree;   /* entry owns it */
            e->flow      = NULL;   /* built in step 4 from flow_path */
            dev->devices = g_list_append(dev->devices, e);
            LOG_DEBUG("[Manifest] device '%s' flow '%s'",
                      dref, fref ? fref : "(none)");

            g_free(dpath);
        }
    }

    /* Alias the first entry so single-device callers keep working. */
    if (dev->devices) {
        yaml_dev_entry_t *first = dev->devices->data;
        dev->tree = first->tree;   /* alias, not owned */
        dev->flow = first->flow;
    }

    g_free(dir);
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    return rc;
}

/* Parse every *.yaml in <root>/<sub>, asserting each file's scope is `want`.
 * A missing directory is not an error (return 0); a scope mismatch is. */
static int load_scope_dir(const char *root, const char *sub,
                          yaml_scope_t want, yaml_device_t *dev)
{
    char *dirpath = g_build_filename(root, sub, NULL);
    GDir *d = g_dir_open(dirpath, 0, NULL);   /* NULL: dir may not exist */
    int rc = 0;

    if (d) {
        const char *name;
        while (rc == 0 && (name = g_dir_read_name(d))) {
            if (!g_str_has_suffix(name, ".yaml"))
                continue;

            char *path = g_build_filename(dirpath, name, NULL);

            /* The path carries its subdir, so the scope is derivable and a
             * misfiled file is caught before parsing (determinism). */
            if (yaml_scope_from_path(path) != want) {
                LOG_WARN("yaml_loader: scope mismatch for %s", path);
                rc = -1;
            } else {
                /* Only shared, dir-scanned scopes reach here; devices and flows
                 * come from the project `devices:` manifest (see parse_main). */
                switch (want) {
                    case YAML_SCOPE_SCHEMAS:
                        rc = parse_schemas(path, dev);
                        break;
                    case YAML_SCOPE_CODECS:
                        rc = parse_codecs(path, dev);
                        break;
                    case YAML_SCOPE_TESTS:
                        rc = parse_tests(path, dev);
                        break;
                    default:
                        rc = -1;
                        break;
                }
            }
            g_free(path);
        }
        g_dir_close(d);
    }

    g_free(dirpath);
    return rc;
}

yaml_device_t *yaml_device_create(void)
{
    yaml_device_t *dev = g_new0(yaml_device_t, 1);

    /* tree/flow start NULL: they alias the first manifest entry once loaded. */
    dev->templates = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free,
                                           (GDestroyNotify) usb_node_free);
    dev->codecs    = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free,
                                           (GDestroyNotify) codec_free);

    return dev;
}

yaml_device_t *yaml_parse_from_file(const char *path)
{
    if (!path)
        return NULL;

    char *dir = g_path_get_dirname(path);
    yaml_device_t *dev = yaml_device_create();

    /* Shared resources first (dir-scanned): schema templates + codec tables.
     * Then the project `devices:` manifest, which loads each device tree using
     * them. tests/ is still dir-scanned (stub). */
    if (load_scope_dir(dir, "schemas", YAML_SCOPE_SCHEMAS, dev) < 0 ||
        load_scope_dir(dir, "codecs",  YAML_SCOPE_CODECS,  dev) < 0 ||
        parse_main(path, dev) < 0 ||
        load_scope_dir(dir, "tests",   YAML_SCOPE_TESTS,   dev) < 0) {

        yaml_device_free(dev);
        g_free(dir);
        return NULL;
    }

    g_free(dir);
    return dev;
}

void yaml_device_free(yaml_device_t *dev)
{
    if (!dev)
        return;
    /* Entries own their trees/flows; dev->tree/flow are just aliases. */
    g_list_free_full(dev->devices, dev_entry_free);
    if (dev->templates)
        g_hash_table_destroy(dev->templates);
    if (dev->codecs)
        g_hash_table_destroy(dev->codecs);
    g_free(dev);
}
