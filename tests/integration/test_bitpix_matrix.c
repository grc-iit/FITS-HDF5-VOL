/* M3.6 verification: H5Dread on every BITPIX value (8 / 16 / 32 / 64 / -32 / -64)
 * returns the deterministic content from the bitpix_matrix.fits builder. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#ifndef FITS_FIXTURES_DIR
#error "FITS_FIXTURES_DIR must be defined"
#endif

#define MX_ROWS 2
#define MX_COLS 3
#define MX_N    (MX_ROWS * MX_COLS)

static void check_dtype(hid_t did, H5T_class_t cls_expected, size_t size_expected,
                         hbool_t signed_expected, const char *what)
{
    hid_t tid = H5Dget_type(did);
    assert(H5Tget_class(tid) == cls_expected);
    assert(H5Tget_size(tid)  == size_expected);
    if (cls_expected == H5T_INTEGER) {
        H5T_sign_t sg = H5Tget_sign(tid);
        H5T_sign_t want = signed_expected ? H5T_SGN_2 : H5T_SGN_NONE;
        if (sg != want) {
            fprintf(stderr, "FAIL %s: sign got=%d want=%d\n", what, sg, want);
            assert(0);
        }
    }
    H5Tclose(tid);
}

#define READ_AND_CHECK(C_TYPE, NATIVE, BASE, EXPECT_FMT)                          \
    do {                                                                          \
        C_TYPE got[MX_N] = {0};                                                   \
        assert(H5Dread(did, NATIVE, H5S_ALL, H5S_ALL, H5P_DEFAULT, got) >= 0);    \
        for (int r = 0; r < MX_ROWS; ++r)                                         \
            for (int c = 0; c < MX_COLS; ++c) {                                   \
                C_TYPE expected = (C_TYPE)((BASE) + r * 10 + c);                  \
                C_TYPE g        = got[r * MX_COLS + c];                           \
                if (g != expected) {                                              \
                    fprintf(stderr, "FAIL HDU%d [%d,%d]: got=" EXPECT_FMT         \
                            " expected=" EXPECT_FMT "\n", hdu, r, c, g, expected);\
                    assert(0);                                                    \
                }                                                                 \
            }                                                                     \
    } while (0)

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(FITS_FIXTURES_DIR "/bitpix_matrix.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    int hdu;

    /* HDU0  BITPIX=8   uint8   base=100 */
    hdu = 0;
    {
        hid_t did = H5Dopen2(fid, "/HDU0/data", H5P_DEFAULT);
        assert(did >= 0);
        check_dtype(did, H5T_INTEGER, 1, /*signed*/0, "HDU0/uint8");
        READ_AND_CHECK(unsigned char, H5T_NATIVE_UINT8, 100, "%u");
        H5Dclose(did);
    }
    /* HDU1  BITPIX=16  int16   base=200 */
    hdu = 1;
    {
        hid_t did = H5Dopen2(fid, "/HDU1/data", H5P_DEFAULT);
        check_dtype(did, H5T_INTEGER, 2, 1, "HDU1/int16");
        READ_AND_CHECK(short, H5T_NATIVE_INT16, 200, "%d");
        H5Dclose(did);
    }
    /* HDU2  BITPIX=32  int32   base=300 */
    hdu = 2;
    {
        hid_t did = H5Dopen2(fid, "/HDU2/data", H5P_DEFAULT);
        check_dtype(did, H5T_INTEGER, 4, 1, "HDU2/int32");
        READ_AND_CHECK(int, H5T_NATIVE_INT32, 300, "%d");
        H5Dclose(did);
    }
    /* HDU3  BITPIX=64  int64   base=400 */
    hdu = 3;
    {
        hid_t did = H5Dopen2(fid, "/HDU3/data", H5P_DEFAULT);
        check_dtype(did, H5T_INTEGER, 8, 1, "HDU3/int64");
        READ_AND_CHECK(long long, H5T_NATIVE_INT64, 400, "%lld");
        H5Dclose(did);
    }
    /* HDU4  BITPIX=-32 float32 base=500 */
    hdu = 4;
    {
        hid_t did = H5Dopen2(fid, "/HDU4/data", H5P_DEFAULT);
        check_dtype(did, H5T_FLOAT, 4, 0, "HDU4/float32");
        READ_AND_CHECK(float, H5T_NATIVE_FLOAT, 500.0f, "%g");
        H5Dclose(did);
    }
    /* HDU5  BITPIX=-64 float64 base=600 */
    hdu = 5;
    {
        hid_t did = H5Dopen2(fid, "/HDU5/data", H5P_DEFAULT);
        check_dtype(did, H5T_FLOAT, 8, 0, "HDU5/float64");
        READ_AND_CHECK(double, H5T_NATIVE_DOUBLE, 600.0, "%g");
        H5Dclose(did);
    }

    H5Fclose(fid); H5Pclose(fapl); H5VLclose(vol);
    printf("OK: BITPIX matrix 8/16/32/64/-32/-64 all round-trip correctly\n");
    return 0;
}
