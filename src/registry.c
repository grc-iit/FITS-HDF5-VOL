/*
 * Connector-side adapter registry. v1 hardcodes the list at compile time; we
 * iterate the array on every H5Fopen, run probe(), pick the highest-confidence
 * match. New formats are added by:
 *   1. Implementing the adapter and exporting a const sciio_adapter_t.
 *   2. Adding it to the `extern` declarations in sciio/registry.h.
 *   3. Adding it to the `sciio_registry` array below.
 */

#include "sciio/registry.h"

static const sciio_adapter_t *sciio_registry[] = {
    &sciio_fits_adapter,
    /* &sciio_dicom_adapter,   // v2 */
    /* &sciio_grib_adapter,    // v2 */
    NULL
};

const sciio_adapter_t *sciio_dispatch_probe(const char *path)
{
    const sciio_adapter_t *winner = NULL;
    int best = 0;
    for (const sciio_adapter_t **a = sciio_registry; *a; ++a) {
        adapter_probe_result_t r = {0};
        if ((*a)->probe(path, &r) != 0) continue;
        if (r.confidence > best) { best = r.confidence; winner = *a; }
    }
    return winner;
}
