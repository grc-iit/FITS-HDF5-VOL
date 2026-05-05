/*
 * Internal: connector-side registry of adapters. Not a public API. Adapter
 * implementations don't include this; they only export their fits_adapter_t
 * instance and the connector links them in.
 *
 * v1 ships a hardcoded compile-time registry. v2 may grow runtime
 * registration (`fits_register_adapter`) without breaking this layout.
 */

#ifndef FITS_REGISTRY_H
#define FITS_REGISTRY_H

#include "fits_hdf5/adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Walk the registry, run probe() on each, return the highest-confidence
 * match (>0). NULL if none claim the file. */
const fits_adapter_t *fits_dispatch_probe(const char *path);

/* Adapters compiled into v1 (extern declarations). Add new ones here. */
extern const fits_adapter_t fits_adapter;

#ifdef __cplusplus
}
#endif
#endif
