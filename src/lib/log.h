#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#define USB_DEBUG_PREFIX "[*]"
#define USB_INFO_PREFIX  "[+]"
#define USB_WARN_PREFIX  "[!]"
#define USB_ERR_PREFIX   "[E]"
#define USB_IN_PREFIX    "[<]"
#define USB_OUT_PREFIX   "[>]"

// Log levels
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

// Current threshold; defined in log.c.
extern int current_log_level;

// Helper macros. DEBUG/ERR also emit file:line metadata.
// Format lives in __VA_ARGS__ so a bare message stays ISO C (no ##__VA_ARGS__).
#define LOG_DEBUG(...) \
    do { if (current_log_level <= LOG_LEVEL_DEBUG) { \
        fprintf(stdout, USB_DEBUG_PREFIX " %s:%s:%d ", \
                __FILE__, __func__, __LINE__); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); } } while (0)

#define LOG_INFO(...) \
    do { if (current_log_level <= LOG_LEVEL_INFO) { \
        fprintf(stdout, USB_INFO_PREFIX " "); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); } } while (0)

#define LOG_WARN(...) \
    do { if (current_log_level <= LOG_LEVEL_WARN) { \
        fprintf(stdout, USB_WARN_PREFIX " "); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); } } while (0)

#define LOG_ERR(...) \
    do { if (current_log_level <= LOG_LEVEL_ERROR) { \
        fprintf(stderr, USB_ERR_PREFIX " %s:%d ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); } } while (0)

#define LOG_IN(...) \
    do { if (current_log_level <= LOG_LEVEL_INFO) { \
        fprintf(stdout, USB_IN_PREFIX " "); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); } } while (0)

#define LOG_OUT(...) \
    do { if (current_log_level <= LOG_LEVEL_INFO) { \
        fprintf(stdout, USB_OUT_PREFIX " "); \
        fprintf(stdout, __VA_ARGS__); \
        fprintf(stdout, "\n"); } } while (0)

#endif /* LOG_H */
