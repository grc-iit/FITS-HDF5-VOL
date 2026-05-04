/* M2.5 verification: H5Literate2 on the root group enumerates HDUn names,
 * H5Gget_info reports the right link count, H5Lexists works. */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fitsio.h>
#include <hdf5.h>

#include "sciio/sciio_vol.h"

static void build_fixture(const char *path)
{
    int status = 0;
    fitsfile *fp = NULL;
    char clobber[256]; snprintf(clobber, sizeof(clobber), "!%s", path);
    assert(fits_create_file(&fp, clobber, &status) == 0);
    long n2[2] = {4, 3};
    assert(fits_create_img(fp, SHORT_IMG, 2, n2, &status) == 0);  /* HDU0 primary */
    assert(fits_create_img(fp, FLOAT_IMG, 2, n2, &status) == 0);  /* HDU1 ext */
    assert(fits_create_img(fp, BYTE_IMG,  2, n2, &status) == 0);  /* HDU2 ext */
    assert(fits_close_file(fp, &status) == 0);
}

typedef struct { int n; char names[8][16]; } collect_t;

static herr_t collect_link(hid_t gid, const char *name, const H5L_info2_t *info, void *data)
{
    (void)gid; (void)info;
    collect_t *c = (collect_t *)data;
    assert(c->n < 8);
    snprintf(c->names[c->n++], 16, "%s", name);
    return 0;
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    assert(vol >= 0);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    assert(H5Pset_vol(fapl, vol, NULL) >= 0);

    char path[] = "/tmp/sciio_iter_XXXXXX.fits";
    int fd = mkstemps(path, 5);
    assert(fd >= 0);
    close(fd);
    build_fixture(path);

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    assert(fid >= 0);

    /* H5Gget_info on the root */
    hid_t root = H5Gopen2(fid, "/", H5P_DEFAULT);
    assert(root >= 0);
    H5G_info_t gi;
    assert(H5Gget_info(root, &gi) >= 0);
    assert(gi.nlinks == 3);

    /* H5Literate2 enumerates HDU0, HDU1, HDU2 in order */
    collect_t c = {0};
    hsize_t idx = 0;
    assert(H5Literate2(root, H5_INDEX_NAME, H5_ITER_INC, &idx, collect_link, &c) >= 0);
    assert(c.n == 3);
    assert(strcmp(c.names[0], "HDU0") == 0);
    assert(strcmp(c.names[1], "HDU1") == 0);
    assert(strcmp(c.names[2], "HDU2") == 0);

    /* H5Lexists */
    assert(H5Lexists(fid, "HDU1", H5P_DEFAULT) > 0);
    assert(H5Lexists(fid, "HDU99", H5P_DEFAULT) == 0);

    /* Open one HDU group: M3+ exposes a single "data" child. */
    hid_t hdu1 = H5Gopen2(fid, "/HDU1", H5P_DEFAULT);
    assert(hdu1 >= 0);
    H5G_info_t hi;
    assert(H5Gget_info(hdu1, &hi) >= 0);
    assert(hi.nlinks == 1 && "HDU image groups now contain a 'data' dataset");
    assert(H5Lexists(hdu1, "data", H5P_DEFAULT) > 0);
    assert(H5Gclose(hdu1) >= 0);

    assert(H5Gclose(root) >= 0);
    assert(H5Fclose(fid) >= 0);
    unlink(path);
    H5Pclose(fapl); H5VLclose(vol);

    printf("OK: H5Literate2 + H5Gget_info + H5Lexists + nested H5Gopen2 succeed\n");
    return 0;
}
