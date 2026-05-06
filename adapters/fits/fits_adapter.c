/*
 * fits-hdf5-vol FITS adapter (M2.3 surface).
 *
 * Implements the slice of the Format-Adapter API needed for HDU enumeration:
 * probe + open/close + root/object_kind. Group iteration, attributes, and
 * dataset I/O are added in M2.5/M2.6/M3 respectively.
 *
 * CFITSIO HDU numbering note: CFITSIO is 1-based (Primary HDU == 1) but the
 * fits model and are 0-based ("HDU0" == Primary). This file stores
 * the index 0-based and translates at the CFITSIO boundary.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fitsio.h>

#include "fits_hdf5/adapter.h"

/* ------------------------------------------------------------------ */
/* Per-HDU descriptor                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char           *name;        /* keyword name, owned */
    adapter_type_t  type;
    int             rank;        /* 0 = scalar, 1 = STRING array (e.g. __raw_header__) */
    size_t          n_elements;  /* used when rank == 1 */
    /* Storage:
     *  INT   -> int64_t in scalar.i64
     *  FLOAT -> double  in scalar.f64
     *  BOOL  -> uint8_t in scalar.u8
     *  STRING scalar  -> heap-allocated NUL-terminated string in str
     *  STRING rank-1  -> heap-allocated array of n_elements strings in strs
     */
    union {
        int64_t i64;
        double  f64;
        uint8_t u8;
        double  cplx[2];   /* {re, im} for ADAPTER_T_COMPLEX */
    } scalar;
    char           *str;         /* owned (STRING scalar) */
    char          **strs;        /* owned (STRING rank-1) */
} fits_attr_t;

/* Per-column metadata for a table HDU (M4). */
typedef struct {
    char           *name;        /* TTYPEn, owned */
    char           *unit;        /* TUNITn, owned; NULL if absent */
    int             cf_typecode; /* CFITSIO type code; negative => VLEN (M4.8) */
    long            repeat;      /* elements per cell (1 = scalar) */
    long            width;       /* bytes per element (chars for TSTRING) */
    adapter_type_t  scalar_type; /* HDF5-flavored type of one element */
} fits_col_t;

typedef struct {
    int   index;        /* 0-based; CFITSIO HDU number = index + 1 */
    int   hdu_type;     /* CFITSIO: IMAGE_HDU, ASCII_TBL, BINARY_TBL */
    int   compressed;   /* nonzero if tile-compressed image */
    char  group_name[16];/* "HDU0", "HDU1", ...; max 6 digits is plenty */
    char *extname;      /* malloc'd; NULL if absent */
    /* Lazy-parsed attribute cache; populated on first attribute access. */
    int   attrs_loaded;
    int   n_attrs;
    fits_attr_t *attrs;
    /* Table-only fields. n_cols = 0 unless this HDU is ASCII_TBL or BINARY_TBL. */
    long          n_rows;
    int           n_cols;
    fits_col_t   *cols;
    /* Lazily-built compound info for the row-view "table" dataset. NULL until
     * the first call to dataset_type on AO_TABLE_DATA. */
    adapter_compound_member_t *cmp_members;
    adapter_compound_info_t    cmp_info;
} fits_hdu_desc_t;

/* ------------------------------------------------------------------ */
/* Concrete adapter handle types — opaque outside this TU              */
/* ------------------------------------------------------------------ */

/* EXTNAME alias: when an HDU has a unique, valid EXTNAME we surface it as a
 * soft link "/<EXTNAME>" pointing to "/HDU<n>". */
typedef struct {
    char *name;          /* owned, e.g. "SCI" */
    char *target;        /* owned, e.g. "/HDU1" */
    int   hdu_index;     /* 0-based */
} fits_alias_t;

struct adapter_file_s {
    fitsfile        *fp;
    int              n_hdus;
    fits_hdu_desc_t *hdus;
    adapter_object_t *root;
    int              n_aliases;
    fits_alias_t    *aliases;
};

/* Internal sub-kind: distinguishes the synthetic groups and the flavors
 * of dataset (image data, table column view, table row view). HDF5 sees
 * only `kind` (GROUP or DATASET); adapter dispatch keys off `sub`. */
typedef enum {
    AO_ROOT,            /* the file root, "/" */
    AO_HDU_GROUP,       /* /HDUn (image, ASCII table, or binary table) */
    AO_COLUMNS_GROUP,   /* /HDUn/columns synthetic subgroup (table HDUs) */
    AO_IMAGE_DATA,      /* /HDUn/data on an image HDU */
    AO_COLUMN_DATA,     /* /HDUn/columns/<TTYPE> on a table HDU */
    AO_TABLE_DATA       /* /HDUn/table compound row view */
} ao_sub_t;

struct adapter_object_s {
    adapter_kind_t   kind;         /* GROUP or DATASET */
    ao_sub_t         sub;
    adapter_file_t  *file;
    int              hdu_index;    /* -1 only when sub=AO_ROOT */
    int              col_index;    /* valid only when sub=AO_COLUMN_DATA */
};

/* ------------------------------------------------------------------ */
/* Probe                                                               */
/* ------------------------------------------------------------------ */

