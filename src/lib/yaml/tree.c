/*
 * yaml/tree.c – Canonical-dialect YAML -> usb_node_t descriptor tree.
 *
 * The whole file is loaded into a yaml_document_t, then walked recursively:
 * mapping -> usb_node, sequence -> repeated props under the key, scalar ->
 * leaf property. Schema/template scalars fold a named base in first; a
 * `codec:`-tagged block encodes to a single byte array (README §6).
 */

#include "yaml/tree.h"
#include "yaml/util.h"
#include "codec.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* Input bounds. These files describe USB descriptors (a few KB) and may be
 * untrusted (fuzzer harness), so reject pathological input up front rather
 * than let a giant or deeply-nested document exhaust memory / the C stack. */
#define YAML_MAX_FILE_BYTES (1u * 1024u * 1024u)   /* 1 MiB */
#define YAML_MAX_DEPTH      32                      /* dev>cfg>iface>ep is shallow */


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


/* Handle a codec block:
 *   report_descriptor:
 *     codec: "hid_report"
 *     items: [ {usage_page: ...}, {usage: ...}, ... ]
 * Encode the ordered items via the named table into one USB_PROP_ARRAY under
 * `key`. Returns TRUE if `map` was a codec block (handled or failed-and-
 * consumed), FALSE if it has no `codec:` field (caller falls back to generic). */
