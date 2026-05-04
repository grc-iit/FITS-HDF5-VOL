/*
 * Internal: connector-side registry of adapters. Not a public API. Adapter
 * implementations don't include this; they only export their sciio_adapter_t
 * instance and the connector links them in.
 *
 * v1 ships a hardcoded compile-time registry. v2 may grow runtime
 * registration (`sciio_register_adapter`) without breaking this layout.
 */

#ifndef SCIIO_REGISTRY_H
#define SCIIO_REGISTRY_H

#include "sciio/adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Walk the registry, run probe() on each, return the highest-confidence
 * match (>0). NULL if none claim the file. */
const sciio_adapter_t *sciio_dispatch_probe(const char *path);

/* Adapters compiled into v1 (extern declarations). Add new ones here. */
extern const sciio_adapter_t sciio_fits_adapter;

#ifdef __cplusplus
}
#endif
#endif
