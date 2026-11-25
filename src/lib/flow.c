#include "flow.h"
#include "yaml/util.h" // for yaml_map_get, yaml_scalar
#include "log.h"
#include "utils.h"
#include "stream/stream_provider.h"

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
    ACT_STREAM_FROM_FILE
} flow_action_type_t;

typedef struct {
    flow_action_type_t type;
    char *respond_with;
    GByteArray *respond_bytes;
    uint8_t respond_with_status;
    char *transition_state;
    int return_nak_if_queue_empty;
    uint8_t ep;
    GQueue *payloads; // of GByteArray*
    char *stream_file;
    gboolean stream_loop;
    stream_provider_t *stream_provider;
} flow_action_t;

typedef struct {
    flow_match_t match;
    GList *actions; // of flow_action_t*
} flow_rule_t;

typedef struct {
    char *name;
    GList *on_enter; // of flow_action_t*
    GList *rules;    // of flow_rule_t*
} flow_state_t;

struct flow {
    char *state; // current state name
    char *next_state;
    GHashTable *states; // name -> flow_state_t*
    GHashTable *ep_queues; // ep (uint8_t) -> GQueue of GByteArray*
    uint8_t last_cbw_tag[4];
    gboolean has_cbw_tag;
};

static void flow_action_free(flow_action_t *act) {
    if (!act)
        return;

    g_free(act->respond_with);
    if (act->respond_bytes)
        g_byte_array_unref(act->respond_bytes);
    g_free(act->transition_state);
    g_free(act->stream_file);

    if (act->stream_provider)
        stream_provider_free(act->stream_provider);

    if (act->payloads)
        g_queue_free_full(act->payloads, (GDestroyNotify)g_byte_array_unref);

    g_free(act);
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
    g_list_free_full(st->on_enter, (GDestroyNotify)flow_action_free);
    g_list_free_full(st->rules, (GDestroyNotify)flow_rule_free);
    g_free(st);
}

flow_t *flow_create(void)
{
    flow_t *f = g_new0(flow_t, 1);

    f->states = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, (GDestroyNotify)flow_state_free);
    f->ep_queues = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_queue_free);

    return f;
}

void flow_free(flow_t *flow)
{
    if (!flow)
        return;

    g_free(flow->state);
    g_free(flow->next_state);
    
    GHashTableIter iter;
    gpointer key, value;
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
    g_hash_table_destroy(flow->states);
    g_free(flow);
}

