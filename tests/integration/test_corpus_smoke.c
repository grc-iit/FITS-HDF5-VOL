/* M2 corpus smoke: structural traversal of a real FITS file via sciio-vol.
 *
 * Asserts:
 *   - H5Fopen succeeds (or fails, depending on argv[2])
 *   - H5Lvisit2 walks every link cleanly
 *   - For each group reached, every attribute in turn is opened, its space
 *     and type queried, its value read, and closed without error
 *
 * Does NOT assert pixel content (datasets land in M3) or table cells (M4). */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>

#include "sciio/sciio_vol.h"

static int verbose = 0;

static herr_t attr_visit(hid_t loc, const char *name, const H5A_info_t *ai, void *user)
{
    (void)ai;
    int *errs = (int *)user;
    hid_t aid = H5Aopen(loc, name, H5P_DEFAULT);
    if (aid < 0) { fprintf(stderr, "  attr_open failed: %s\n", name); ++*errs; return 0; }

    hid_t tid = H5Aget_type(aid);
    hid_t sid = H5Aget_space(aid);
    if (tid < 0 || sid < 0) {
        fprintf(stderr, "  attr_get_type/space failed: %s\n", name);
        ++*errs;
        if (tid >= 0) H5Tclose(tid);
        if (sid >= 0) H5Sclose(sid);
        H5Aclose(aid);
        return 0;
    }

    /* Allocate a buffer big enough for the value and read it. We don't check
     * the value — we only need the read path to not crash. */
    H5T_class_t cls = H5Tget_class(tid);
    if (cls == H5T_STRING && H5Tis_variable_str(tid)) {
        int rank = H5Sget_simple_extent_ndims(sid);
        size_t n_elems = 1;
        if (rank > 0) {
            hsize_t dims[8];
            H5Sget_simple_extent_dims(sid, dims, NULL);
            for (int i = 0; i < rank; ++i) n_elems *= (size_t)dims[i];
        }
        char **bufs = calloc(n_elems, sizeof(char *));
        if (H5Aread(aid, tid, bufs) < 0) { fprintf(stderr, "  attr_read failed: %s\n", name); ++*errs; }
        else for (size_t i = 0; i < n_elems; ++i) free(bufs[i]);
        free(bufs);
    } else {
        size_t sz = H5Tget_size(tid);
        size_t n = (size_t)H5Sget_simple_extent_npoints(sid);
        if (n == 0) n = 1;
        void *buf = malloc(sz * n);
        if (H5Aread(aid, tid, buf) < 0) { fprintf(stderr, "  attr_read failed: %s\n", name); ++*errs; }
        free(buf);
    }

    H5Tclose(tid); H5Sclose(sid); H5Aclose(aid);
    return 0;
}

typedef struct {
    hid_t fid;
    int   errs;
    int   n_groups;
    int   n_attrs_total;
    int   n_reads_ok;
    int   n_reads_rejected;   /* expected failures, e.g. ZIMAGE */
} walk_ctx_t;

/* Open a dataset link, query space+type, attempt whole-dataset read into a
 * temp buffer. Tile-compressed images (plan §7.6) intentionally fail here. */
static void try_dataset_read(walk_ctx_t *c, const char *path)
{
    hid_t did = H5Dopen2(c->fid, path, H5P_DEFAULT);
    if (did < 0) { fprintf(stderr, "  H5Dopen failed: %s\n", path); ++c->errs; return; }

    hid_t sid = H5Dget_space(did);
    hid_t tid = H5Dget_type(did);
    if (sid < 0 || tid < 0) {
        fprintf(stderr, "  get space/type failed: %s\n", path); ++c->errs;
        if (sid >= 0) H5Sclose(sid); if (tid >= 0) H5Tclose(tid);
        H5Dclose(did); return;
    }

    hssize_t npts = H5Sget_simple_extent_npoints(sid);
    size_t   tsz  = H5Tget_size(tid);
    /* Refuse runaway allocations — corpus files are < 2MB but stay defensive. */
    if (npts <= 0 || (size_t)npts * tsz > 64u * 1024u * 1024u) {
        H5Sclose(sid); H5Tclose(tid); H5Dclose(did); return;
    }

    void *buf = malloc((size_t)npts * tsz);
    if (!buf) {
        H5Sclose(sid); H5Tclose(tid); H5Dclose(did);
        ++c->errs; return;
    }

    /* Suppress HDF5's auto-print for this read; we expect it to fail on
     * compressed-image data and don't want noise in the test output. */
    H5E_auto2_t old_fn; void *old_data;
    H5Eget_auto2(H5E_DEFAULT, &old_fn, &old_data);
    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
    herr_t rc = H5Dread(did, tid, H5S_ALL, H5S_ALL, H5P_DEFAULT, buf);
    H5Eset_auto2(H5E_DEFAULT, old_fn, old_data);

    if (rc < 0) ++c->n_reads_rejected;
    else        ++c->n_reads_ok;

    free(buf);
    H5Sclose(sid); H5Tclose(tid); H5Dclose(did);
}

