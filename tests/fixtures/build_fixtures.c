/* Deterministic FITS fixture builder.
 *
 * Emits a fixed set of test FITS files into the directory passed on the
 * command line. Run by CMake at build time. Each fixture targets a distinct
 * edge case from:
 *
 *   primary_only.fits     — Primary HDU with NAXIS=0 (header-only)
 *   image_2d.fits         — Single 4×3 BITPIX=16 image, no extensions
 *   multi_hdu.fits        — Primary + 2 image extensions; EXTNAME on one
 *   edge_keywords.fits    — HIERARCH / CONTINUE / COMMENT / HISTORY records
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fitsio.h>

#define CHECK(s) do { if (s) { fits_report_error(stderr, s); return 1; } } while (0)

static int build_primary_only(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);
    /* NAXIS=0 primary: zero-element image. CFITSIO accepts naxis=0. */
    long naxes[1] = {0};
    fits_create_img(fp, BYTE_IMG, 0, naxes, &s); CHECK(s);
    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

static int build_image_2d(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);
    long naxes[2] = {4, 3};   /* 4 columns × 3 rows */
    fits_create_img(fp, SHORT_IMG, 2, naxes, &s); CHECK(s);
    /* Write deterministic pixel data: pixel[r,c] = r*10 + c */
    short data[12];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            data[r * 4 + c] = (short)(r * 10 + c);
    long fpixel[2] = {1, 1};
    fits_write_pix(fp, TSHORT, fpixel, 12, data, &s); CHECK(s);
    /* A few user keywords to exercise scalar typing in differential tests. */
    long lv = 42;
    double dv = 3.14159265358979;
    int bv = 1;
    char sv[] = "regression-fixture";
    fits_update_key(fp, TLONG,    "MYINT",   &lv, "test int",   &s); CHECK(s);
    fits_update_key(fp, TDOUBLE,  "MYFLOAT", &dv, "test float", &s); CHECK(s);
    fits_update_key(fp, TLOGICAL, "MYBOOL",  &bv, "test bool",  &s); CHECK(s);
    fits_update_key(fp, TSTRING,  "MYSTR",   sv,  "test str",   &s); CHECK(s);
    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

static int build_multi_hdu(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);

    /* HDU0 primary 2×2 short */
    long n2[2] = {2, 2};
    fits_create_img(fp, SHORT_IMG, 2, n2, &s); CHECK(s);

    /* HDU1 image extension 2×2 float, EXTNAME='SCI' */
    fits_create_img(fp, FLOAT_IMG, 2, n2, &s); CHECK(s);
    char sci[] = "SCI";
    fits_update_key(fp, TSTRING, "EXTNAME", sci, "Extension name", &s); CHECK(s);

    /* HDU2 image extension 2×2 byte, no EXTNAME */
    fits_create_img(fp, BYTE_IMG, 2, n2, &s); CHECK(s);

    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

static int build_edge_keywords(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);
    long n[2] = {2, 2};
    fits_create_img(fp, SHORT_IMG, 2, n, &s); CHECK(s);

    /* HIERARCH keyword (long name) */
    double hv = 6.022e23;
    fits_update_key(fp, TDOUBLE,
                    "HIERARCH ESO INS TEMP", &hv, "ESO instrument temp", &s); CHECK(s);

    /* CONTINUE: a string longer than 68 chars. CFITSIO writes CONTINUE cards
     * automatically when using the long-string convention. */
    char longstr[] = "This string is intentionally longer than the 68-char "
                      "limit so that CFITSIO writes CONTINUE cards. Reading "
                      "it back must reassemble the parts.";
    fits_write_key_longwarn(fp, &s); CHECK(s);  /* enable long-string convention */
    fits_update_key_longstr(fp, "MYLONG", longstr, "long string", &s); CHECK(s);

    /* COMMENT records — three of them in a row. */
    fits_write_comment(fp, "First comment line", &s); CHECK(s);
    fits_write_comment(fp, "Second comment line", &s); CHECK(s);
    fits_write_comment(fp, "Third comment line", &s); CHECK(s);

    /* HISTORY records — two of them. */
    fits_write_history(fp, "Created by build_fixtures", &s); CHECK(s);
    fits_write_history(fp, "For fits-hdf5-vol M2.7 test corpus", &s); CHECK(s);

    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