const char *flow_current_state(flow_t *flow)
{
    return flow ? flow->state : NULL;
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

static flow_action_t *parse_stream_action(yaml_document_t *doc, yaml_node_t *v, uint8_t ep,
                                          const char *flow_dir, const char *base_dir)
{
    flow_action_t *a = g_new0(flow_action_t, 1);
    const char *file_str = NULL;
    gboolean loop_val = TRUE;

    a->type = ACT_STREAM_FROM_FILE;
    a->ep = ep;

    if (v->type == YAML_MAPPING_NODE) {
        yaml_node_t *fn = yaml_map_get(doc, v, "file");
        yaml_node_t *ln = yaml_map_get(doc, v, "loop");
        if (fn != NULL)
            file_str = yaml_scalar(fn);
        if (ln != NULL) {
            const char *ls = yaml_scalar(ln);
            loop_val = (g_strcmp0(ls, "false") != 0 && g_strcmp0(ls, "0") != 0 && g_strcmp0(ls, "no") != 0);
        }
    } else if (v->type == YAML_SCALAR_NODE) {
        file_str = yaml_scalar(v);
    }

    if (file_str != NULL) {
        char *resolved = resolve_flow_asset(base_dir, flow_dir, file_str);
        a->stream_file = g_strdup(file_str);
        a->stream_loop = loop_val;
        a->stream_provider = stream_provider_new(resolved, loop_val, ep);
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
            a->transition_state = g_strdup(yaml_scalar(v));
        } else if ((v = yaml_map_get(doc, item, "return_nak_if_queue_empty"))) {
            a = g_new0(flow_action_t, 1);
            a->type = ACT_RETURN_NAK_IF_QUEUE_EMPTY;
            a->return_nak_if_queue_empty = (g_strcmp0(yaml_scalar(v), "true") == 0);
        } else if ((v = yaml_map_get(doc, item, "queue_endpoint_data"))) {
            a = parse_queue_endpoint_data(doc, v);
        } else if ((v = yaml_map_get(doc, item, "stream_from_file")) ||
                   (v = yaml_map_get(doc, item, "stream"))) {
            a = parse_stream_action(doc, v, ep_hint, flow_dir, base_dir);
        }

        if (a && a->type == ACT_STREAM_FROM_FILE && (v = yaml_map_get(doc, item, "loop"))) {
            const char *ls = yaml_scalar(v);
            a->stream_loop = (g_strcmp0(ls, "false") != 0 && g_strcmp0(ls, "0") != 0 && g_strcmp0(ls, "no") != 0);
            if (a->stream_provider != NULL) {
                a->stream_provider->loop = a->stream_loop;
            }
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
    yaml_node_t *states_n = yaml_map_get(&doc, top, "states");
    if (states_n && states_n->type == YAML_SEQUENCE_NODE) {
        for (yaml_node_item_t *i = states_n->data.sequence.items.start; i < states_n->data.sequence.items.top; i++) {
            yaml_node_t *item = yaml_document_get_node(&doc, *i);
            if (!item || item->type != YAML_MAPPING_NODE) continue;
            
            flow_state_t *st = g_new0(flow_state_t, 1);
            yaml_node_t *name_n = yaml_map_get(&doc, item, "name");
            if (name_n) {
                const char *s = yaml_scalar(name_n);
                if (s) st->name = g_strdup(s);
            }
            
            yaml_node_t *enter_n = yaml_map_get(&doc, item, "on_enter");
            if (enter_n) st->on_enter = parse_actions(&doc, enter_n, 0, flow_dir, base_dir);
            
            yaml_node_t *rules_n = yaml_map_get(&doc, item, "rules");
            if (rules_n) st->rules = parse_rules(&doc, rules_n, flow_dir, base_dir);
            
            if (st->name) {
                if (!f->state) f->state = g_strdup(st->name); // first state is default
                g_hash_table_insert(f->states, st->name, st);
            } else {
                flow_state_free(st);
            }
        }
    }

    g_free(flow_dir);
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
    fclose(fp);
    return f;
}

static void execute_actions(flow_t *flow, GList *actions, uint8_t *out, uint16_t *len, int *handled) {
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
                LOG_DEBUG("[flow] ACT_TRANSITION_STATE");
                g_free(flow->next_state);
                flow->next_state = g_strdup(a->transition_state);
                break;
            case ACT_QUEUE_ENDPOINT_DATA: {
                GQueue *q = g_hash_table_lookup(flow->ep_queues, GUINT_TO_POINTER((guint)a->ep));
                if (!q) {
                    q = g_queue_new();
                    g_hash_table_insert(flow->ep_queues, GUINT_TO_POINTER((guint)a->ep), q);
                }
                for (GList *pl = a->payloads->head; pl; pl = pl->next) {
                    GByteArray *arr = pl->data;
                    /* If this is a 13-byte CSW ("USBS" at offset 0) and we tracked a CBW tag,
                     * dynamically update CSW tag at offset 4..7 to match CBW tag */
                    if (arr->len == 13 && memcmp(arr->data, "USBS", 4) == 0 && flow->has_cbw_tag) {
                        GByteArray *csw_copy = g_byte_array_sized_new(13);
                        g_byte_array_append(csw_copy, arr->data, 13);
                        memcpy(csw_copy->data + 4, flow->last_cbw_tag, 4);
                        g_queue_push_tail(q, csw_copy);
                    } else {
                        g_queue_push_tail(q, g_byte_array_ref(arr));
                    }
                }
                break;
            }
            case ACT_RETURN_NAK_IF_QUEUE_EMPTY:
                break;
            case ACT_STREAM_FROM_FILE:
                break;
        }
    }
}

