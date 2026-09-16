#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "log.h"
#include "utils.h"
#include "usb_yaml.h"

#define ROOT_FIELDS(X) \
    X(packet)

#define PACKET_SCOPES(X) \
    X(USBpcap) X(USBpcapSetup) X(Raw)

#define USBPCAP_FIELDS(X) \
    X(header_len) X(irp_id) X(status) X(function) X(info) \
    X(bus) X(device) X(endpoint) X(transfer) X(data_length)

#define SETUP_FIELDS(X) \
    X(bmRequestType) X(bRequest) X(wValue) X(wIndex) X(wLength)

#define RAW_FIELDS(X) \
    X(load)

#define GENERATE_ENUM(id) FIELD_##id,
#define GENERATE_SCOPE_ENUM(id) SCOPE_##id,

#define MATCH_STRING_TO_ENUM(id) if (strcmp(str, #id) == 0) return FIELD_##id;
#define MATCH_STRING_TO_SCOPE(id) if (strcmp(str, #id) == 0) return SCOPE_##id;

typedef enum {
    ROOT_UNKNOWN = 0,
    ROOT_FIELDS(GENERATE_ENUM)
} root_fields_enum_t;

typedef enum {
    SCOPE_UNKNOWN = 0,
    PACKET_SCOPES(GENERATE_SCOPE_ENUM)
} packet_key_enum_t;

typedef enum {
    FIELD_UNKNOWN = 0,
    USBPCAP_FIELDS(GENERATE_ENUM)
} usb_pcap_fields_enum_t;

typedef enum {
    FIELD_SETUP_UNKNOWN = 0,
    SETUP_FIELDS(GENERATE_ENUM)
} usb_pcap_setup_fields_enum_t;

typedef enum {
    FIELD_RAW_UNKNOWN = 0,
    RAW_FIELDS(GENERATE_ENUM)
} raw_fields_enum_t;

typedef enum {
    STATE_EXPECT_KEY,
    STATE_EXPECT_VALUE
} yaml_parser_state_enum_t;

static inline root_fields_enum_t parse_root_field(const char *str) {
    ROOT_FIELDS(MATCH_STRING_TO_ENUM)
    return ROOT_UNKNOWN;
}

static inline packet_key_enum_t parse_scope_name(const char *str) {
    PACKET_SCOPES(MATCH_STRING_TO_SCOPE)
    return SCOPE_UNKNOWN;
}

static inline usb_pcap_fields_enum_t parse_usbpcap_field(const char *str) {
    USBPCAP_FIELDS(MATCH_STRING_TO_ENUM)
    return FIELD_UNKNOWN;
}

static inline usb_pcap_setup_fields_enum_t parse_setup_field(const char *str) {
    SETUP_FIELDS(MATCH_STRING_TO_ENUM)
    return FIELD_SETUP_UNKNOWN;
}

static inline raw_fields_enum_t parse_raw_field(const char *str) {
    RAW_FIELDS(MATCH_STRING_TO_ENUM)
    return FIELD_RAW_UNKNOWN;
}

void map_usbpcap(usb_pcap_t *b,
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

void map_usb_pcap_setup(usb_control_packet_header_t *s,
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

void map_raw(packet_raw_t *r,
             const char *key, const char *val)
{
    if (!r)
        return;

    switch (parse_setup_field(key)) {
        case FIELD_load:
            size_t raw_len = strlen(val);
            r->load = malloc(raw_len);
            memcpy(r->load, val, raw_len);
            r->len = raw_len;
            break;
        default: break;
    }
}

packet_t *read_yaml(const char *path)
{
    yaml_parser_t parser;
    yaml_event_t event;
    FILE *fp = NULL;
    int done = 0;

    yaml_parser_state_enum_t state = STATE_EXPECT_KEY;
    packet_key_enum_t active_scope = SCOPE_UNKNOWN;

    char current_key[128] = "";
    char clean_value[128] = "";

    packet_t current_packet = { NULL };
    int packet_counter = 0;

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

    while (!done) {
        if (!yaml_parser_parse(&parser, &event)) {
            fprintf(stderr, "Parser error %d\n", parser.error);
            break;
        }

        // Handle different event types
        switch (event.type) {
            case YAML_NO_EVENT:
                break;
            case YAML_STREAM_START_EVENT:
                printf("[Stream Start]\n");
                break;
            case YAML_STREAM_END_EVENT:
                printf("[Stream End]\n");
                done = 1; // End loop when stream finishes
                break;
            case YAML_DOCUMENT_START_EVENT:
                printf("  [Document Start]\n");
                break;
            case YAML_DOCUMENT_END_EVENT:
                printf("  [Document End]\n");
                break;
            case YAML_MAPPING_START_EVENT:
                printf("    [Mapping Start]\n");
                break;
            case YAML_MAPPING_END_EVENT:
                printf("    [Mapping End]\n");
                break;
            case YAML_SEQUENCE_START_EVENT:
                printf("    [Sequence Start]\n");
                break;
            case YAML_SEQUENCE_END_EVENT:
                printf("    [Sequence End]\n");
                break;
            case YAML_SCALAR_EVENT:
                // Print the raw string text of the scalar value
                printf("      Scalar value: %s\n", event.data.scalar.value);
                break;
            case YAML_ALIAS_EVENT:
                printf("      [Alias]: %s\n", event.data.alias.anchor);
                break;
        }

        yaml_event_delete(&event);
    }

    yaml_parser_delete(&parser);

    fclose(fp);
}
