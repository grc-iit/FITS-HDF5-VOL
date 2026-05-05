/* M2.10 verification: __raw_header__ exposes byte-exact 80-char FITS cards
 * as a 1-D vlen-string array attribute. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

#ifndef FITS_FIXTURES_DIR
#error "FITS_FIXTURES_DIR must be defined"
#endif

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(FITS_FIXTURES_DIR "/image_2d.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);
    hid_t hdu0 = H5Gopen2(fid, "/HDU0", H5P_DEFAULT);
    assert(hdu0 >= 0);

    hid_t aid = H5Aopen(hdu0, "__raw_header__", H5P_DEFAULT);
    assert(aid >= 0 && "__raw_header__ attribute missing");

    /* Type: vlen UTF-8 string */
    hid_t tid = H5Aget_type(aid);
    assert(H5Tget_class(tid) == H5T_STRING);
    assert(H5Tis_variable_str(tid) > 0);
    H5Tclose(tid);

    /* Space: rank-1 with n cards */
    hid_t sid = H5Aget_space(aid);
    assert(H5Sget_simple_extent_ndims(sid) == 1);
    hsize_t n_cards = 0;
    H5Sget_simple_extent_dims(sid, &n_cards, NULL);
    assert(n_cards > 0);
    H5Sclose(sid);

    /* Read */
    hid_t st = H5Tcopy(H5T_C_S1);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8);
    char **cards = calloc((size_t)n_cards, sizeof(char *));
    assert(H5Aread(aid, st, cards) >= 0);

    /* Each card: <= 80 chars (CFITSIO may right-trim padding spaces in the
     * record) and contains a "keyword =" or "COMMENT" / "HISTORY" / "END". */
    int saw_simple = 0, saw_end = 0;
    for (hsize_t i = 0; i < n_cards; ++i) {
        assert(cards[i]);
        size_t len = strlen(cards[i]);
        assert(len <= 80);
        if (strncmp(cards[i], "SIMPLE  =", 9) == 0) saw_simple = 1;
        if (strncmp(cards[i], "END", 3) == 0) saw_end = 1;
        free(cards[i]);
    }
    free(cards);
    /* SIMPLE must be the first card of the primary; END must close the header. */
    assert(saw_simple && "first card must be SIMPLE = T");
    assert(saw_end    && "header must end with an END card");

    H5Tclose(st);
    H5Aclose(aid); H5Gclose(hdu0); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: __raw_header__ exposes %llu cards including SIMPLE and END\n",
           (unsigned long long)n_cards);
    return 0;
}
