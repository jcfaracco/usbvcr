#include "flow.h"
#include "yaml/util.h" // for yaml_map_get, yaml_scalar
#include "log.h"
#include "utils.h"
#include "bytes.h"
#include "stream/stream_provider.h"
#include "storage/disk_provider.h"

#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <yaml.h>

typedef struct {
    int has_bmRequestType; uint8_t bmRequestType;
    int has_bRequest;      uint8_t bRequest;
    int has_wValue;        uint16_t wValue;
    int has_wIndex;        uint16_t wIndex;
    int has_endpoint_poll; uint8_t endpoint_poll;
    int has_endpoint_out;  uint8_t endpoint_out;
    GByteArray *match_payload;
} flow_match_t;

typedef enum {
    ACT_RESPOND_WITH,
    ACT_RESPOND_WITH_STATUS,
    ACT_TRANSITION_STATE,
    ACT_RETURN_NAK_IF_QUEUE_EMPTY,
    ACT_QUEUE_ENDPOINT_DATA,
    ACT_ENDPOINT_PROVIDER
} flow_action_type_t;

typedef struct {
    flow_action_type_t type;
    char *respond_with;
    GByteArray *respond_bytes;
    uint8_t respond_with_status;
    char *transition_machine;
    char *transition_state;
    int return_nak_if_queue_empty;
    uint8_t ep;
    GQueue *payloads; // of GByteArray*
    flow_endpoint_ops_t provider_ops;
    void *provider_ctx;
} flow_action_t;

typedef struct {
    flow_match_t match;
    GList *actions; // of flow_action_t*
} flow_rule_t;

typedef struct {
    char *name;
    char *extends;
    GList *on_enter; // of flow_action_t*
    GList *rules;    // of flow_rule_t*
} flow_state_t;

typedef struct flow_machine {
    char *name;
    int interface_num;
    char *state; // current state name
    char *next_state;
    GHashTable *states; // name -> flow_state_t*
} flow_machine_t;

typedef struct {
    uint8_t ep;
    flow_endpoint_ops_t ops;
    void *ctx;
} flow_endpoint_binding_t;

struct flow {
    GList *machines; // of flow_machine_t*
    GHashTable *machine_map; // name -> flow_machine_t*
    flow_machine_t *default_machine;
    GList *common_rules; // of flow_rule_t*
    GHashTable *ep_queues; // ep (uint8_t) -> GQueue of GByteArray*
    GHashTable *ep_bindings; // ep (uint8_t) -> flow_endpoint_binding_t*
};

static void flow_action_free(flow_action_t *act) {
    if (!act)
        return;

    g_free(act->respond_with);
    if (act->respond_bytes)
        g_byte_array_unref(act->respond_bytes);
    g_free(act->transition_machine);
    g_free(act->transition_state);

    if (act->provider_ops.free && act->provider_ctx) {
        act->provider_ops.free(act->provider_ctx);
        act->provider_ctx = NULL;
    }

    if (act->payloads)
        g_queue_free_full(act->payloads, (GDestroyNotify)g_byte_array_unref);

    g_free(act);
}

static void flow_endpoint_binding_free(flow_endpoint_binding_t *b)
{
    if (!b)
        return;

    if (b->ops.free && b->ctx)
        b->ops.free(b->ctx);
    g_free(b);
}

static void flow_rule_free(flow_rule_t *rule) {
    if (!rule)
        return;

    if (rule->match.match_payload)
        g_byte_array_unref(rule->match.match_payload);
    g_list_free_full(rule->actions, (GDestroyNotify)flow_action_free);
    g_free(rule);
}

static void flow_state_free(flow_state_t *st) {
    if (!st)
        return;

    g_free(st->name);
    g_free(st->extends);
    g_list_free_full(st->on_enter, (GDestroyNotify)flow_action_free);
    g_list_free_full(st->rules, (GDestroyNotify)flow_rule_free);
    g_free(st);
}

static flow_machine_t *flow_machine_new(const char *name, int interface_num)
{
    flow_machine_t *m = g_new0(flow_machine_t, 1);

    m->name = g_strdup(name ? name : "default");
    m->interface_num = interface_num;
    m->states = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)flow_state_free);

    return m;
}

static void flow_machine_free(flow_machine_t *m)
{
    if (!m)
        return;

    g_free(m->name);
    g_free(m->state);
    g_free(m->next_state);
    g_hash_table_destroy(m->states);
    g_free(m);
}

flow_t *flow_create(void)
{
    flow_t *f = g_new0(flow_t, 1);

    f->machine_map = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);
    f->ep_queues = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_queue_free);
    f->ep_bindings = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)flow_endpoint_binding_free);

    return f;
}

