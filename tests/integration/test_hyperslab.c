/* M3.9 verification: regular hyperslab selections (start, count, stride) on
 * a 4×3 BITPIX=16 image. Builder writes pixel[r,c] = r*10 + c. */

#include <assert.h>
#include <stdio.h>

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
    hid_t fsp = H5Dget_space(did);   /* dims [3, 4]: row, col */

    /* Case A: 2×2 sub-region starting at (1, 1).
     * Expected:
     *   pixel[1,1]=11  pixel[1,2]=12
     *   pixel[2,1]=21  pixel[2,2]=22  */
    {
        hsize_t start[2]  = {1, 1};
        hsize_t count[2]  = {2, 2};
        assert(H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, NULL, count, NULL) >= 0);

        hsize_t mdims[2] = {2, 2};
        hid_t msp = H5Screate_simple(2, mdims, NULL);

        short buf[4] = {0};
        assert(H5Dread(did, H5T_NATIVE_INT16, msp, fsp, H5P_DEFAULT, buf) >= 0);
        assert(buf[0] == 11);
        assert(buf[1] == 12);
        assert(buf[2] == 21);
        assert(buf[3] == 22);
        H5Sclose(msp);
    }

    /* Case B: strided read along axis 1 — every other column on row 0.
     * Source is [3,4]: row 0 → 0, 1, 2, 3. With stride=2 we get cols 0 and 2:
     *   pixel[0,0]=0, pixel[0,2]=2. */
    {
        hsize_t start[2]  = {0, 0};
        hsize_t stride[2] = {1, 2};
        hsize_t count[2]  = {1, 2};   /* 2 elements along axis 1 */
        assert(H5Sselect_hyperslab(fsp, H5S_SELECT_SET, start, stride, count, NULL) >= 0);

        hsize_t mdims[1] = {2};
        hid_t msp = H5Screate_simple(1, mdims, NULL);

        short buf[2] = {0};
        assert(H5Dread(did, H5T_NATIVE_INT16, msp, fsp, H5P_DEFAULT, buf) >= 0);
        assert(buf[0] == 0);
        assert(buf[1] == 2);
        H5Sclose(msp);
    }

    H5Sclose(fsp); H5Dclose(did); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: hyperslab + stride round-trip correct\n");
    return 0;
}
