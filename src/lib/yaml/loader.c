/*
 * yaml/loader.c – Canonical-dialect project loader.
 *
 * Orchestration only: resolve a project's scopes (schemas/ codecs/ tests/)
 * and its `devices:` manifest, delegating the YAML->usb_node build to
 * yaml/tree.{c,h} and codec tables to codec.{c,h}.
 *
 * TODO:
 *   - autofill null length/count fields (README §3.1)
 *   - build ValidationSuite from tests/ (README §3.4)
 */

#include "yaml/loader.h"
#include "flow.h"
#include "yaml/tree.h"
#include "yaml/util.h"
#include "codec.h"
#include "log.h"
#include "version.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <yaml.h>


typedef struct {
    const char *name;
    yaml_scope_t scope;
} yaml_scope_entry_t;

/* Directory name -> scope. The directory is authoritative; the scope is
 * never stated in-file. */
static const yaml_scope_entry_t scope_by_dir[] = {
    { "schemas", YAML_SCOPE_SCHEMAS },
    { "codecs",  YAML_SCOPE_CODECS  },
    { "devices", YAML_SCOPE_DEVICES },
    { "flows",   YAML_SCOPE_FLOWS   },
    { "tests",   YAML_SCOPE_TESTS   },
};

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
    usb_node_t *root = yaml_tree_load(path, dev->schemas, dev->codecs, &ns);
    if (!root) {
        g_free(ns);
        return -1;
    }

    schema_register(dev->schemas, root, ns);

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

/* Flows: parse each behavior flow (flows/<name>.yaml) into dev->flows. */
static int parse_flows(const char *path, yaml_device_t *dev)
{
    char *dir;
    flow_t *f;
    char *base;
    char *name;

    dir = g_path_get_dirname(path);
    f = flow_load_with_dir(path, dir);
    g_free(dir);
    if (!f) {
        LOG_ERR("yaml_loader: failed to load flow '%s'", path);
        return -1;
    }

    base = g_path_get_basename(path);
    name = g_strdup(base);
    if (g_str_has_suffix(name, ".yaml")) {
        name[strlen(name) - 5] = '\0';
    } else if (g_str_has_suffix(name, ".yml")) {
        name[strlen(name) - 4] = '\0';
    }

    LOG_DEBUG("[Flow] registered '%s' from %s", name, path);
    g_hash_table_insert(dev->flows, name, f);
    g_free(base);
    return 0;
}

void *yaml_device_find_flow(yaml_device_t *dev, const char *ref)
{
    flow_t *f;
    char *base;

    if (!dev || !dev->flows || !ref) {
        return NULL;
    }

    /* 1. Direct lookup */
    f = g_hash_table_lookup(dev->flows, ref);
    if (f) {
        return f;
    }

    /* 2. Lookup by basename without extension */
    base = g_path_get_basename(ref);
    if (g_str_has_suffix(base, ".yaml")) {
        base[strlen(base) - 5] = '\0';
    } else if (g_str_has_suffix(base, ".yml")) {
        base[strlen(base) - 4] = '\0';
    }

    f = g_hash_table_lookup(dev->flows, base);
    g_free(base);
    return f;
}

static flow_t *find_flow_for_device(yaml_device_t *dev, const char *dref)
{
    flow_t *f = NULL;

    if (!dev || !dev->flows) {
        return NULL;
    }

    if (dref) {
        char *dbase = g_path_get_basename(dref);
        char *with_flow;

        if (g_str_has_suffix(dbase, ".yaml")) {
            dbase[strlen(dbase) - 5] = '\0';
        } else if (g_str_has_suffix(dbase, ".yml")) {
            dbase[strlen(dbase) - 4] = '\0';
        }

        /* Try exact device name as flow name */
        f = g_hash_table_lookup(dev->flows, dbase);
        if (!f) {
            /* Try <device>_flow */
            with_flow = g_strdup_printf("%s_flow", dbase);
            f = g_hash_table_lookup(dev->flows, with_flow);
            g_free(with_flow);
        }
        g_free(dbase);
    }

    if (!f && g_hash_table_size(dev->flows) == 1) {
        GHashTableIter iter;
        gpointer value;

        g_hash_table_iter_init(&iter, dev->flows);
        if (g_hash_table_iter_next(&iter, NULL, &value)) {
            f = value;
        }
    }

    if (!f) {
        /* Fall back to standard enumeration flow if available */
        f = g_hash_table_lookup(dev->flows, "enumeration_flow");
    }

    return f;
}

