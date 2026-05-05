/* HDF5 → FITS converter. NOT part of fits-hdf5-vol (which is read-only); this
 * uses CFITSIO directly to write the FITS side. Reads HDF5 via the native
 * VOL only — no fits-hdf5-vol involved.
 *
 * Walks /HDU0..HDUn groups. For each group:
 *   - copies the `data` dataset to a new image HDU
 *   - copies every attribute as a FITS header keyword (best-effort: int,
 *     float, bool, string, vlen-string array)
 *
 * This is a demonstration utility, not production code.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hdf5.h>
#include <fitsio.h>

#define DIE(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while (0)
#define CFITS(s) do { if (s) { fits_report_error(stderr, s); exit(1); } } while (0)

/* Map HDF5 dataset type to FITS BITPIX + CFITSIO write-type. */
static int h5_to_bitpix_dtype(hid_t tid, int *out_bitpix, int *out_dtype)
{
    H5T_class_t cls = H5Tget_class(tid);
    size_t sz = H5Tget_size(tid);
    if (cls == H5T_INTEGER) {
        H5T_sign_t sg = H5Tget_sign(tid);
        if (sg == H5T_SGN_NONE) {
            switch (sz) {
                case 1: *out_bitpix = BYTE_IMG;     *out_dtype = TBYTE;     return 0;
                case 2: *out_bitpix = SHORT_IMG;    *out_dtype = TUSHORT;   return 0;
                case 4: *out_bitpix = LONG_IMG;     *out_dtype = TUINT;     return 0;
                case 8: *out_bitpix = LONGLONG_IMG; *out_dtype = TULONGLONG;return 0;
            }
        } else {
            switch (sz) {
                case 1: *out_bitpix = SBYTE_IMG;    *out_dtype = TSBYTE;    return 0;
                case 2: *out_bitpix = SHORT_IMG;    *out_dtype = TSHORT;    return 0;
                case 4: *out_bitpix = LONG_IMG;     *out_dtype = TINT;      return 0;
                case 8: *out_bitpix = LONGLONG_IMG; *out_dtype = TLONGLONG; return 0;
            }
        }
    } else if (cls == H5T_FLOAT) {
        switch (sz) {
            case 4: *out_bitpix = FLOAT_IMG;  *out_dtype = TFLOAT;  return 0;
            case 8: *out_bitpix = DOUBLE_IMG; *out_dtype = TDOUBLE; return 0;
        }
    }
    return -1;
}

/* Copy one HDF5 attribute to a FITS keyword. Skips structural keywords
 * (BITPIX/NAXIS/SIMPLE/EXTEND) — CFITSIO writes those itself when the image
 * HDU is created. */
static int is_structural_kw(const char *name)
{
    if (strcmp(name, "SIMPLE")   == 0) return 1;
    if (strcmp(name, "BITPIX")   == 0) return 1;
    if (strcmp(name, "EXTEND")   == 0) return 1;
    if (strcmp(name, "NAXIS")    == 0) return 1;
    if (strncmp(name, "NAXIS", 5) == 0) {
        for (const char *p = name + 5; *p; ++p)
            if (*p < '0' || *p > '9') return 0;
        return 1;
    }
    if (strcmp(name, "__raw_header__") == 0) return 1;
    if (strcmp(name, "GCOUNT") == 0 || strcmp(name, "PCOUNT") == 0) return 1;
    if (strcmp(name, "COMMENT") == 0 || strcmp(name, "HISTORY") == 0) return 1;
    if (strcmp(name, "BSCALE") == 0 || strcmp(name, "BZERO") == 0) return 1;
    return 0;
}

typedef struct { fitsfile *fp; } attr_ctx_t;

