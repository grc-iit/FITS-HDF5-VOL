/* M2.17 verification: two FITS files open simultaneously through the
 * connector report distinct contents. Asserts the adapter holds no global
 * state. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

#ifndef SCIIO_FIXTURES_DIR
#error "SCIIO_FIXTURES_DIR must be defined"
#endif

typedef struct { int n; char names[8][16]; } collect_t;
static herr_t collect(hid_t g, const char *name, const H5L_info2_t *i, void *u)
{
    (void)g; (void)i;
    collect_t *c = (collect_t *)u;
    snprintf(c->names[c->n++], 16, "%s", name);
    return 0;
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);

    /* Open two distinct fixtures concurrently. */
    hid_t f_a = H5Fopen(SCIIO_FIXTURES_DIR "/image_2d.fits",  H5F_ACC_RDONLY, fapl);
    hid_t f_b = H5Fopen(SCIIO_FIXTURES_DIR "/multi_hdu.fits", H5F_ACC_RDONLY, fapl);
    assert(f_a >= 0 && f_b >= 0);

    /* Each must report its own root contents. */
    hid_t r_a = H5Gopen2(f_a, "/", H5P_DEFAULT);
    hid_t r_b = H5Gopen2(f_b, "/", H5P_DEFAULT);

    collect_t ca = {0}, cb = {0};
    hsize_t ia = 0, ib = 0;
    assert(H5Literate2(r_a, H5_INDEX_NAME, H5_ITER_INC, &ia, collect, &ca) >= 0);
    assert(H5Literate2(r_b, H5_INDEX_NAME, H5_ITER_INC, &ib, collect, &cb) >= 0);

    assert(ca.n == 1);  /* image_2d: 1 HDU */
    assert(strcmp(ca.names[0], "HDU0") == 0);

    assert(cb.n == 4);  /* multi_hdu: 3 HDUs + SCI alias */
    int seen_sci = 0;
    for (int i = 0; i < cb.n; ++i) if (strcmp(cb.names[i], "SCI") == 0) seen_sci = 1;
    assert(seen_sci);

    /* Cross-check: a's HDU0 has user attrs that b's doesn't. */
    hid_t hdu0_a = H5Gopen2(f_a, "/HDU0", H5P_DEFAULT);
    hid_t hdu0_b = H5Gopen2(f_b, "/HDU0", H5P_DEFAULT);
    assert(H5Aexists(hdu0_a, "MYINT") > 0);
    assert(H5Aexists(hdu0_b, "MYINT") == 0);
    H5Gclose(hdu0_a); H5Gclose(hdu0_b);

    /* Close in non-LIFO order to exercise independence. */
    H5Gclose(r_a);
    H5Fclose(f_a);
    H5Gclose(r_b);
    H5Fclose(f_b);

    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: two FITS files served concurrently from independent adapter state\n");
    return 0;
}
