/* M4.8 verification: variable-length array column (TFORM 'PI') reads as
 * an HDF5 vlen<int16> dataset; per-row lengths and contents match the
 * deterministic fixture. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

#ifndef SCIIO_FIXTURES_DIR
#error "SCIIO_FIXTURES_DIR must be defined"
#endif

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(SCIIO_FIXTURES_DIR "/table_vlen.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    hid_t did = H5Dopen2(fid, "/HDU1/columns/vlen_int", H5P_DEFAULT);
    assert(did >= 0);

    hid_t tid = H5Dget_type(did);
    assert(H5Tget_class(tid) == H5T_VLEN);
    H5Tclose(tid);

    hid_t sid = H5Dget_space(did);
    hsize_t dims[1];
    H5Sget_simple_extent_dims(sid, dims, NULL);
    assert(dims[0] == 4);
    H5Sclose(sid);

    /* Each element is hvl_t {len, p}. The fixture wrote row r with (r+1)
     * shorts starting at r*100. */
    hvl_t buf[4] = {{0,0}};
    hid_t mt = H5Tvlen_create(H5T_NATIVE_INT16);
    assert(H5Dread(did, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf) >= 0);

    for (int r = 0; r < 4; ++r) {
        size_t want_len = (size_t)(r + 1);
        if (buf[r].len != want_len) {
            fprintf(stderr, "FAIL row %d: len got=%zu want=%zu\n",
                    r, buf[r].len, want_len);
            assert(0);
        }
        short *p = (short *)buf[r].p;
        for (int i = 0; i < r + 1; ++i) {
            short want = (short)(r * 100 + i);
            assert(p[i] == want);
        }
        free(buf[r].p);
    }
    H5Tclose(mt);

    H5Dclose(did); H5Fclose(fid); H5Pclose(fapl); H5VLclose(vol);
    printf("OK: vlen_int column rows {1,2,3,4} read with deterministic content\n");
    return 0;
}
