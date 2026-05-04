/* M3.11 verification: a tile-compressed image (ZIMAGE) surfaces normally
 * for introspection but H5Dread is rejected with H5E_UNSUPPORTED
 *
 * Uses comp.fits from the astropy corpus, which has a single tile-compressed
 * image extension. */

#include <assert.h>
#include <stdio.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

#ifndef SCIIO_CORPUS_DIR
#error "SCIIO_CORPUS_DIR must be defined"
#endif

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);

    hid_t fid = H5Fopen(SCIIO_CORPUS_DIR "/comp.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0 && "comp.fits must open for introspection");

    /* HDU1 is the tile-compressed image extension. The group must exist with
     * its header keywords available as attributes. */
    hid_t hdu = H5Gopen2(fid, "/HDU1", H5P_DEFAULT);
    assert(hdu >= 0);
    H5G_info_t gi;
    assert(H5Gget_info(hdu, &gi) >= 0);
    assert(gi.nlinks == 1 && "compressed HDU still surfaces a 'data' child");

    /* Open the dataset — succeeds (introspection path). H5Dread must fail. */
    hid_t did = H5Dopen2(hdu, "data", H5P_DEFAULT);
    assert(did >= 0);

    /* Reading must fail with a logged diagnostic. Suppress HDF5's auto-print
     * for the expected failure. */
    H5E_auto2_t old_func; void *old_data;
    H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

    hid_t tid = H5Dget_type(did);
    hid_t sid = H5Dget_space(did);
    hsize_t n = (hsize_t)H5Sget_simple_extent_npoints(sid);
    void *buf = malloc(n * H5Tget_size(tid));
    herr_t rc = H5Dread(did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
    free(buf);
    H5Tclose(tid); H5Sclose(sid);

    H5Eset_auto2(H5E_DEFAULT, old_func, old_data);
    assert(rc < 0 && "H5Dread on a compressed image must fail in v1");

    H5Dclose(did); H5Gclose(hdu); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: tile-compressed image surfaces but H5Dread is rejected\n");
    return 0;
}
