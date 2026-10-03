/*
 * codec.c – Generic table-driven encoder (README §6).
 *
 * codec_load parses a codecs/<name>.yaml table into lookup tables; codec_encode
 * (phase 2) turns a symbolic item stream into bytes using them. No per-class C:
 * all descriptor knowledge lives in the data table.
 *
 * Encoder contract (README §6.2, implemented in codec_encode):
 *   1. lookup key in tokens -> (type, tag, width, flagset)
 *   2. resolve value: number as-is; string via symbols[key];
 *      comma-list OR-folded via flagsets
 *   3. width:auto -> pick 0/1/2/4 bytes by magnitude -> bSize code
 *   4. emit prefix (tag<<4 | type<<2 | bSize) then operand little-endian
 */

#include "codec.h"
#include "yaml/util.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <yaml.h>

/* Codec tables are small; reject pathological input up front (fuzzer harness). */
#define CODEC_MAX_FILE_BYTES (1u * 1024u * 1024u)   /* 1 MiB */

/* One entry of the table's `tokens:` map: how to emit an item with this key. */
typedef struct {
    guint8   type;      /* item_types[...] resolved: main/global/local */
    guint8   tag;       /* HID short-item tag nibble (0x0..0xF) */
    int      width;     /* -1 = auto (pick by magnitude), else 0/1/2/4 bytes */
    gboolean is_signed; /* value is two's-complement (affects auto width) */
    char    *flagset;   /* name into `flagsets`, or NULL */
} codec_token_t;

struct codec {
    char *codec_name;      /* value of table `codec:` field */
    GHashTable *tokens;    /* key(char*)  -> codec_token_t*              */
    GHashTable *symbols;   /* key(char*)  -> GHashTable(name -> GUINT)   */
    GHashTable *flagsets;  /* set(char*)  -> GHashTable(flag -> GUINT)   */
};


/* Parse a flat `name: uint` mapping into a new GHashTable(char* -> GUINT).
 * Values accept decimal or 0x-hex (g_ascii_strtoull base 0). */
static GHashTable *parse_uint_map(yaml_document_t *doc, yaml_node_t *map)
{
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!map || map->type != YAML_MAPPING_NODE)
        return h;

    for (yaml_node_pair_t *p = map->data.mapping.pairs.start;
         p < map->data.mapping.pairs.top; p++) {
        const char *name = yaml_scalar(yaml_document_get_node(doc, p->key));
        const char *val  = yaml_scalar(yaml_document_get_node(doc, p->value));
        if (!name || !val)
            continue;
        guint v = (guint) g_ascii_strtoull(val, NULL, 0);
        g_hash_table_insert(h, g_strdup(name), GUINT_TO_POINTER(v));
    }
    return h;
}

/* Parse one token spec (a `tokens:` value mapping). `item_types` resolves the
 * symbolic `type:` name to its numeric item-type code. */
static codec_token_t *parse_token(yaml_document_t *doc, yaml_node_t *tmap,
                                  GHashTable *item_types)
{
    codec_token_t *t = g_new0(codec_token_t, 1);

    const char *type = yaml_scalar(yaml_map_get(doc, tmap, "type"));
    if (type && g_hash_table_contains(item_types, type))
        t->type = (guint8) GPOINTER_TO_UINT(g_hash_table_lookup(item_types, type));
    else
        LOG_WARN("codec: token has unknown item type '%s'", type ? type : "(none)");

    const char *tag = yaml_scalar(yaml_map_get(doc, tmap, "tag"));
    t->tag = tag ? (guint8) g_ascii_strtoull(tag, NULL, 0) : 0;

    const char *w = yaml_scalar(yaml_map_get(doc, tmap, "width"));
    if (!w || strcmp(w, "auto") == 0)
        t->width = -1;
    else
        t->width = (int) g_ascii_strtoull(w, NULL, 0);

    const char *sg = yaml_scalar(yaml_map_get(doc, tmap, "signed"));
    t->is_signed = sg && (strcmp(sg, "true") == 0 || strcmp(sg, "1") == 0);

    const char *fs = yaml_scalar(yaml_map_get(doc, tmap, "flagset"));
    t->flagset = fs ? g_strdup(fs) : NULL;

    return t;
}

static void codec_token_free(gpointer p)
{
    codec_token_t *t = p;
    if (!t)
        return;
    g_free(t->flagset);
    g_free(t);
}