static void dev_entry_free(gpointer p)
{
    yaml_dev_entry_t *e = p;
    if (!e)
        return;
    g_free(e->name);
    g_free(e->flow_path);
    g_free(e->file);
    usb_node_free(e->tree);
    g_free(e);
}

static char *resolve_asset_path(const char *dir, const char *sub, const char *ref)
{
    char *path;
    char *filename;

    if (!ref)
    {
        return NULL;
    }

    if (g_path_is_absolute(ref))
    {
        if (g_file_test(ref, G_FILE_TEST_EXISTS))
        {
            return g_strdup(ref);
        }
        return NULL;
    }

    /* Check directly under dir (e.g. "video.mjpeg", "payloads/video.mjpeg") */
    path = g_build_filename(dir, ref, NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
    {
        return path;
    }
    g_free(path);

    if (strchr(ref, '/') || g_str_has_suffix(ref, ".yaml") || g_str_has_suffix(ref, ".yml"))
    {
        path = g_build_filename(dir, ref, NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
        {
            return path;
        }
        g_free(path);

        /* Try directly under datadir */
        path = usbvcr_find_data_file(NULL, ref);
        if (path != NULL)
        {
            return path;
        }

        /* Try under datadir/sub if ref starts with sub/ */
        if (sub != NULL && g_str_has_prefix(ref, sub) && ref[strlen(sub)] == '/')
        {
            path = usbvcr_find_data_file(sub, ref + strlen(sub) + 1);
            if (path != NULL)
            {
                return path;
            }
        }

        return NULL;
    }

    /* Bare ID (e.g. "keyboard", "mouse"): check local <dir>/<sub>/<ref>.yaml first */
    filename = g_strdup_printf("%s.yaml", ref);
    path = g_build_filename(dir, sub, filename, NULL);
    if (g_file_test(path, G_FILE_TEST_EXISTS))
    {
        g_free(filename);
        return path;
    }
    g_free(path);
    g_free(filename);

    /* Fallback to datadir/<sub>/<ref>.yaml */
    return usbvcr_find_data_file(sub, ref);
}

/* Project file: read the `devices:` manifest — a sequence of {device, flow}
 * pairs — and load each device tree (schema templates, codec tables, and flows
 * must be loaded first). Paths are resolved relative to the project dir.
 * A missing project file is not an error (no devices). */
static int parse_main(const char *path, yaml_device_t *dev)
{
    yaml_parser_t parser;
    yaml_document_t doc;
    yaml_node_t *devs = NULL;
    FILE *fp = NULL;
    const char *version;
    char *dir = NULL;
    int rc = 0;

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        LOG_ERR("No project file '%s'", path);
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        LOG_ERR("Unable to open the file '%s'", path);
        return -1;
    }

    if (!yaml_parser_initialize(&parser)) {
        fclose(fp);
        return -1;
    }

    yaml_parser_set_input_file(&parser, fp);

    if (!yaml_parser_load(&parser, &doc)) {
        LOG_ERR("YAML error(%lu, %lu): %s",
                (unsigned long) parser.problem_mark.line + 1,
                (unsigned long) parser.problem_mark.column + 1,
                parser.problem);
        yaml_parser_delete(&parser);
        fclose(fp);
        return -1;
    }

    dir = g_path_get_dirname(path);
    yaml_node_t *root = yaml_document_get_root_node(&doc);

    /* Fallback version check. */
    version = yaml_scalar(yaml_map_get(&doc, root, "version"));
    g_free(dev->name);
    dev->name = g_strdup(yaml_scalar(yaml_map_get(&doc, root, "name")));
    if (match_version(version) == YAML_VERSION_INVALID) {
        LOG_ERR("Mismatch version '%s'", version);
        g_free(dir);
        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
        fclose(fp);
        return -1;
    }

    yaml_node_t *conn = yaml_map_get(&doc, root, "connection");
    g_free(dev->engine);
    g_free(dev->udc);
    g_free(dev->host);
    g_free(dev->port);
    dev->engine = g_strdup(yaml_scalar(yaml_map_get(&doc, conn, "engine")));
    dev->udc    = g_strdup(yaml_scalar(yaml_map_get(&doc, conn, "udc")));
    dev->host   = g_strdup(yaml_scalar(yaml_map_get(&doc, conn, "host")));
    dev->port   = g_strdup(yaml_scalar(yaml_map_get(&doc, conn, "port")));

    g_free(dev->record);
    dev->record = g_strdup(yaml_scalar(yaml_map_get(&doc, conn, "record")));

    const char *dbg = yaml_scalar(yaml_map_get(&doc, conn, "debug"));
    dev->debug = dbg && (g_ascii_strcasecmp(dbg, "true") == 0 ||
                         g_ascii_strcasecmp(dbg, "yes") == 0 ||
                         strcmp(dbg, "1") == 0);

    const char *conn_speed = yaml_scalar(yaml_map_get(&doc, conn, "speed"));
    const char *root_speed = yaml_scalar(yaml_map_get(&doc, root, "speed"));
    int parsed_speed = usb_parse_speed(conn_speed ? conn_speed : root_speed);
    if (parsed_speed >= 0)
        dev->speed = parsed_speed;

    const char *root_file = yaml_scalar(yaml_map_get(&doc, root, "file"));
    const char *root_loop = yaml_scalar(yaml_map_get(&doc, root, "loop"));
    char *resolved_file = root_file ? resolve_asset_path(dir, "payloads", root_file) : NULL;
    gboolean parsed_loop = FALSE;
    if (root_loop) {
        parsed_loop = (g_ascii_strcasecmp(root_loop, "true") == 0 ||
                       g_ascii_strcasecmp(root_loop, "yes") == 0 ||
                       strcmp(root_loop, "1") == 0);
    }

    /* Top-level `pcap:` — alternate to `devices:`, resolved relative to the
     * project dir like device/flow refs below. */
    g_free(dev->pcap);
    const char *pref = yaml_scalar(yaml_map_get(&doc, root, "pcap"));
    dev->pcap = pref ? g_build_filename(dir, pref, NULL) : NULL;

    devs = yaml_map_get(&doc, root, "devices");

    if (!devs || devs->type != YAML_SEQUENCE_NODE) {
        if (!dev->pcap)
            LOG_WARN("yaml_loader: project '%s' has no devices: manifest", path);
    } else {
        for (yaml_node_item_t *i = devs->data.sequence.items.start;
             rc == 0 && i < devs->data.sequence.items.top; i++) {
            yaml_node_t *item = yaml_document_get_node(&doc, *i);
            if (!item || item->type != YAML_MAPPING_NODE)
                continue;

            const char *dref = yaml_scalar(yaml_map_get(&doc, item, "device"));
            const char *fref = yaml_scalar(yaml_map_get(&doc, item, "flow"));
            if (!dref) {
                LOG_WARN("yaml_loader: manifest entry with no device:, skipping");
                continue;
            }

            char *dpath = resolve_asset_path(dir, "devices", dref);
            if (!dpath) {
                LOG_ERR("yaml_loader: cannot find device '%s'", dref);
                rc = -1;
                break;
            }

            char *ns = NULL;
            usb_node_t *tree = yaml_tree_load(dpath, dev->schemas,
                                              dev->codecs, &ns);
            g_free(ns);
            if (!tree) {
                LOG_ERR("yaml_loader: failed to load device '%s'", dpath);
                g_free(dpath);
                rc = -1;
                break;
            }

            char *fpath = fref ? resolve_asset_path(dir, "flows", fref) : NULL;
            flow_t *flow = yaml_device_find_flow(dev, fref);

            if (!flow && fpath) {
                flow = flow_load_with_dir(fpath, dir);
                if (flow) {
                    char *base = g_path_get_basename(fpath);
                    if (g_str_has_suffix(base, ".yaml")) {
                        base[strlen(base) - 5] = '\0';
                    } else if (g_str_has_suffix(base, ".yml")) {
                        base[strlen(base) - 4] = '\0';
                    }
                    g_hash_table_insert(dev->flows, base, flow);
                }
            }

            if (!flow && !fref) {
                flow = find_flow_for_device(dev, dref);
            }

            yaml_dev_entry_t *e = g_new0(yaml_dev_entry_t, 1);
            const char *dspeed = yaml_scalar(yaml_map_get(&doc, item, "speed"));
            const char *efile = yaml_scalar(yaml_map_get(&doc, item, "file"));
            const char *eloop = yaml_scalar(yaml_map_get(&doc, item, "loop"));
            int item_speed = usb_parse_speed(dspeed);
            e->name      = g_strdup(dref);
            e->flow_path = fpath;
            e->tree      = tree;   /* entry owns it */
            e->flow      = flow;   /* aliased from dev->flows */
            e->speed     = (item_speed >= 0) ? item_speed : dev->speed;
            if (efile) {
                e->file = resolve_asset_path(dir, "payloads", efile);
            } else if (resolved_file) {
                e->file = g_strdup(resolved_file);
            }
            if (eloop) {
                e->loop = (g_ascii_strcasecmp(eloop, "true") == 0 ||
                           g_ascii_strcasecmp(eloop, "yes") == 0 ||
                           strcmp(eloop, "1") == 0);
            } else {
                e->loop = parsed_loop;
            }
            dev->devices = g_list_append(dev->devices, e);
            LOG_DEBUG("[Manifest] device '%s' flow '%s'",
                      dref, fref ? fref : (flow ? "(auto)" : "(none)"));

            g_free(dpath);
        }
    }

    /* Alias the first entry so single-device callers keep working. */
    if (dev->devices) {
        yaml_dev_entry_t *first = dev->devices->data;
        dev->tree = first->tree;   /* alias, not owned */
        dev->flow = first->flow;
        dev->file = first->file;   /* alias, not owned */
        dev->loop = first->loop;
        if (first->speed >= 0)
            dev->speed = first->speed;
    } else {
        dev->flow = find_flow_for_device(dev, NULL);
        dev->file = resolved_file; /* standalone device owns it */
        dev->loop = parsed_loop;
        resolved_file = NULL;
    }

    g_free(resolved_file);
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
                    case YAML_SCOPE_FLOWS:
                        rc = parse_flows(path, dev);
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
    dev->speed     = -1;
    dev->schemas   = schema_new();
    dev->codecs    = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free,
                                           (GDestroyNotify) codec_free);
    dev->flows     = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           g_free,
                                           (GDestroyNotify) flow_free);

    return dev;
}

