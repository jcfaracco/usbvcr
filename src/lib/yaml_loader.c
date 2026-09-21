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
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <yaml.h>

#define YAML_MAX_DEPTH 128

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

typedef enum {
    YAML_STATE_NONE = 0,
    YAML_STATE_EXPECT_START,
    YAML_STATE_EXPECT_END,
    YAML_STATE_EXPECT_KEY,
    YAML_STATE_EXPECT_VALUE,
    YAML_STATE_EXPECT_ARRAY,
} yaml_parser_state_enum_t;

// Context structure to control the YAML state
typedef struct {
    GQueue *state;       // Stack of yaml_parser_state_enum_t
    char *current_key;   // Temporary key value
    yaml_event_t *event; // Current YAML event
} yaml_context_t;

/* Function to see the stack head */
yaml_parser_state_enum_t current_state(GQueue *stack) {
    if (!stack || g_queue_is_empty(stack))
        return YAML_STATE_EXPECT_START;

    return GPOINTER_TO_INT(g_queue_peek_head(stack));
}

// Callback typedef
typedef int (*yaml_callback_t)(yaml_context_t *event, yaml_device_t *dev);

// Generic function to read the file and dispatch the events using a callback
int parse_yaml_with_callback(const char *path,
                             yaml_callback_t cb,
                             yaml_device_t *dev) {
    yaml_parser_t parser;
    yaml_context_t ctx;
    yaml_event_t event;
    FILE *fp = NULL;
    int rc = -1;

    fp = fopen(path, "r");
    if (!fp) {
        LOG_ERR("Unable to open file '%s'", path);
        return rc;
    }

    if (!yaml_parser_initialize(&parser)) {
        fclose(fp);
        return rc;
    }
    yaml_parser_set_input_file(&parser, fp);

    ctx.state = g_queue_new();

    do {
        if (!yaml_parser_parse(&parser, &event)) {
            LOG_ERR("YAML error(%lu, %lu): %s\n",
                    (unsigned long) parser.problem_mark.line + 1,
                    (unsigned long) parser.problem_mark.column + 1,
                    parser.problem);
            yaml_parser_delete(&parser);
            fclose(fp);
            return 0;
        }

        ctx.event = &event;

        // Callback exectution
        if (cb)
            rc = cb(&ctx, dev);

        if (event.type != YAML_STREAM_END_EVENT) {
            yaml_event_delete(&event);
        }
    } while (event.type != YAML_STREAM_END_EVENT);

    yaml_event_delete(&event);
    yaml_parser_delete(&parser);
    fclose(fp);
    return rc;
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

/* ── per-scope parsers (STUBS) ──────────────────────────────────────────
 * Each consumes one file and folds it into `dev`. Return 0 on success, -1 on
 * parse error. Fill these in as the dialect lands (README §9 step 2). */

static int parse_schema(const char *path, yaml_device_t *dev)
{
    (void)path; (void)dev;
    return 0; /* TODO: register templates under their namespace */
}

static int parse_device(yaml_context_t *ctx, yaml_device_t *dev)
{
    (void)dev;
    yaml_parser_state_enum_t state = current_state(ctx->state);

    switch (ctx->event->type) {
        case YAML_MAPPING_START_EVENT:
            if (state == YAML_STATE_EXPECT_ARRAY) {
                LOG_DEBUG("[Callback] Dict mapping init.");
            } else if (state == YAML_STATE_EXPECT_VALUE) {
                LOG_DEBUG("[Callback] Subdict mapping init.");
            }

            g_queue_push_head(ctx->state, GINT_TO_POINTER(YAML_STATE_EXPECT_KEY));
            break;
        case YAML_SCALAR_EVENT:
            if (state == YAML_STATE_EXPECT_KEY) {
                LOG_DEBUG("[Callback] Read key: %s", ctx->event->data.scalar.value);

                /* Next item should be a value. */
                g_queue_pop_head(ctx->state);
                g_queue_push_head(ctx->state, GINT_TO_POINTER(YAML_STATE_EXPECT_VALUE));
            } else if (state == YAML_STATE_EXPECT_VALUE) {
                LOG_DEBUG("[Callback] Read value: %s", ctx->event->data.scalar.value);

                /* Next item should be a key again. */
                g_queue_pop_head(ctx->state);
                g_queue_push_head(ctx->state, GINT_TO_POINTER(YAML_STATE_EXPECT_KEY));
            } else if (state == YAML_STATE_EXPECT_ARRAY) {
                LOG_DEBUG("[Callback] Array item (%u): %s", state, ctx->event->data.scalar.value);
            } else {
                LOG_DEBUG("[Callback] Unknown item (%u): %s", state, ctx->event->data.scalar.value);
            }
            break;
        case YAML_SEQUENCE_START_EVENT:
            LOG_DEBUG("[Callback] Array enter");
            g_queue_push_head(ctx->state, GINT_TO_POINTER(YAML_STATE_EXPECT_ARRAY));
            break;
        case YAML_SEQUENCE_END_EVENT:
            g_queue_pop_head(ctx->state); // Quit array state

            // Do we need father state?
            if (current_state(ctx->state) == YAML_STATE_EXPECT_VALUE) {
                g_queue_pop_head(ctx->state);
                g_queue_push_head(ctx->state, GINT_TO_POINTER(YAML_STATE_EXPECT_KEY));
            }
            LOG_DEBUG("[Callback] Array exit");
            break;
        case YAML_MAPPING_END_EVENT:
            g_queue_pop_head(ctx->state); // Quit STATE_KEY/VALUE

            if (current_state(ctx->state) == YAML_STATE_EXPECT_VALUE) {
                g_queue_pop_head(ctx->state);
                g_queue_push_head(ctx->state, GINT_TO_POINTER(YAML_STATE_EXPECT_KEY));
            }
            LOG_DEBUG("[Callback] Dict mapping exit");
            break;

        default:
            // Other events
            break;
    }

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
                LOG_WARN("yaml_loader: scope mismatch for %s", path);
                rc = -1;
            } else {
                switch (want) {
                case YAML_SCOPE_SCHEMA: rc = parse_schema(path, dev); break;
                case YAML_SCOPE_DEVICE:
                    rc = parse_yaml_with_callback(path, parse_device, dev);
                    break;
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
