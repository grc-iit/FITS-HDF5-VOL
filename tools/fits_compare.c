/* Compare two FITS files HDU-by-HDU: dims + pixel content. Headers may
 * differ in writer-injected COMMENT records, so we don't strict-compare
 * the full header — just the data and shape. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fitsio.h>

#define CFITS(s) do { if (s) { fits_report_error(stderr, s); exit(1); } } while (0)

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <a.fits> <b.fits>\n", argv[0]); return 2;
    }
    int sa = 0, sb = 0;
    fitsfile *a, *b;
    fits_open_file(&a, argv[1], READONLY, &sa); CFITS(sa);
    fits_open_file(&b, argv[2], READONLY, &sb); CFITS(sb);

    int na = 0, nb = 0;
    fits_get_num_hdus(a, &na, &sa); CFITS(sa);
    fits_get_num_hdus(b, &nb, &sb); CFITS(sb);
    if (na != nb) {
        printf("FAIL: HDU count differs: %d vs %d\n", na, nb);
        return 1;
    }

    int total_diffs = 0;
    for (int h = 1; h <= na; ++h) {
        int ta = 0, tb = 0;
        fits_movabs_hdu(a, h, &ta, &sa); CFITS(sa);
        fits_movabs_hdu(b, h, &tb, &sb); CFITS(sb);

        int rk_a = 0, rk_b = 0;
        fits_get_img_dim(a, &rk_a, &sa);
        fits_get_img_dim(b, &rk_b, &sb);
        long da[8] = {0}, db[8] = {0};
        if (rk_a > 0) fits_get_img_size(a, rk_a, da, &sa);
        if (rk_b > 0) fits_get_img_size(b, rk_b, db, &sb);

        int bp_a = 0, bp_b = 0;
        fits_get_img_type(a, &bp_a, &sa);
        fits_get_img_type(b, &bp_b, &sb);

        printf("HDU%d: rank %d|%d  bitpix %d|%d  dims [", h-1, rk_a, rk_b, bp_a, bp_b);
        for (int k = 0; k < rk_a; ++k) printf(" %ld", da[k]);
        printf(" ] | [");
        for (int k = 0; k < rk_b; ++k) printf(" %ld", db[k]);
        printf(" ]");

        if (rk_a != rk_b || bp_a != bp_b) {
            printf("  → SHAPE/TYPE DIFFER\n");
            ++total_diffs;
            continue;
        }
        for (int k = 0; k < rk_a; ++k) if (da[k] != db[k]) {
            printf("  → DIM DIFFER\n"); ++total_diffs; goto next;
        }
        if (rk_a == 0) { printf("  (header-only)\n"); continue; }

        /* Read both as bytes and compare. */
        size_t nel = 1;
        for (int k = 0; k < rk_a; ++k) nel *= (size_t)da[k];
        size_t bytes_per = (size_t)((bp_a < 0 ? -bp_a : bp_a) / 8);
        size_t total = nel * bytes_per;
        unsigned char *ba = malloc(total);
        unsigned char *bb = malloc(total);
        long fpixel[8] = {1,1,1,1,1,1,1,1};

        /* Read with the dtype that matches BITPIX exactly. Disable scaling
         * so we compare raw stored values. */
        fits_set_bscale(a, 1.0, 0.0, &sa);
        fits_set_bscale(b, 1.0, 0.0, &sb);
        int dt_read;
        switch (bp_a) {
            case BYTE_IMG:     dt_read = TBYTE;     break;
            case SBYTE_IMG:    dt_read = TSBYTE;    break;
            case SHORT_IMG:    dt_read = TSHORT;    break;
            case LONG_IMG:     dt_read = TINT;      break;
            case LONGLONG_IMG: dt_read = TLONGLONG; break;
            case FLOAT_IMG:    dt_read = TFLOAT;    break;
            case DOUBLE_IMG:   dt_read = TDOUBLE;   break;
            default:
                printf("  unsupported BITPIX=%d\n", bp_a);
                free(ba); free(bb);
                ++total_diffs;
                goto next;
        }
        fits_read_pix(a, dt_read, fpixel, nel, NULL, ba, NULL, &sa);
        fits_read_pix(b, dt_read, fpixel, nel, NULL, bb, NULL, &sb);

        int diff = memcmp(ba, bb, total);
        if (diff == 0) printf("  ✓ pixels match (%zu bytes)\n", total);
        else { printf("  ✗ PIXELS DIFFER\n"); ++total_diffs; }
        free(ba); free(bb);
next: ;
    }

    fits_close_file(a, &sa);
    fits_close_file(b, &sb);
    return total_diffs == 0 ? 0 : 1;
}