static gboolean build_codec_block(yaml_document_t *doc, yaml_node_t *map,
                                  GHashTable *codecs, usb_node_t *out,
                                  const char *key)
{
    const char *cname = yaml_scalar(yaml_map_get(doc, map, "codec"));
    if (!cname)
        return FALSE;   /* not a codec block */

    codec_t *c = codecs ? g_hash_table_lookup(codecs, cname) : NULL;
    if (!c) {
        LOG_WARN("yaml_tree: unknown codec '%s' for '%s'", cname, key);
        return TRUE;    /* consumed: don't expand items generically */
    }

    /* Build the ordered item list; codec_item_t borrows the yaml scalar strings
     * (valid until the document is deleted, well after codec_encode). */
    GList *items = NULL;
    yaml_node_t *seq = yaml_map_get(doc, map, "items");
    if (seq && seq->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *i = seq->data.sequence.items.start;
             i < seq->data.sequence.items.top; i++) {
            yaml_node_t *im = yaml_document_get_node(doc, *i);
            if (!im || im->type != YAML_MAPPING_NODE)
                continue;
            for (yaml_node_pair_t *p = im->data.mapping.pairs.start;
                 p < im->data.mapping.pairs.top; p++) {
                const char *k = yaml_scalar(yaml_document_get_node(doc, p->key));
                const char *v = yaml_scalar(yaml_document_get_node(doc, p->value));
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
        LOG_WARN("yaml_tree: codec '%s' failed to encode '%s'", cname, key);

    g_byte_array_free(bytes, TRUE);
    g_list_free_full(items, g_free);
    return TRUE;
}

/* Handle a raw byte block:
 *   bos_descriptor:
 *     hex: "050f16000207100202000000 0a1003000e00010aff00"
 * Emit the decoded bytes verbatim as one USB_PROP_ARRAY under `key`. This is
 * the escape hatch (README §3.2) for descriptors whose field widths don't
 * follow the name-prefix convention (e.g. BOS device-capability bmAttributes is
 * 4 bytes). Whitespace between bytes is ignored. Returns TRUE if `map` was a
 * hex block (FALSE if it has no `hex:` field, so the caller falls back to the
 * generic subtree builder). */
static gboolean build_raw_block(yaml_document_t *doc, yaml_node_t *map,
                                usb_node_t *out, const char *key)
{
    const char *hex = yaml_scalar(yaml_map_get(doc, map, "hex"));
    if (!hex)
        return FALSE;   /* not a raw block */

    GByteArray *bytes = g_byte_array_new();
    int hi = -1;
    for (const char *p = hex; *p; p++) {
        if (g_ascii_isspace(*p))
            continue;
        if (!g_ascii_isxdigit(*p)) {
            LOG_WARN("yaml_tree: non-hex char in '%s' hex block", key);
            continue;
        }
        int nib = g_ascii_xdigit_value(*p);
        if (hi < 0) {
            hi = nib;
        } else {
            guint8 b = (guint8) ((hi << 4) | nib);
            g_byte_array_append(bytes, &b, 1);
            hi = -1;
        }
    }
    if (hi >= 0)
        LOG_WARN("yaml_tree: '%s' hex block has odd digit count", key);

    if (bytes->len > 0)
        usb_node_set_prop(out, create_prop_array(key, bytes->data, bytes->len));

    g_byte_array_free(bytes, TRUE);
    return TRUE;
}

/* ── Recursive tree builder (libyaml document API) ────────────────────── */
static void build_map(yaml_document_t *doc, yaml_node_t *map,
                      schema_t *schemas, GHashTable *codecs,
                      usb_node_t *out, char **ns, int depth);

/* Attach a scalar `key: val` onto `out`, choosing the property kind from the
 * value (namespace / template-ref / null / integer / ignored string). */
static void build_scalar(schema_t *schemas, usb_node_t *out,
                         const char *key, const char *val, char **ns)
{
    uint32_t n;

    if (strcmp(key, "namespace") == 0) {
        g_free(*ns);
        *ns = g_strdup(val);
        return;
    }

    /* A device node's base: fold the named schema template in first, so the
     * device's own later fields override it. */
    if (strcmp(key, "schema") == 0 || strcmp(key, "template") == 0) {
        usb_node_t *base = schema_lookup(schemas, val);
        if (base)
            usb_node_merge(out, base);
        else
            LOG_WARN("yaml_tree: unknown schema/template '%s'", val);
        return;
    }

    if (scalar_is_null(val)) {
        usb_node_set_prop(out, create_prop_null(key));
        return;
    }

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
                        yaml_node_t *v, schema_t *schemas,
                        GHashTable *codecs, usb_node_t *out, char **ns,
                        int depth)
{
    if (depth > YAML_MAX_DEPTH) {
        LOG_WARN("yaml_tree: nesting past %d levels, truncating '%s'",
                 YAML_MAX_DEPTH, key);
        return;
    }

    switch (v->type) {
    case YAML_SCALAR_NODE:
        build_scalar(schemas, out, key,
                     (const char *) v->data.scalar.value, ns);
        break;

    case YAML_MAPPING_NODE: {
        /* A `hex:` block is raw bytes; a `codec:`-tagged block encodes to a
         * single byte array. Either becomes one leaf, not a subtree. */
        if (build_raw_block(doc, v, out, key))
            break;
        if (build_codec_block(doc, v, codecs, out, key))
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
        build_map(doc, v, schemas, codecs, child, ns, depth + 1);
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
                build_map(doc, item, schemas, codecs, child, ns, depth + 1);
            } else {
                build_value(doc, key, item, schemas, codecs, out, ns,
                            depth + 1);
            }
        }
        break;

    default:
        break;
    }
}

static void build_map(yaml_document_t *doc, yaml_node_t *map,
                      schema_t *schemas, GHashTable *codecs,
                      usb_node_t *out, char **ns, int depth)
{
    if (map->type != YAML_MAPPING_NODE)
        return;   /* depth is gated in build_value before recursing here */

    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        yaml_node_t *k = yaml_document_get_node(doc, p->key);
        yaml_node_t *v = yaml_document_get_node(doc, p->value);
        if (!k || !v || k->type != YAML_SCALAR_NODE)
            continue;
        build_value(doc, (const char *) k->data.scalar.value, v,
                    schemas, codecs, out, ns, depth);
    }
}

usb_node_t *yaml_tree_load(const char *path, schema_t *schemas,
                           GHashTable *codecs, char **ns_out)
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
            LOG_ERR("yaml_tree: '%s' is %ld bytes, over %u limit",
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
        build_map(&doc, top, schemas, codecs, root, ns_out, 0);

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    return root;
}