/* One image HDU per BITPIX. Each is a 2x3 (rows x cols) grid where
 * pixel[r,c] = BASE + r*10 + c. The BASE constant is unique per HDU so
 * a wrong-HDU read is detectable.
 *
 * HDU layout (0-based):
 *   HDU0  BITPIX=8     base=100   uint8
 *   HDU1  BITPIX=16    base=200   int16
 *   HDU2  BITPIX=32    base=300   int32
 *   HDU3  BITPIX=64    base=400   int64
 *   HDU4  BITPIX=-32   base=500   float32
 *   HDU5  BITPIX=-64   base=600   float64
 */
#define MX_ROWS 2
#define MX_COLS 3
#define MX_N    (MX_ROWS * MX_COLS)

static int build_bitpix_matrix(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);

    long naxes[2] = {MX_COLS, MX_ROWS};   /* FITS Fortran-order: NAXIS1=cols */
    long fpixel[2] = {1, 1};

    /* HDU0 BITPIX=8 */
    fits_create_img(fp, BYTE_IMG, 2, naxes, &s); CHECK(s);
    {
        unsigned char arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = (unsigned char)(100 + r*10 + c);
        fits_write_pix(fp, TBYTE, fpixel, MX_N, arr, &s); CHECK(s);
    }

    /* HDU1 BITPIX=16 */
    fits_create_img(fp, SHORT_IMG, 2, naxes, &s); CHECK(s);
    {
        short arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = (short)(200 + r*10 + c);
        fits_write_pix(fp, TSHORT, fpixel, MX_N, arr, &s); CHECK(s);
    }

    /* HDU2 BITPIX=32 */
    fits_create_img(fp, LONG_IMG, 2, naxes, &s); CHECK(s);
    {
        int arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = 300 + r*10 + c;
        fits_write_pix(fp, TINT, fpixel, MX_N, arr, &s); CHECK(s);
    }

    /* HDU3 BITPIX=64 */
    fits_create_img(fp, LONGLONG_IMG, 2, naxes, &s); CHECK(s);
    {
        long long arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = (long long)(400 + r*10 + c);
        fits_write_pix(fp, TLONGLONG, fpixel, MX_N, arr, &s); CHECK(s);
    }

    /* HDU4 BITPIX=-32 */
    fits_create_img(fp, FLOAT_IMG, 2, naxes, &s); CHECK(s);
    {
        float arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = 500.0f + (float)(r*10 + c);
        fits_write_pix(fp, TFLOAT, fpixel, MX_N, arr, &s); CHECK(s);
    }

    /* HDU5 BITPIX=-64 */
    fits_create_img(fp, DOUBLE_IMG, 2, naxes, &s); CHECK(s);
    {
        double arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = 600.0 + (double)(r*10 + c);
        fits_write_pix(fp, TDOUBLE, fpixel, MX_N, arr, &s); CHECK(s);
    }

    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

/* Three image HDUs exercising the FITS unsigned-int convention via BZERO.
 *  HDU0 BITPIX=16, BZERO=32768                  → uint16 base=60000
 *  HDU1 BITPIX=32, BZERO=2147483648             → uint32 base=4000000000
 *  HDU2 BITPIX=64, BZERO=9223372036854775808    → uint64 base=18000000000000000000
 *
 * Bases are chosen above the corresponding signed maximum so a missing
 * unsigned-aware path (interpreting them as signed) would surface as
 * negative values during the readback test. */
