/* M4.9 verification: /HDU1/table compound row-view dataset has correct
 * datatype and content. Tested on file001.fits (7-double ASCII catalog). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

typedef struct {
    double iden, ra, dec, type, d25, incl, rv;
} row_t;

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen("/home/isa-grc/fits-tests/ftt4b/file001.fits",
                         H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    hid_t did = H5Dopen2(fid, "/HDU1/table", H5P_DEFAULT);
    assert(did >= 0);

    /* Type: 7-member compound, all doubles. */
    hid_t tid = H5Dget_type(did);
    assert(H5Tget_class(tid) == H5T_COMPOUND);
    assert(H5Tget_nmembers(tid) == 7);
    H5Tclose(tid);

    hid_t sid = H5Dget_space(did);
    hsize_t dims[1];
    H5Sget_simple_extent_dims(sid, dims, NULL);
    assert(dims[0] == 10);
    H5Sclose(sid);

    /* Build a memory compound matching our row_t layout. */
    hid_t mt = H5Tcreate(H5T_COMPOUND, sizeof(row_t));
    H5Tinsert(mt, "IDEN.", offsetof(row_t, iden), H5T_NATIVE_DOUBLE);
    H5Tinsert(mt, "RA",    offsetof(row_t, ra),   H5T_NATIVE_DOUBLE);
    H5Tinsert(mt, "DEC",   offsetof(row_t, dec),  H5T_NATIVE_DOUBLE);
    H5Tinsert(mt, "TYPE",  offsetof(row_t, type), H5T_NATIVE_DOUBLE);
    H5Tinsert(mt, "D25",   offsetof(row_t, d25),  H5T_NATIVE_DOUBLE);
    H5Tinsert(mt, "INCL.", offsetof(row_t, incl), H5T_NATIVE_DOUBLE);
    H5Tinsert(mt, "RV",    offsetof(row_t, rv),   H5T_NATIVE_DOUBLE);

    row_t rows[10] = {{0}};
    assert(H5Dread(did, mt, H5S_ALL, H5S_ALL, H5P_DEFAULT, rows) >= 0);

    /* Spot-check first row against h5dump-confirmed values. */
    assert(rows[0].iden ==  -1116.59);
    assert(rows[0].dec  ==   59.5667);
    assert(rows[0].type ==      3.0);

    /* Cross-check against per-column read for one column. */
    hid_t did_ra = H5Dopen2(fid, "/HDU1/columns/RA", H5P_DEFAULT);
    double ra_col[10];
    H5Dread(did_ra, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, ra_col);
    H5Dclose(did_ra);
    for (int i = 0; i < 10; ++i) assert(rows[i].ra == ra_col[i]);

    H5Tclose(mt); H5Dclose(did); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: /HDU1/table compound row view matches per-column reads\n");
    return 0;
}
