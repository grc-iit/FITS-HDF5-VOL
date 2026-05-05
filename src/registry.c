/*
 * Connector-side adapter registry. v1 hardcodes the list at compile time; we
 * iterate the array on every H5Fopen, run probe(), pick the highest-confidence
 * match. New formats are added by:
 *   1. Implementing the adapter and exporting a const fits_adapter_t.
 *   2. Adding it to the `extern` declarations in fits/registry.h.
 *   3. Adding it to the `fits_registry` array below.
 */

#include "fits_hdf5/registry.h"

static const fits_adapter_t *fits_registry[] = {
    &fits_adapter,
    /* &fits_dicom_adapter,   // v2 */
    /* &fits_grib_adapter,    // v2 */
    NULL
};

const fits_adapter_t *fits_dispatch_probe(const char *path)
{
    const fits_adapter_t *winner = NULL;
    int best = 0;
    for (const fits_adapter_t **a = fits_registry; *a; ++a) {
        adapter_probe_result_t r = {0};
        if ((*a)->probe(path, &r) != 0) continue;
        if (r.confidence > best) { best = r.confidence; winner = *a; }
    }
    return winner;
}
