/* M2.9 verification: a FITS file that declares GROUPS = T is rejected by
 * H5Fopen with a non-zero failure. */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <fitsio.h>
#include <hdf5.h>

#include "sciio/sciio_vol.h"

static void build_random_groups_lookalike(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[256]; snprintf(buf, sizeof(buf), "!%s", path);
    assert(fits_create_file(&fp, buf, &s) == 0);
    long naxes[2] = {2, 2};
    assert(fits_create_img(fp, SHORT_IMG, 2, naxes, &s) == 0);
    int t = 1;
    assert(fits_update_key(fp, TLOGICAL, "GROUPS", &t, "Random Groups", &s) == 0);
    assert(fits_close_file(fp, &s) == 0);
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    assert(vol >= 0);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    assert(H5Pset_vol(fapl, vol, NULL) >= 0);

    char path[] = "/tmp/sciio_rg_XXXXXX.fits";
    int fd = mkstemps(path, 5); assert(fd >= 0); close(fd);
    build_random_groups_lookalike(path);

    /* Suppress HDF5 default error printing (the failure is expected). */
    H5E_auto2_t old_func; void *old_data;
    H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    assert(fid < 0 && "Random Groups HDU must be rejected");

    H5Eset_auto2(H5E_DEFAULT, old_func, old_data);

    unlink(path);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: H5Fopen rejected Random Groups (GROUPS = T) HDU\n");
    return 0;
}
