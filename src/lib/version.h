#ifndef VERSION_H
#define VERSION_H

#define USB_VCR_VERSION "1.0.0"

typedef enum {
    YAML_VERSION_INVALID,
    YAML_VERSION_1_0,
} yaml_versions_enum_t;

typedef struct yaml_version yaml_version_t;

/* Map a version string to its enum (YAML_VERSION_INVALID if unsupported). */
yaml_versions_enum_t match_version(const char *ver);

#endif /* VERSION_H */