void flow_free(flow_t *flow)
{
    GHashTableIter iter;
    gpointer key, value;

    if (!flow)
        return;

    if (flow->common_rules)
        g_list_free_full(flow->common_rules, (GDestroyNotify)flow_rule_free);
    
    g_hash_table_iter_init(&iter, flow->ep_queues);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        GQueue *q = value;
        if (q) {
            while (!g_queue_is_empty(q)) {
                g_byte_array_unref(g_queue_pop_head(q));
            }
        }        
    }
    g_hash_table_destroy(flow->ep_queues);
    g_hash_table_destroy(flow->ep_bindings);

    if (flow->machines)
        g_list_free_full(flow->machines, (GDestroyNotify)flow_machine_free);
    if (flow->machine_map)
        g_hash_table_destroy(flow->machine_map);

    g_free(flow);
}

int flow_bind_endpoint(flow_t *flow, uint8_t ep, const flow_endpoint_ops_t *ops, void *ctx)
{
    flow_endpoint_binding_t *b;

    if (!flow || !ops)
        return -1;

    b = g_new0(flow_endpoint_binding_t, 1);
    b->ep = ep;
    b->ops = *ops;
    b->ctx = ctx;

    g_hash_table_insert(flow->ep_bindings, GUINT_TO_POINTER((guint)ep), b);
    return 0;
}

void flow_unbind_endpoint(flow_t *flow, uint8_t ep)
{
    if (!flow || !flow->ep_bindings)
        return;

    g_hash_table_remove(flow->ep_bindings, GUINT_TO_POINTER((guint)ep));
}

const char *flow_current_state(flow_t *flow)
{
    if (!flow || !flow->default_machine)
        return NULL;

    return flow->default_machine->state;
}

const char *flow_machine_state(flow_t *flow, const char *machine_name)
{
    flow_machine_t *m;

    if (!flow || !machine_name || !flow->machine_map)
        return NULL;

    m = g_hash_table_lookup(flow->machine_map, machine_name);
    return m ? m->state : NULL;
}

static uint32_t parse_int(const char *s) {
    return s ? strtoul(s, NULL, 0) : 0;
}

static GByteArray* parse_hex_string(const char *hex_str) {
    GByteArray *arr = g_byte_array_new();

    if (!hex_str)
        return arr;

    size_t len = strlen(hex_str);
    for (size_t i = 0; i + 1 < len; i += 2) {
        char buf[3] = {hex_str[i], hex_str[i+1], 0};
        uint8_t byte = (uint8_t)strtoul(buf, NULL, 16);
        g_byte_array_append(arr, &byte, 1);
    }
    return arr;
}

static flow_match_t parse_match(yaml_document_t *doc, yaml_node_t *n) {
    flow_match_t m = { 0 };

    if (n->type != YAML_MAPPING_NODE)
        return m;
    
    yaml_node_t *v;
    if ((v = yaml_map_get(doc, n, "bmRequestType"))) {
        m.has_bmRequestType = 1; m.bmRequestType = parse_int(yaml_scalar(v));
    }
    if ((v = yaml_map_get(doc, n, "bRequest"))) {
        m.has_bRequest = 1; m.bRequest = parse_int(yaml_scalar(v));
    }
    if ((v = yaml_map_get(doc, n, "wValue"))) {
        m.has_wValue = 1; m.wValue = parse_int(yaml_scalar(v));
    }
    if ((v = yaml_map_get(doc, n, "wIndex"))) {
        m.has_wIndex = 1; m.wIndex = parse_int(yaml_scalar(v));
    }
    if ((v = yaml_map_get(doc, n, "endpoint_poll"))) {
        m.has_endpoint_poll = 1; m.endpoint_poll = parse_int(yaml_scalar(v));
    }
    if ((v = yaml_map_get(doc, n, "endpoint_out"))) {
        m.has_endpoint_out = 1; m.endpoint_out = parse_int(yaml_scalar(v));
    }
    if ((v = yaml_map_get(doc, n, "payload"))) {
        yaml_node_t *hex_n = yaml_map_get(doc, v, "hex");
        if (hex_n)
            m.match_payload = parse_hex_string(yaml_scalar(hex_n));
    } else if ((v = yaml_map_get(doc, n, "hex"))) {
        m.match_payload = parse_hex_string(yaml_scalar(v));
    }
    return m;
}

static flow_action_t* parse_queue_endpoint_data(yaml_document_t *doc, yaml_node_t *v) {
    flow_action_t *a = g_new0(flow_action_t, 1);
    a->type = ACT_QUEUE_ENDPOINT_DATA;
    a->payloads = g_queue_new();

    yaml_node_t *ep_n = yaml_map_get(doc, v, "endpoint");
    if (ep_n) a->ep = parse_int(yaml_scalar(ep_n));

    yaml_node_t *pay_n = yaml_map_get(doc, v, "payloads");
    if (pay_n && pay_n->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *pi = pay_n->data.sequence.items.start; pi < pay_n->data.sequence.items.top; pi++) {
            yaml_node_t *pitem = yaml_document_get_node(doc, *pi);
            if (pitem && pitem->type == YAML_MAPPING_NODE) {
                yaml_node_t *hex_n = yaml_map_get(doc, pitem, "hex");
                if (hex_n)
                    g_queue_push_tail(a->payloads, parse_hex_string(yaml_scalar(hex_n)));
            }
        }
    }
    return a;
}

