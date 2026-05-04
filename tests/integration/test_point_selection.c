/* M3.10 verification: point selections on a 4×3 BITPIX=16 image. Builder
 * writes pixel[r,c] = r*10 + c. */

#include <assert.h>
#include <stdio.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

#ifndef SCIIO_FIXTURES_DIR
#error "SCIIO_FIXTURES_DIR must be defined"
#endif

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(SCIIO_FIXTURES_DIR "/image_2d.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    hid_t did = H5Dopen2(fid, "/HDU0/data", H5P_DEFAULT);
    hid_t fsp = H5Dget_space(did);   /* dims [3, 4] */

    /* Select three non-contiguous points (r,c): (0,0), (1,2), (2,1).
     * Expected values: 0, 12, 21. */
    hsize_t coords[3 * 2] = {
        0, 0,
        1, 2,
        2, 1,
    };
    assert(H5Sselect_elements(fsp, H5S_SELECT_SET, 3, coords) >= 0);

    hsize_t mdims[1] = {3};
    hid_t msp = H5Screate_simple(1, mdims, NULL);

    short buf[3] = {0};
    assert(H5Dread(did, H5T_NATIVE_INT16, msp, fsp, H5P_DEFAULT, buf) >= 0);
    assert(buf[0] == 0);
    assert(buf[1] == 12);
    assert(buf[2] == 21);

    H5Sclose(msp); H5Sclose(fsp);
    H5Dclose(did); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: point selection (0,0)+(1,2)+(2,1) read 0/12/21\n");
    return 0;
}
