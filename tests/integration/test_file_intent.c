/* M2.15 verification: H5Fget_intent reports H5F_ACC_RDONLY for a fits-hdf5-vol
 * opened file. */

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

    unsigned intent = 0;
    assert(H5Fget_intent(fid, &intent) >= 0);
    assert(intent == H5F_ACC_RDONLY);

    assert(H5Fclose(fid) >= 0);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: H5Fget_intent reports RDONLY\n");
    return 0;
}
