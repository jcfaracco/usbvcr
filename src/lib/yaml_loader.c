/*
 * yaml_loader.c – Canonical-dialect project loader.
 *
 * Orchestration only: resolve a project's scopes (schemas/ codecs/ tests/)
 * and its `devices:` manifest, delegating the YAML->usb_node build to
 * yaml_tree.{c,h} and codec tables to codec.{c,h}.
 *
 * TODO:
 *   - autofill null length/count fields (README §3.1)
 *   - load flows/ into Flow model (flow.h) — manifest step 4
 *   - build ValidationSuite from tests/ (README §3.4)
 */

#include "yaml_loader.h"
#include "flow.h"
#include "yaml_tree.h"
#include "codec.h"
#include "log.h"
#include "version.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <yaml.h>


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

static void dev_entry_free(gpointer p)
{
    yaml_dev_entry_t *e = p;
    if (!e)
        return;
    g_free(e->name);
    g_free(e->flow_path);
    usb_node_free(e->tree);
    if (e->flow) { flow_free(e->flow); }
    g_free(e);
}

/* Project file: read the `devices:` manifest — a sequence of {device, flow}
 * pairs — and load each device tree (schema templates and codec tables must be
 * loaded first). Paths are resolved relative to the project dir. The flow is
 * recorded by path only; building the Flow model is step 4. A missing project
 * file is not an error (no devices). */
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
    version = yaml_tree_scalar(yaml_tree_map_get(&doc, root, "version"));
    if (match_version(version) == YAML_VERSION_INVALID) {
        LOG_ERR("Mismatch version '%s'", version);
        g_free(dir);
        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
        fclose(fp);
        return -1;
    }

    devs = yaml_tree_map_get(&doc, root, "devices");

    if (!devs || devs->type != YAML_SEQUENCE_NODE) {
        LOG_WARN("yaml_loader: project '%s' has no devices: manifest", path);
    } else {
        for (yaml_node_item_t *i = devs->data.sequence.items.start;
             rc == 0 && i < devs->data.sequence.items.top; i++) {
            yaml_node_t *item = yaml_document_get_node(&doc, *i);
            if (!item || item->type != YAML_MAPPING_NODE)
                continue;

            const char *dref = yaml_tree_scalar(yaml_tree_map_get(&doc, item, "device"));
            const char *fref = yaml_tree_scalar(yaml_tree_map_get(&doc, item, "flow"));
            if (!dref) {
                LOG_WARN("yaml_loader: manifest entry with no device:, skipping");
                continue;
            }

            char *dpath = g_build_filename(dir, dref, NULL);
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

            yaml_dev_entry_t *e = g_new0(yaml_dev_entry_t, 1);
            e->name      = g_strdup(dref);
            e->flow_path = fref ? g_build_filename(dir, fref, NULL) : NULL;
            e->tree      = tree;   /* entry owns it */
            e->flow      = e->flow_path ? flow_load(e->flow_path) : NULL;
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
    dev->schemas   = schema_new();
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
    /* Entries own their trees/flows; dev->tree/flow are just aliases. */
    g_list_free_full(dev->devices, dev_entry_free);
    schema_free(dev->schemas);
    if (dev->codecs)
        g_hash_table_destroy(dev->codecs);
    g_free(dev);
}