static int fits_probe(const char *path, adapter_probe_result_t *out)
{
    if (!path || !out) return -1;
    out->name = "fits";
    out->confidence = 0;

    FILE *fh = fopen(path, "rb");
    if (!fh) return -1;

    /* The first 80-char card of any FITS file is "SIMPLE  =                    T..." */
    char head[80] = {0};
    size_t n = fread(head, 1, sizeof(head), fh);
    fclose(fh);
    if (n < 30) return 0;  /* too short to be FITS */

    if (memcmp(head, "SIMPLE  =", 9) == 0) {
        /* Look for the boolean T or F in the value field (cols 11..30) */
        for (int i = 10; i < 30; ++i) {
            if (head[i] == 'T') { out->confidence = 100; return 0; }
            if (head[i] == 'F') { out->confidence = 100; return 0; }
        }
        out->confidence = 50;  /* SIMPLE present but value malformed */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Open / close                                                        */
/* ------------------------------------------------------------------ */

static void fits_attr_destroy(fits_attr_t *a)
{
    free(a->name);
    free(a->str);
    if (a->strs) {
        for (size_t i = 0; i < a->n_elements; ++i) free(a->strs[i]);
        free(a->strs);
    }
}

static void fits_col_destroy(fits_col_t *c)
{
    free(c->name);
    free(c->unit);
}

static void hdu_desc_destroy(fits_hdu_desc_t *d)
{
    free(d->extname);
    if (d->attrs) {
        for (int i = 0; i < d->n_attrs; ++i) fits_attr_destroy(&d->attrs[i]);
        free(d->attrs);
    }
    if (d->cols) {
        for (int i = 0; i < d->n_cols; ++i) fits_col_destroy(&d->cols[i]);
        free(d->cols);
    }
    free(d->cmp_members);
}

/* Map a CFITSIO column type code (from fits_get_eqcoltype) to an adapter
 * scalar type. CFITSIO's `width` semantics differ between table flavors:
 *   - BINARY_TBL: width = bytes per element
 *   - ASCII_TBL : width = character count of the field
 * For numeric types we therefore hardcode HDF5-native sizes; for TSTRING
 * width is the character count which equals byte count for fixed strings. */
static int cfitsio_col_to_adapter(int cf_typecode, long width, int hdu_type,
                                   adapter_type_t *out)
{
    (void)hdu_type;  /* same fixed mapping works for both flavors today */
    int abs_tc = cf_typecode < 0 ? -cf_typecode : cf_typecode;
    switch (abs_tc) {
        case TBYTE:      out->cls = ADAPTER_T_UINT;   out->size = 1; return 0;
        case TSBYTE:     out->cls = ADAPTER_T_INT;    out->size = 1; return 0;
        case TLOGICAL:   out->cls = ADAPTER_T_BOOL;   out->size = 1; return 0;
        case TSHORT:     out->cls = ADAPTER_T_INT;    out->size = 2; return 0;
        case TUSHORT:    out->cls = ADAPTER_T_UINT;   out->size = 2; return 0;
        case TINT:       out->cls = ADAPTER_T_INT;    out->size = 4; return 0;
        case TUINT:      out->cls = ADAPTER_T_UINT;   out->size = 4; return 0;
        case TLONG:      out->cls = ADAPTER_T_INT;    out->size = 4; return 0;
        case TULONG:     out->cls = ADAPTER_T_UINT;   out->size = 4; return 0;
        case TLONGLONG:  out->cls = ADAPTER_T_INT;    out->size = 8; return 0;
        case TULONGLONG: out->cls = ADAPTER_T_UINT;   out->size = 8; return 0;
        case TFLOAT:     out->cls = ADAPTER_T_FLOAT;  out->size = 4; return 0;
        case TDOUBLE:    out->cls = ADAPTER_T_FLOAT;  out->size = 8; return 0;
        case TSTRING:    out->cls = ADAPTER_T_STRING; out->size = (size_t)width; return 0;
        case TCOMPLEX:   out->cls = ADAPTER_T_COMPLEX; out->size = 8;  return 0;
        case TDBLCOMPLEX:out->cls = ADAPTER_T_COMPLEX; out->size = 16; return 0;
        default:
            return -1;
    }
}

static adapter_file_t *fits_open(const char *path, unsigned flags)
{
    if (flags != 0) return NULL;  /* read-only only */

    int status = 0;
    fitsfile *fp = NULL;
    if (fits_open_file(&fp, path, READONLY, &status) != 0) return NULL;

    int n_hdus = 0;
    if (fits_get_num_hdus(fp, &n_hdus, &status) != 0 || n_hdus < 1) {
        fits_close_file(fp, &status);
        return NULL;
    }

    adapter_file_t *f = calloc(1, sizeof(*f));
    if (!f) { fits_close_file(fp, &status); return NULL; }
    f->fp = fp;
    f->n_hdus = n_hdus;
    f->hdus = calloc((size_t)n_hdus, sizeof(*f->hdus));
    if (!f->hdus) goto fail;

    for (int i = 0; i < n_hdus; ++i) {
        int hdu_type = 0;
        status = 0;
        if (fits_movabs_hdu(fp, i + 1, &hdu_type, &status) != 0) goto fail;

        /* Plan §7.6: Random Groups HDUs are out of scope for v1. */
        int groups = 0;
        int s2 = 0;
        if (fits_read_key_log(fp, "GROUPS", &groups, NULL, &s2) == 0 && groups) {
            fprintf(stderr,
                "fits-hdf5-vol: HDU %d is a Random Groups HDU, which is not "
                "supported in v1. \n", i);
            goto fail;
        }

        f->hdus[i].index = i;
        f->hdus[i].hdu_type = hdu_type;
        snprintf(f->hdus[i].group_name, sizeof(f->hdus[i].group_name), "HDU%d", i);

        /* Plan §7.6: tile-compressed images surface for introspection but
         * H5Dread is rejected. CFITSIO presents the decompressed view by
         * default — fits_is_compressed_image returns 1 when this HDU is one. */
        int s2c = 0;
        f->hdus[i].compressed = fits_is_compressed_image(f->fp, &s2c);

        /* Table HDU metadata cache (M4.1). Populated only for ASCII_TBL /
         * BINARY_TBL. We log + skip individual columns we can't classify;
         * the HDU still surfaces with the rest of its columns. */
        if (hdu_type == ASCII_TBL || hdu_type == BINARY_TBL) {
            int ncols = 0; long nrows = 0; int s2 = 0;
            fits_get_num_cols(fp, &ncols, &s2);
            fits_get_num_rows(fp, &nrows, &s2);
            f->hdus[i].n_rows = nrows;
            f->hdus[i].n_cols = ncols;
            if (ncols > 0) {
                f->hdus[i].cols = calloc((size_t)ncols, sizeof(fits_col_t));
                for (int c = 1; c <= ncols; ++c) {
                    fits_col_t *col = &f->hdus[i].cols[c - 1];
                    char val[FLEN_VALUE]; int s3 = 0;

                    char ttype_kw[16]; snprintf(ttype_kw, sizeof(ttype_kw), "TTYPE%d", c);
                    if (fits_read_key(fp, TSTRING, ttype_kw, val, NULL, &s3) == 0) {
                        col->name = strdup(val);
                        /* '/' is the HDF5 path separator; sanitize it so column
                         * link names like "[Fe/H]" don't corrupt path resolution. */
                        for (char *p = col->name; *p; ++p)
                            if (*p == '/') *p = '_';
                    } else {
                        char tmp[16]; snprintf(tmp, sizeof(tmp), "col%d", c);
                        col->name = strdup(tmp);
                    }

                    s3 = 0;
                    char tunit_kw[16]; snprintf(tunit_kw, sizeof(tunit_kw), "TUNIT%d", c);
                    if (fits_read_key(fp, TSTRING, tunit_kw, val, NULL, &s3) == 0)
                        col->unit = strdup(val);

                    int tc = 0; long repeat = 0, width = 0; s3 = 0;
                    if (fits_get_eqcoltype(fp, c, &tc, &repeat, &width, &s3) != 0) {
                        fprintf(stderr,
                            "fits-hdf5-vol: HDU %d col %d: fits_get_eqcoltype failed (%d)\n",
                            i, c, s3);
                        col->cf_typecode = 0;
                        continue;
                    }
                    col->cf_typecode = tc;
                    col->repeat = repeat;
                    col->width = width;
                    if (cfitsio_col_to_adapter(tc, width, hdu_type, &col->scalar_type) != 0) {
                        fprintf(stderr,
                            "fits-hdf5-vol: HDU %d col '%s': unsupported CFITSIO typecode %d, "
                            "column will not surface\n", i, col->name, tc);
                        col->cf_typecode = 0;
                        continue;
                    }
                    /* CFITSIO returns negative typecodes for VLEN (TFORM 'P'/'Q').
                     * Skip vlen-string for now (TFORM 'PA') — separate handling. */
                    if (tc < 0) {
                        if (col->scalar_type.cls == ADAPTER_T_STRING) {
                            fprintf(stderr,
                                "fits-hdf5-vol: HDU %d col '%s': vlen-string columns "
                                "(TFORM 'PA') deferred; column hidden\n", i, col->name);
                            col->cf_typecode = 0;
                            continue;
                        }
                        col->scalar_type.is_vlen = 1;
                    }
                    /* Multi-element cells: TFORM repeat>1. For
                     * STRING columns (rA), repeat is the string length and
                     * the cell is still scalar. For numeric/bool columns,
                     * we expose each cell as an HDF5 array element shaped by
                     * TDIMn (or a flat [repeat] if TDIMn is absent). */
                    if (repeat > 1 && col->scalar_type.cls != ADAPTER_T_STRING) {
                        int naxis = 0;
                        long naxes[8] = {0};
                        int s4 = 0;
                        fits_read_tdim(fp, c, 8, &naxis, naxes, &s4);
                        if (s4 != 0 || naxis < 1) {
                            naxis = 1;
                            naxes[0] = repeat;
                        }
                        if (naxis > 8) {
                            fprintf(stderr,
                                "fits-hdf5-vol: HDU %d col '%s': TDIM rank %d exceeds 8, "
                                "skipping column\n", i, col->name, naxis);
                            col->cf_typecode = 0;
                            continue;
                        }
                        col->scalar_type.array_rank = naxis;
                        for (int k = 0; k < naxis; ++k)
                            col->scalar_type.array_dims[k] = (uint64_t)naxes[k];
                    }
                }
            }
        }

        char value[FLEN_VALUE] = {0};
        char comment[FLEN_COMMENT] = {0};
        status = 0;
        if (fits_read_keyword(fp, "EXTNAME", value, comment, &status) == 0) {
            /* CFITSIO returns the value with surrounding single quotes */
            char *v = value;
            if (*v == '\'') ++v;
            char *end = strrchr(v, '\'');
            if (end) *end = '\0';
            /* Trim FITS-mandated trailing spaces inside the quoted string */
            for (char *p = v; *p; ++p) {
                if (*p == ' ' && (p[1] == ' ' || p[1] == '\0')) { *p = '\0'; break; }
            }
            if (*v) f->hdus[i].extname = strdup(v);
        }
        /* missing EXTNAME is not an error — clear status */
    }

    /* Build EXTNAME alias map: an EXTNAME becomes a soft link only if
     *  - it's non-empty,
     *  - it doesn't collide with an "HDU<n>" name,
     *  - it's a valid HDF5 link name (no '/' inside),
     *  - it's unique across the file. */
    f->aliases = calloc((size_t)n_hdus, sizeof(*f->aliases));
    if (!f->aliases) goto fail;
    int produced_aliases = 0;
    for (int i = 0; i < n_hdus; ++i) {
        const char *en = f->hdus[i].extname;
        if (!en || !*en) continue;
        if (strchr(en, '/')) continue;
        if (strncmp(en, "HDU", 3) == 0) {
            int rest_is_digits = 1;
            for (const char *p = en + 3; *p; ++p)
                if (*p < '0' || *p > '9') { rest_is_digits = 0; break; }
            if (rest_is_digits) continue;
        }
        /* uniqueness: skip if any earlier alias already used this name */
        int dup = 0;
        for (int j = 0; j < produced_aliases; ++j) {
            if (strcmp(f->aliases[j].name, en) == 0) { dup = 1; break; }
        }
        if (dup) {
            fprintf(stderr,
                "fits-hdf5-vol: HDU %d EXTNAME=\"%s\" collides with an earlier "
                "HDU; skipping soft link.\n", i, en);
            continue;
        }
        fits_alias_t *a = &f->aliases[produced_aliases];
        a->name = strdup(en);
        char tgt[32]; snprintf(tgt, sizeof(tgt), "/HDU%d", i);
        a->target = strdup(tgt);
        a->hdu_index = i;
        if (a->name && a->target) ++produced_aliases;
        else { free(a->name); free(a->target); memset(a, 0, sizeof(*a)); }
    }
    f->n_aliases = produced_aliases;

    f->root = calloc(1, sizeof(*f->root));
    if (!f->root) goto fail;
    f->root->kind = ADAPTER_KIND_GROUP;
    f->root->sub = AO_ROOT;
    f->root->file = f;
    f->root->hdu_index = -1;

    return f;

fail: {
        int s = 0;
        fits_close_file(fp, &s);
        if (f) {
            if (f->hdus) {
                for (int i = 0; i < n_hdus; ++i) hdu_desc_destroy(&f->hdus[i]);
                free(f->hdus);
            }
            free(f->root);
            free(f);
        }
        return NULL;
    }
}

static void fits_close(adapter_file_t *f)
{
    if (!f) return;
    int s = 0;
    if (f->fp) fits_close_file(f->fp, &s);
    if (f->hdus) {
        for (int i = 0; i < f->n_hdus; ++i) hdu_desc_destroy(&f->hdus[i]);
        free(f->hdus);
    }
    if (f->aliases) {
        for (int i = 0; i < f->n_aliases; ++i) {
            free(f->aliases[i].name);
            free(f->aliases[i].target);
        }
        free(f->aliases);
    }
    free(f->root);
    free(f);
}

/* ------------------------------------------------------------------ */
/* Object navigation (M2.3 subset: root + kind)                        */
/* ------------------------------------------------------------------ */

static adapter_object_t *fits_root(adapter_file_t *f)
{
    return f ? f->root : NULL;
}

static adapter_kind_t fits_object_kind(const adapter_object_t *o)
{
    assert(o);
    return o->kind;
}

/* The remaining navigation/attribute/free_string entry points are added in
 * M2.5/M2.6. Linker stubs to keep the API header satisfied for callers that
 * may reference them: */

/* True iff this image HDU has a non-empty data unit (NAXIS>0). */
static int hdu_has_image_data(adapter_file_t *f, int idx)
{
    if (idx < 0 || idx >= f->n_hdus) return 0;
    if (f->hdus[idx].hdu_type != IMAGE_HDU) return 0;
    int status = 0, naxis = 0, t = 0;
    if (fits_movabs_hdu(f->fp, idx + 1, &t, &status) != 0) return 0;
    if (fits_get_img_dim(f->fp, &naxis, &status) != 0) return 0;
    return naxis > 0;
}

static int hdu_is_table(adapter_file_t *f, int idx)
{
    if (idx < 0 || idx >= f->n_hdus) return 0;
    int t = f->hdus[idx].hdu_type;
    return (t == ASCII_TBL || t == BINARY_TBL) && f->hdus[idx].n_cols > 0;
}

/* Find a column index by TTYPE name (case-sensitive). Returns -1 if absent
 * or if the column is unsupported (cf_typecode==0 from M4.1 mapping). */
static int find_column(fits_hdu_desc_t *d, const char *name)
{
    for (int i = 0; i < d->n_cols; ++i) {
        if (d->cols[i].cf_typecode != 0 && strcmp(d->cols[i].name, name) == 0)
            return i;
    }
    return -1;
}

static adapter_object_t *make_aobj(adapter_file_t *f, adapter_kind_t k, ao_sub_t sub,
                                    int hdu_idx, int col_idx)
{
    adapter_object_t *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->kind = k;
    o->sub  = sub;
    o->file = f;
    o->hdu_index = hdu_idx;
    o->col_index = col_idx;
    return o;
}

static adapter_object_t *fits_object_open(adapter_file_t *f, adapter_object_t *parent, const char *name)
{
    if (!f || !parent || !name) return NULL;

    switch (parent->sub) {
    case AO_ROOT: {
        int hdu_index = -1;
        for (int i = 0; i < f->n_hdus; ++i)
            if (strcmp(f->hdus[i].group_name, name) == 0) { hdu_index = i; break; }
        if (hdu_index < 0) {
            for (int i = 0; i < f->n_aliases; ++i)
                if (strcmp(f->aliases[i].name, name) == 0) {
                    hdu_index = f->aliases[i].hdu_index; break;
                }
        }
        if (hdu_index < 0) return NULL;
        return make_aobj(f, ADAPTER_KIND_GROUP, AO_HDU_GROUP, hdu_index, -1);
    }

    case AO_HDU_GROUP: {
        if (strcmp(name, "data") == 0 && hdu_has_image_data(f, parent->hdu_index))
            return make_aobj(f, ADAPTER_KIND_DATASET, AO_IMAGE_DATA, parent->hdu_index, -1);
        if (hdu_is_table(f, parent->hdu_index)) {
            if (strcmp(name, "columns") == 0)
                return make_aobj(f, ADAPTER_KIND_GROUP,   AO_COLUMNS_GROUP, parent->hdu_index, -1);
            if (strcmp(name, "table") == 0)
                return make_aobj(f, ADAPTER_KIND_DATASET, AO_TABLE_DATA,    parent->hdu_index, -1);
        }
        return NULL;
    }

    case AO_COLUMNS_GROUP: {
        int col = find_column(&f->hdus[parent->hdu_index], name);
        if (col < 0) return NULL;
        return make_aobj(f, ADAPTER_KIND_DATASET, AO_COLUMN_DATA, parent->hdu_index, col);
    }

    default:
        return NULL;  /* datasets have no children */
    }
}

static void fits_object_close(adapter_object_t *o) { free(o); }

static int fits_group_iterate(adapter_object_t *g, adapter_link_cb cb, void *user)
{
    if (!g || !cb || g->kind != ADAPTER_KIND_GROUP) return -1;
    adapter_file_t *f = g->file;

    switch (g->sub) {
    case AO_ROOT: {
        for (int i = 0; i < f->n_hdus; ++i) {
            int rc = cb(f->hdus[i].group_name, user);
            if (rc != 0) return rc;
        }
        for (int i = 0; i < f->n_aliases; ++i) {
            int rc = cb(f->aliases[i].name, user);
            if (rc != 0) return rc;
        }
        return 0;
    }
    case AO_HDU_GROUP: {
        if (hdu_has_image_data(f, g->hdu_index)) return cb("data", user);
        if (hdu_is_table(f, g->hdu_index)) {
            int rc = cb("columns", user); if (rc != 0) return rc;
            return cb("table", user);
        }
        return 0;
    }
    case AO_COLUMNS_GROUP: {
        fits_hdu_desc_t *d = &f->hdus[g->hdu_index];
        for (int i = 0; i < d->n_cols; ++i) {
            if (d->cols[i].cf_typecode == 0) continue;   /* skipped in M4.1 */
            int rc = cb(d->cols[i].name, user);
            if (rc != 0) return rc;
        }
        return 0;
    }
    default: return 0;  /* datasets aren't iterable */
    }
}

static int fits_link_info(adapter_object_t *parent, const char *name, adapter_link_info_t *out)
{
    if (!parent || !name || !out) return -1;
    adapter_file_t *f = parent->file;

    switch (parent->sub) {
    case AO_HDU_GROUP:
        if (strcmp(name, "data") == 0 && hdu_has_image_data(f, parent->hdu_index)) {
            out->kind = ADAPTER_LINK_HARD; out->target = NULL; return 0;
        }
        if (hdu_is_table(f, parent->hdu_index) &&
            (strcmp(name, "columns") == 0 || strcmp(name, "table") == 0)) {
            out->kind = ADAPTER_LINK_HARD; out->target = NULL; return 0;
        }
        return -1;
    case AO_COLUMNS_GROUP:
        if (find_column(&f->hdus[parent->hdu_index], name) >= 0) {
            out->kind = ADAPTER_LINK_HARD; out->target = NULL; return 0;
        }
        return -1;
    case AO_ROOT:
        break;
    default:
        return -1;
    }
    /* HDUn → hard */
    for (int i = 0; i < f->n_hdus; ++i) {
        if (strcmp(f->hdus[i].group_name, name) == 0) {
            out->kind = ADAPTER_LINK_HARD;
            out->target = NULL;
            return 0;
        }
    }
    /* EXTNAME alias → soft */
    for (int i = 0; i < f->n_aliases; ++i) {
        if (strcmp(f->aliases[i].name, name) == 0) {
            out->kind = ADAPTER_LINK_SOFT;
            out->target = f->aliases[i].target;
            return 0;
        }
    }
    return -1;
}

/* Skip keywords whose meaning is structural and shouldn't surface as
 * H5 attributes. END marks header end; SIMPLE/XTENSION/BITPIX/NAXISn/EXTEND
 * describe the dataset shape, which the dataset itself exposes. We DO surface
 * EXTNAME because users frequently look it up. M2.6a keeps this conservative
 * and surfaces everything except END (CFITSIO already strips END from the
 * iteration but be defensive). */
static int is_structural_keyword(const char *name)
{
    return strcmp(name, "END") == 0;
}

static int parse_hdu_keywords(fitsfile *fp, fits_hdu_desc_t *desc)
{
    int status = 0;
    /* Move to the HDU first — we may be on a different one. */
    int hdu_type = 0;
    if (fits_movabs_hdu(fp, desc->index + 1, &hdu_type, &status) != 0) return -1;

    int n_keys = 0, more = 0;
    if (fits_get_hdrspace(fp, &n_keys, &more, &status) != 0) return -1;

    /* +3 slots for synthetic COMMENT, HISTORY, and __raw_header__. */
    fits_attr_t *attrs = calloc((size_t)n_keys + 3, sizeof(*attrs));
    if (!attrs) return -1;
    int produced = 0;

    char *comment_join = NULL;
    char *history_join = NULL;

    for (int i = 1; i <= n_keys; ++i) {
        char keyname[FLEN_KEYWORD], value[FLEN_VALUE], comment[FLEN_COMMENT];
        keyname[0] = value[0] = comment[0] = '\0';
        status = 0;
        if (fits_read_keyn(fp, i, keyname, value, comment, &status) != 0) {
            fprintf(stderr,
                "fits-hdf5-vol: HDU %d card %d: fits_read_keyn failed (status=%d), skipping.\n",
                desc->index, i, status);
            continue;
        }
        if (keyname[0] == '\0' || is_structural_keyword(keyname)) continue;

        /* COMMENT and HISTORY: accumulate the free-form text (returned in
         * `comment` since these records have no "=" separator). Trim trailing
         * spaces, then \n-join. */
        if (strcmp(keyname, "COMMENT") == 0 || strcmp(keyname, "HISTORY") == 0) {
            char *txt = comment;
            size_t len = strlen(txt);
            while (len > 0 && txt[len - 1] == ' ') txt[--len] = '\0';
            char **acc = (keyname[0] == 'C') ? &comment_join : &history_join;
            size_t blen = *acc ? strlen(*acc) : 0;
            char *grown = realloc(*acc, blen + (blen ? 1 : 0) + len + 1);
            if (!grown) continue;
            if (blen) grown[blen++] = '\n';
            memcpy(grown + blen, txt, len);
            grown[blen + len] = '\0';
            *acc = grown;
            continue;
        }

        /* Classify by value */
        char dtype = 'C'; /* default to string */
        status = 0;
        if (fits_get_keytype(value, &dtype, &status) != 0) {
            fprintf(stderr,
                "fits-hdf5-vol: HDU %d \"%s\": fits_get_keytype failed (status=%d, value=\"%s\"), skipping.\n",
                desc->index, keyname, status, value);
            continue;
        }

        fits_attr_t *a = &attrs[produced];
        a->name = strdup(keyname);
        if (!a->name) continue;
        /* HIERARCH: convert internal spaces to dots */
        if (strchr(a->name, ' ')) {
            for (char *p = a->name; *p; ++p) if (*p == ' ') *p = '.';
        }

        /* Some FITS files (e.g. NRAO PACKFITS text dumps) repeat the same
         * non-COMMENT/HISTORY keyword across many cards. HDF5 attribute
         * names must be unique per parent, so we keep the first occurrence
         * and log subsequent ones. The full ordered card sequence is still
         * available via the __raw_header__ byte-exact array. */
        int dup = 0;
        for (int j = 0; j < produced; ++j) {
            if (strcmp(attrs[j].name, a->name) == 0) { dup = 1; break; }
        }
        if (dup) {
            fprintf(stderr,
                "fits-hdf5-vol: HDU %d card %d: duplicate keyword \"%s\" — keeping "
                "first occurrence; full card sequence available in __raw_header__.\n",
                desc->index, i, a->name);
            free(a->name);
            memset(a, 0, sizeof(*a));
            continue;
        }

        switch (dtype) {
            case 'I': {
                long long lv = 0;
                int s2 = 0;
                if (fits_read_key_lnglng(fp, keyname, &lv, comment, &s2) != 0) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d \"%s\": typed int read failed (status=%d), skipping.\n",
                        desc->index, keyname, s2);
                    free(a->name); memset(a, 0, sizeof(*a));
                    break;
                }
                a->type.cls = ADAPTER_T_INT;
                a->type.size = sizeof(int64_t);
                a->scalar.i64 = (int64_t)lv;
                ++produced;
                break;
            }
            case 'F': {
                double dv = 0;
                int s2 = 0;
                if (fits_read_key_dbl(fp, keyname, &dv, comment, &s2) != 0) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d \"%s\": typed float read failed (status=%d), skipping.\n",
                        desc->index, keyname, s2);
                    free(a->name); memset(a, 0, sizeof(*a));
                    break;
                }
                a->type.cls = ADAPTER_T_FLOAT;
                a->type.size = sizeof(double);
                a->scalar.f64 = dv;
                ++produced;
                break;
            }
            case 'L': {
                int bv = 0;
                int s2 = 0;
                if (fits_read_key_log(fp, keyname, &bv, comment, &s2) != 0) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d \"%s\": typed bool read failed (status=%d), skipping.\n",
                        desc->index, keyname, s2);
                    free(a->name); memset(a, 0, sizeof(*a));
                    break;
                }
                a->type.cls = ADAPTER_T_BOOL;
                a->type.size = sizeof(uint8_t);
                a->scalar.u8 = bv ? 1 : 0;
                ++produced;
                break;
            }
            case 'C': {
                /* fits_read_key_longstr reassembles CONTINUE cards. Returns a
                 * malloc'd string freed with fits_free_memory. NO silent
                 * fallback — a real failure here is logged and skipped. */
                char *full = NULL;
                int s2 = 0;
                if (fits_read_key_longstr(fp, keyname, &full, comment, &s2) != 0 || !full) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d \"%s\": fits_read_key_longstr failed (status=%d), skipping.\n",
                        desc->index, keyname, s2);
                    free(a->name); memset(a, 0, sizeof(*a));
                    break;
                }
                a->type.cls = ADAPTER_T_STRING;
                a->type.size = 0;
                a->str = strdup(full);
                fits_free_memory(full, &s2);
                if (!a->str) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d \"%s\": out of memory copying string value, skipping.\n",
                        desc->index, keyname);
                    free(a->name); memset(a, 0, sizeof(*a));
                    break;
                }
                ++produced;
                break;
            }
            case 'X': {
                double cplx[2] = {0, 0};
                int s2 = 0;
                if (fits_read_key_dblcmp(fp, keyname, cplx, comment, &s2) != 0) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d \"%s\": fits_read_key_dblcmp failed (status=%d), skipping.\n",
                        desc->index, keyname, s2);
                    free(a->name); memset(a, 0, sizeof(*a));
                    break;
                }
                a->type.cls = ADAPTER_T_COMPLEX;
                a->type.size = sizeof(double) * 2;
                a->scalar.cplx[0] = cplx[0];
                a->scalar.cplx[1] = cplx[1];
                ++produced;
                break;
            }
            default:
                fprintf(stderr,
                    "fits-hdf5-vol: HDU %d \"%s\": unsupported keyword type '%c', skipping.\n",
                    desc->index, keyname, dtype);
                free(a->name); memset(a, 0, sizeof(*a));
                break;
        }
    }

    /* Emit synthesized COMMENT and HISTORY attributes if any records were
     * collected. Plan §7.5: one VLEN-string attr per name, \n-joined. */
    if (comment_join) {
        fits_attr_t *a = &attrs[produced];
        a->name = strdup("COMMENT");
        a->type.cls = ADAPTER_T_STRING;
        a->type.size = 0;
        a->str = comment_join;   /* take ownership */
        if (a->name) ++produced;
        else { free(a->str); free(a->name); }
    }
    if (history_join) {
        fits_attr_t *a = &attrs[produced];
        a->name = strdup("HISTORY");
        a->type.cls = ADAPTER_T_STRING;
        a->type.size = 0;
        a->str = history_join;
        if (a->name) ++produced;
        else { free(a->str); free(a->name); }
    }

    /* __raw_header__: 1-D vlen-string array, one element per 80-char card,
     * in file order. Plan §7.5. */
    if (n_keys > 0) {
        /* fits_get_hdrspace excludes END; allocate one extra slot. */
        char **cards = calloc((size_t)n_keys + 1, sizeof(*cards));
        if (cards) {
            int got = 0;
            for (int i = 1; i <= n_keys + 1; ++i) {
                char card[FLEN_CARD] = {0};
                int s2 = 0;
                if (fits_read_record(fp, i, card, &s2) != 0) break;
                cards[got] = strdup(card);
                if (cards[got]) ++got;
            }
            if (got > 0) {
                fits_attr_t *a = &attrs[produced];
                a->name = strdup("__raw_header__");
                a->type.cls = ADAPTER_T_STRING;
                a->type.size = 0;
                a->rank = 1;
                a->n_elements = (size_t)got;
                a->strs = cards;
                if (a->name) ++produced;
                else {
                    for (int i = 0; i < got; ++i) free(cards[i]);
                    free(cards);
                }
            } else {
                free(cards);
            }
        }
    }

    desc->attrs = attrs;
    desc->n_attrs = produced;
    desc->attrs_loaded = 1;
    return 0;
}

