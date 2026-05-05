/* M6.7 regression: file005.fits is a PACKFITS text dump that has 7
 * "TEXTFILE" keywords. HDF5 attribute names must be unique per group,
 * so the adapter dedupes — keep the first, drop the rest with a warning.
 * Iterating /HDU0 must therefore yield each name exactly once, and copying
 * /HDU0's attrs to a native HDF5 file must succeed without "already exists"
 * errors. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#define FILE005 "/home/isa-grc/fits-tests/ftt4b/file005.fits"

typedef struct { int n_textfile; int n_total; } counts_t;

static herr_t cb(hid_t loc, const char *name, const H5A_info_t *info, void *user)
{
    (void)loc; (void)info;
    counts_t *c = (counts_t *)user;
    if (strcmp(name, "TEXTFILE") == 0) ++c->n_textfile;
    ++c->n_total;
    return 0;
}

int main(void)
{
    /* Suppress the duplicate-keyword warnings on stderr — they are expected
     * (the file genuinely has 7 TEXTFILE cards). */
    freopen("/dev/null", "w", stderr);

    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(FILE005, H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);
    hid_t hdu0 = H5Gopen2(fid, "/HDU0", H5P_DEFAULT);
    assert(hdu0 >= 0);

    counts_t c = {0};
    hsize_t idx = 0;
    assert(H5Aiterate2(hdu0, H5_INDEX_NAME, H5_ITER_INC, &idx, cb, &c) >= 0);

    /* The dedupe rule says exactly one TEXTFILE attribute regardless of how
     * many duplicate cards the file contained. */
    assert(c.n_textfile == 1);
    /* TEXTFILE is one of: SIMPLE, BITPIX, NAXIS, ORIGIN, TEXTFILE,
     * __raw_header__ — six distinct names total. */
    assert(c.n_total == 6);

    H5Gclose(hdu0); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);

    /* Restore stderr so the success message lands. */
    freopen("/dev/tty", "w", stderr);
    printf("OK: file005's 7 duplicate TEXTFILE cards collapse to one attribute\n");
    return 0;
}
