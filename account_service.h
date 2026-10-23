#ifndef CADBAY_ACCOUNT_SERVICE_H
#define CADBAY_ACCOUNT_SERVICE_H

#include <stddef.h>

/*
 * Read the server-owned account record and resolve its authorization status.
 * The browser must not be trusted for this value.
 */
int account_service_get_status(const char *accounts_dir,
                               const char *user_id,
                               char *status_out,
                               size_t status_size);

#endif
