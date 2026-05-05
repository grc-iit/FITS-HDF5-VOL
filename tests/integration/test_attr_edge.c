/* M2.6b verification: HIERARCH, CONTINUE, COMMENT, HISTORY against
 * edge_keywords.fits. The fixture is generated deterministically by
 * tests/fixtures/build_fixtures.c. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#ifndef FITS_FIXTURES_DIR
#error "FITS_FIXTURES_DIR must be defined"
#endif

static char *read_string_attr(hid_t loc, const char *name)
{
    hid_t aid = H5Aopen(loc, name, H5P_DEFAULT);
    assert(aid >= 0);
    hid_t st = H5Tcopy(H5T_C_S1);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8);
    char *s = NULL;
    assert(H5Aread(aid, st, &s) >= 0);
    H5Tclose(st);
    H5Aclose(aid);
    return s;
}

static double read_double_attr(hid_t loc, const char *name)
{
    hid_t aid = H5Aopen(loc, name, H5P_DEFAULT);
    assert(aid >= 0);
    double v = 0;
    assert(H5Aread(aid, H5T_NATIVE_DOUBLE, &v) >= 0);
    H5Aclose(aid);
    return v;
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    assert(vol >= 0);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    assert(H5Pset_vol(fapl, vol, NULL) >= 0);

    hid_t fid = H5Fopen(FITS_FIXTURES_DIR "/edge_keywords.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);
    hid_t hdu0 = H5Gopen2(fid, "/HDU0", H5P_DEFAULT);
    assert(hdu0 >= 0);

    /* HIERARCH ESO INS TEMP — surfaced as ESO.INS.TEMP, value 6.022e23. */
    double hv = read_double_attr(hdu0, "ESO.INS.TEMP");
    assert(hv > 6.02e23 && hv < 6.03e23);

    /* CONTINUE long string — must be reassembled to its full form. */
    char *longstr = read_string_attr(hdu0, "MYLONG");
    assert(longstr);
    /* The fixture emits 3 fragments separated by CFITSIO's CONTINUE machinery.
     * We expect the reassembled result to contain text from all three. */
    assert(strstr(longstr, "intentionally longer") != NULL);
    assert(strstr(longstr, "CONTINUE cards")        != NULL);
    assert(strstr(longstr, "reassemble the parts")  != NULL);
    free(longstr);

    /* COMMENT — assert our 3 records are present and in fixture order.
     * (CFITSIO injects boilerplate COMMENTs of its own; we
     * surface every keyword in file order, so we check substring presence
     * and relative ordering, not exact line count.) */
    char *cmt = read_string_attr(hdu0, "COMMENT");
    assert(cmt);
    char *p1 = strstr(cmt, "First comment line");
    char *p2 = strstr(cmt, "Second comment line");
    char *p3 = strstr(cmt, "Third comment line");
    assert(p1 && p2 && p3);
    assert(p1 < p2 && p2 < p3);
    free(cmt);

    /* HISTORY — both records present and in order. */
    char *hst = read_string_attr(hdu0, "HISTORY");
    assert(hst);
    char *h1 = strstr(hst, "Created by build_fixtures");
    char *h2 = strstr(hst, "M2.7 test corpus");
    assert(h1 && h2 && h1 < h2);
    free(hst);

    H5Gclose(hdu0); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: HIERARCH + CONTINUE + COMMENT + HISTORY mapping verified\n");
    return 0;
}
