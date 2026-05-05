/* M2.11 verification: EXTNAME → soft link /<EXTNAME> pointing to /HDUn
 *. Tested against multi_hdu.fits (EXTNAME='SCI' on HDU1). */

#include <assert.h>
#include <stdio.h>
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
    hid_t fid = H5Fopen(FITS_FIXTURES_DIR "/multi_hdu.fits", H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    /* H5Lexists on the soft link's name */
    assert(H5Lexists(fid, "SCI", H5P_DEFAULT) > 0);

    /* H5Lget_info: type must be SOFT */
    H5L_info2_t info;
    assert(H5Lget_info2(fid, "SCI", &info, H5P_DEFAULT) >= 0);
    assert(info.type == H5L_TYPE_SOFT);

    /* H5Lget_val: target = "/HDU1" */
    char buf[64] = {0};
    assert(H5Lget_val(fid, "SCI", buf, sizeof(buf), H5P_DEFAULT) >= 0);
    assert(strcmp(buf, "/HDU1") == 0);

    /* H5Gopen2 on the soft link must dereference and return HDU1's group. */
    hid_t g_via_link  = H5Gopen2(fid, "/SCI",  H5P_DEFAULT);
    hid_t g_via_index = H5Gopen2(fid, "/HDU1", H5P_DEFAULT);
    assert(g_via_link  >= 0);
    assert(g_via_index >= 0);
    /* Both groups should report the same nlinks (same underlying HDU). */
    H5G_info_t a, b;
    H5Gget_info(g_via_link,  &a);
    H5Gget_info(g_via_index, &b);
    assert(a.nlinks == b.nlinks);
    H5Gclose(g_via_link); H5Gclose(g_via_index);

    H5Fclose(fid); H5Pclose(fapl); H5VLclose(vol);
    printf("OK: EXTNAME soft link wired (SCI -> /HDU1)\n");
    return 0;
}
