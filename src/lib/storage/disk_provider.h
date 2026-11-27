#ifndef DISK_PROVIDER_H
#define DISK_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>
#include "flow.h"

typedef struct disk_provider disk_provider_t;

disk_provider_t *disk_provider_new(const char *path, uint8_t ep_in, uint8_t ep_out);
void             disk_provider_free(disk_provider_t *dp);
const flow_endpoint_ops_t *disk_provider_get_ops(void);

/* Returns 0 on success, or -1 on unhandled */
int              disk_provider_handle_out(disk_provider_t *dp, const uint8_t *buf, size_t len,
                                          GQueue *in_queue);

uint8_t          disk_provider_get_ep_in(disk_provider_t *dp);
uint8_t          disk_provider_get_ep_out(disk_provider_t *dp);

#endif /* DISK_PROVIDER_H */
