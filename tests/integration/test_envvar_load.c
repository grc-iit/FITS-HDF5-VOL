/* M1.5 verification: connector loads transparently via env vars and opens
 * a real FITS file. The program does NOT call H5VLregister_connector_by_name. */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <fitsio.h>
#include <hdf5.h>

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
}

int main(void)
{
    const char *connector = getenv("HDF5_VOL_CONNECTOR");
    const char *plugin    = getenv("HDF5_PLUGIN_PATH");
    assert(connector && "HDF5_VOL_CONNECTOR must be set for this test");
    assert(plugin    && "HDF5_PLUGIN_PATH must be set for this test");

    char path[] = "/tmp/fits_env_XXXXXX.fits";
    int fd = mkstemps(path, 5);
    assert(fd >= 0);
    close(fd);
    build_minimal_fits(path);

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (fid < 0) { H5Eprint2(H5E_DEFAULT, stderr); unlink(path); }
    assert(fid >= 0 && "H5Fopen with H5P_DEFAULT failed; env-var auto-load not working");

    hid_t gid = H5Gopen2(fid, "/", H5P_DEFAULT);
    assert(gid >= 0);
    assert(H5Gclose(gid) >= 0);
    assert(H5Fclose(fid) >= 0);

    unlink(path);
    printf("OK: env-var auto-load works (HDF5_VOL_CONNECTOR=\"%s\")\n", connector);
    return 0;
}