static herr_t link_visit(hid_t loc, const char *name, const H5L_info2_t *info, void *user)
{
    (void)loc;
    walk_ctx_t *c = (walk_ctx_t *)user;
    if (verbose) fprintf(stderr, "  link: %s (type=%d)\n", name, (int)info->type);

    /* Only follow hard links (groups for us); soft links pointing into the
     * same tree would double-visit. */
    if (info->type != H5L_TYPE_HARD) return 0;

    /* Open object as a group. If it isn't a group (won't happen in M2 since we
     * surface only groups), we just skip. */
    H5O_info2_t oi;
    if (H5Oget_info_by_name3(c->fid, name, &oi, H5O_INFO_BASIC, H5P_DEFAULT) < 0) {
        fprintf(stderr, "  H5Oget_info_by_name3 failed: %s\n", name);
        ++c->errs;
        return 0;
    }
    if (oi.type == H5O_TYPE_DATASET) {
        try_dataset_read(c, name);
        return 0;
    }
    if (oi.type != H5O_TYPE_GROUP) return 0;

    hid_t gid = H5Gopen2(c->fid, name, H5P_DEFAULT);
    if (gid < 0) { fprintf(stderr, "  H5Gopen2 failed: %s\n", name); ++c->errs; return 0; }
    ++c->n_groups;

    int before = c->errs;
    hsize_t aidx = 0;
    if (H5Aiterate2(gid, H5_INDEX_NAME, H5_ITER_INC, &aidx, attr_visit, &c->errs) < 0) {
        fprintf(stderr, "  H5Aiterate2 failed: %s\n", name);
        ++c->errs;
    }
    c->n_attrs_total += (int)aidx;
    if (c->errs > before)
        fprintf(stderr, "  (%d new attribute errors in group %s)\n", c->errs - before, name);

    H5Gclose(gid);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s <fits-path> [reject]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int expect_reject = (argc == 3 && strcmp(argv[2], "reject") == 0);
    if (getenv("SCIIO_CORPUS_VERBOSE")) verbose = 1;

    hid_t vol = H5VLregister_connector_by_name(SCIIO_VOL_NAME, H5P_DEFAULT);
    assert(vol >= 0);
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS); H5Pset_vol(fapl, vol, NULL);

    if (expect_reject) {
        H5E_auto2_t old_func; void *old_data;
        H5Eget_auto2(H5E_DEFAULT, &old_func, &old_data);
        H5Eset_auto2(H5E_DEFAULT, NULL, NULL);
        hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
        H5Eset_auto2(H5E_DEFAULT, old_func, old_data);
        if (fid >= 0) {
            fprintf(stderr, "FAIL: %s should have been rejected but H5Fopen succeeded\n", path);
            return 1;
        }
        printf("OK: %s rejected as expected\n", path);
        H5Pclose(fapl); H5VLclose(vol);
        return 0;
    }

    hid_t fid = H5Fopen(path, H5F_ACC_RDONLY, fapl);
    if (fid < 0) {
        H5Eprint2(H5E_DEFAULT, stderr);
        fprintf(stderr, "FAIL: H5Fopen on %s\n", path);
        return 1;
    }

    walk_ctx_t ctx = { .fid = fid };
    /* Visit the root group itself first (H5Lvisit doesn't include the loc). */
    hid_t root = H5Gopen2(fid, "/", H5P_DEFAULT);
    assert(root >= 0);
    H5L_info2_t fake = { .type = H5L_TYPE_HARD };
    link_visit(root, "/", &fake, &ctx);
    H5Gclose(root);

    hsize_t lidx = 0;
    if (H5Lvisit2(fid, H5_INDEX_NAME, H5_ITER_INC, link_visit, &ctx) < 0) {
        H5Eprint2(H5E_DEFAULT, stderr);
        fprintf(stderr, "FAIL: H5Lvisit2 on %s\n", path);
        ctx.errs++;
    }
    H5Fclose(fid); H5Pclose(fapl); H5VLclose(vol);

    if (ctx.errs > 0) {
        fprintf(stderr, "FAIL: %s — %d errors during traversal\n", path, ctx.errs);
        return 1;
    }
    printf("OK: %s — %d groups, %d attrs, %d reads ok, %d reads rejected, no errors\n",
           path, ctx.n_groups, ctx.n_attrs_total,
           ctx.n_reads_ok, ctx.n_reads_rejected);
    return 0;
}
