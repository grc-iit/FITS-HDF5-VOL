/* M1.4/M2.4 verification: H5Fopen → H5Gopen("/") → H5Gclose → H5Fclose
 * round-trip through the sciio-vol connector against a real FITS file. */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <fitsio.h>
#include <hdf5.h>

#include "sciio/sciio_vol.h"

static void build_minimal_fits(const char *path)
{
    int status = 0;
    fitsfile *fp = NULL;
    char clobber[256];
    snprintf(clobber, sizeof(clobber), "!%s", path);
    assert(fits_create_file(&fp, clobber, &status) == 0);
    long naxes[2] = {2, 2};
    assert(fits_create_img(fp, SHORT_IMG, 2, naxes, &status) == 0);
    assert(fits_close_file(fp, &status) == 0);
    assert(status == 0);
}

int main(void)
{
    hid_t vol_id = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    assert(vol_id >= 0);

    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    assert(fapl >= 0);
    assert(H5Pset_vol(fapl, vol_id, NULL) >= 0);

    char path[] = "/tmp/sciio_oc_XXXXXX.fits";
    int fd = mkstemps(path, 5);
    assert(fd >= 0);
    close(fd);
    build_minimal_fits(path);

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    assert(fid >= 0 && "H5Fopen via sciio-vol failed");

    hid_t gid = H5Gopen2(fid, "/", H5P_DEFAULT);
    assert(gid >= 0 && "H5Gopen2 of root via sciio-vol failed");

    assert(H5Gclose(gid) >= 0);
    assert(H5Fclose(fid) >= 0);

    unlink(path);
    assert(H5Pclose(fapl) >= 0);
    assert(H5VLclose(vol_id) >= 0);

    printf("OK: H5Fopen + H5Gopen(\"/\") + H5Gclose + H5Fclose on real FITS file\n");
    return 0;
}