static int build_unsigned_bzero(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);

    long naxes[2] = {MX_COLS, MX_ROWS};
    long fpixel[2] = {1, 1};

    /* HDU0 uint16. CFITSIO requires BZERO/BSCALE to be set explicitly before
     * the first unsigned write — fits_set_bscale informs the scaling path,
     * fits_update_key writes the actual header keywords the file will carry.
     * (Without these, fits_write_pix(TUSHORT) returns FITSIO 412.) */
    fits_create_img(fp, SHORT_IMG, 2, naxes, &s); CHECK(s);
    {
        double bz = 32768.0, bs = 1.0;
        fits_update_key(fp, TDOUBLE, "BZERO",  &bz, NULL, &s); CHECK(s);
        fits_update_key(fp, TDOUBLE, "BSCALE", &bs, NULL, &s); CHECK(s);
        fits_set_bscale(fp, bs, bz, &s); CHECK(s);

        unsigned short arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = (unsigned short)(60000u + r*10 + c);
        fits_write_pix(fp, TUSHORT, fpixel, MX_N, arr, &s); CHECK(s);
    }

    /* HDU1 uint32 — same dance with BZERO=2^31. */
    fits_create_img(fp, LONG_IMG, 2, naxes, &s); CHECK(s);
    {
        double bz = 2147483648.0, bs = 1.0;
        fits_update_key(fp, TDOUBLE, "BZERO",  &bz, NULL, &s); CHECK(s);
        fits_update_key(fp, TDOUBLE, "BSCALE", &bs, NULL, &s); CHECK(s);
        fits_set_bscale(fp, bs, bz, &s); CHECK(s);

        unsigned int arr[MX_N];
        for (int r = 0; r < MX_ROWS; ++r) for (int c = 0; c < MX_COLS; ++c)
            arr[r*MX_COLS + c] = 4000000000u + (unsigned)(r*10 + c);
        fits_write_pix(fp, TUINT, fpixel, MX_N, arr, &s); CHECK(s);
    }

    /* uint64 case (BITPIX=64, BZERO=2^63) is intentionally NOT exercised here.
     * CFITSIO 4.3 (Ubuntu package) returns FITSIO 412 on TULONGLONG writes
     * with the auto-BZERO path; this is a CFITSIO limitation, not an adapter
     * gap. The adapter's uint64 detection in adapter_dataset_type still fires
     * if a real archive provides such a file. Tracked for v1 hardening. */

    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

/* General BSCALE/BZERO rescale: BITPIX=16 with BSCALE=0.5, BZERO=10.
 * Raw int16 values raw[i] = 2*i + 2 ; physical = 0.5*raw + 10 = i + 11.
 * Plan §7.2: dataset reports float64 when scaling is non-trivial. */
static int build_scaled_image(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);

    long naxes[2] = {MX_COLS, MX_ROWS};
    long fpixel[2] = {1, 1};
    fits_create_img(fp, SHORT_IMG, 2, naxes, &s); CHECK(s);

    /* Write raw values: disable in-memory scaling so values land on disk
     * verbatim, then set the file's BSCALE/BZERO keywords to the rescale
     * the reader should see. */
    fits_set_bscale(fp, 1.0, 0.0, &s); CHECK(s);
    short raw[MX_N];
    for (int i = 0; i < MX_N; ++i) raw[i] = (short)(2 * i + 2);
    fits_write_pix(fp, TSHORT, fpixel, MX_N, raw, &s); CHECK(s);

    double bscale = 0.5, bzero = 10.0;
    fits_update_key(fp, TDOUBLE, "BSCALE", &bscale, NULL, &s); CHECK(s);
    fits_update_key(fp, TDOUBLE, "BZERO",  &bzero,  NULL, &s); CHECK(s);

    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

/* Binary table with multi-element cells:
 *   col1 "vec3" TFORM='3J'  (1-D length 3, no TDIM)
 *   col2 "mat22" TFORM='4J' + TDIM='(2,2)'  (shape 2x2)
 * 5 rows, deterministic content for differential testing. */
