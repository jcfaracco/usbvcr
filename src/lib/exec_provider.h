#ifndef EXEC_PROVIDER_H
#define EXEC_PROVIDER_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>
#include "flow.h"

typedef struct exec_provider exec_provider_t;

exec_provider_t *exec_provider_new(const char *command, const char *working_dir,
                                   uint8_t ep_in, uint8_t ep_out);
void             exec_provider_free(exec_provider_t *ep);
const flow_endpoint_ops_t *exec_provider_get_ops(void);

uint8_t          exec_provider_get_ep_in(exec_provider_t *ep);
uint8_t          exec_provider_get_ep_out(exec_provider_t *ep);

#endif /* EXEC_PROVIDER_H */