static char *resolve_flow_asset(const char *base_dir, const char *flow_dir, const char *file)
{
    char *p = NULL;

    if (file == NULL)
        return NULL;

    if (g_path_is_absolute(file))
        return g_strdup(file);

    if (base_dir != NULL) {
        p = g_build_filename(base_dir, file, NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            return p;
        g_free(p);
    }

    if (flow_dir != NULL) {
        p = g_build_filename(flow_dir, file, NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            return p;
        g_free(p);

        p = g_build_filename(flow_dir, "..", file, NULL);
        if (g_file_test(p, G_FILE_TEST_EXISTS))
            return p;
        g_free(p);
    }

    if (g_file_test(file, G_FILE_TEST_EXISTS))
        return g_strdup(file);

    if (base_dir != NULL)
        return g_build_filename(base_dir, file, NULL);

    if (flow_dir != NULL)
        return g_build_filename(flow_dir, "..", file, NULL);

    return g_strdup(file);
}

static flow_action_t *parse_stream_action(yaml_document_t *doc, yaml_node_t *item,
                                          yaml_node_t *v, uint8_t ep,
                                          const char *flow_dir, const char *base_dir)
{
    flow_action_t *a = g_new0(flow_action_t, 1);
    const char *file_str = NULL;
    gboolean loop_val = TRUE;
    yaml_node_t *ln;

    a->type = ACT_ENDPOINT_PROVIDER;
    a->ep = ep;

    if (v->type == YAML_MAPPING_NODE) {
        yaml_node_t *fn = yaml_map_get(doc, v, "file");
        ln = yaml_map_get(doc, v, "loop");
        if (fn != NULL)
            file_str = yaml_scalar(fn);
        if (ln != NULL) {
            const char *ls = yaml_scalar(ln);
            loop_val = (g_strcmp0(ls, "false") != 0 && g_strcmp0(ls, "0") != 0 && g_strcmp0(ls, "no") != 0);
        }
    } else if (v->type == YAML_SCALAR_NODE) {
        file_str = yaml_scalar(v);
    }

    if (item && (ln = yaml_map_get(doc, item, "loop"))) {
        const char *ls = yaml_scalar(ln);
        loop_val = (g_strcmp0(ls, "false") != 0 && g_strcmp0(ls, "0") != 0 && g_strcmp0(ls, "no") != 0);
    }

    if (file_str != NULL) {
        char *resolved = resolve_flow_asset(base_dir, flow_dir, file_str);
        stream_provider_t *sp = stream_provider_new(resolved, loop_val, ep);
        a->provider_ops = *stream_provider_get_ops();
        a->provider_ctx = sp;
        g_free(resolved);
    }

    return a;
}

static flow_action_t *parse_disk_action(yaml_document_t *doc, yaml_node_t *v, uint8_t ep_hint,
                                        const char *flow_dir, const char *base_dir)
{
    flow_action_t *a = g_new0(flow_action_t, 1);
    const char *file_str = NULL;
    uint8_t ep_in = 0x81;
    uint8_t ep_out = (ep_hint != 0) ? ep_hint : 0x02;

    a->type = ACT_ENDPOINT_PROVIDER;
    a->ep = ep_out;

    if (v->type == YAML_MAPPING_NODE) {
        yaml_node_t *fn = yaml_map_get(doc, v, "file");
        yaml_node_t *ein = yaml_map_get(doc, v, "endpoint_in");
        yaml_node_t *eout = yaml_map_get(doc, v, "endpoint_out");
        if (fn != NULL)
            file_str = yaml_scalar(fn);
        if (ein != NULL) {
            const char *s = yaml_scalar(ein);
            if (s) ep_in = (uint8_t)strtoul(s, NULL, 0);
        }
        if (eout != NULL) {
            const char *s = yaml_scalar(eout);
            if (s) ep_out = (uint8_t)strtoul(s, NULL, 0);
        }
    } else if (v->type == YAML_SCALAR_NODE) {
        file_str = yaml_scalar(v);
    }

    if (file_str != NULL) {
        char *resolved = resolve_flow_asset(base_dir, flow_dir, file_str);
        disk_provider_t *dp = disk_provider_new(resolved, ep_in, ep_out);
        a->provider_ops = *disk_provider_get_ops();
        a->provider_ctx = dp;
        g_free(resolved);
    }

    return a;
}

static GList* parse_actions(yaml_document_t *doc, yaml_node_t *seq, uint8_t ep_hint,
                            const char *flow_dir, const char *base_dir) {
    GList *list = NULL;
    if (!seq || seq->type != YAML_SEQUENCE_NODE) return NULL;

    for (yaml_node_item_t *i = seq->data.sequence.items.start; i < seq->data.sequence.items.top; i++) {
        yaml_node_t *item = yaml_document_get_node(doc, *i);
        if (!item || item->type != YAML_MAPPING_NODE) continue;

        yaml_node_t *v;
        flow_action_t *a = NULL;
        if ((v = yaml_map_get(doc, item, "respond_with"))) {
            a = g_new0(flow_action_t, 1);
            a->type = ACT_RESPOND_WITH;
            if (v->type == YAML_MAPPING_NODE) {
                yaml_node_t *hex_n = yaml_map_get(doc, v, "hex");
                if (hex_n)
                    a->respond_bytes = parse_hex_string(yaml_scalar(hex_n));
            } else {
                a->respond_with = g_strdup(yaml_scalar(v));
            }
        } else if ((v = yaml_map_get(doc, item, "respond_with_status"))) {
            a = g_new0(flow_action_t, 1);
            a->type = ACT_RESPOND_WITH_STATUS;
            a->respond_with_status = parse_int(yaml_scalar(v));
        } else if ((v = yaml_map_get(doc, item, "transition_state"))) {
            a = g_new0(flow_action_t, 1);
            a->type = ACT_TRANSITION_STATE;
            if (v->type == YAML_MAPPING_NODE) {
                yaml_node_t *mn = yaml_map_get(doc, v, "machine");
                yaml_node_t *sn = yaml_map_get(doc, v, "state");
                if (mn)
                    a->transition_machine = g_strdup(yaml_scalar(mn));
                if (sn)
                    a->transition_state = g_strdup(yaml_scalar(sn));
            } else {
                a->transition_state = g_strdup(yaml_scalar(v));
            }
        } else if ((v = yaml_map_get(doc, item, "return_nak_if_queue_empty"))) {
            a = g_new0(flow_action_t, 1);
            a->type = ACT_RETURN_NAK_IF_QUEUE_EMPTY;
            a->return_nak_if_queue_empty = (g_strcmp0(yaml_scalar(v), "true") == 0);
        } else if ((v = yaml_map_get(doc, item, "queue_endpoint_data"))) {
            a = parse_queue_endpoint_data(doc, v);
        } else if ((v = yaml_map_get(doc, item, "stream_from_file")) ||
                   (v = yaml_map_get(doc, item, "stream"))) {
            a = parse_stream_action(doc, item, v, ep_hint, flow_dir, base_dir);
        } else if ((v = yaml_map_get(doc, item, "disk_from_file")) ||
                   (v = yaml_map_get(doc, item, "attach_disk"))) {
            a = parse_disk_action(doc, v, ep_hint, flow_dir, base_dir);
        }

        if (a)
            list = g_list_append(list, a);
    }
    return list;
}

static GList* parse_rules(yaml_document_t *doc, yaml_node_t *seq,
                          const char *flow_dir, const char *base_dir) {
    GList *list = NULL;
    if (!seq || seq->type != YAML_SEQUENCE_NODE) return NULL;
    
    for (yaml_node_item_t *i = seq->data.sequence.items.start; i < seq->data.sequence.items.top; i++) {
        yaml_node_t *item = yaml_document_get_node(doc, *i);
        if (!item || item->type != YAML_MAPPING_NODE) continue;
        
        flow_rule_t *r = g_new0(flow_rule_t, 1);
        yaml_node_t *m_node = yaml_map_get(doc, item, "match");
        if (m_node) {
            r->match = parse_match(doc, m_node);
        }
        yaml_node_t *a_node = yaml_map_get(doc, item, "actions");
        if (a_node) {
            r->actions = parse_actions(doc, a_node, r->match.endpoint_poll, flow_dir, base_dir);
        }
        list = g_list_append(list, r);
    }
    return list;
}

static void parse_machine_states(yaml_document_t *doc, yaml_node_t *states_n,
                                 flow_machine_t *m,
                                 const char *flow_dir, const char *base_dir)
{
    yaml_node_item_t *i;

    if (!states_n || states_n->type != YAML_SEQUENCE_NODE)
        return;

    for (i = states_n->data.sequence.items.start; i < states_n->data.sequence.items.top; i++) {
        yaml_node_t *item = yaml_document_get_node(doc, *i);
        flow_state_t *st;
        yaml_node_t *name_n;
        yaml_node_t *ext_n;
        yaml_node_t *enter_n;
        yaml_node_t *rules_n;

        if (!item || item->type != YAML_MAPPING_NODE)
            continue;

        st = g_new0(flow_state_t, 1);
        name_n = yaml_map_get(doc, item, "name");
        if (name_n) {
            const char *s = yaml_scalar(name_n);
            if (s)
                st->name = g_strdup(s);
        }

        ext_n = yaml_map_get(doc, item, "extends");
        if (ext_n) {
            const char *s = yaml_scalar(ext_n);
            if (s)
                st->extends = g_strdup(s);
        }

        enter_n = yaml_map_get(doc, item, "on_enter");
        if (enter_n)
            st->on_enter = parse_actions(doc, enter_n, 0, flow_dir, base_dir);

        rules_n = yaml_map_get(doc, item, "rules");
        if (rules_n)
            st->rules = parse_rules(doc, rules_n, flow_dir, base_dir);

        if (st->name) {
            if (!m->state)
                m->state = g_strdup(st->name); /* first state is default initial state */
            g_hash_table_insert(m->states, st->name, st);
        } else {
            flow_state_free(st);
        }
    }
}

flow_t *flow_load(const char *path) {
    return flow_load_with_dir(path, NULL);
}

flow_t *flow_load_with_dir(const char *path, const char *base_dir) {
    FILE *fp = NULL;
    char *flow_dir = NULL;

    if (!path)
        return NULL;

    fp = fopen(path, "rb");
    if (!fp) return NULL;

    flow_dir = g_path_get_dirname(path);

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        g_free(flow_dir);
        fclose(fp);
        return NULL;
    }
    yaml_parser_set_input_file(&parser, fp);

    yaml_document_t doc;
    if (!yaml_parser_load(&parser, &doc)) {
        yaml_parser_delete(&parser);
        g_free(flow_dir);
        fclose(fp);
        return NULL;
    }

    yaml_node_t *top = yaml_document_get_root_node(&doc);
    if (!top || top->type != YAML_MAPPING_NODE) {
        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
        g_free(flow_dir);
        fclose(fp);
        return NULL;
    }

    flow_t *f = flow_create();

    yaml_node_t *common_n = yaml_map_get(&doc, top, "common_rules");
    if (common_n)
        f->common_rules = parse_rules(&doc, common_n, flow_dir, base_dir);

    yaml_node_t *states_n = yaml_map_get(&doc, top, "states");
    if (states_n && states_n->type == YAML_SEQUENCE_NODE) {
        flow_machine_t *def_m = flow_machine_new("default", -1);
        parse_machine_states(&doc, states_n, def_m, flow_dir, base_dir);
        f->machines = g_list_append(f->machines, def_m);
        g_hash_table_insert(f->machine_map, def_m->name, def_m);
        f->default_machine = def_m;
    }

    yaml_node_t *machines_n = yaml_map_get(&doc, top, "machines");
    if (machines_n && machines_n->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *i = machines_n->data.sequence.items.start; i < machines_n->data.sequence.items.top; i++) {
            yaml_node_t *item = yaml_document_get_node(&doc, *i);
            const char *mname = "machine";
            int iface_num = -1;
            yaml_node_t *mn_n;
            yaml_node_t *iface_n;
            yaml_node_t *mst_n;
            yaml_node_t *init_n;
            flow_machine_t *m;

            if (!item || item->type != YAML_MAPPING_NODE)
                continue;

            mn_n = yaml_map_get(&doc, item, "name");
            if (mn_n) {
                const char *s = yaml_scalar(mn_n);
                if (s)
                    mname = s;
            }

            iface_n = yaml_map_get(&doc, item, "interface");
            if (iface_n) {
                iface_num = (int)parse_int(yaml_scalar(iface_n));
            }

            m = flow_machine_new(mname, iface_num);
            mst_n = yaml_map_get(&doc, item, "states");
            if (mst_n)
                parse_machine_states(&doc, mst_n, m, flow_dir, base_dir);

            init_n = yaml_map_get(&doc, item, "initial_state");
            if (init_n) {
                const char *is = yaml_scalar(init_n);
                if (is && g_hash_table_contains(m->states, is)) {
                    g_free(m->state);
                    m->state = g_strdup(is);
                }
            }

            f->machines = g_list_append(f->machines, m);
            g_hash_table_insert(f->machine_map, m->name, m);
            if (!f->default_machine) {
                f->default_machine = m;
            }
        }
    }

    g_free(flow_dir);
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    return f;
}