static int build_table_multidim(const char *path)
{
    int s = 0; fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);

    /* Empty primary. */
    long no[1] = {0};
    fits_create_img(fp, BYTE_IMG, 0, no, &s); CHECK(s);

    char *ttype[]  = {"vec3", "mat22"};
    char *tform[]  = {"3J",  "4J"};
    char *tunit[]  = {"",     ""};
    fits_create_tbl(fp, BINARY_TBL, /*nrows*/0, 2, ttype, tform, tunit,
                    "M47", &s); CHECK(s);

    char tdim[] = "(2,2)";
    fits_update_key(fp, TSTRING, "TDIM2", tdim, NULL, &s); CHECK(s);

    /* Write 5 rows. vec3[r] = {r*10+0, r*10+1, r*10+2};
     * mat22[r] = flat {r*100+0, r*100+1, r*100+2, r*100+3}. */
    int vec3[5][3];
    int mat22[5][4];
    for (int r = 0; r < 5; ++r) {
        for (int k = 0; k < 3; ++k) vec3 [r][k] = r * 10  + k;
        for (int k = 0; k < 4; ++k) mat22[r][k] = r * 100 + k;
    }
    fits_write_col(fp, TINT, 1, 1, 1, 15, vec3,  &s); CHECK(s);
    fits_write_col(fp, TINT, 2, 1, 1, 20, mat22, &s); CHECK(s);

    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

/* Binary table with one variable-length array column.
 *  col1 "vlen_int" TFORM='PI'   (vlen of int16)
 * 4 rows; row r holds (r+1) consecutive int16 values starting at r*100. */
static int build_table_vlen(const char *path)
{
    int s = 0; fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);

    long no[1] = {0};
    fits_create_img(fp, BYTE_IMG, 0, no, &s); CHECK(s);

    char *ttype[] = {"vlen_int"};
    char *tform[] = {"1PI"};
    char *tunit[] = {""};
    fits_create_tbl(fp, BINARY_TBL, 0, 1, ttype, tform, tunit, "M48", &s); CHECK(s);

    for (int r = 0; r < 4; ++r) {
        int n = r + 1;
        short v[8];
        for (int i = 0; i < n; ++i) v[i] = (short)(r * 100 + i);
        fits_write_col(fp, TSHORT, /*colnum*/1, /*firstrow*/r + 1,
                       /*firstelem*/1, n, v, &s); CHECK(s);
    }

    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

static int build_complex_keyword(const char *path)
{
    int s = 0;
    fitsfile *fp;
    char buf[1024]; snprintf(buf, sizeof(buf), "!%s", path);
    fits_create_file(&fp, buf, &s); CHECK(s);
    long n[2] = {2, 2};
    fits_create_img(fp, SHORT_IMG, 2, n, &s); CHECK(s);
    /* Plan §7.5 complex: pair of doubles → HDF5 compound {re, im}. */
    double cplx[2] = {1.5, -2.25};
    fits_write_key_dblcmp(fp, "MYCMPLX", cplx, 6, "complex test", &s); CHECK(s);
    fits_close_file(fp, &s); CHECK(s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    const char *dir = argv[1];

    char p[1024];
    int rc = 0;
    snprintf(p, sizeof(p), "%s/primary_only.fits",   dir); rc |= build_primary_only(p);
    snprintf(p, sizeof(p), "%s/image_2d.fits",       dir); rc |= build_image_2d(p);
    snprintf(p, sizeof(p), "%s/multi_hdu.fits",      dir); rc |= build_multi_hdu(p);
    snprintf(p, sizeof(p), "%s/edge_keywords.fits",  dir); rc |= build_edge_keywords(p);
    snprintf(p, sizeof(p), "%s/complex_keyword.fits", dir); rc |= build_complex_keyword(p);
    snprintf(p, sizeof(p), "%s/bitpix_matrix.fits",   dir); rc |= build_bitpix_matrix(p);
    snprintf(p, sizeof(p), "%s/unsigned_bzero.fits",  dir); rc |= build_unsigned_bzero(p);
    snprintf(p, sizeof(p), "%s/scaled_image.fits",    dir); rc |= build_scaled_image(p);
    snprintf(p, sizeof(p), "%s/table_multidim.fits",  dir); rc |= build_table_multidim(p);
    snprintf(p, sizeof(p), "%s/table_vlen.fits",      dir); rc |= build_table_vlen(p);
    if (rc == 0) printf("OK: 10 fixtures emitted in %s\n", dir);
    return rc;
}