/* Populate `dst` (key -> parse_uint_map(value)) from a map-of-maps node
 * (used for both `symbols:` and `flagsets:`). */
static void parse_nested_maps(yaml_document_t *doc, yaml_node_t *outer,
                              GHashTable *dst)
{
    if (!outer || outer->type != YAML_MAPPING_NODE)
        return;
    for (yaml_node_pair_t *p = outer->data.mapping.pairs.start;
         p < outer->data.mapping.pairs.top; p++) {
        const char *key = yaml_scalar(yaml_document_get_node(doc, p->key));
        yaml_node_t *inner = yaml_document_get_node(doc, p->value);
        if (!key)
            continue;
        g_hash_table_insert(dst, g_strdup(key), parse_uint_map(doc, inner));
    }
}

codec_t *codec_load(const char *table_path)
{
    FILE *fp = fopen(table_path, "r");
    if (!fp) {
        LOG_ERR("codec: unable to open '%s'", table_path);
        return NULL;
    }

    /* Bound input before handing it to libyaml. */
    if (fseek(fp, 0, SEEK_END) == 0) {
        long sz = ftell(fp);
        if (sz > (long) CODEC_MAX_FILE_BYTES) {
            LOG_ERR("codec: '%s' is %ld bytes, over %u limit",
                    table_path, sz, CODEC_MAX_FILE_BYTES);
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
        LOG_ERR("codec: YAML error(%lu, %lu): %s",
                (unsigned long) parser.problem_mark.line + 1,
                (unsigned long) parser.problem_mark.column + 1,
                parser.problem);
        yaml_parser_delete(&parser);
        fclose(fp);
        return NULL;
    }

    codec_t *c = NULL;
    yaml_node_t *root = yaml_document_get_root_node(&doc);

    if (!root || root->type != YAML_MAPPING_NODE) {
        LOG_ERR("codec: '%s' has no top-level mapping", table_path);
    } else {
        c = g_new0(codec_t, 1);
        const char *name = yaml_scalar(yaml_map_get(&doc, root, "codec"));
        c->codec_name = g_strdup(name ? name : "");
        c->tokens   = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, codec_token_free);
        c->symbols  = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, (GDestroyNotify) g_hash_table_destroy);
        c->flagsets = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, (GDestroyNotify) g_hash_table_destroy);

        /* item_types only resolves token `type:` names; scoped to this load. */
        GHashTable *item_types =
            parse_uint_map(&doc, yaml_map_get(&doc, root, "item_types"));

        yaml_node_t *tokens = yaml_map_get(&doc, root, "tokens");
        if (tokens && tokens->type == YAML_MAPPING_NODE) {
            for (yaml_node_pair_t *p = tokens->data.mapping.pairs.start;
                 p < tokens->data.mapping.pairs.top; p++) {
                const char *key = yaml_scalar(yaml_document_get_node(&doc, p->key));
                yaml_node_t *tmap = yaml_document_get_node(&doc, p->value);
                if (!key)
                    continue;
                g_hash_table_insert(c->tokens, g_strdup(key),
                                    parse_token(&doc, tmap, item_types));
            }
        }

        parse_nested_maps(&doc, yaml_map_get(&doc, root, "symbols"),  c->symbols);
        parse_nested_maps(&doc, yaml_map_get(&doc, root, "flagsets"), c->flagsets);

        g_hash_table_destroy(item_types);
        LOG_DEBUG("[Codec] loaded table '%s' (%u tokens, %u symbol maps, "
                  "%u flagsets)", c->codec_name,
                  g_hash_table_size(c->tokens),
                  g_hash_table_size(c->symbols),
                  g_hash_table_size(c->flagsets));
    }

    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    return c;
}

const char *codec_name(const codec_t *codec)
{
    return codec ? codec->codec_name : NULL;
}

void codec_free(codec_t *codec)
{
    if (!codec)
        return;
    g_free(codec->codec_name);
    if (codec->tokens)
        g_hash_table_destroy(codec->tokens);
    if (codec->symbols)
        g_hash_table_destroy(codec->symbols);
    if (codec->flagsets)
        g_hash_table_destroy(codec->flagsets);
    g_free(codec);
}

/* HID short items carry 0/1/2/4 operand bytes; bSize encodes that as 0/1/2/3. */
static int size_code(int bytes)
{
    switch (bytes) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return 2;
    case 4:  return 3;
    default: return 1;   /* unreachable for well-formed widths */
    }
}

