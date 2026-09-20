#include "utils.h"
#include "version.h"

struct yaml_version {
    yaml_versions_enum_t version;
    char *str;
};

/* Strings of each supported version of the YAML structured. */
static const yaml_version_t yaml_versions[] = {
    { YAML_VERSION_1_0, "1.0" },
    { YAML_VERSION_INVALID, NULL }
};

yaml_versions_enum_t match_version(const char *ver) {
    size_t i = 0;

    if (!ver || !*ver)
        return YAML_VERSION_INVALID;

    while (yaml_versions[i].version != YAML_VERSION_INVALID) {
        if (!cmp_versions(ver, yaml_versions[i].str))
            return yaml_versions[i].version;
	i++;
    }

    return YAML_VERSION_INVALID;
}