yaml_device_t *yaml_parse_from_file(const char *path)
{
    if (!path)
        return NULL;

    char *dir = g_path_get_dirname(path);
    char *datadir = usbvcr_get_data_dir();
    yaml_device_t *dev = yaml_device_create();

    /* Pre-load global shared resources (schemas, codecs, and flows) from data dir first */
    if (datadir != NULL) {
        if (g_strcmp0(datadir, dir) != 0) {
            load_scope_dir(datadir, "schemas", YAML_SCOPE_SCHEMAS, dev);
            load_scope_dir(datadir, "codecs",  YAML_SCOPE_CODECS,  dev);
            load_scope_dir(datadir, "flows",   YAML_SCOPE_FLOWS,   dev);
        }
        g_free(datadir);
    }

    /* Shared resources first (dir-scanned): schema templates + codec tables + flows.
     * Then the project `devices:` manifest, which loads each device tree using
     * them. tests/ is still dir-scanned (stub). */
    if (load_scope_dir(dir, "schemas", YAML_SCOPE_SCHEMAS, dev) < 0 ||
        load_scope_dir(dir, "codecs",  YAML_SCOPE_CODECS,  dev) < 0 ||
        load_scope_dir(dir, "flows",   YAML_SCOPE_FLOWS,   dev) < 0 ||
        load_scope_dir(dir, "tests",   YAML_SCOPE_TESTS,   dev) < 0 ||
        parse_main(path, dev) < 0) {


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
    g_free(dev->name);
    g_free(dev->engine);
    g_free(dev->udc);
    g_free(dev->host);
    g_free(dev->port);
    g_free(dev->record);
    g_free(dev->pcap);
    if (dev->devices == NULL)
        g_free(dev->file);
    /* Entries own their trees; dev->tree/flow/file are just aliases. */
    g_list_free_full(dev->devices, dev_entry_free);
    schema_free(dev->schemas);
    if (dev->codecs)
        g_hash_table_destroy(dev->codecs);
    if (dev->flows)
        g_hash_table_destroy(dev->flows);
    g_free(dev);
}