static fits_hdu_desc_t *get_hdu_desc(adapter_object_t *o)
{
    /* Header keywords belong to the HDU group itself, not to the synthetic
     * columns subgroup or any dataset. M4.6 will add attribute support on
     * AO_COLUMN_DATA for TUNITn — handled there, not here. */
    if (!o || o->sub != AO_HDU_GROUP) return NULL;
    return &o->file->hdus[o->hdu_index];
}

static int ensure_attrs_loaded(adapter_object_t *o)
{
    fits_hdu_desc_t *d = get_hdu_desc(o);
    if (!d) return -1;
    if (d->attrs_loaded) return 0;
    return parse_hdu_keywords(o->file->fp, d);
}

/* ------------------------------------------------------------------ */
/* Dataset I/O (M3 surface — image HDUs only for now)                  */
/* ------------------------------------------------------------------ */

static int fits_dataset_space(adapter_object_t *ds, adapter_space_t *out)
{
    if (!ds || !out) return -1;
    if (ds->kind != ADAPTER_KIND_DATASET) return -1;
    adapter_file_t *f = ds->file;

    if (ds->sub == AO_COLUMN_DATA || ds->sub == AO_TABLE_DATA) {
        /* Column dataset and the row-view 'table' dataset are both rank-1
         * with length n_rows. The element type is what differs. */
        out->rank = 1;
        out->dims[0] = (uint64_t)f->hdus[ds->hdu_index].n_rows;
        return 0;
    }

    int status = 0, t = 0;
    if (fits_movabs_hdu(f->fp, ds->hdu_index + 1, &t, &status) != 0) return -1;

    int naxis = 0;
    if (fits_get_img_dim(f->fp, &naxis, &status) != 0 || naxis < 1 || naxis > 8) return -1;
    long fits_dims[8] = {0};
    if (fits_get_img_size(f->fp, naxis, fits_dims, &status) != 0) return -1;

    /* FITS uses Fortran order (NAXIS1 fastest-varying); HDF5 uses C order
     * (last dim fastest). Reverse: dims[0] = NAXISn, ..., dims[n-1] = NAXIS1. */
    out->rank = naxis;
    for (int i = 0; i < naxis; ++i)
        out->dims[i] = (uint64_t)fits_dims[naxis - 1 - i];
    return 0;
}

