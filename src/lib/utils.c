#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <limits.h>
#endif

#include "utils.h"


/* Returns 1 if v1 > v2, -1 if v1 < v2, and 0 if v1 == v2 */
int cmp_versions(const char *v1, const char *v2) {
    const char *p1 = v1;
    const char *p2 = v2;

    while (*p1 || *p2) {
        long num1 = 0;
        long num2 = 0;

        if (*p1) {
            num1 = strtol(p1, (char **)&p1, 10);
        }
        if (*p2) {
            num2 = strtol(p2, (char **)&p2, 10);
        }

        if (num1 > num2) return 1;
        if (num1 < num2) return -1;

        // Skip the dot separator if present
        if (*p1 == '.') p1++;
        if (*p2 == '.') p2++;
    }

    return 0;
}


/* Return index of first string in `set` equal to `s`, else -1. */
int str_index_in_set(const char *s, const char *const set[], size_t n) {
    if (!s)
        return -1;
    for (size_t i = 0; i < n; i++)
        if (set[i] && strcmp(s, set[i]) == 0)
            return (int)i;
    return -1;
}


/* TODO: Check if there is an option instead of developing it. */
void trim_string(char *dst, const char *src, size_t dst_size) {
    size_t len = 0;

    while (isspace((unsigned char)*src))
        src++;

    while (src[len] != '\0' && src[len] != '#' && len < dst_size - 1) {
        len++;
    }

    while (len > 0 && isspace((unsigned char)src[len - 1]))
        len--;

    strncpy(dst, src, len);
    dst[len] = '\0';
}


char *usbvcr_get_data_dir(void)
{
    const char *env;
    char *path = NULL;
    const gchar * const *system_dirs;
#if defined(__APPLE__)
    char ep_buf[PATH_MAX];
    uint32_t ep_size = sizeof(ep_buf);
#endif

    env = g_getenv("USBVCR_DATA_DIR");
    if (env != NULL && g_file_test(env, G_FILE_TEST_IS_DIR))
    {
        return g_strdup(env);
    }

    if (g_file_test("data", G_FILE_TEST_IS_DIR))
    {
        return g_canonicalize_filename("data", NULL);
    }

    if (g_file_test("../data", G_FILE_TEST_IS_DIR))
    {
        return g_canonicalize_filename("../data", NULL);
    }

#if defined(__APPLE__)
    if (_NSGetExecutablePath(ep_buf, &ep_size) == 0)
    {
        path = g_canonicalize_filename(ep_buf, NULL);
    }
#else
    path = g_file_read_link("/proc/self/exe", NULL);
#endif

    if (path != NULL)
    {
        char *bindir = g_path_get_dirname(path);
        char *candidate;

        g_free(path);

        candidate = g_build_filename(bindir, "..", "..", "data", NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_DIR))
        {
            g_free(bindir);
            return candidate;
        }
        g_free(candidate);

        candidate = g_build_filename(bindir, "..", "data", NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_DIR))
        {
            g_free(bindir);
            return candidate;
        }
        g_free(candidate);

        candidate = g_build_filename(bindir, "..", "share", "usbvcr", NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_DIR))
        {
            g_free(bindir);
            return candidate;
        }
        g_free(candidate);
        g_free(bindir);
    }

#ifdef USBVCR_DATA_DIR
    if (g_file_test(USBVCR_DATA_DIR, G_FILE_TEST_IS_DIR))
    {
        return g_strdup(USBVCR_DATA_DIR);
    }
#endif

    system_dirs = g_get_system_data_dirs();
    if (system_dirs != NULL)
    {
        for (gsize i = 0; system_dirs[i] != NULL; i++)
        {
            char *candidate = g_build_filename(system_dirs[i], "usbvcr", NULL);
            if (g_file_test(candidate, G_FILE_TEST_IS_DIR))
            {
                return candidate;
            }
            g_free(candidate);
        }
    }

    return NULL;
}


char *usbvcr_find_data_file(const char *subdir, const char *filename)
{
    char *datadir;
    char *target;

    if (filename == NULL)
    {
        return NULL;
    }

    datadir = usbvcr_get_data_dir();
    if (datadir == NULL)
    {
        return NULL;
    }

    if (subdir != NULL)
    {
        target = g_build_filename(datadir, subdir, filename, NULL);
    }
    else
    {
        target = g_build_filename(datadir, filename, NULL);
    }

    if (g_file_test(target, G_FILE_TEST_EXISTS))
    {
        g_free(datadir);
        return target;
    }

    if (!g_str_has_suffix(filename, ".yaml") && !g_str_has_suffix(filename, ".yml"))
    {
        char *with_yaml = g_strconcat(target, ".yaml", NULL);
        if (g_file_test(with_yaml, G_FILE_TEST_EXISTS))
        {
            g_free(target);
            g_free(datadir);
            return with_yaml;
        }
        g_free(with_yaml);
    }

    g_free(target);
    g_free(datadir);
    return NULL;
}