static void execute_actions(flow_t *flow, GList *actions, uint8_t *out, uint16_t *len,
                            const uint8_t *ep_buf, size_t ep_len, int *handled);

static int flow_machine_transition_to(flow_t *flow, flow_machine_t *m, const char *new_state)
{
    flow_state_t *st;
    char *copy;

    if (!flow || !m || !new_state)
        return -1;

    st = g_hash_table_lookup(m->states, new_state);
    if (!st) {
        LOG_WARN("[flow] Transition target state '%s' does not exist in machine '%s'",
                 new_state, m->name ? m->name : "(unnamed)");
        return -1;
    }

    if (g_strcmp0(m->state, new_state) == 0)
        return 0;

    LOG_DEBUG("[flow] Machine '%s' transition '%s' -> '%s'",
              m->name ? m->name : "(unnamed)",
              m->state ? m->state : "(none)", new_state);

    copy = g_strdup(new_state);
    g_free(m->state);
    m->state = copy;

    if (m->next_state) {
        g_free(m->next_state);
        m->next_state = NULL;
    }

    if (st->on_enter) {
        execute_actions(flow, st->on_enter, NULL, NULL, NULL, 0, NULL);
    }

    return 0;
}

int flow_transition_machine_to(flow_t *flow, const char *machine_name, const char *new_state)
{
    flow_machine_t *m;

    if (!flow || !machine_name || !new_state)
        return -1;

    m = g_hash_table_lookup(flow->machine_map, machine_name);
    if (!m) {
        LOG_WARN("[flow] Machine '%s' not found for transition to '%s'", machine_name, new_state);
        return -1;
    }

    return flow_machine_transition_to(flow, m, new_state);
}