static int fits_dataset_type(adapter_object_t *ds, adapter_type_t *out)
{
    if (!ds || !out) return -1;
    if (ds->kind != ADAPTER_KIND_DATASET) return -1;
    adapter_file_t *f = ds->file;

    if (ds->sub == AO_COLUMN_DATA) {
        *out = f->hdus[ds->hdu_index].cols[ds->col_index].scalar_type;
        return 0;
    }
    if (ds->sub == AO_TABLE_DATA) {
        fits_hdu_desc_t *d = &f->hdus[ds->hdu_index];
        /* Lazy-build the compound members on first call. Skips columns that
         * are unsupported (cf_typecode==0) or vlen (deferred from compound
         * representation in v1; standalone /columns/<vlen> still works). */
        if (!d->cmp_members) {
            int valid = 0;
            for (int i = 0; i < d->n_cols; ++i) {
                if (d->cols[i].cf_typecode == 0)        continue;
                if (d->cols[i].scalar_type.is_vlen)     continue;
                ++valid;
            }
            adapter_compound_member_t *m = calloc((size_t)valid, sizeof(*m));
            if (!m) return -1;
            size_t off = 0;
            int j = 0;
            for (int i = 0; i < d->n_cols; ++i) {
                fits_col_t *c = &d->cols[i];
                if (c->cf_typecode == 0)    continue;
                if (c->scalar_type.is_vlen) continue;
                m[j].name   = c->name;
                m[j].type   = c->scalar_type;
                m[j].offset = off;
                size_t per = c->scalar_type.size;
                for (int k = 0; k < c->scalar_type.array_rank; ++k)
                    per *= (size_t)c->scalar_type.array_dims[k];
                off += per;
                ++j;
            }
            d->cmp_members = m;
            d->cmp_info.n_members = j;
            d->cmp_info.row_size  = off;
            d->cmp_info.members   = m;
        }
        out->cls   = ADAPTER_T_COMPOUND;
        out->size  = d->cmp_info.row_size;
        out->extra = &d->cmp_info;
        return 0;
    }

    int status = 0, t = 0;
    if (fits_movabs_hdu(f->fp, ds->hdu_index + 1, &t, &status) != 0) return -1;

    int bitpix = 0;
    if (fits_get_img_type(f->fp, &bitpix, &status) != 0) return -1;

    /* Read BSCALE/BZERO if present. Missing keys are not errors — they default
     * to (1.0, 0.0). */
    double bscale = 1.0, bzero = 0.0;
    int s2 = 0;
    fits_read_key_dbl(f->fp, "BSCALE", &bscale, NULL, &s2);
    s2 = 0;
    fits_read_key_dbl(f->fp, "BZERO", &bzero, NULL, &s2);

    /* Plan §3.4 / §7.2: unsigned-integer convention. */
    if (bscale == 1.0) {
        if (bitpix == SHORT_IMG    && bzero == 32768.0) {
            out->cls = ADAPTER_T_UINT; out->size = 2; return 0;
        }
        if (bitpix == LONG_IMG     && bzero == 2147483648.0) {
            out->cls = ADAPTER_T_UINT; out->size = 4; return 0;
        }
        if (bitpix == LONGLONG_IMG && bzero == 9223372036854775808.0) {
            out->cls = ADAPTER_T_UINT; out->size = 8; return 0;
        }
    }

    /* Plan §7.2: general BSCALE/BZERO rescale → float64. CFITSIO applies
     * physical = BSCALE * raw + BZERO when we read with TDOUBLE. */
    if (bscale != 1.0 || bzero != 0.0) {
        out->cls = ADAPTER_T_FLOAT; out->size = 8;
        return 0;
    }

    /* Plain BITPIX mapping (M3.6 cases). */
    switch (bitpix) {
        case BYTE_IMG:     out->cls = ADAPTER_T_UINT;  out->size = 1; return 0;
        case SHORT_IMG:    out->cls = ADAPTER_T_INT;   out->size = 2; return 0;
        case LONG_IMG:     out->cls = ADAPTER_T_INT;   out->size = 4; return 0;
        case LONGLONG_IMG: out->cls = ADAPTER_T_INT;   out->size = 8; return 0;
        case FLOAT_IMG:    out->cls = ADAPTER_T_FLOAT; out->size = 4; return 0;
        case DOUBLE_IMG:   out->cls = ADAPTER_T_FLOAT; out->size = 8; return 0;
        default:
            fprintf(stderr,
                "fits-hdf5-vol: HDU %d: unsupported BITPIX=%d\n",
                ds->hdu_index, bitpix);
            return -1;
    }
}

