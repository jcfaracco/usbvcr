#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <yaml.h>

#include "log.h"
#include "utils.h"
#include "usb_yaml.h"

#define ROOT_SCOPES(X) \
    X(packet)

#define PACKET_SCOPES(X) \
    X(USBpcap) X(USBpcapSetup) X(Raw)

#define USBPCAP_FIELDS(X) \
    X(header_len) X(irp_id) X(status) X(function) X(info) \
    X(bus) X(device) X(endpoint) X(transfer) X(data_length)

#define USBPCAP_SETUP_FIELDS(X) \
    X(bmRequestType) X(bRequest) X(wValue) X(wIndex) X(wLength)

#define RAW_FIELDS(X) \
    X(load)

#define GENERATE_ENUM(id) FIELD_##id,
#define GENERATE_SCOPE_ENUM(id) SCOPE_##id,

#define MATCH_STRING_TO_ENUM(id) if (strcmp(str, #id) == 0) return FIELD_##id;
#define MATCH_STRING_TO_SCOPE(id) if (strcmp(str, #id) == 0) return SCOPE_##id;

typedef enum {
    SCOPE_ROOT_UNKNOWN = 0,
    ROOT_SCOPES(GENERATE_SCOPE_ENUM)
} root_scopes_enum_t;

typedef enum {
    SCOPE_PACKET_UNKNOWN = 0,
    PACKET_SCOPES(GENERATE_SCOPE_ENUM)
} packet_scopes_enum_t;

typedef enum {
    FIELD_UNKNOWN = 0,
    USBPCAP_FIELDS(GENERATE_ENUM)
} usb_pcap_fields_enum_t;

typedef enum {
    FIELD_SETUP_UNKNOWN = 0,
    USBPCAP_SETUP_FIELDS(GENERATE_ENUM)
} usb_pcap_setup_fields_enum_t;

typedef enum {
    FIELD_RAW_UNKNOWN = 0,
    RAW_FIELDS(GENERATE_ENUM)
} raw_fields_enum_t;

static inline root_scopes_enum_t parse_root_scope_name(const char *str)
{
    ROOT_SCOPES(MATCH_STRING_TO_SCOPE)
    return SCOPE_ROOT_UNKNOWN;
}

static inline packet_scopes_enum_t parse_packet_scope_name(const char *str)
{
    PACKET_SCOPES(MATCH_STRING_TO_SCOPE)
    return SCOPE_PACKET_UNKNOWN;
}

static inline usb_pcap_fields_enum_t parse_usbpcap_field(const char *str)
{
    USBPCAP_FIELDS(MATCH_STRING_TO_ENUM)
    return FIELD_UNKNOWN;
}

static inline usb_pcap_setup_fields_enum_t parse_setup_field(const char *str)
{
    USBPCAP_SETUP_FIELDS(MATCH_STRING_TO_ENUM)
    return FIELD_SETUP_UNKNOWN;
}

static inline raw_fields_enum_t parse_raw_field(const char *str)
{
    RAW_FIELDS(MATCH_STRING_TO_ENUM)
    return FIELD_RAW_UNKNOWN;
}

static inline bool is_packet(const char *str)
{
    return (parse_root_scope_name(str) == SCOPE_packet);
}

static inline bool is_sub_packet(const char *str)
{
    return (parse_packet_scope_name(str) != SCOPE_PACKET_UNKNOWN);
}

void fill_usb_pcap(usb_pcap_t *b,
                   const char *key, const char *val)
{
    if (!b)
        return;

    switch (parse_usbpcap_field(key)) {
        case FIELD_header_len:  b->header_len = atoi(val); break;
        case FIELD_irp_id:      b->irp_id = strtoull(val, NULL, 16); break;
        case FIELD_status:      b->status = atoi(val); break;
        case FIELD_function:    b->function = (int)strtol(val, NULL, 16); break;
        case FIELD_info:        b->info = atoi(val); break;
        case FIELD_bus:         b->bus = atoi(val); break;
        case FIELD_device:      b->device = atoi(val); break;
        case FIELD_endpoint:    b->endpoint = (int)strtol(val, NULL, 16); break;
        case FIELD_transfer:    b->transfer = atoi(val); break;
        case FIELD_data_length: b->data_length = atoi(val); break;
        default: break;
    }
}