int flow_transition_to(flow_t *flow, const char *new_state)
{
    char *dot;
    GList *l;

    if (!flow || !new_state)
        return -1;

    /* If target is "machine.state", transition that machine */
    dot = strchr(new_state, '.');
    if (dot) {
        char *mname = g_strndup(new_state, dot - new_state);
        const char *sname = dot + 1;
        int rc = flow_transition_machine_to(flow, mname, sname);
        g_free(mname);
        return rc;
    }

    /* Otherwise, check if default machine has this state */
    if (flow->default_machine && g_hash_table_contains(flow->default_machine->states, new_state)) {
        return flow_machine_transition_to(flow, flow->default_machine, new_state);
    }

    /* Next search all machines for this state */
    for (l = flow->machines; l; l = l->next) {
        flow_machine_t *m = l->data;
        if (m != flow->default_machine && g_hash_table_contains(m->states, new_state)) {
            return flow_machine_transition_to(flow, m, new_state);
        }
    }

    LOG_WARN("[flow] Transition target state '%s' does not exist", new_state);
    return -1;
}

static void execute_actions(flow_t *flow, GList *actions, uint8_t *out, uint16_t *len,
                            const uint8_t *ep_buf, size_t ep_len, int *handled) {
    for (GList *l = actions; l; l = l->next) {
        flow_action_t *a = l->data;
        switch (a->type) {
            case ACT_RESPOND_WITH:
                LOG_DEBUG("[flow] ACT_RESPOND_WITH");
                if (out && len) {
                    if (a->respond_bytes) {
                        size_t blen = a->respond_bytes->len;
                        if (blen > 1024) blen = 1024;
                        memcpy(out, a->respond_bytes->data, blen);
                        *len = (uint16_t)blen;
                        if (handled) *handled = 1;
                    } else if (a->respond_with) {
                        size_t slen = strlen(a->respond_with) + 1;
                        if (slen > 1024) slen = 1024;
                        memcpy(out, a->respond_with, slen);
                        *len = slen;
                        if (handled) *handled = 1;
                    }
                }
                break;
            case ACT_RESPOND_WITH_STATUS:
                LOG_DEBUG("[flow] ACT_RESPOND_WITH_STATUS");
                if (out && len) {
                    *len = 0;
                    if (handled) *handled = 1;
                }
                break;
            case ACT_TRANSITION_STATE:
                LOG_DEBUG("[flow] ACT_TRANSITION_STATE to '%s'", a->transition_state);
                if (a->transition_machine) {
                    flow_transition_machine_to(flow, a->transition_machine, a->transition_state);
                } else {
                    flow_transition_to(flow, a->transition_state);
                }
                break;
            case ACT_QUEUE_ENDPOINT_DATA: {
                GQueue *q = g_hash_table_lookup(flow->ep_queues, GUINT_TO_POINTER((guint)a->ep));
                if (!q) {
                    q = g_queue_new();
                    g_hash_table_insert(flow->ep_queues, GUINT_TO_POINTER((guint)a->ep), q);
                }
                for (GList *pl = a->payloads->head; pl; pl = pl->next) {
                    GByteArray *arr = pl->data;
                    g_queue_push_tail(q, g_byte_array_ref(arr));
                }
                break;
            }
            case ACT_RETURN_NAK_IF_QUEUE_EMPTY:
                break;
            case ACT_ENDPOINT_PROVIDER:
                if (a->provider_ops.consume_out && a->provider_ctx && ep_buf && ep_len > 0) {
                    GQueue *q = g_hash_table_lookup(flow->ep_queues, GUINT_TO_POINTER((guint)0x81));
                    if (!q) {
                        q = g_queue_new();
                        g_hash_table_insert(flow->ep_queues, GUINT_TO_POINTER((guint)0x81), q);
                    }
                    if (a->provider_ops.consume_out(a->provider_ctx, a->ep, ep_buf, ep_len, q) == 0) {
                        if (handled) *handled = 1;
                    }
                }
                break;
        }
    }
}

