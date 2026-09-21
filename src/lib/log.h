#ifndef USBREDIR_CLIENT_LOG_H
#define USBREDIR_CLIENT_LOG_H

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
#define LOG_DEBUG(fmt, ...) \
    do { if (current_log_level <= LOG_LEVEL_DEBUG) \
        fprintf(stderr, USB_DEBUG_PREFIX " %s:%s:%d " fmt "\n", \
                __FILE__, __func__, __LINE__, ##__VA_ARGS__); } while (0)

#define LOG_INFO(fmt, ...) \
    do { if (current_log_level <= LOG_LEVEL_INFO) \
        fprintf(stdout, USB_INFO_PREFIX " " fmt "\n", ##__VA_ARGS__); } while (0)

#define LOG_WARN(fmt, ...) \
    do { if (current_log_level <= LOG_LEVEL_WARN) \
        fprintf(stdout, USB_WARN_PREFIX " " fmt "\n", ##__VA_ARGS__); } while (0)

#define LOG_ERR(fmt, ...) \
    do { if (current_log_level <= LOG_LEVEL_ERROR) \
        fprintf(stderr, USB_ERR_PREFIX " %s:%d " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); } while (0)

#define LOG_IN(fmt, ...) \
    do { if (current_log_level <= LOG_LEVEL_INFO) \
        fprintf(stdout, USB_IN_PREFIX " " fmt "\n", ##__VA_ARGS__); } while (0)

#define LOG_OUT(fmt, ...) \
    do { if (current_log_level <= LOG_LEVEL_INFO) \
        fprintf(stdout, USB_OUT_PREFIX " " fmt "\n", ##__VA_ARGS__); } while (0)

#endif