int flow_control(flow_t *flow, const uint8_t *setup,
                 uint8_t *out, uint16_t *len)
{
    if (!flow || !flow->state) return -1;
    flow_state_t *st = g_hash_table_lookup(flow->states, flow->state);
    if (!st) return -1;
    
    uint8_t bmRequestType = setup[0];
    uint8_t bRequest = setup[1];
    uint16_t wValue = setup[2] | (setup[3] << 8);
    uint16_t wIndex = setup[4] | (setup[5] << 8);

    for (GList *l = st->rules; l; l = l->next) {
        flow_rule_t *r = l->data;
        int match = 1;
        if (r->match.has_bmRequestType && r->match.bmRequestType != bmRequestType) match = 0;
        if (r->match.has_bRequest && r->match.bRequest != bRequest) match = 0;
        if (r->match.has_wValue && r->match.wValue != wValue) match = 0;
        if (r->match.has_wIndex && r->match.wIndex != wIndex) match = 0;
        if (r->match.has_endpoint_poll || r->match.has_endpoint_out) match = 0;
        
        if (match) {
            int handled = 0;
            execute_actions(flow, r->actions, out, len, &handled);
            if (handled) return 0;
        }
    }
    return -1;
}

int flow_ep_in(flow_t *flow, uint8_t ep, uint8_t *buf, size_t cap)
{
    if (!flow || !flow->state) return 0;
    
    flow_state_t *st = g_hash_table_lookup(flow->states, flow->state);
    if (st) {
        for (GList *l = st->rules; l; l = l->next) {
            flow_rule_t *r = l->data;
            if (r->match.has_endpoint_poll && r->match.endpoint_poll == ep) {
                for (GList *al = r->actions; al; al = al->next) {
                    flow_action_t *a = al->data;
                    if (a->type == ACT_STREAM_FROM_FILE && a->stream_provider != NULL) {
                        int n = stream_provider_produce_packet(a->stream_provider, ep, buf, cap);
                        if (n >= 0) {
                            return n;
                        }
                    }
                }
                execute_actions(flow, r->actions, NULL, NULL, NULL);
            }
        }
    }

    GQueue *q = g_hash_table_lookup(flow->ep_queues, GUINT_TO_POINTER((guint)ep));
    if (q && !g_queue_is_empty(q)) {
        GByteArray *arr = g_queue_pop_head(q);
        size_t len = arr->len;
        if (len > cap) len = cap;
        memcpy(buf, arr->data, len);
        g_byte_array_unref(arr);
        return len;
    }
    
    return 0; // NAK
}

int flow_ep_out(flow_t *flow, uint8_t ep, const uint8_t *buf, size_t len)
{
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

        /* If this is a 31-byte MSC Command Block Wrapper ("USBC" at offset 0),
         * capture its 4-byte Tag at offset 4..7 */
        if (len >= 31 && memcmp(buf, "USBC", 4) == 0) {
            memcpy(flow->last_cbw_tag, buf + 4, 4);
            flow->has_cbw_tag = TRUE;
        }
    } else {
        LOG_IN("ep 0x%02x OUT %zu bytes", ep, len);
    }

    if (!flow->state)
        return 0;

    flow_state_t *st = g_hash_table_lookup(flow->states, flow->state);
    if (st) {
        for (GList *l = st->rules; l; l = l->next) {
            flow_rule_t *r = l->data;
            if (r->match.has_endpoint_out && r->match.endpoint_out == ep) {
                if (r->match.match_payload != NULL) {
                    /* If match payload specified, check if buf contains or matches prefix */
                    if (len < r->match.match_payload->len ||
                        memcmp(buf, r->match.match_payload->data, r->match.match_payload->len) != 0)
                        continue;
                }
                int handled = 0;
                execute_actions(flow, r->actions, NULL, NULL, &handled);
            }
        }
    }

    return 0;
}

void flow_tick(flow_t *flow)
{
    if (!flow)
        return;

    if (flow->state) {
        flow_state_t *st = g_hash_table_lookup(flow->states, flow->state);
        if (st) {
            for (GList *l = st->rules; l; l = l->next) {
                flow_rule_t *r = l->data;
                for (GList *al = r->actions; al; al = al->next) {
                    flow_action_t *a = al->data;
                    if (a->type == ACT_STREAM_FROM_FILE && a->stream_provider != NULL) {
                        stream_provider_tick(a->stream_provider);
                    }
                }
            }
        }
    }

    if (flow->next_state) {
        g_free(flow->state);
        flow->state = flow->next_state;
        flow->next_state = NULL;
        
        flow_state_t *st = g_hash_table_lookup(flow->states, flow->state);
        if (st && st->on_enter) {
            execute_actions(flow, st->on_enter, NULL, NULL, NULL);
        }
    }
}
