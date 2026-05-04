/* M2.8 structural differential: read every user attribute we placed in
 * image_2d.fits via the builder, and assert exact values + types. The
 * fixture contents are deterministic so this is a strong correctness signal. */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

#ifndef SCIIO_FIXTURES_DIR
#error "SCIIO_FIXTURES_DIR must be defined"
#endif

static hid_t open_fixture(hid_t *out_vol, hid_t *out_fapl)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    assert(vol >= 0);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    assert(H5Pset_vol(fapl, vol, NULL) >= 0);
    hid_t fid = H5Fopen(SCIIO_FIXTURES_DIR "/image_2d.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);
    *out_vol = vol;
    *out_fapl = fapl;
    return fid;
}

static void check_int_attr(hid_t loc, const char *name, int64_t expected)
{
    hid_t aid = H5Aopen(loc, name, H5P_DEFAULT);
    assert(aid >= 0);

    /* Type check: must be integer of expected size. */
    hid_t tid = H5Aget_type(aid);
    assert(H5Tget_class(tid) == H5T_INTEGER);
    H5Tclose(tid);

    /* Space check: scalar. */
    hid_t sid = H5Aget_space(aid);
    assert(H5Sget_simple_extent_type(sid) == H5S_SCALAR);
    H5Sclose(sid);

    /* Value check. */
    int64_t got = 0;
    assert(H5Aread(aid, H5T_NATIVE_INT64, &got) >= 0);
    if (got != expected) {
        fprintf(stderr, "FAIL: %s expected %lld got %lld\n",
                name, (long long)expected, (long long)got);
        assert(0);
    }
    H5Aclose(aid);
}

static void check_float_attr(hid_t loc, const char *name, double expected, double tol)
{
    hid_t aid = H5Aopen(loc, name, H5P_DEFAULT);
    assert(aid >= 0);
    hid_t tid = H5Aget_type(aid);
    assert(H5Tget_class(tid) == H5T_FLOAT);
    H5Tclose(tid);
    double got = 0;
    assert(H5Aread(aid, H5T_NATIVE_DOUBLE, &got) >= 0);
    assert(fabs(got - expected) <= tol);
    H5Aclose(aid);
}

static void check_bool_attr(hid_t loc, const char *name, int expected)
{
    hid_t aid = H5Aopen(loc, name, H5P_DEFAULT);
    assert(aid >= 0);
    unsigned char got = 0;
    assert(H5Aread(aid, H5T_NATIVE_HBOOL, &got) >= 0);
    assert((int)got == expected);
    H5Aclose(aid);
}

static void check_string_attr(hid_t loc, const char *name, const char *expected)
{
    hid_t aid = H5Aopen(loc, name, H5P_DEFAULT);
    assert(aid >= 0);
    hid_t tid = H5Aget_type(aid);
    assert(H5Tget_class(tid) == H5T_STRING);
    assert(H5Tis_variable_str(tid) > 0);
    H5Tclose(tid);

    hid_t st = H5Tcopy(H5T_C_S1);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8);
    char *got = NULL;
    assert(H5Aread(aid, st, &got) >= 0);
    assert(got && strcmp(got, expected) == 0);
    free(got);
    H5Tclose(st);
    H5Aclose(aid);
}

int main(void)
{
    hid_t vol, fapl;
    hid_t fid = open_fixture(&vol, &fapl);
    hid_t hdu0 = H5Gopen2(fid, "/HDU0", H5P_DEFAULT);
    assert(hdu0 >= 0);

    /* User-emitted keywords from build_fixtures.c::build_image_2d() */
    check_int_attr(hdu0,    "MYINT",   42);
    check_float_attr(hdu0,  "MYFLOAT", 3.14159265358979, 1e-12);
    check_bool_attr(hdu0,   "MYBOOL",  1);
    check_string_attr(hdu0, "MYSTR",   "regression-fixture");

    /* Structural keywords CFITSIO emits for a 4×3 SHORT_IMG primary. */
    check_int_attr(hdu0, "BITPIX", 16);
    check_int_attr(hdu0, "NAXIS",  2);
    check_int_attr(hdu0, "NAXIS1", 4);
    check_int_attr(hdu0, "NAXIS2", 3);

    H5Gclose(hdu0); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: image_2d.fits attribute differential passed\n");
    return 0;
}