static int match_and_execute_control(flow_t *flow, GList *rules,
                                     uint8_t bmRequestType, uint8_t bRequest,
                                     uint16_t wValue, uint16_t wIndex,
                                     uint8_t *out, uint16_t *len)
{
    GList *l;

    for (l = rules; l; l = l->next) {
        flow_rule_t *r = l->data;
        int match = 1;

        if (r->match.has_bmRequestType && r->match.bmRequestType != bmRequestType)
            match = 0;
        if (r->match.has_bRequest && r->match.bRequest != bRequest)
            match = 0;
        if (r->match.has_wValue && r->match.wValue != wValue)
            match = 0;
        if (r->match.has_wIndex && r->match.wIndex != wIndex)
            match = 0;
        if (r->match.has_endpoint_poll || r->match.has_endpoint_out)
            match = 0;

        if (match) {
            int handled = 0;

            execute_actions(flow, r->actions, out, len, NULL, 0, &handled);
            if (handled)
                return 0;
        }
    }

    return -1;
}

int flow_control(flow_t *flow, const uint8_t *setup,
                 uint8_t *out, uint16_t *len)
{
    GList *ml;
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;

    if (!flow)
        return -1;

    bmRequestType = setup[0];
    bRequest = setup[1];
    wValue = get_u16le(&setup[2]);
    wIndex = get_u16le(&setup[4]);

    for (ml = flow->machines; ml; ml = ml->next) {
        flow_machine_t *m = ml->data;
        flow_state_t *st;
        flow_state_t *curr;
        int depth;

        if (!m || !m->state)
            continue;

        st = g_hash_table_lookup(m->states, m->state);
        curr = st;
        depth = 0;

        while (curr && depth++ < 16) {
            if (match_and_execute_control(flow, curr->rules, bmRequestType, bRequest,
                                          wValue, wIndex, out, len) == 0)
                return 0;

            if (curr->extends)
                curr = g_hash_table_lookup(m->states, curr->extends);
            else
                break;
        }
    }

    if (flow->common_rules) {
        if (match_and_execute_control(flow, flow->common_rules, bmRequestType, bRequest,
                                      wValue, wIndex, out, len) == 0)
            return 0;
    }

    return -1;
}

