/* M1.3 verification: connector registers, is reported registered, then closes.
 * Failure aborts via assert. */

#include <assert.h>
#include <stdio.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

int main(void)
{
    hid_t vol_id = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    assert(vol_id >= 0 && "H5VLregister_connector_by_name failed");

    htri_t exists = H5VLis_connector_registered_by_name(FITS_HDF5_VOL_NAME);
    assert(exists > 0 && "connector not reported as registered");

    herr_t close_status = H5VLclose(vol_id);
    assert(close_status >= 0 && "H5VLclose failed");

    printf("OK: connector \"%s\" registered, queried, closed\n", FITS_HDF5_VOL_NAME);
    return 0;
}
