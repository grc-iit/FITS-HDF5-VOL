/* M2.13 verification: H5F_ACC_RDWR is rejected with a clear failure
 * (plan §3 read-only contract). Read access on the same file still works. */

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

    /* RDONLY succeeds on the same file. */
    hid_t fid_ro = H5Fopen(SCIIO_FIXTURES_DIR "/image_2d.fits", H5F_ACC_RDONLY, fapl);
    assert(fid_ro >= 0 && "RDONLY open must succeed");
    assert(H5Fclose(fid_ro) >= 0);

    /* RDWR must fail. Suppress HDF5 default error printing for this branch. */
    H5E_auto2_t old_func; void *old_data;
    H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

    hid_t fid_rw = H5Fopen(SCIIO_FIXTURES_DIR "/image_2d.fits", H5F_ACC_RDWR, fapl);
    H5Eset_auto2(H5E_DEFAULT, old_func, old_data);
    assert(fid_rw < 0 && "RDWR open must fail (read-only connector)");

    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: RDONLY accepted; RDWR rejected\n");
    return 0;
}