/* Map an adapter scalar type to a CFITSIO read-type constant (TBYTE etc.). */
static int adapter_cfitsio_dtype(const adapter_type_t *t)
{
    switch (t->cls) {
        case ADAPTER_T_UINT:
            switch (t->size) {
                case 1: return TBYTE;
                case 2: return TUSHORT;
                case 4: return TUINT;
                case 8: return TULONGLONG;
            }
            break;
        case ADAPTER_T_INT:
            switch (t->size) {
                case 1: return TSBYTE;
                case 2: return TSHORT;
                case 4: return TINT;
                case 8: return TLONGLONG;
            }
            break;
        case ADAPTER_T_FLOAT:
            switch (t->size) {
                case 4: return TFLOAT;
                case 8: return TDOUBLE;
            }
            break;
        default: break;
    }
    return 0;
}

static int fits_dataset_read(adapter_object_t *ds,
                         const uint64_t *start,
                         const uint64_t *count,
                         const uint64_t *stride,
                         const uint64_t *block,
                         void           *dst)
{
    if (!ds || !start || !count || !dst) return -1;
    if (ds->kind != ADAPTER_KIND_DATASET) return -1;

    if (ds->sub == AO_TABLE_DATA) {
        adapter_file_t *f = ds->file;
        fits_hdu_desc_t *d = &f->hdus[ds->hdu_index];
        if (!d->cmp_members) {
            adapter_type_t throwaway;
            if (fits_dataset_type(ds, &throwaway) != 0) return -1;
        }
        long nrows = (long)count[0];

        /* Per-column read into a temporary buffer, then memcpy into the
         * compound row at the right offset. Vlen members are skipped at
         * compound build time, so every member here is scalar or array. */
        for (int m = 0; m < d->cmp_info.n_members; ++m) {
            const adapter_compound_member_t *mem = &d->cmp_info.members[m];
            int col_index = -1;
            for (int i = 0; i < d->n_cols; ++i)
                if (d->cols[i].name == mem->name) { col_index = i; break; }
            if (col_index < 0) return -1;

            size_t per = mem->type.size;
            for (int k = 0; k < mem->type.array_rank; ++k)
                per *= (size_t)mem->type.array_dims[k];

            void *col_buf = malloc((size_t)nrows * per);
            if (!col_buf) return -1;
            adapter_object_t tmp = {
                .kind = ADAPTER_KIND_DATASET, .sub = AO_COLUMN_DATA,
                .file = f, .hdu_index = ds->hdu_index, .col_index = col_index
            };
            if (fits_dataset_read(&tmp, start, count, NULL, NULL, col_buf) != 0) {
                free(col_buf); return -1;
            }

            char *row_dst = (char *)dst;
            for (long r = 0; r < nrows; ++r)
                memcpy(row_dst + (size_t)r * d->cmp_info.row_size + mem->offset,
                       (char *)col_buf + (size_t)r * per, per);
            free(col_buf);
        }
        return 0;
    }

    if (ds->sub == AO_COLUMN_DATA) {
        adapter_file_t *f = ds->file;
        fits_hdu_desc_t *d = &f->hdus[ds->hdu_index];
        fits_col_t *col = &d->cols[ds->col_index];

        if (stride && stride[0] != 1) {
            fprintf(stderr,
                "fits-hdf5-vol: column hyperslab stride!=1 not supported "
                "(col '%s', stride=%llu)\n",
                col->name, (unsigned long long)stride[0]);
            return -1;
        }
        long firstrow = (long)start[0] + 1;
        long nrows    = (long)count[0];

        /* VLEN column path (TFORM 'P'/'Q'): one HDF5 hvl_t per row, each
         * pointing at a freshly-allocated buffer the user reclaims via
         * H5Treclaim. Per-row length comes from fits_read_descript. */
        if (col->scalar_type.is_vlen) {
            int cf_dtype;
            int abs_tc = -col->cf_typecode;   /* drop the vlen sign */
            if (col->scalar_type.cls == ADAPTER_T_BOOL)         cf_dtype = TLOGICAL;
            else if (col->scalar_type.cls == ADAPTER_T_COMPLEX) cf_dtype = (col->scalar_type.size == 8) ? TCOMPLEX : TDBLCOMPLEX;
            else                                                cf_dtype = abs_tc;

            int s2 = 0, t2 = 0;
            if (fits_movabs_hdu(f->fp, ds->hdu_index + 1, &t2, &s2) != 0) return -1;

            /* Each HDF5 hvl_t is `{ size_t len; void *p; }`. */
            typedef struct { size_t len; void *p; } hvl_local_t;
            hvl_local_t *out = (hvl_local_t *)dst;
            for (long r = 0; r < nrows; ++r) {
                long row = firstrow + r;
                long row_repeat = 0, offset = 0;
                int s3 = 0;
                if (fits_read_descript(f->fp, ds->col_index + 1, row,
                                       &row_repeat, &offset, &s3) != 0) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d col '%s' row %ld: read_descript status=%d\n",
                        ds->hdu_index, col->name, row, s3);
                    return -1;
                }
                out[r].len = (size_t)row_repeat;
                if (row_repeat == 0) { out[r].p = NULL; continue; }
                void *p = malloc((size_t)row_repeat * col->scalar_type.size);
                if (!p) return -1;
                int s4 = 0;
                if (fits_read_col(f->fp, cf_dtype, ds->col_index + 1, row, 1,
                                  row_repeat, NULL, p, NULL, &s4) != 0) {
                    fprintf(stderr,
                        "fits-hdf5-vol: HDU %d col '%s' row %ld: vlen read status=%d\n",
                        ds->hdu_index, col->name, row, s4);
                    free(p);
                    return -1;
                }
                out[r].p = p;
            }
            return 0;
        }

        int status = 0, t = 0;
        if (fits_movabs_hdu(f->fp, ds->hdu_index + 1, &t, &status) != 0) return -1;

        if (col->scalar_type.cls == ADAPTER_T_STRING) {
            /* TSTRING expects char *ptrs[nrows] each pointing to a char buffer
             * of at least width+1 bytes. We then memcpy width chars per row
             * into the HDF5 fixed-length-string destination, null-padded. */
            long w = col->width;
            char *flat = calloc((size_t)nrows, (size_t)w + 1);
            char **ptrs = calloc((size_t)nrows, sizeof(char *));
            if (!flat || !ptrs) { free(flat); free(ptrs); return -1; }
            for (long i = 0; i < nrows; ++i) ptrs[i] = flat + i * (w + 1);

            /* Pass NULL nulval — no null-value substitution needed; avoids
             * passing an unterminated char to CFITSIO's internal strcmp. */
            if (fits_read_col(f->fp, TSTRING, ds->col_index + 1,
                              firstrow, 1, nrows, NULL, ptrs, NULL, &status) != 0) {
                fprintf(stderr,
                    "fits-hdf5-vol: HDU %d col '%s': fits_read_col(TSTRING) status=%d\n",
                    ds->hdu_index, col->name, status);
                free(flat); free(ptrs);
                return -1;
            }
            char *out = (char *)dst;
            for (long i = 0; i < nrows; ++i) {
                size_t len = strnlen(ptrs[i], (size_t)w);
                memcpy(out + i * w, ptrs[i], len);
                if (len < (size_t)w) memset(out + i * w + len, 0, (size_t)w - len);
            }
            free(flat); free(ptrs);
            return 0;
        }

        /* Numeric / bool / complex: pick the matching CFITSIO read-type. */
        int cf_dtype;
        if (col->scalar_type.cls == ADAPTER_T_BOOL)
            cf_dtype = TLOGICAL;
        else if (col->scalar_type.cls == ADAPTER_T_COMPLEX)
            cf_dtype = (col->scalar_type.size == 8) ? TCOMPLEX : TDBLCOMPLEX;
        else {
            cf_dtype = adapter_cfitsio_dtype(&col->scalar_type);
            if (cf_dtype == 0) {
                fprintf(stderr,
                    "fits-hdf5-vol: HDU %d col '%s': no CFITSIO read-type\n",
                    ds->hdu_index, col->name);
                return -1;
            }
        }
        long nelements = nrows;
        if (col->repeat > 1) nelements *= col->repeat;
        if (fits_read_col(f->fp, cf_dtype, ds->col_index + 1,
                          firstrow, 1, nelements, NULL, dst, NULL, &status) != 0) {
            fprintf(stderr,
                "fits-hdf5-vol: HDU %d col '%s': fits_read_col status=%d\n",
                ds->hdu_index, col->name, status);
            return -1;
        }
        return 0;
    }

    /* Plan §7.6: tile-compressed images don't read in v1. Group + attrs are
     * still introspectable so callers can identify the file. */
    if (ds->file->hdus[ds->hdu_index].compressed) {
        fprintf(stderr,
            "fits-hdf5-vol: HDU %d is a tile-compressed image; H5Dread is not "
            "supported in v1 (planned for v2). Use H5Aiterate on the HDU "
            "group to inspect compression metadata.\n", ds->hdu_index);
        return -1;
    }
    /* M3.5 limit: stride/block must be NULL or all 1. M3.9 will lift this. */
    if (block) {
        for (int i = 0; ; ++i) { if (block[i] != 1) {
            fprintf(stderr, "fits-hdf5-vol: adapter_dataset_read block!=1 not implemented yet (M3.9)\n");
            return -1; } if (block[i] == 0) break; }
    }

    adapter_file_t *f = ds->file;
    int status = 0, t = 0;
    if (fits_movabs_hdu(f->fp, ds->hdu_index + 1, &t, &status) != 0) return -1;

    adapter_space_t sp = {0};
    if (fits_dataset_space(ds, &sp) != 0) return -1;

    adapter_type_t ty = {0};
    if (fits_dataset_type(ds, &ty) != 0) return -1;
    int cf_dtype = adapter_cfitsio_dtype(&ty);
    if (cf_dtype == 0) {
        fprintf(stderr, "fits-hdf5-vol: HDU %d: no CFITSIO dtype for adapter cls=%d size=%zu\n",
                ds->hdu_index, (int)ty.cls, ty.size);
        return -1;
    }

    /* Translate HDF5 (C-order, 0-based) start/count/stride to CFITSIO
     * (Fortran-order, 1-based) fpixel/lpixel/inc. */
    long fpixel[8], lpixel[8], inc[8];
    for (int i = 0; i < sp.rank; ++i) {
        int j = sp.rank - 1 - i;          /* Fortran axis */
        long s  = (long)start[i];
        long c  = (long)count[i];
        long st = stride ? (long)stride[i] : 1;
        if (c < 1) {
            fprintf(stderr, "fits-hdf5-vol: adapter_dataset_read count[%d]=%ld\n", i, c);
            return -1;
        }
        fpixel[j] = s + 1;
        lpixel[j] = s + (c - 1) * st + 1;
        inc[j]    = st;
    }

    int anynul = 0;
    if (fits_read_subset(f->fp, cf_dtype, fpixel, lpixel, inc,
                         /*nulval=*/NULL, dst, &anynul, &status) != 0) {
        fprintf(stderr, "fits-hdf5-vol: HDU %d: fits_read_subset status=%d\n",
                ds->hdu_index, status);
        return -1;
    }
    return 0;
}

