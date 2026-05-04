/* M2.19 verification: complex (pair) FITS keyword surfaces as an HDF5
 * compound {double re; double im;} attribute (plan §7.5). */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

#ifndef SCIIO_FIXTURES_DIR
#error "SCIIO_FIXTURES_DIR must be defined"
#endif

typedef struct { double re, im; } cplx_t;

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);
    hid_t fid = H5Fopen(SCIIO_FIXTURES_DIR "/complex_keyword.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);
    hid_t hdu0 = H5Gopen2(fid, "/HDU0", H5P_DEFAULT);
    assert(hdu0 >= 0);

    hid_t aid = H5Aopen(hdu0, "MYCMPLX", H5P_DEFAULT);
    assert(aid >= 0);

    /* Type must be a compound with two double members named "re" and "im". */
    hid_t tid = H5Aget_type(aid);
    assert(H5Tget_class(tid) == H5T_COMPOUND);
    assert(H5Tget_nmembers(tid) == 2);
    char *m0 = H5Tget_member_name(tid, 0);
    char *m1 = H5Tget_member_name(tid, 1);
    assert(m0 && strcmp(m0, "re") == 0);
    assert(m1 && strcmp(m1, "im") == 0);
    H5free_memory(m0); H5free_memory(m1);
    H5Tclose(tid);

    /* Read into a host struct via a memory compound type. */
    hid_t mem = H5Tcreate(H5T_COMPOUND, sizeof(cplx_t));
    H5Tinsert(mem, "re", offsetof(cplx_t, re), H5T_NATIVE_DOUBLE);
    H5Tinsert(mem, "im", offsetof(cplx_t, im), H5T_NATIVE_DOUBLE);
    cplx_t v = {0, 0};
    assert(H5Aread(aid, mem, &v) >= 0);
    assert(v.re ==  1.5);
    assert(v.im == -2.25);
    H5Tclose(mem);

    H5Aclose(aid); H5Gclose(hdu0); H5Fclose(fid);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: complex MYCMPLX reads as compound {re=1.5, im=-2.25}\n");
    return 0;
}