void fill_usb_pcap_setup(usb_control_packet_header_t *s,
                         const char *key, const char *val)
{
    if (!s)
        return;

    switch (parse_setup_field(key)) {
        case FIELD_bmRequestType: s->requesttype = (int)strtol(val, NULL, 16); break;
        case FIELD_bRequest:      s->request = (int)strtol(val, NULL, 16); break;
        case FIELD_wValue:        s->value = (int)strtol(val, NULL, 16); break;
        case FIELD_wIndex:        s->index = (int)strtol(val, NULL, 16); break;
        case FIELD_wLength:       s->length = atoi(val); break;
        default: break;
    }
}

void fill_raw(packet_raw_t *r,
              const char *key, const char *val)
{
    if (!r)
        return;

    switch (parse_raw_field(key)) {
        case FIELD_load: {
            size_t raw_len = strlen(val);
            r->load = g_memdup2(val, raw_len);
            r->len = raw_len;
            break;
        }
        default: break;
    }
}

GList *read_yaml(const char *path)
{
    yaml_parser_t parser;
    yaml_event_t event;
    packet_scopes_enum_t packet_scope = SCOPE_PACKET_UNKNOWN;
    packet_t *cur_packet = NULL;
    GList *packets = NULL;
    FILE *fp = NULL;
    int done = false;
    char *key = NULL;

    fp = fopen(path, "r");
    if (!fp) {
        LOG_ERR("Failed to open file %s\n", path);
        return NULL;
    }

    if (!yaml_parser_initialize(&parser)) {
        LOG_ERR("Failed to initialize yaml parser!\n");
        fclose(fp);
        return NULL;
    }

    yaml_parser_set_input_file(&parser, fp);

    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) {
            LOG_ERR("YAML parser error %d\n", parser.error);
            break;
        }

        switch (event.type) {
            case YAML_STREAM_END_EVENT:
                done = 1; /* End loop when stream finishes */
                break;
            case YAML_SEQUENCE_END_EVENT:
                /* A completed packet closes with a sequence end; the outer
                 * sequence has no pending packet, so guard against NULL. */
                if (cur_packet) {
                    packets = g_list_append(packets, cur_packet);
                    cur_packet = NULL;
                }
                break;
            case YAML_SCALAR_EVENT: {
                const char *val = (const char *) event.data.scalar.value;

                if (is_packet(val)) {
                    cur_packet = g_new0(packet_t, 1);
                    cur_packet->usb_pcap = g_new0(usb_pcap_t, 1);
                    cur_packet->usb_pcap_setup = g_new0(usb_control_packet_header_t, 1);
                    cur_packet->raw = g_new0(packet_raw_t, 1);
                    packet_scope = SCOPE_PACKET_UNKNOWN;
                } else if (is_sub_packet(val)) {
                    packet_scope = parse_packet_scope_name(val);
                } else if (!key) {
                    key = g_strdup(val);
                } else {
                    if (cur_packet) {
                        switch (packet_scope) {
                            case SCOPE_USBpcap:
                                fill_usb_pcap(cur_packet->usb_pcap, key, val);
                                break;
                            case SCOPE_USBpcapSetup:
                                fill_usb_pcap_setup(cur_packet->usb_pcap_setup, key, val);
                                break;
                            case SCOPE_Raw:
                                fill_raw(cur_packet->raw, key, val);
                                break;
                            default:
                                break;
                        }
                    }
                    g_clear_pointer(&key, g_free);
                }
                break;
            }
            default:
                break;
        }

        yaml_event_delete(&event);
    }

    g_free(key);

    yaml_parser_delete(&parser);

    fclose(fp);

    return packets;
}