static int match_and_execute_ep_in(flow_t *flow, GList *rules,
                                   uint8_t ep, uint8_t *buf, size_t cap,
                                   int *handled)
{
    GList *l;

    for (l = rules; l; l = l->next) {
        flow_rule_t *r = l->data;

        if (r->match.has_endpoint_poll && r->match.endpoint_poll == ep) {
            GList *al;

            *handled = 1;
            for (al = r->actions; al; al = al->next) {
                flow_action_t *a = al->data;

                if (a->type == ACT_ENDPOINT_PROVIDER && a->provider_ops.produce_in && a->provider_ctx) {
                    int n = a->provider_ops.produce_in(a->provider_ctx, ep, buf, cap);
                    if (n >= 0)
                        return n;
                }
            }
            execute_actions(flow, r->actions, NULL, NULL, NULL, 0, NULL);
            return -1;
        }
    }

    return -1;
}

int flow_ep_in(flow_t *flow, uint8_t ep, uint8_t *buf, size_t cap)
{
    GList *ml;
    GQueue *q;
    flow_endpoint_binding_t *b;
    int handled = 0;

    if (!flow)
        return 0;

    for (ml = flow->machines; ml; ml = ml->next) {
        flow_machine_t *m = ml->data;
        flow_state_t *st;
        flow_state_t *curr;
        int depth;

        if (!m || !m->state)
            continue;

        st = g_hash_table_lookup(m->states, m->state);
        curr = st;
        depth = 0;

        while (curr && depth++ < 16) {
            int n = match_and_execute_ep_in(flow, curr->rules, ep, buf, cap, &handled);
            if (n >= 0)
                return n;
            if (handled)
                break;

            if (curr->extends)
                curr = g_hash_table_lookup(m->states, curr->extends);
            else
                break;
        }

        if (handled)
            break;
    }

    if (!handled && flow->common_rules) {
        int n = match_and_execute_ep_in(flow, flow->common_rules, ep, buf, cap, &handled);
        if (n >= 0)
            return n;
    }

    /* Check directly bound endpoint providers */
    b = g_hash_table_lookup(flow->ep_bindings, GUINT_TO_POINTER((guint)ep));
    if (!b)
        b = g_hash_table_lookup(flow->ep_bindings, GUINT_TO_POINTER(0));
    if (b && b->ops.produce_in && b->ctx) {
        int n = b->ops.produce_in(b->ctx, ep, buf, cap);
        if (n >= 0)
            return n;
    }

    q = g_hash_table_lookup(flow->ep_queues, GUINT_TO_POINTER((guint)ep));
    if (q && !g_queue_is_empty(q)) {
        GByteArray *arr = g_queue_peek_head(q);

        if (arr->len <= cap) {
            size_t len = arr->len;

            g_queue_pop_head(q);
            memcpy(buf, arr->data, len);
            g_byte_array_unref(arr);
            return (int)len;
        } else {
            memcpy(buf, arr->data, cap);
            g_byte_array_remove_range(arr, 0, (guint)cap);
            return (int)cap;
        }
    }

    return 0; // NAK
}

