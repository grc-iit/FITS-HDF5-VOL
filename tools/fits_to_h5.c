/* Convert FITS → native HDF5 using fits-hdf5-vol on input, native VOL on output.
 * Demonstrates the "save FITS as HDF5" workflow with the current capabilities. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hdf5.h>

static herr_t copy_attr(hid_t loc, const char *name, const H5A_info_t *info, void *user)
{
    (void)info;
    hid_t dst = *(hid_t *)user;
    hid_t a_src = H5Aopen(loc, name, H5P_DEFAULT);
    hid_t t = H5Aget_type(a_src);
    hid_t s = H5Aget_space(a_src);
    /* Allocate buffer of correct size and read. */
    size_t tsz = H5Tget_size(t);
    hssize_t n = H5Sget_simple_extent_npoints(s);
    if (n <= 0) n = 1;
    void *buf = calloc((size_t)n, tsz);
    if (H5T_STRING == H5Tget_class(t) && H5Tis_variable_str(t)) {
        /* vlen string: buf is char* per element */
        H5Aread(a_src, t, buf);
    } else {
        H5Aread(a_src, t, buf);
    }
    /* Create on destination and write. */
    hid_t a_dst = H5Acreate2(dst, name, t, s, H5P_DEFAULT, H5P_DEFAULT);
    if (a_dst >= 0) {
        H5Awrite(a_dst, t, buf);
        H5Aclose(a_dst);
    }
    free(buf);
    H5Tclose(t); H5Sclose(s); H5Aclose(a_src);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <src.fits> <dst.h5>\n", argv[0]); return 2; }

    /* Source FAPL via fits-hdf5-vol */
    hid_t vol = H5VLregister_connector_by_name("fits", H5P_DEFAULT);
    hid_t src_fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_vol(src_fapl, vol, NULL);

    /* Destination FAPL: leave on native VOL (default) */
    hid_t dst_fapl = H5Pcreate(H5P_FILE_ACCESS);

    hid_t src = H5Fopen(argv[1], H5F_ACC_RDONLY, src_fapl);
    if (src < 0) { fprintf(stderr, "open src failed\n"); return 1; }
    hid_t dst = H5Fcreate(argv[2], H5F_ACC_TRUNC, H5P_DEFAULT, dst_fapl);
    if (dst < 0) { fprintf(stderr, "create dst failed\n"); return 1; }

    /* Copy root attrs */
    hid_t src_root = H5Gopen2(src, "/", H5P_DEFAULT);
    hid_t dst_root = H5Gopen2(dst, "/", H5P_DEFAULT);
    hsize_t idx0 = 0;
    H5Aiterate2(src_root, H5_INDEX_NAME, H5_ITER_INC, &idx0, copy_attr, &dst_root);
    H5Gclose(src_root); H5Gclose(dst_root);

    /* Walk HDU0..HDU99, copying group attrs and (if present) /HDUn/data. */
    int datasets_copied = 0;
    int groups_copied = 0;
    for (int i = 0; i < 100; ++i) {
        char hpath[16]; snprintf(hpath, sizeof(hpath), "/HDU%d", i);
        if (H5Lexists(src, hpath, H5P_DEFAULT) <= 0) break;

        hid_t s_grp = H5Gopen2(src, hpath, H5P_DEFAULT);
        hid_t d_grp = H5Gcreate2(dst, hpath, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        hsize_t idx = 0;
        H5Aiterate2(s_grp, H5_INDEX_NAME, H5_ITER_INC, &idx, copy_attr, &d_grp);
        ++groups_copied;

        /* Copy data dataset if present. */
        char dpath[32]; snprintf(dpath, sizeof(dpath), "%s/data", hpath);
        if (H5Lexists(src, dpath, H5P_DEFAULT) > 0) {
            hid_t s_d = H5Dopen2(src, dpath, H5P_DEFAULT);
            hid_t s_t = H5Dget_type(s_d);
            hid_t s_s = H5Dget_space(s_d);
            hssize_t nelem = H5Sget_simple_extent_npoints(s_s);
            void *buf = malloc((size_t)nelem * H5Tget_size(s_t));

            H5E_auto2_t fn; void *fd;
            H5Eget_auto2(H5E_DEFAULT, &fn, &fd);
            H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
            herr_t rd = H5Dread(s_d, s_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
            H5Eset_auto2(H5E_DEFAULT, fn, fd);

            if (rd >= 0) {
                hid_t d_d = H5Dcreate2(dst, dpath, s_t, s_s,
                                       H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
                H5Dwrite(d_d, s_t, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
                /* Copy any per-dataset attrs. */
                hsize_t dattr = 0;
                H5Aiterate2(s_d, H5_INDEX_NAME, H5_ITER_INC, &dattr, copy_attr, &d_d);
                H5Dclose(d_d);
                ++datasets_copied;
            } else {
                fprintf(stderr, "  %s: read rejected (compressed?), skipped\n", dpath);
            }
            free(buf);
            H5Sclose(s_s); H5Tclose(s_t); H5Dclose(s_d);
        }
        H5Gclose(s_grp); H5Gclose(d_grp);
    }

    H5Fclose(src); H5Fclose(dst);
    H5Pclose(src_fapl); H5Pclose(dst_fapl); H5VLclose(vol);
    printf("OK: %d groups, %d datasets copied\n", groups_copied, datasets_copied);
    return 0;
}