static int fits_attr_count(adapter_object_t *o, size_t *out)
{
    if (!o || !out) return -1;

    /* Plan §7.3: per-column dataset surfaces TUNITn as a "units" attribute. */
    if (o->sub == AO_COLUMN_DATA) {
        fits_col_t *col = &o->file->hdus[o->hdu_index].cols[o->col_index];
        *out = col->unit ? 1 : 0;
        return 0;
    }

    fits_hdu_desc_t *d = get_hdu_desc(o);
    if (!d) { *out = 0; return 0; }
    if (ensure_attrs_loaded(o) != 0) return -1;
    *out = (size_t)d->n_attrs;
    return 0;
}

static int fits_attr_info_by_idx(adapter_object_t *o, size_t idx, adapter_attr_info_t *info)
{
    if (!o || !info) return -1;

    if (o->sub == AO_COLUMN_DATA) {
        fits_col_t *col = &o->file->hdus[o->hdu_index].cols[o->col_index];
        if (idx == 0 && col->unit) {
            info->name = "units";
            info->type.cls = ADAPTER_T_STRING;
            info->type.size = 0;
            info->space.rank = 0;
            return 0;
        }
        return -1;
    }

    fits_hdu_desc_t *d = get_hdu_desc(o);
    if (!d) return -1;
    if (ensure_attrs_loaded(o) != 0) return -1;
    if (idx >= (size_t)d->n_attrs) return -1;
    fits_attr_t *a = &d->attrs[idx];
    info->name = a->name;
    info->type = a->type;
    info->space.rank = a->rank;
    if (a->rank == 1) info->space.dims[0] = (uint64_t)a->n_elements;
    return 0;
}

