/* M2.14 verification: an empty / non-FITS file fails at adapter_probe and
 * H5Fopen returns < 0 with a clear "does not match a supported format"
 * error in the HDF5 error stack. */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

/* Walk the HDF5 error stack and look for our specific message substring. */
static herr_t look_for_msg(unsigned n, const H5E_error2_t *err, void *user)
{
    (void)n;
    int *found = (int *)user;
    if (err->desc && strstr(err->desc, "does not match a supported format"))
        *found = 1;
    return 0;
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);

    /* Empty file. */
    char path[] = "/tmp/fits_nf_XXXXXX.bin";
    int fd = mkstemps(path, 4);
    assert(fd >= 0);
    /* Write a single non-FITS byte so the file isn't degenerate-empty. */
    assert(write(fd, "Z", 1) == 1);
    close(fd);

    H5E_auto2_t old_func; void *old_data;
    H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    assert(fid < 0 && "non-FITS H5Fopen must fail");

    /* The error stack should carry our diagnostic string. */
    int found = 0;
    H5Ewalk2(H5E_DEFAULT, H5E_WALK_DOWNWARD, look_for_msg, &found);
    H5Eset_auto2(H5E_DEFAULT, old_func, old_data);
    assert(found && "expected diagnostic 'does not match a supported format' in error stack");

    unlink(path);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: non-FITS file rejected with diagnostic\n");
    return 0;
}
