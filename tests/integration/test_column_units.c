/* M4.6 verification: TUNITn surfaces as a "units" attribute on the
 * per-column dataset. Uses ascii.fits (cols a/b → "pixels"/"counts"). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#ifndef FITS_CORPUS_DIR
#error "FITS_CORPUS_DIR must be defined"
#endif

static char *read_units(hid_t obj)
{
    hid_t aid = H5Aopen(obj, "units", H5P_DEFAULT);
    if (aid < 0) return NULL;
    hid_t st = H5Tcopy(H5T_C_S1);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8);
    char *s = NULL;
    assert(H5Aread(aid, st, &s) >= 0);
    H5Tclose(st);
    H5Aclose(aid);
    return s;
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(FITS_CORPUS_DIR "/ascii.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    /* Column "a" has TUNIT='pixels'. */
    {
        hid_t did = H5Dopen2(fid, "/HDU1/columns/a", H5P_DEFAULT);
        assert(did >= 0);
        assert(H5Aexists(did, "units") > 0);
        char *u = read_units(did);
        assert(u && strcmp(u, "pixels") == 0);
        free(u);
        H5Dclose(did);
    }

    /* Column "b" has TUNIT='counts'. */
    {
        hid_t did = H5Dopen2(fid, "/HDU1/columns/b", H5P_DEFAULT);
        char *u = read_units(did);
        assert(u && strcmp(u, "counts") == 0);
        free(u);
        H5Dclose(did);
    }

    H5Fclose(fid); H5Pclose(fapl); H5VLclose(vol);
    printf("OK: TUNITn surfaces as 'units' attribute on per-column datasets\n");
    return 0;
}
