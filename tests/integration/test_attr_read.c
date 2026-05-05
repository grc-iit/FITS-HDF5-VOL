/* M2.6a verification: open a FITS file with int / float / bool / string
 * keywords, then read each back via the HDF5 attribute API and assert
 * value + dtype. */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fitsio.h>
#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

static void build_fixture(const char *path)
{
    int status = 0;
    fitsfile *fp = NULL;
    char clobber[256]; snprintf(clobber, sizeof(clobber), "!%s", path);
    assert(fits_create_file(&fp, clobber, &status) == 0);
    long n[2] = {2, 2};
    assert(fits_create_img(fp, SHORT_IMG, 2, n, &status) == 0);

    long lv = 42;
    double dv = 3.14159;
    int bv = 1;
    char sv[] = "hello";

    assert(fits_update_key(fp, TLONG,    "MYINT",   &lv, "test int",   &status) == 0);
    assert(fits_update_key(fp, TDOUBLE,  "MYFLOAT", &dv, "test float", &status) == 0);
    assert(fits_update_key(fp, TLOGICAL, "MYBOOL",  &bv, "test bool",  &status) == 0);
    assert(fits_update_key(fp, TSTRING,  "MYSTR",   sv,  "test str",   &status) == 0);
    assert(fits_close_file(fp, &status) == 0);
    assert(status == 0);
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    assert(vol >= 0);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    assert(H5Pset_vol(fapl, vol, NULL) >= 0);

    char path[] = "/tmp/fits_attr_XXXXXX.fits";
    int fd = mkstemps(path, 5);
    assert(fd >= 0);
    close(fd);
    build_fixture(path);

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);
    hid_t hdu0 = H5Gopen2(fid, "/HDU0", H5P_DEFAULT);
    assert(hdu0 >= 0);

    /* INT */
    {
        hid_t aid = H5Aopen(hdu0, "MYINT", H5P_DEFAULT);
        assert(aid >= 0);
        int64_t v = 0;
        assert(H5Aread(aid, H5T_NATIVE_INT64, &v) >= 0);
        assert(v == 42);
        assert(H5Aclose(aid) >= 0);
    }

    /* FLOAT */
    {
        hid_t aid = H5Aopen(hdu0, "MYFLOAT", H5P_DEFAULT);
        assert(aid >= 0);
        double v = 0;
        assert(H5Aread(aid, H5T_NATIVE_DOUBLE, &v) >= 0);
        assert(v > 3.14 && v < 3.15);
        assert(H5Aclose(aid) >= 0);
    }

    /* BOOL */
    {
        hid_t aid = H5Aopen(hdu0, "MYBOOL", H5P_DEFAULT);
        assert(aid >= 0);
        unsigned char v = 0;
        assert(H5Aread(aid, H5T_NATIVE_HBOOL, &v) >= 0);
        assert(v == 1);
        assert(H5Aclose(aid) >= 0);
    }

    /* STRING */
    {
        hid_t aid = H5Aopen(hdu0, "MYSTR", H5P_DEFAULT);
        assert(aid >= 0);
        hid_t st = H5Tcopy(H5T_C_S1);
        H5Tset_size(st, H5T_VARIABLE);
        H5Tset_cset(st, H5T_CSET_UTF8);
        char *s = NULL;
        assert(H5Aread(aid, st, &s) >= 0);
        assert(s && strcmp(s, "hello") == 0);
        free(s);
        H5Tclose(st);
        assert(H5Aclose(aid) >= 0);
    }

    /* H5Aexists */
    assert(H5Aexists(hdu0, "MYINT") > 0);
    assert(H5Aexists(hdu0, "NOPE") == 0);

    assert(H5Gclose(hdu0) >= 0);
    assert(H5Fclose(fid) >= 0);
    unlink(path);
    H5Pclose(fapl); H5VLclose(vol);

    printf("OK: attr_read int/float/bool/string + H5Aexists succeed\n");
    return 0;
}
