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

#include <stdlib.h>
#include <string.h>
#include <glib.h>

typedef struct {
    const char *name;
    yaml_scope_t scope;
} scope_entry;

/* Directory name -> scope. The directory is authoritative; the scope is
 * never stated in-file. */
static const scope_entry scope_by_dir[] = {
    { "devices", YAML_SCOPE_DEVICE },
    { "schemas", YAML_SCOPE_SCHEMA },
    { "flows",   YAML_SCOPE_FLOW   },
    { "codecs",  YAML_SCOPE_CODEC  },
    { "tests",   YAML_SCOPE_TEST   },
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

/* ── per-scope parsers (STUBS) ──────────────────────────────────────────
 * Each consumes one file and folds it into `dev`. Return 0 on success, -1 on
 * parse error. Fill these in as the dialect lands (README §9 step 2). */

static int parse_schema(const char *path, yaml_device_t *dev)
{
    (void)path; (void)dev;
    return 0; /* TODO: register templates under their namespace */
}

static int parse_device(const char *path, yaml_device_t *dev)
{
    (void)path; (void)dev;
    return 0; /* TODO: build UsbNode tree; imports/template/override; autofill */
}

static int parse_codec(const char *path, yaml_device_t *dev)
{
    (void)path; (void)dev;
    return 0; /* TODO: load codec table (codec.h) */
}

static int parse_flow(const char *path, yaml_device_t *dev)
{
    (void)path; (void)dev;
    return 0; /* TODO: build Flow model (flow.h) */
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
                g_warning("yaml_loader: scope mismatch for %s", path);
                rc = -1;
            } else {
                switch (want) {
                case YAML_SCOPE_SCHEMA: rc = parse_schema(path, dev); break;
                case YAML_SCOPE_DEVICE: rc = parse_device(path, dev); break;
                case YAML_SCOPE_CODEC:  rc = parse_codec(path, dev);  break;
                case YAML_SCOPE_FLOW:   rc = parse_flow(path, dev);   break;
                default:                rc = -1;                      break;
                }
            }
            g_free(path);
        }
        g_dir_close(d);
    }

    g_free(dirpath);
    return rc;
}

yaml_device_t *yaml_loader_load(const char *dir)
{
    if (!dir)
        return NULL;

    yaml_device_t *dev = g_new0(yaml_device_t, 1);

    /* Dependency order: schemas (templates) -> devices (use templates) ->
     * codecs -> flows. tests/ are loaded by the ValidationSuite runner, not
     * here. */
    if (load_scope_dir(dir, "schemas", YAML_SCOPE_SCHEMA, dev) < 0 ||
        load_scope_dir(dir, "devices", YAML_SCOPE_DEVICE, dev) < 0 ||
        load_scope_dir(dir, "codecs",  YAML_SCOPE_CODEC,  dev) < 0 ||
        load_scope_dir(dir, "flows",   YAML_SCOPE_FLOW,   dev) < 0) {
        yaml_device_free(dev);
        return NULL;
    }

    return dev;
}

void yaml_device_free(yaml_device_t *dev)
{
    if (!dev)
        return;
    if (dev->tree)
        usb_node_free(dev->tree);
    /* TODO: flow_free(dev->flow); */
    g_free(dev);
}