static int fits_attr_read_by_name(adapter_object_t *o, const char *name, void *dst)
{
    if (!o || !name || !dst) return -1;

    if (o->sub == AO_COLUMN_DATA) {
        fits_col_t *col = &o->file->hdus[o->hdu_index].cols[o->col_index];
        if (col->unit && strcmp(name, "units") == 0) {
            char **out = (char **)dst;
            *out = strdup(col->unit);
            return *out ? 0 : -1;
        }
        return -1;
    }

    fits_hdu_desc_t *d = get_hdu_desc(o);
    if (!d) return -1;
    if (ensure_attrs_loaded(o) != 0) return -1;
    for (int i = 0; i < d->n_attrs; ++i) {
        fits_attr_t *a = &d->attrs[i];
        if (strcmp(a->name, name) != 0) continue;
        switch (a->type.cls) {
            case ADAPTER_T_INT:    *(int64_t *)dst = a->scalar.i64; return 0;
            case ADAPTER_T_FLOAT:  *(double  *)dst = a->scalar.f64; return 0;
            case ADAPTER_T_BOOL:   *(uint8_t *)dst = a->scalar.u8;  return 0;
            case ADAPTER_T_COMPLEX: {
                double *out = (double *)dst;
                out[0] = a->scalar.cplx[0];
                out[1] = a->scalar.cplx[1];
                return 0;
            }
            case ADAPTER_T_STRING: {
                char **out = (char **)dst;
                if (a->rank == 0) {
                    *out = a->str ? strdup(a->str) : strdup("");
                    return *out ? 0 : -1;
                }
                /* rank-1: write n_elements string pointers, each freshly allocated. */
                for (size_t k = 0; k < a->n_elements; ++k) {
                    out[k] = a->strs[k] ? strdup(a->strs[k]) : strdup("");
                    if (!out[k]) {
                        for (size_t j = 0; j < k; ++j) { free(out[j]); out[j] = NULL; }
                        return -1;
                    }
                }
                return 0;
            }
            default: return -1;
        }
    }
    return -1;
}

static void fits_free_string(char *s) { free(s); }

/* ------------------------------------------------------------------ */
/* Adapter vtable export                                               */
/* ------------------------------------------------------------------ */

const fits_adapter_t fits_adapter = {
    .name              = "fits",
    .api_version_major = FITS_ADAPTER_API_VERSION_MAJOR,
    .api_version_minor = FITS_ADAPTER_API_VERSION_MINOR,
    .probe             = fits_probe,
    .open              = fits_open,
    .close             = fits_close,
    .root              = fits_root,
    .object_open       = fits_object_open,
    .object_close      = fits_object_close,
    .object_kind       = fits_object_kind,
    .group_iterate     = fits_group_iterate,
    .link_info         = fits_link_info,
    .attr_count        = fits_attr_count,
    .attr_info_by_idx  = fits_attr_info_by_idx,
    .attr_read_by_name = fits_attr_read_by_name,
    .free_string       = fits_free_string,
    .dataset_space     = fits_dataset_space,
    .dataset_type      = fits_dataset_type,
    .dataset_read      = fits_dataset_read,
};