static herr_t copy_attr(hid_t loc, const char *name, const H5A_info_t *info, void *user)
{
    (void)info;
    attr_ctx_t *c = (attr_ctx_t *)user;
    if (is_structural_kw(name)) return 0;

    hid_t a = H5Aopen(loc, name, H5P_DEFAULT);
    hid_t t = H5Aget_type(a);
    int s = 0;

    H5T_class_t cls = H5Tget_class(t);
    if (cls == H5T_INTEGER) {
        long long v = 0;
        H5Aread(a, H5T_NATIVE_INT64, &v);
        fits_update_key(c->fp, TLONGLONG, name, &v, NULL, &s);
    } else if (cls == H5T_FLOAT) {
        double v = 0; H5Aread(a, H5T_NATIVE_DOUBLE, &v);
        fits_update_key(c->fp, TDOUBLE, name, &v, NULL, &s);
    } else if (cls == H5T_STRING && H5Tis_variable_str(t)) {
        char *v = NULL; H5Aread(a, t, &v);
        if (v) {
            fits_update_key(c->fp, TSTRING, name, v, NULL, &s);
            free(v);
        }
    }
    if (s) { fits_clear_errmark(); fits_report_error(stderr, s); s = 0; }

    H5Tclose(t); H5Aclose(a);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) DIE("usage: h5_to_fits <src.h5> <dst.fits>");

    /* Native HDF5 input — NO fits-hdf5-vol. */
    hid_t src = H5Fopen(argv[1], H5F_ACC_RDONLY, H5P_DEFAULT);
    if (src < 0) DIE("H5Fopen src");

    /* Fresh FITS output via CFITSIO. */
    int s = 0;
    fitsfile *fp;
    char clobber[1024]; snprintf(clobber, sizeof(clobber), "!%s", argv[2]);
    fits_create_file(&fp, clobber, &s); CFITS(s);

    int hdus_emitted = 0;
    for (int i = 0; i < 100; ++i) {
        char hpath[16]; snprintf(hpath, sizeof(hpath), "/HDU%d", i);
        if (H5Lexists(src, hpath, H5P_DEFAULT) <= 0) break;

        hid_t s_grp = H5Gopen2(src, hpath, H5P_DEFAULT);

        char dpath[32]; snprintf(dpath, sizeof(dpath), "%s/data", hpath);
        if (H5Lexists(src, dpath, H5P_DEFAULT) > 0) {
            hid_t s_d = H5Dopen2(src, dpath, H5P_DEFAULT);
            hid_t s_t = H5Dget_type(s_d);
            hid_t s_s = H5Dget_space(s_d);

            int bitpix = 0, cf_dtype = 0;
            if (h5_to_bitpix_dtype(s_t, &bitpix, &cf_dtype) != 0) DIE("unsupported dtype");

            int rank = H5Sget_simple_extent_ndims(s_s);
            hsize_t hdims[8];
            H5Sget_simple_extent_dims(s_s, hdims, NULL);
            /* HDF5 (C-order) → FITS (Fortran-order): reverse axes. */
            long fdims[8];
            for (int k = 0; k < rank; ++k) fdims[k] = (long)hdims[rank - 1 - k];

            fits_create_img(fp, bitpix, rank, fdims, &s); CFITS(s);

            hssize_t nelem = H5Sget_simple_extent_npoints(s_s);
            size_t tsz = H5Tget_size(s_t);
            void *buf = malloc((size_t)nelem * tsz);
            H5Dread(s_d, s_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
            long fpixel[8] = {1,1,1,1,1,1,1,1};
            fits_write_pix(fp, cf_dtype, fpixel, nelem, buf, &s); CFITS(s);
            free(buf);

            H5Sclose(s_s); H5Tclose(s_t); H5Dclose(s_d);
        } else {
            /* Header-only HDU. Create an empty image. */
            long zero[1] = {0};
            fits_create_img(fp, BYTE_IMG, 0, zero, &s); CFITS(s);
        }

        attr_ctx_t ctx = { .fp = fp };
        hsize_t idx = 0;
        H5Aiterate2(s_grp, H5_INDEX_NAME, H5_ITER_INC, &idx, copy_attr, &ctx);
        H5Gclose(s_grp);
        ++hdus_emitted;
    }

    fits_close_file(fp, &s); CFITS(s);
    H5Fclose(src);
    printf("OK: emitted %d HDUs to %s\n", hdus_emitted, argv[2]);
    return 0;
}
