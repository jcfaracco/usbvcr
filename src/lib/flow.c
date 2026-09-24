#include "flow.h"
#include "yaml_util.h" // for yaml_map_get, yaml_scalar
#include "log.h"
#include "utils.h"

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
} flow_match_t;

typedef enum {
    ACT_RESPOND_WITH,
    ACT_RESPOND_WITH_STATUS,
    ACT_TRANSITION_STATE,
    ACT_RETURN_NAK_IF_QUEUE_EMPTY,
    ACT_QUEUE_ENDPOINT_DATA
} flow_action_type_t;

typedef struct {
    flow_action_type_t type;
    char *respond_with;
    uint8_t respond_with_status;
    char *transition_state;
    int return_nak_if_queue_empty;
    uint8_t ep;
    GQueue *payloads; // of GByteArray*
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
};

static void flow_action_free(flow_action_t *act) {
    if (!act)
        return;

    g_free(act->respond_with);
    g_free(act->transition_state);

    if (act->payloads)
        g_queue_free_full(act->payloads, (GDestroyNotify)g_byte_array_unref);

    g_free(act);
}

static void flow_rule_free(flow_rule_t *rule) {
    if (!rule)
        return;

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
    return m;
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

static GList* parse_actions(yaml_document_t *doc, yaml_node_t *seq) {
    GList *list = NULL;
    if (!seq || seq->type != YAML_SEQUENCE_NODE) return NULL;

    for (yaml_node_item_t *i = seq->data.sequence.items.start; i < seq->data.sequence.items.top; i++) {
        yaml_node_t *item = yaml_document_get_node(doc, *i);
        if (!item || item->type != YAML_MAPPING_NODE) continue;
        
        yaml_node_t *v;
        if ((v = yaml_map_get(doc, item, "respond_with"))) {
            flow_action_t *a = g_new0(flow_action_t, 1);
            a->type = ACT_RESPOND_WITH;
            a->respond_with = g_strdup(yaml_scalar(v));
            list = g_list_append(list, a);
        } else if ((v = yaml_map_get(doc, item, "respond_with_status"))) {
            flow_action_t *a = g_new0(flow_action_t, 1);
            a->type = ACT_RESPOND_WITH_STATUS;
            a->respond_with_status = parse_int(yaml_scalar(v));
            list = g_list_append(list, a);
        } else if ((v = yaml_map_get(doc, item, "transition_state"))) {
            flow_action_t *a = g_new0(flow_action_t, 1);
            a->type = ACT_TRANSITION_STATE;
            a->transition_state = g_strdup(yaml_scalar(v));
            list = g_list_append(list, a);
        } else if ((v = yaml_map_get(doc, item, "return_nak_if_queue_empty"))) {
            flow_action_t *a = g_new0(flow_action_t, 1);
            a->type = ACT_RETURN_NAK_IF_QUEUE_EMPTY;
            a->return_nak_if_queue_empty = (g_strcmp0(yaml_scalar(v), "true") == 0);
            list = g_list_append(list, a);
        } else if ((v = yaml_map_get(doc, item, "queue_endpoint_data"))) {
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
                        if (hex_n) {
                            g_queue_push_tail(a->payloads, parse_hex_string(yaml_scalar(hex_n)));
                        }
                    }
                }
            }
            list = g_list_append(list, a);
        }
    }
    return list;
}

static GList* parse_rules(yaml_document_t *doc, yaml_node_t *seq) {
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
            r->actions = parse_actions(doc, a_node);
        }
        list = g_list_append(list, r);
    }
    return list;
}

flow_t *flow_load(const char *path) {
    FILE *fp = NULL;

    if (!path)
        return NULL;

    fp = fopen(path, "rb");
    if (!fp) return NULL;

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fclose(fp);
        return NULL;
    }
    yaml_parser_set_input_file(&parser, fp);

    yaml_document_t doc;
    if (!yaml_parser_load(&parser, &doc)) {
        yaml_parser_delete(&parser);
        fclose(fp);
        return NULL;
    }

    yaml_node_t *top = yaml_document_get_root_node(&doc);
    if (!top || top->type != YAML_MAPPING_NODE) {
        yaml_document_delete(&doc);
        yaml_parser_delete(&parser);
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
            if (enter_n) st->on_enter = parse_actions(&doc, enter_n);
            
            yaml_node_t *rules_n = yaml_map_get(&doc, item, "rules");
            if (rules_n) st->rules = parse_rules(&doc, rules_n);
            
            if (st->name) {
                if (!f->state) f->state = g_strdup(st->name); // first state is default
                g_hash_table_insert(f->states, st->name, st);
            } else {
                flow_state_free(st);
            }
        }
    }

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
                if (out && len && a->respond_with) {
                    size_t slen = strlen(a->respond_with) + 1;
                    if (slen > 1024) slen = 1024;
                    memcpy(out, a->respond_with, slen);
                    *len = slen;
                    if (handled) *handled = 1;
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
                    g_queue_push_tail(q, g_byte_array_ref(arr));
                }
                break;
            }
            case ACT_RETURN_NAK_IF_QUEUE_EMPTY:
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
        if (r->match.has_endpoint_poll) match = 0;
        
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
    (void)flow; (void)ep; (void)buf; (void)len;
    return 0;
}

void flow_tick(flow_t *flow)
{
    if (!flow)
        return;

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
