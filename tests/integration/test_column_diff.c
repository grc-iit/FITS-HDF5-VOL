/* M4.5 differential: every column from a real table HDU read two ways —
 * via fits-hdf5-vol + H5Dread, and directly via CFITSIO — must agree byte-for-byte
 * after we account for the type CFITSIO returns. Run on tb.fits (binary,
 * 4 cols including a 3A skipped) and file001.fits (ASCII, 7 double cols). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fitsio.h>
#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#ifndef FITS_FIXTURES_DIR
#error "FITS_FIXTURES_DIR must be defined"
#endif
#ifndef FITS_CORPUS_DIR
#error "FITS_CORPUS_DIR must be defined"
#endif

/* file001.fits: 7 cols all TDOUBLE in ASCII, 10 rows. Compare fits-hdf5-vol vs
 * direct CFITSIO TDOUBLE read. */
static void diff_file001(hid_t vol)
{
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    const char *path = "/home/isa-grc/fits-tests/ftt4b/file001.fits";
    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    /* CFITSIO direct */
    int s = 0;
    fitsfile *fp;
    fits_open_file(&fp, path, READONLY, &s); assert(s == 0);
    fits_movabs_hdu(fp, 2, NULL, &s);

    const char *cols[] = {"IDEN.", "RA", "DEC", "TYPE", "D25", "INCL.", "RV"};
    for (int c = 0; c < 7; ++c) {
        char hpath[64]; snprintf(hpath, sizeof(hpath), "/HDU1/columns/%s", cols[c]);
        hid_t did = H5Dopen2(fid, hpath, H5P_DEFAULT);
        assert(did >= 0 && "H5Dopen on column");

        hid_t tid = H5Dget_type(did);
        assert(H5Tget_class(tid) == H5T_FLOAT && H5Tget_size(tid) == 8);
        H5Tclose(tid);

        double via_vol[10] = {0}, via_cfitsio[10] = {0};
        assert(H5Dread(did, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, via_vol) >= 0);

        int s2 = 0;
        fits_read_col(fp, TDOUBLE, c + 1, 1, 1, 10, NULL, via_cfitsio, NULL, &s2);
        assert(s2 == 0);

        for (int i = 0; i < 10; ++i) {
            if (via_vol[i] != via_cfitsio[i]) {
                fprintf(stderr,
                    "FAIL file001 col '%s' row %d: vol=%g cfitsio=%g\n",
                    cols[c], i, via_vol[i], via_cfitsio[i]);
                assert(0);
            }
        }
        H5Dclose(did);
    }

    fits_close_file(fp, &s);
    H5Fclose(fid); H5Pclose(fapl);
    printf("  file001.fits: 7 columns × 10 rows agree\n");
}

/* tb.fits: 4 cols. c1=int32, c2=3A string (skipped — M4.7), c3=float32, c4=bool.
 * 2 rows. */
static void diff_tb(hid_t vol)
{
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    const char *path = FITS_CORPUS_DIR "/tb.fits";
    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    int s = 0;
    fitsfile *fp;
    fits_open_file(&fp, path, READONLY, &s); assert(s == 0);
    fits_movabs_hdu(fp, 2, NULL, &s);

    /* c1 — int32 */
    {
        hid_t did = H5Dopen2(fid, "/HDU1/columns/c1", H5P_DEFAULT);
        int via_vol[2] = {0}, via_cf[2] = {0};
        assert(H5Dread(did, H5T_NATIVE_INT32, H5S_ALL, H5S_ALL, H5P_DEFAULT, via_vol) >= 0);
        int s2 = 0;
        fits_read_col(fp, TINT, 1, 1, 1, 2, NULL, via_cf, NULL, &s2);
        assert(s2 == 0);
        assert(via_vol[0] == via_cf[0] && via_vol[1] == via_cf[1]);
        H5Dclose(did);
    }
    /* c3 — float32 */
    {
        hid_t did = H5Dopen2(fid, "/HDU1/columns/c3", H5P_DEFAULT);
        float via_vol[2] = {0}, via_cf[2] = {0};
        assert(H5Dread(did, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, via_vol) >= 0);
        int s2 = 0;
        fits_read_col(fp, TFLOAT, 3, 1, 1, 2, NULL, via_cf, NULL, &s2);
        assert(s2 == 0);
        assert(via_vol[0] == via_cf[0] && via_vol[1] == via_cf[1]);
        H5Dclose(did);
    }
    /* c4 — bool */
    {
        hid_t did = H5Dopen2(fid, "/HDU1/columns/c4", H5P_DEFAULT);
        unsigned char via_vol[2] = {0}, via_cf[2] = {0};
        assert(H5Dread(did, H5T_NATIVE_HBOOL, H5S_ALL, H5S_ALL, H5P_DEFAULT, via_vol) >= 0);
        int s2 = 0;
        fits_read_col(fp, TLOGICAL, 4, 1, 1, 2, NULL, via_cf, NULL, &s2);
        assert(s2 == 0);
        assert(via_vol[0] == via_cf[0] && via_vol[1] == via_cf[1]);
        H5Dclose(did);
    }

    fits_close_file(fp, &s);
    H5Fclose(fid); H5Pclose(fapl);
    printf("  tb.fits: c1/c3/c4 agree (c2 multi-element string deferred to M4.7)\n");
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    assert(vol >= 0);
    diff_file001(vol);
    diff_tb(vol);
    H5VLclose(vol);
    printf("OK: column reads match direct CFITSIO\n");
    return 0;
}
