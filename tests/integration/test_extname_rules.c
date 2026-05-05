/* M2.16 verification: EXTNAME validation
 *
 * Build a fixture in-process with four extensions whose EXTNAME values
 * trigger each rejection rule:
 *   HDU1  EXTNAME='SAME'      -> kept (first wins)
 *   HDU2  EXTNAME='SAME'      -> dropped (duplicate)
 *   HDU3  EXTNAME='bad/name'  -> dropped (slash)
 *   HDU4  EXTNAME='HDU0'      -> dropped (shadows primary)
 *
 * Expected aliases: only "SAME" → "/HDU1". */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fitsio.h>
#include <hdf5.h>

#include "fits_hdf5/fits_hdf5_vol.h"

static void build_fixture(const char *path)
{
    int s = 0; fitsfile *fp;
    char buf[256]; snprintf(buf, sizeof(buf), "!%s", path);
    assert(fits_create_file(&fp, buf, &s) == 0);
    long n[2] = {2, 2};
    assert(fits_create_img(fp, SHORT_IMG, 2, n, &s) == 0);            /* HDU0 */
    assert(fits_create_img(fp, SHORT_IMG, 2, n, &s) == 0);            /* HDU1 */
    char same[] = "SAME";   assert(fits_update_key(fp, TSTRING, "EXTNAME", same, NULL, &s) == 0);
    assert(fits_create_img(fp, SHORT_IMG, 2, n, &s) == 0);            /* HDU2 */
    char same2[] = "SAME";  assert(fits_update_key(fp, TSTRING, "EXTNAME", same2, NULL, &s) == 0);
    assert(fits_create_img(fp, SHORT_IMG, 2, n, &s) == 0);            /* HDU3 */
    char bad[]  = "bad/name"; assert(fits_update_key(fp, TSTRING, "EXTNAME", bad, NULL, &s) == 0);
    assert(fits_create_img(fp, SHORT_IMG, 2, n, &s) == 0);            /* HDU4 */
    char shdw[] = "HDU0";   assert(fits_update_key(fp, TSTRING, "EXTNAME", shdw, NULL, &s) == 0);
    assert(fits_close_file(fp, &s) == 0);
}

typedef struct { int n; char names[16][32]; } collect_t;
static herr_t collect(hid_t g, const char *name, const H5L_info2_t *i, void *u)
{
    (void)g; (void)i;
    collect_t *c = (collect_t *)u;
    snprintf(c->names[c->n++], 32, "%s", name);
    return 0;
}

static int has(collect_t *c, const char *name)
{
    for (int i = 0; i < c->n; ++i) if (strcmp(c->names[i], name) == 0) return 1;
    return 0;
}

int main(void)
{
    hid_t vol = H5VLregister_connector_by_name(FITS_HDF5_VOL_NAME, H5P_DEFAULT);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);

    char path[] = "/tmp/fits_extr_XXXXXX.fits";
    int fd = mkstemps(path, 5); assert(fd >= 0); close(fd);
    build_fixture(path);

    /* Suppress diagnostics from EXTNAME-collision warning to stderr. */
    fflush(stderr);
    int saved = dup(2);
    int devnull = open("/dev/null", 1);
    dup2(devnull, 2);

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);

    /* Restore stderr. */
    fflush(stderr);
    dup2(saved, 2); close(saved); close(devnull);

    assert(fid >= 0);

    /* Iterate root and collect link names. */
    hid_t root = H5Gopen2(fid, "/", H5P_DEFAULT);
    collect_t c = {0};
    hsize_t idx = 0;
    assert(H5Literate2(root, H5_INDEX_NAME, H5_ITER_INC, &idx, collect, &c) >= 0);
    H5Gclose(root);

    /* All 5 HDUs must be present. */
    assert(has(&c, "HDU0"));
    assert(has(&c, "HDU1"));
    assert(has(&c, "HDU2"));
    assert(has(&c, "HDU3"));
    assert(has(&c, "HDU4"));

    /* Only the FIRST 'SAME' wins. */
    assert(has(&c, "SAME"));

    /* Slash-containing alias must NOT appear. */
    assert(!has(&c, "bad/name"));
    assert(!has(&c, "bad"));

    /* Shadow alias 'HDU0' must NOT appear as a duplicate. There is exactly
     * one entry whose name is "HDU0" — and it's the primary, not the alias. */
    int hdu0_count = 0;
    for (int i = 0; i < c.n; ++i) if (strcmp(c.names[i], "HDU0") == 0) ++hdu0_count;
    assert(hdu0_count == 1);

    /* /SAME must point to /HDU1, not /HDU2. */
    char tgt[64] = {0};
    assert(H5Lget_val(fid, "SAME", tgt, sizeof(tgt), H5P_DEFAULT) >= 0);
    assert(strcmp(tgt, "/HDU1") == 0);

    H5Fclose(fid);
    unlink(path);
    H5Pclose(fapl); H5VLclose(vol);
    printf("OK: EXTNAME collision/slash/shadow rules enforced (alias count=%d)\n", c.n - 5);
    return 0;
}
