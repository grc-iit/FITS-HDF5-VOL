/* M4.7 verification: table columns with repeat>1 surface as 1-D datasets
 * whose elements are HDF5 array datatypes shaped from TFORM repeat or TDIMn
 * (plan §7.3). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

#ifndef SCIIO_FIXTURES_DIR
#error "SCIIO_FIXTURES_DIR must be defined"
#endif

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(SCIIO_FIXTURES_DIR "/table_multidim.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    /* col1 "vec3" — TFORM=3J, no TDIM. Element type is ARRAY[3] of int32.
     * Dataset shape is [5] (5 rows). Reading lays out 5*3 ints contiguously. */
    {
        hid_t did = H5Dopen2(fid, "/HDU1/columns/vec3", H5P_DEFAULT);
        assert(did >= 0);
        hid_t tid = H5Dget_type(did);
        assert(H5Tget_class(tid) == H5T_ARRAY);
        assert(H5Tget_array_ndims(tid) == 1);
        hsize_t adims[1];
        H5Tget_array_dims2(tid, adims);
        assert(adims[0] == 3);
        H5Tclose(tid);

        hid_t sid = H5Dget_space(did);
        assert(H5Sget_simple_extent_ndims(sid) == 1);
        hsize_t dims[1];
        H5Sget_simple_extent_dims(sid, dims, NULL);
        assert(dims[0] == 5);
        H5Sclose(sid);

        int buf[5][3] = {{0}};
        /* Read with the array compound type: rebuild it for memory compat. */
        hid_t mt = H5Tarray_create2(H5T_NATIVE_INT32, 1, (hsize_t[]){3});
        assert(H5Dread(did, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) >= 0);
        H5Tclose(mt);

        for (int r = 0; r < 5; ++r) for (int k = 0; k < 3; ++k) {
            int want = r * 10 + k;
            if (buf[r][k] != want) {
                fprintf(stderr, "FAIL vec3[%d][%d] got=%d want=%d\n", r, k, buf[r][k], want);
                assert(0);
            }
        }
        H5Dclose(did);
    }

    /* col2 "mat22" — TFORM=4J + TDIM=(2,2). Element type is ARRAY[2,2] int32. */
    {
        hid_t did = H5Dopen2(fid, "/HDU1/columns/mat22", H5P_DEFAULT);
        hid_t tid = H5Dget_type(did);
        assert(H5Tget_class(tid) == H5T_ARRAY);
        assert(H5Tget_array_ndims(tid) == 2);
        hsize_t adims[2];
        H5Tget_array_dims2(tid, adims);
        assert(adims[0] == 2 && adims[1] == 2);
        H5Tclose(tid);

        int buf[5][2][2] = {{{0}}};
        hid_t mt = H5Tarray_create2(H5T_NATIVE_INT32, 2, (hsize_t[]){2,2});
        assert(H5Dread(did, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) >= 0);
        H5Tclose(mt);

        for (int r = 0; r < 5; ++r) {
            int *p = (int *)&buf[r][0][0];
            for (int k = 0; k < 4; ++k) {
                int want = r * 100 + k;
                if (p[k] != want) {
                    fprintf(stderr, "FAIL mat22[%d] flat[%d] got=%d want=%d\n",
                            r, k, p[k], want);
                    assert(0);
                }
            }
        }
        H5Dclose(did);
    }

    H5Fclose(fid); H5Pclose(fapl); H5VLclose(vol);
    printf("OK: TFORM repeat>1 (vec3) and TDIM=(2,2) (mat22) read correctly\n");
    return 0;
}
