/* M3.8 verification: BITPIX=16 image with non-trivial BSCALE/BZERO surfaces
 * as a float64 dataset, with CFITSIO applying physical = BSCALE*raw + BZERO
 * on read. */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#ifndef FITS_FIXTURES_DIR
#error "FITS_FIXTURES_DIR must be defined"
#endif

#define MX_ROWS 2
#define MX_COLS 3
#define MX_N    (MX_ROWS * MX_COLS)

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(FITS_FIXTURES_DIR "/scaled_image.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    hid_t did = H5Dopen2(fid, "/HDU0/data", H5P_DEFAULT);
    assert(did >= 0);

    /* Plan §7.2: scaled image must surface as float64. */
    hid_t tid = H5Dget_type(did);
    assert(H5Tget_class(tid) == H5T_FLOAT);
    assert(H5Tget_size(tid)  == 8);
    H5Tclose(tid);

    double buf[MX_N];
    assert(H5Dread(did, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) >= 0);

    /* Builder wrote raw[i] = 2i+2 with BSCALE=0.5 BZERO=10, so physical = i+11. */
    for (int i = 0; i < MX_N; ++i) {
        double want = (double)(i + 11);
        if (fabs(buf[i] - want) > 1e-12) {
            fprintf(stderr, "FAIL: pixel[%d] expected %g got %g\n", i, want, buf[i]);
            assert(0);
        }
    }

    H5Dclose(did); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: BSCALE/BZERO general rescale produces float64 with physical values\n");
    return 0;
}
