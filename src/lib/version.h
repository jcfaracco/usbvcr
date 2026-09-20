#ifndef __VERSION_H_
#define __VERSION_H_


typedef enum {
    YAML_VERSION_INVALID,
    YAML_VERSION_1_0,
} yaml_versions_enum_t;

typedef struct yaml_version yaml_version_t;

/* Map a version string to its enum (YAML_VERSION_INVALID if unsupported). */
yaml_versions_enum_t match_version(const char *ver);

#endif