/* Smallest of 1/2/4 bytes that holds an unsigned value (0 => 1 byte). */
static int auto_bytes_u(guint64 v)
{
    if (v <= 0xFF)   return 1;
    if (v <= 0xFFFF) return 2;
    return 4;
}

/* Smallest of 1/2/4 bytes that holds a two's-complement signed value. */
static int auto_bytes_s(gint64 v)
{
    if (v >= -128 && v <= 127)     return 1;
    if (v >= -32768 && v <= 32767) return 2;
    return 4;
}

/* Comma-list of flags OR-folded via the named flagset. */
static gboolean resolve_flagset_value(codec_t *codec, const codec_token_t *tok,
                                      const char *key, const char *val,
                                      guint64 *uv, gint64 *sv)
{
    GHashTable *fs = g_hash_table_lookup(codec->flagsets, tok->flagset);
    if (!fs) {
        LOG_ERR("codec: token '%s' names unknown flagset '%s'",
                key, tok->flagset);
        return FALSE;
    }
    guint64 v = 0;
    gchar **parts = g_strsplit(val, ",", -1);
    for (gchar **p = parts; *p; p++) {
        gchar *f = g_strstrip(*p);
        if (*f == '\0')
            continue;
        if (!g_hash_table_contains(fs, f)) {
            LOG_ERR("codec: unknown flag '%s' for token '%s'", f, key);
            g_strfreev(parts);
            return FALSE;
        }
        v |= GPOINTER_TO_UINT(g_hash_table_lookup(fs, f));
    }
    g_strfreev(parts);
    *uv = v;
    *sv = (gint64) v;
    return TRUE;
}

/* Resolve one item's value to a raw magnitude. Sets *uv (bytes emitted from
 * this) and *sv (signed view, for auto-width). Returns FALSE on hard error. */
static gboolean resolve_value(codec_t *codec, const codec_token_t *tok,
                              const char *key, const char *val,
                              guint64 *uv, gint64 *sv)
{
    if (tok->flagset)
        return resolve_flagset_value(codec, tok, key, val, uv, sv);

    /* Plain integer (numeric fallback always allowed, README §6.4). */
    char *end = NULL;
    if (tok->is_signed) {
        gint64 n = g_ascii_strtoll(val, &end, 0);
        if (end && end != val && *end == '\0') {
            *sv = n;
            *uv = (guint64) n;
            return TRUE;
        }
    } else {
        guint64 n = g_ascii_strtoull(val, &end, 0);
        if (end && end != val && *end == '\0') {
            *uv = n;
            *sv = (gint64) n;
            return TRUE;
        }
    }

    /* Symbolic name via symbols[key]. */
    GHashTable *sym = g_hash_table_lookup(codec->symbols, key);
    if (!sym || !g_hash_table_contains(sym, val)) {
        LOG_ERR("codec: unknown symbol '%s' for token '%s'", val, key);
        return FALSE;
    }
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(sym, val));
    *uv = n;
    *sv = (gint64) n;
    return TRUE;
}

int codec_encode(codec_t *codec, const GList *items, GByteArray *out)
{
    if (!codec || !out)
        return -1;

    for (const GList *l = items; l; l = l->next) {
        const codec_item_t *it = l->data;
        if (!it || !it->key)
            continue;

        codec_token_t *tok = g_hash_table_lookup(codec->tokens, it->key);
        if (!tok) {
            LOG_ERR("codec: unknown token '%s'", it->key);
            return -1;
        }

        /* Zero-width items (e.g. end_collection) are prefix-only. */
        if (tok->width == 0) {
            guint8 prefix = (guint8) ((tok->tag << 4) | (tok->type << 2) | 0);
            g_byte_array_append(out, &prefix, 1);
            continue;
        }

        guint64 uv = 0;
        gint64  sv = 0;
        if (!resolve_value(codec, tok, it->key,
                           it->value ? it->value : "", &uv, &sv))
            return -1;

        int bytes = (tok->width >= 0)
                  ? tok->width
                  : (tok->is_signed ? auto_bytes_s(sv) : auto_bytes_u(uv));

        guint8 prefix = (guint8) ((tok->tag << 4) | (tok->type << 2)
                                  | size_code(bytes));
        g_byte_array_append(out, &prefix, 1);
        for (int i = 0; i < bytes; i++) {
            guint8 b = (guint8) ((uv >> (8 * i)) & 0xFF);
            g_byte_array_append(out, &b, 1);
        }
    }
    return 0;
}