static int match_and_execute_ep_out(flow_t *flow, GList *rules,
                                    uint8_t ep, const uint8_t *buf, size_t len)
{
    GList *l;
    int handled = 0;

    for (l = rules; l; l = l->next) {
        flow_rule_t *r = l->data;

        if (r->match.has_endpoint_out && r->match.endpoint_out == ep) {
            if (r->match.match_payload != NULL) {
                if (len < r->match.match_payload->len ||
                    memcmp(buf, r->match.match_payload->data, r->match.match_payload->len) != 0)
                    continue;
            }
            execute_actions(flow, r->actions, NULL, NULL, buf, len, &handled);
        }
    }

    return handled;
}

int flow_ep_out(flow_t *flow, uint8_t ep, const uint8_t *buf, size_t len)
{
    GList *ml;
    flow_endpoint_binding_t *b;
    int handled = 0;

    if (!flow)
        return -1;

    if (buf && len) {
        char hex[3 * 32 + 4];
        size_t shown = len < 32 ? len : 32;
        size_t o = 0;
        for (size_t i = 0; i < shown; i++)
            o += snprintf(hex + o, sizeof(hex) - o, "%02x ", buf[i]);
        if (len > shown)
            snprintf(hex + o, sizeof(hex) - o, "...");
        LOG_IN("ep 0x%02x OUT %zu bytes: %s", ep, len, hex);
    } else {
        LOG_IN("ep 0x%02x OUT %zu bytes", ep, len);
    }

    for (ml = flow->machines; ml; ml = ml->next) {
        flow_machine_t *m = ml->data;
        flow_state_t *st;
        flow_state_t *curr;
        int depth;

        if (!m || !m->state)
            continue;

        st = g_hash_table_lookup(m->states, m->state);
        curr = st;
        depth = 0;

        while (curr && depth++ < 16) {
            handled = match_and_execute_ep_out(flow, curr->rules, ep, buf, len);
            if (handled)
                break;

            if (curr->extends)
                curr = g_hash_table_lookup(m->states, curr->extends);
            else
                break;
        }

        if (handled)
            break;
    }

    if (!handled && flow->common_rules) {
        handled = match_and_execute_ep_out(flow, flow->common_rules, ep, buf, len);
    }

    if (!handled) {
        b = g_hash_table_lookup(flow->ep_bindings, GUINT_TO_POINTER((guint)ep));
        if (!b)
            b = g_hash_table_lookup(flow->ep_bindings, GUINT_TO_POINTER(0));
        if (b && b->ops.consume_out && b->ctx) {
            GQueue *q = g_hash_table_lookup(flow->ep_queues, GUINT_TO_POINTER((guint)0x81));
            if (!q) {
                q = g_queue_new();
                g_hash_table_insert(flow->ep_queues, GUINT_TO_POINTER((guint)0x81), q);
            }
            if (b->ops.consume_out(b->ctx, ep, buf, len, q) == 0)
                handled = 1;
        }
    }

    return 0;
}

static void tick_rules(GList *rules)
{
    GList *l;

    for (l = rules; l; l = l->next) {
        flow_rule_t *r = l->data;
        GList *al;

        for (al = r->actions; al; al = al->next) {
            flow_action_t *a = al->data;

            if (a->type == ACT_ENDPOINT_PROVIDER && a->provider_ops.tick && a->provider_ctx) {
                a->provider_ops.tick(a->provider_ctx);
            }
        }
    }
}

void flow_tick(flow_t *flow)
{
    GList *ml;
    GHashTableIter iter;
    gpointer key, value;

    if (!flow)
        return;

    for (ml = flow->machines; ml; ml = ml->next) {
        flow_machine_t *m = ml->data;
        flow_state_t *st;
        flow_state_t *curr;
        int depth;

        if (!m || !m->state)
            continue;

        st = g_hash_table_lookup(m->states, m->state);
        curr = st;
        depth = 0;

        while (curr && depth++ < 16) {
            tick_rules(curr->rules);
            if (curr->extends)
                curr = g_hash_table_lookup(m->states, curr->extends);
            else
                break;
        }
    }

    tick_rules(flow->common_rules);

    /* Tick bound endpoint providers */
    g_hash_table_iter_init(&iter, flow->ep_bindings);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        flow_endpoint_binding_t *b = value;
        if (b && b->ops.tick && b->ctx) {
            b->ops.tick(b->ctx);
        }
    }

    for (ml = flow->machines; ml; ml = ml->next) {
        flow_machine_t *m = ml->data;

        if (m && m->next_state) {
            char *target = m->next_state;

            m->next_state = NULL;
            flow_machine_transition_to(flow, m, target);
            g_free(target);
        }
    }
}
