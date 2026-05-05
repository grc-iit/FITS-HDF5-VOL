/* M3.5 verification: H5Dread of the whole 4×3 BITPIX=16 primary image in
 * image_2d.fits returns the deterministic content the fixture builder wrote
 * (pixel[r,c] = r*10 + c). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#ifndef FITS_FIXTURES_DIR
#error "FITS_FIXTURES_DIR must be defined"
#endif

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(FITS_FIXTURES_DIR "/image_2d.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    hid_t did = H5Dopen2(fid, "/HDU0/data", H5P_DEFAULT);
    assert(did >= 0);

    /* Confirm shape: rank 2, dims [3, 4] in C order (NAXIS2 first). */
    hid_t sid = H5Dget_space(did);
    assert(H5Sget_simple_extent_ndims(sid) == 2);
    hsize_t dims[2];
    H5Sget_simple_extent_dims(sid, dims, NULL);
    assert(dims[0] == 3 && dims[1] == 4);
    H5Sclose(sid);

    /* Confirm dtype: int16. */
    hid_t tid = H5Dget_type(did);
    assert(H5Tget_class(tid) == H5T_INTEGER);
    assert(H5Tget_size(tid) == 2);
    H5Tclose(tid);

    /* Read everything. */
    short buf[12] = {0};
    assert(H5Dread(did, H5T_NATIVE_INT16, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) >= 0);

    /* Builder wrote: data[r*4 + c] = r*10 + c, with r in [0,3), c in [0,4). */
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            short expected = (short)(r * 10 + c);
            short got      = buf[r * 4 + c];
            if (got != expected) {
                fprintf(stderr, "FAIL: pixel[%d,%d] expected %d got %d\n",
                        r, c, expected, got);
                assert(0);
            }
        }

    assert(H5Dclose(did) >= 0);
    assert(H5Fclose(fid) >= 0);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: H5Dread BITPIX=16 4x3 round-trips deterministic pixel values\n");
    return 0;
}
