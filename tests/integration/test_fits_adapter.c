/* M2.3 verification: probe + open enumerate HDUs from a CFITSIO-built file. */

#define _DEFAULT_SOURCE   /* mkstemps */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fitsio.h>

#include "fits_hdf5/adapter.h"
#include "fits_hdf5/registry.h"

static void build_fixture(const char *path)
{
    int status = 0;
    fitsfile *fp = NULL;
    /* CFITSIO refuses to clobber unless prefixed with '!' */
    char clobber[256];
    snprintf(clobber, sizeof(clobber), "!%s", path);
    assert(fits_create_file(&fp, clobber, &status) == 0);

    /* HDU0: primary 2-D image */
    long naxes_p[2] = {4, 3};
    assert(fits_create_img(fp, SHORT_IMG, 2, naxes_p, &status) == 0);

    /* HDU1: image extension with EXTNAME='SCI' */
    long naxes_e[2] = {2, 2};
    assert(fits_create_img(fp, FLOAT_IMG, 2, naxes_e, &status) == 0);
    char ext[] = "SCI";
    assert(fits_update_key(fp, TSTRING, "EXTNAME", ext, "Extension name", &status) == 0);

    /* HDU2: another image extension, no EXTNAME */
    assert(fits_create_img(fp, BYTE_IMG, 2, naxes_e, &status) == 0);

    assert(fits_close_file(fp, &status) == 0);
    assert(status == 0);
}

int main(void)
{
    char path[] = "/tmp/fits_m23_XXXXXX.fits";
    int fd = mkstemps(path, 5);
    assert(fd >= 0);
    close(fd);

    build_fixture(path);

    /* Drive the adapter through its vtable, the same way the connector does. */
    const fits_adapter_t *a = &fits_adapter;

    adapter_probe_result_t pr = {0};
    assert(a->probe(path, &pr) == 0);
    assert(pr.confidence == 100);
    assert(strcmp(pr.name, "fits") == 0);

    adapter_file_t *f = a->open(path, 0);
    assert(f && "adapter open failed");

    adapter_object_t *root = a->root(f);
    assert(root);
    assert(a->object_kind(root) == ADAPTER_KIND_GROUP);

    a->close(f);
    unlink(path);

    printf("OK: probed and enumerated 3-HDU FITS fixture\n");
    return 0;
}
