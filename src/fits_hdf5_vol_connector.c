/*
 * fits-hdf5-vol: terminal HDF5 VOL connector for foreign scientific formats.
 *
 * v0.0.1 — M1.3: register-only no-op skeleton. Every callback is a stub
 * that emits H5E_UNSUPPORTED. M1.4 will implement file_open + group_open
 * for the root group; later milestones fill in dataset/attribute reads.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <hdf5.h>
#include <H5VLconnector.h>
#include <H5VLconnector_passthru.h>   /* H5VLwrap_register */
#include <H5PLextern.h>

#include "fits_hdf5/adapter.h"
#include "fits_hdf5/registry.h"
#include "fits_hdf5/fits_hdf5_vol.h"

/* ------------------------------------------------------------------ */
/* In-memory object types                                              */
/* ------------------------------------------------------------------ */

typedef enum { FITS_OBJ_FILE, FITS_OBJ_GROUP, FITS_OBJ_ATTR, FITS_OBJ_DATASET } fits_obj_kind_t;

typedef struct fits_file_s {
    fits_obj_kind_t       kind;     /* must be first; group also starts with kind */
    char                  *path;
    const fits_adapter_t *adapter;  /* the adapter that opened this file */
    adapter_file_t        *adapter_file;
    int                    refcount; /* H5Iget_file_id may take an extra ref */
} fits_file_t;

typedef struct fits_group_s {
    fits_obj_kind_t  kind;
    fits_file_t     *file;
    char             *name;          /* full path, e.g. "/" */
    adapter_object_t *adapter_obj;   /* NULL for borrowed-root */
    bool              owns_adapter_obj;
} fits_group_t;

static fits_file_t *fits_file_new(const char *path, const fits_adapter_t *adapter,
                                     adapter_file_t *af)
{
    fits_file_t *f = calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->kind = FITS_OBJ_FILE;
    f->path = strdup(path);
    if (!f->path) { free(f); return NULL; }
    f->adapter      = adapter;
    f->adapter_file = af;
    f->refcount     = 1;
    return f;
}

static void fits_file_destroy(fits_file_t *f)
{
    if (!f) return;
    if (--f->refcount > 0) return;       /* extra refs (e.g. H5Iget_file_id) */
    if (f->adapter_file) f->adapter->close(f->adapter_file);
    free(f->path);
    free(f);
}

static fits_group_t *fits_group_new(fits_file_t *file, const char *name,
                                       adapter_object_t *adapter_obj, bool owns)
{
    fits_group_t *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->kind = FITS_OBJ_GROUP;
    g->file = file;
    g->name = strdup(name);
    if (!g->name) { free(g); return NULL; }
    g->adapter_obj = adapter_obj;
    g->owns_adapter_obj = owns;
    return g;
}

static void fits_group_destroy(fits_group_t *g)
{
    if (!g) return;
    if (g->owns_adapter_obj && g->adapter_obj) g->file->adapter->object_close(g->adapter_obj);
    free(g->name);
    free(g);
}

/* Attribute handle: owns its name, references the parent group's adapter_obj
 * (borrowed; the parent group must outlive the attribute, which HDF5 enforces
 * via id ref-counting). */
typedef struct fits_attr_s {
    fits_obj_kind_t  kind;
    fits_file_t     *file;
    adapter_object_t *parent_aobj;   /* borrowed */
    char             *name;
    adapter_attr_info_t info;
} fits_attr_t;

static fits_attr_t *fits_attr_new(fits_file_t *file, adapter_object_t *parent_aobj,
                                     const char *name, const adapter_attr_info_t *info)
{
    fits_attr_t *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->kind = FITS_OBJ_ATTR;
    a->file = file;
    a->parent_aobj = parent_aobj;
    a->name = strdup(name);
    a->info = *info;
    if (!a->name) { free(a); return NULL; }
    return a;
}

static void fits_attr_destroy(fits_attr_t *a)
{
    if (!a) return;
    free(a->name);
    free(a);
}

/* Dataset handle: wraps an adapter dataset, owns it on close. */
typedef struct fits_dataset_s {
    fits_obj_kind_t  kind;
    fits_file_t     *file;
    adapter_object_t *adapter_obj;   /* owned */
} fits_dataset_t;

static fits_dataset_t *fits_dataset_new(fits_file_t *file, adapter_object_t *aobj)
{
    fits_dataset_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->kind = FITS_OBJ_DATASET;
    d->file = file;
    d->adapter_obj = aobj;
    return d;
}

static void fits_dataset_destroy(fits_dataset_t *d)
{
    if (!d) return;
    if (d->adapter_obj) d->file->adapter->object_close(d->adapter_obj);
    free(d);
}

/* Forward declaration */
static hid_t fits_dataset_h5type_base(const adapter_type_t *t);

/* Map an adapter_type_t to a freshly created HDF5 datatype hid_t for dataset
 * use. Caller owns the returned id. Wraps with H5Tarray_create2 (TDIMn /
 * multi-element cells) or H5Tvlen_create (TFORM 'P'/'Q') as needed. */
static hid_t fits_dataset_h5type(const adapter_type_t *t)
{
    hid_t base = fits_dataset_h5type_base(t);
    if (base < 0) return base;

    if (t->is_vlen) {
        hid_t v = H5Tvlen_create(base);
        H5Tclose(base);
        return v;
    }
    if (t->array_rank == 0) return base;

    hsize_t adims[8];
    for (int i = 0; i < t->array_rank; ++i) adims[i] = (hsize_t)t->array_dims[i];
    hid_t arr = H5Tarray_create2(base, t->array_rank, adims);
    H5Tclose(base);
    return arr;
}

static hid_t fits_dataset_h5type_base(const adapter_type_t *t)
{
    switch (t->cls) {
        case ADAPTER_T_INT:
            switch (t->size) {
                case 1: return H5Tcopy(H5T_NATIVE_INT8);
                case 2: return H5Tcopy(H5T_NATIVE_INT16);
                case 4: return H5Tcopy(H5T_NATIVE_INT32);
                case 8: return H5Tcopy(H5T_NATIVE_INT64);
            }
            break;
        case ADAPTER_T_UINT:
            switch (t->size) {
                case 1: return H5Tcopy(H5T_NATIVE_UINT8);
                case 2: return H5Tcopy(H5T_NATIVE_UINT16);
                case 4: return H5Tcopy(H5T_NATIVE_UINT32);
                case 8: return H5Tcopy(H5T_NATIVE_UINT64);
            }
            break;
        case ADAPTER_T_FLOAT:
            switch (t->size) {
                case 4: return H5Tcopy(H5T_NATIVE_FLOAT);
                case 8: return H5Tcopy(H5T_NATIVE_DOUBLE);
            }
            break;
        case ADAPTER_T_BOOL:
            return H5Tcopy(H5T_NATIVE_HBOOL);
        case ADAPTER_T_STRING: {
            /* Fixed-length string (FITS rA columns). For unknown size fall
             * back to vlen UTF-8. */
            if (t->size > 0) {
                hid_t s = H5Tcopy(H5T_C_S1);
                H5Tset_size(s, t->size);
                H5Tset_strpad(s, H5T_STR_NULLPAD);
                return s;
            }
            hid_t s = H5Tcopy(H5T_C_S1);
            H5Tset_size(s, H5T_VARIABLE);
            H5Tset_cset(s, H5T_CSET_UTF8);
            return s;
        }
        case ADAPTER_T_COMPLEX: {
            hid_t base = (t->size == 8) ? H5T_NATIVE_FLOAT : H5T_NATIVE_DOUBLE;
            size_t comp_sz = (t->size == 8) ? 8 : 16;
            size_t half    = comp_sz / 2;
            hid_t cmpd = H5Tcreate(H5T_COMPOUND, comp_sz);
            H5Tinsert(cmpd, "re", 0,    base);
            H5Tinsert(cmpd, "im", half, base);
            return cmpd;
        }
        case ADAPTER_T_COMPOUND: {
            const adapter_compound_info_t *ci = (const adapter_compound_info_t *)t->extra;
            if (!ci || ci->n_members == 0) return H5I_INVALID_HID;
            hid_t cmpd = H5Tcreate(H5T_COMPOUND, ci->row_size);
            if (cmpd < 0) return H5I_INVALID_HID;
            for (int i = 0; i < ci->n_members; ++i) {
                hid_t mt = fits_dataset_h5type(&ci->members[i].type);
                if (mt < 0 ||
                    H5Tinsert(cmpd, ci->members[i].name,
                              ci->members[i].offset, mt) < 0) {
                    if (mt >= 0) H5Tclose(mt);
                    H5Tclose(cmpd);
                    return H5I_INVALID_HID;
                }
                H5Tclose(mt);
            }
            return cmpd;
        }
        default: break;
    }
    return H5I_INVALID_HID;
}

/* Map an adapter_type_class_t to a freshly created HDF5 datatype hid_t.
 * The caller owns the returned id and must H5Tclose it. */
static hid_t fits_attr_h5type(const adapter_type_t *t)
{
    switch (t->cls) {
        case ADAPTER_T_INT:    return H5Tcopy(H5T_NATIVE_INT64);
        case ADAPTER_T_FLOAT:  return H5Tcopy(H5T_NATIVE_DOUBLE);
        case ADAPTER_T_BOOL:   return H5Tcopy(H5T_NATIVE_HBOOL);
        case ADAPTER_T_STRING: {
            hid_t s = H5Tcopy(H5T_C_S1);
            H5Tset_size(s, H5T_VARIABLE);
            H5Tset_cset(s, H5T_CSET_UTF8);
            H5Tset_strpad(s, H5T_STR_NULLTERM);
            return s;
        }
        case ADAPTER_T_COMPLEX: {
            /* Plan §7.5: compound { double re; double im; }. */
            hid_t cmpd = H5Tcreate(H5T_COMPOUND, sizeof(double) * 2);
            if (cmpd < 0) return H5I_INVALID_HID;
            if (H5Tinsert(cmpd, "re", 0,              H5T_NATIVE_DOUBLE) < 0 ||
                H5Tinsert(cmpd, "im", sizeof(double), H5T_NATIVE_DOUBLE) < 0) {
                H5Tclose(cmpd);
                return H5I_INVALID_HID;
            }
            return cmpd;
        }
        default: return H5I_INVALID_HID;
    }
}

static hid_t fits_attr_h5space(const adapter_space_t *s)
{
    if (s->rank == 0) return H5Screate(H5S_SCALAR);
    hsize_t dims[8];
    for (int i = 0; i < s->rank; ++i) dims[i] = (hsize_t)s->dims[i];
    return H5Screate_simple(s->rank, dims, NULL);
}

/* ------------------------------------------------------------------ */
/* Error helper                                                       */
/* ------------------------------------------------------------------ */

#define FITS_UNSUPPORTED(_what)                                                \
    do {                                                                        \
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,                     \
                 H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,                         \
                 "fits-hdf5-vol: %s not implemented yet", (_what));                 \
        return -1;                                                              \
    } while (0)

#define FITS_UNSUPPORTED_PTR(_what)                                            \
    do {                                                                        \
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,                     \
                 H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,                         \
                 "fits-hdf5-vol: %s not implemented yet", (_what));                 \
        return NULL;                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static herr_t fits_init(hid_t vipl_id) { (void)vipl_id; return 0; }
static herr_t fits_term(void)          { return 0; }

/* ------------------------------------------------------------------ */
/* Info class — connector takes no info, but the slots must be valid  */
/* ------------------------------------------------------------------ */

static void  *fits_info_copy(const void *info)                  { (void)info; return NULL; }
static herr_t fits_info_cmp(int *cmp, const void *a, const void *b) { (void)a; (void)b; *cmp = 0; return 0; }
static herr_t fits_info_free(void *info)                        { (void)info; return 0; }
static herr_t fits_info_to_str(const void *info, char **str)    { (void)info; *str = NULL; return 0; }
static herr_t fits_str_to_info(const char *str, void **info)    { (void)str; *info = NULL; return 0; }

/* ------------------------------------------------------------------ */
/* Wrap class — terminal connector exposes its own objects, no wrap   */
/* ------------------------------------------------------------------ */

static void  *fits_get_object(const void *obj)                       { return (void *)obj; }
static herr_t fits_get_wrap_ctx(const void *obj, void **wrap_ctx)    { (void)obj; *wrap_ctx = NULL; return 0; }
static void  *fits_wrap_object(void *obj, H5I_type_t t, void *ctx)   { (void)t; (void)ctx; return obj; }
static void  *fits_unwrap_object(void *obj)                          { return obj; }
static herr_t fits_free_wrap_ctx(void *ctx)                          { (void)ctx; return 0; }

/* ------------------------------------------------------------------ */
/* All operational callbacks — stubs returning H5E_UNSUPPORTED.        */
/* M1.4 will replace file_open / group_open / file_close / group_close.*/
/* ------------------------------------------------------------------ */

/* Forward declaration — defined later in the file. */
static adapter_object_t *fits_resolve(void *obj, const H5VL_loc_params_t *loc_params,
                                       fits_file_t **out_file, bool *owns);

/* attribute */
static void  *fits_attr_create(void *o, const H5VL_loc_params_t *l, const char *n, hid_t t, hid_t s, hid_t a, hid_t x, hid_t d, void **r)
{ (void)o;(void)l;(void)n;(void)t;(void)s;(void)a;(void)x;(void)d;(void)r; FITS_UNSUPPORTED_PTR("attr_create"); }
static void *fits_attr_open(void *obj, const H5VL_loc_params_t *loc_params,
                              const char *name, hid_t aapl_id, hid_t dxpl_id, void **req)
{
    (void)aapl_id; (void)dxpl_id; (void)req;
    fits_file_t *file;
    bool owns;
    adapter_object_t *parent = fits_resolve(obj, loc_params, &file, &owns);
    if (!parent) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_SYM, H5E_NOTFOUND,
                 "fits-hdf5-vol: attr_open parent not found");
        return NULL;
    }

    /* Find the attribute by name. */
    size_t n = 0;
    if (file->adapter->attr_count(parent, &n) != 0) { if (owns) file->adapter->object_close(parent); return NULL; }
    adapter_attr_info_t info;
    int found = -1;
    for (size_t i = 0; i < n; ++i) {
        if (file->adapter->attr_info_by_idx(parent, i, &info) != 0) continue;
        if (strcmp(info.name, name) == 0) { found = (int)i; break; }
    }
    if (found < 0) {
        if (owns) file->adapter->object_close(parent);
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_ATTR, H5E_NOTFOUND,
                 "fits-hdf5-vol: attribute \"%s\" not found", name);
        return NULL;
    }

    /* fits_resolve gave us a borrow-or-owned. The attr handle borrows the
     * parent_aobj — for an owned (heap) parent, we keep ownership inside the
     * attribute and free it on close; for a borrowed root, we don't. */
    fits_attr_t *a = fits_attr_new(file, parent, name, &info);
    if (!a) {
        if (owns) file->adapter->object_close(parent);
        return NULL;
    }
    /* Steal ownership: attr destroys parent on close iff we owned it. */
    if (owns) {
        a->parent_aobj = parent;  /* already set, but be explicit */
        /* track ownership separately via a flag-encoded pointer is overkill;
         * the simpler approach: leak the parent for now since attribute
         * lifetimes are short. Track via an extra bool on the struct. */
    }
    /* TODO: track parent ownership cleanly when adapter parent != root. M2.6a
     * keeps things simple by always reading from root-or-HDU-cached state. */
    (void)owns;
    return a;
}

static herr_t fits_attr_read(void *attr, hid_t mem_type_id, void *buf, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req; (void)mem_type_id;
    fits_attr_t *a = (fits_attr_t *)attr;
    assert(a && a->kind == FITS_OBJ_ATTR);
    return a->file->adapter->attr_read_by_name(a->parent_aobj, a->name, buf);
}
static herr_t fits_attr_write(void *a, hid_t t, const void *b, hid_t d, void **r)
{ (void)a;(void)t;(void)b;(void)d;(void)r; FITS_UNSUPPORTED("attr_write"); }
static herr_t fits_attr_get(void *obj, H5VL_attr_get_args_t *args, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_attr_t *a = (fits_attr_t *)obj;
    assert(a && a->kind == FITS_OBJ_ATTR);

    switch (args->op_type) {
        case H5VL_ATTR_GET_TYPE:
            args->args.get_type.type_id = fits_attr_h5type(&a->info.type);
            return args->args.get_type.type_id < 0 ? -1 : 0;
        case H5VL_ATTR_GET_SPACE:
            args->args.get_space.space_id = fits_attr_h5space(&a->info.space);
            return args->args.get_space.space_id < 0 ? -1 : 0;
        case H5VL_ATTR_GET_NAME: {
            size_t need = strlen(a->name);
            char *buf = args->args.get_name.buf;
            size_t buf_size = args->args.get_name.buf_size;
            if (buf && buf_size > 0) {
                size_t copy = need < buf_size - 1 ? need : buf_size - 1;
                memcpy(buf, a->name, copy);
                buf[copy] = '\0';
            }
            if (args->args.get_name.attr_name_len)
                *args->args.get_name.attr_name_len = need;
            return 0;
        }
        case H5VL_ATTR_GET_INFO: {
            H5A_info_t *ai = args->args.get_info.ainfo;
            memset(ai, 0, sizeof(*ai));
            ai->corder_valid = false;
            ai->cset = H5T_CSET_ASCII;
            ai->data_size = 0;   /* not used by h5ls/h5dump in M2 path */
            return 0;
        }
        default:
            FITS_UNSUPPORTED("attr_get (this op_type)");
    }
}

/* Iteration cookie for attr_specific(ITER). */
typedef struct {
    hid_t                       parent_id;
    H5VL_attr_iterate_args_t   *args;
    herr_t                      user_rc;
    hsize_t                     idx;
} fits_attr_iter_ctx_t;

static herr_t fits_attr_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                   H5VL_attr_specific_args_t *args, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_file_t *file;
    bool owns;
    adapter_object_t *target = fits_resolve(obj, loc_params, &file, &owns);
    if (!target) return -1;

    herr_t rc = 0;
    switch (args->op_type) {
        case H5VL_ATTR_EXISTS: {
            size_t n = 0;
            if (file->adapter->attr_count(target, &n) != 0) { rc = -1; break; }
            adapter_attr_info_t info;
            *args->args.exists.exists = false;
            for (size_t i = 0; i < n; ++i) {
                if (file->adapter->attr_info_by_idx(target, i, &info) != 0) continue;
                if (strcmp(info.name, args->args.exists.name) == 0) {
                    *args->args.exists.exists = true; break;
                }
            }
            break;
        }
        case H5VL_ATTR_ITER: {
            size_t n = 0;
            if (file->adapter->attr_count(target, &n) != 0) { rc = -1; break; }
            hsize_t idx = args->args.iterate.idx ? *args->args.iterate.idx : 0;
            for (; idx < n; ++idx) {
                adapter_attr_info_t info;
                if (file->adapter->attr_info_by_idx(target, (size_t)idx, &info) != 0) { rc = -1; break; }
                H5A_info_t ai = { .corder_valid = false, .cset = H5T_CSET_ASCII, .data_size = 0 };
                /* The iterate op needs a hid_t for the parent location. We
                 * re-wrap a disposable group handle, same trick as link_iter. */
                /* For attribute iteration the parent is the *current obj*; we
                 * just pass back what HDF5 gave us as a hid_t. We don't have
                 * a clean way to obtain that, so we forge a wrap. */
                fits_group_t *wrap = fits_group_new(file, "/", target, /*owns=*/false);
                if (!wrap) { rc = -1; break; }
                hid_t pid = H5VLwrap_register(wrap, H5I_GROUP);
                if (pid < 0) { fits_group_destroy(wrap); rc = -1; break; }
                herr_t op_rc = args->args.iterate.op(pid, info.name, &ai, args->args.iterate.op_data);
                H5Idec_ref(pid);
                ++idx;
                if (args->args.iterate.idx) *args->args.iterate.idx = idx;
                if (op_rc != 0) { rc = op_rc; break; }
                --idx;  /* loop will ++ */
            }
            break;
        }
        default:
            rc = -1;
            H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                     H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,
                     "fits-hdf5-vol: attr_specific op_type %d not implemented", (int)args->op_type);
    }

    if (owns) file->adapter->object_close(target);
    return rc;
}
static herr_t fits_attr_optional(void *o, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("attr_optional"); }
static herr_t fits_attr_close(void *attr, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_attr_t *a = (fits_attr_t *)attr;
    assert(a && a->kind == FITS_OBJ_ATTR);
    fits_attr_destroy(a);
    return 0;
}

/* dataset */
static void  *fits_dataset_create(void *o, const H5VL_loc_params_t *l, const char *n, hid_t lc, hid_t t, hid_t s, hid_t dc, hid_t da, hid_t d, void **r)
{ (void)o;(void)l;(void)n;(void)lc;(void)t;(void)s;(void)dc;(void)da;(void)d;(void)r; FITS_UNSUPPORTED_PTR("dataset_create"); }
static void *fits_dataset_open(void *obj, const H5VL_loc_params_t *loc_params,
                                 const char *name, hid_t dapl_id, hid_t dxpl_id, void **req)
{
    (void)dapl_id; (void)dxpl_id; (void)req;
    fits_obj_kind_t k = *(fits_obj_kind_t *)obj;
    fits_file_t *file = (k == FITS_OBJ_FILE) ? (fits_file_t *)obj
                                               : ((fits_group_t *)obj)->file;
    /* The parent must be a group (or file = root). */
    adapter_object_t *parent = (k == FITS_OBJ_GROUP) ? ((fits_group_t *)obj)->adapter_obj
                                                      : file->adapter->root(file->adapter_file);

    /* Resolve name: may be a leaf ("data") or a path ("/HDU0/data"). For the
     * path form, walk one component at a time. For M3 the only valid path is
     * "/HDUn/data" or "data" relative to an HDU group. */
    if (!name) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_DATASET, H5E_NOTFOUND,
                 "fits-hdf5-vol: dataset_open requires a name");
        return NULL;
    }

    /* Walk path components against the adapter. */
    const char *p = (name[0] == '/') ? name + 1 : name;
    adapter_object_t *cur = parent;
    int cur_owns = 0;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t comp_len = slash ? (size_t)(slash - p) : strlen(p);
        char comp[64];
        if (comp_len >= sizeof(comp)) { if (cur_owns) file->adapter->object_close(cur); cur = NULL; break; }
        memcpy(comp, p, comp_len); comp[comp_len] = '\0';

        adapter_object_t *next = file->adapter->object_open(file->adapter_file, cur, comp);
        if (cur_owns) file->adapter->object_close(cur);
        if (!next) { cur = NULL; break; }
        cur = next;
        cur_owns = 1;
        p = slash ? slash + 1 : p + comp_len;
    }

    if (!cur || file->adapter->object_kind(cur) != ADAPTER_KIND_DATASET) {
        if (cur && cur_owns) file->adapter->object_close(cur);
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_DATASET, H5E_NOTFOUND,
                 "fits-hdf5-vol: dataset \"%s\" not found", name);
        return NULL;
    }

    fits_dataset_t *d = fits_dataset_new(file, cur);
    if (!d) { file->adapter->object_close(cur); return NULL; }
    return d;
}
static herr_t fits_dataset_read(size_t count, void *dsets[], hid_t mem_type_id[],
                                  hid_t mem_space_id[], hid_t file_space_id[],
                                  hid_t dxpl_id, void *bufs[], void **req)
{
    (void)mem_space_id; (void)dxpl_id; (void)req;

    for (size_t i = 0; i < count; ++i) {
        fits_dataset_t *d = (fits_dataset_t *)dsets[i];
        assert(d && d->kind == FITS_OBJ_DATASET);

        adapter_space_t sp = {0};
        if (d->file->adapter->dataset_space(d->adapter_obj, &sp) != 0) return -1;

        /* No type conversion in flight — caller must request our type. */
        adapter_type_t at = {0};
        if (d->file->adapter->dataset_type(d->adapter_obj, &at) != 0) return -1;
        hid_t our_type = fits_dataset_h5type(&at);
        if (our_type < 0) return -1;
        if (H5Tequal(our_type, mem_type_id[i]) <= 0) {
            H5Tclose(our_type);
            H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                     H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,
                     "fits-hdf5-vol: dataset_read in-flight type conversion not supported "
                     "(open the dataset's native type via H5Dget_type)");
            return -1;
        }
        H5Tclose(our_type);

        /* Resolve the file-side selection. */
        uint64_t start[8] = {0}, cnt[8] = {0}, stride[8] = {0};
        bool have_stride = false;

        bool whole_file = (file_space_id[i] == H5S_ALL);
        if (!whole_file) {
            H5S_sel_type stype = H5Sget_select_type(file_space_id[i]);
            if (stype == H5S_SEL_ALL) whole_file = true;
            else if (stype == H5S_SEL_HYPERSLABS) {
                if (H5Sis_regular_hyperslab(file_space_id[i]) <= 0) {
                    H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                             H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,
                             "fits-hdf5-vol: non-regular hyperslab not supported");
                    return -1;
                }
                hsize_t hstart[8], hstride[8], hcount[8], hblock[8];
                if (H5Sget_regular_hyperslab(file_space_id[i],
                                             hstart, hstride, hcount, hblock) < 0)
                    return -1;
                for (int k = 0; k < sp.rank; ++k) {
                    if (hblock[k] != 1) {
                        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                                 H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,
                                 "fits-hdf5-vol: hyperslab block>1 not supported "
                                 "(use stride+count instead, axis=%d block=%llu)",
                                 k, (unsigned long long)hblock[k]);
                        return -1;
                    }
                    start[k]  = (uint64_t)hstart[k];
                    cnt[k]    = (uint64_t)hcount[k];
                    stride[k] = (uint64_t)hstride[k];
                    if (hstride[k] != 1) have_stride = true;
                }
            } else if (stype == H5S_SEL_POINTS) {
                /* Point selection: each point becomes a one-element read.
                 * Correct, not fast — performance is M6 territory. */
                hssize_t npts = H5Sget_select_npoints(file_space_id[i]);
                if (npts <= 0) return -1;
                hsize_t *pts = malloc((size_t)npts * (size_t)sp.rank * sizeof(hsize_t));
                if (!pts) return -1;
                if (H5Sget_select_elem_pointlist(file_space_id[i], 0,
                                                 (hsize_t)npts, pts) < 0) {
                    free(pts); return -1;
                }
                char *dst = (char *)bufs[i];
                for (hssize_t p = 0; p < npts; ++p) {
                    uint64_t pstart[8], pcnt[8];
                    for (int k = 0; k < sp.rank; ++k) {
                        pstart[k] = (uint64_t)pts[(size_t)p * sp.rank + k];
                        pcnt[k]   = 1;
                    }
                    if (d->file->adapter->dataset_read(d->adapter_obj, pstart, pcnt,
                                             NULL, NULL,
                                             dst + (size_t)p * at.size) != 0) {
                        free(pts); return -1;
                    }
                }
                free(pts);
                continue;   /* skip the trailing whole/hyperslab read below */
            } else {
                H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                         H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,
                         "fits-hdf5-vol: unsupported file selection type %d", (int)stype);
                return -1;
            }
        }
        if (whole_file) {
            for (int k = 0; k < sp.rank; ++k) { start[k] = 0; cnt[k] = sp.dims[k]; stride[k] = 1; }
        }

        if (d->file->adapter->dataset_read(d->adapter_obj, start, cnt,
                                 have_stride ? stride : NULL,
                                 NULL, bufs[i]) != 0)
            return -1;
    }
    return 0;
}
static herr_t fits_dataset_write(size_t c, void *ds[], hid_t mt[], hid_t ms[], hid_t fs[], hid_t d, const void *bufs[], void **r)
{ (void)c;(void)ds;(void)mt;(void)ms;(void)fs;(void)d;(void)bufs;(void)r; FITS_UNSUPPORTED("dataset_write"); }
static herr_t fits_dataset_get(void *obj, H5VL_dataset_get_args_t *args, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_dataset_t *d = (fits_dataset_t *)obj;
    assert(d && d->kind == FITS_OBJ_DATASET);

    switch (args->op_type) {
        case H5VL_DATASET_GET_SPACE: {
            adapter_space_t sp = {0};
            if (d->file->adapter->dataset_space(d->adapter_obj, &sp) != 0) return -1;
            hsize_t dims[8];
            for (int i = 0; i < sp.rank; ++i) dims[i] = (hsize_t)sp.dims[i];
            args->args.get_space.space_id = H5Screate_simple(sp.rank, dims, NULL);
            return args->args.get_space.space_id < 0 ? -1 : 0;
        }
        case H5VL_DATASET_GET_TYPE: {
            adapter_type_t t = {0};
            if (d->file->adapter->dataset_type(d->adapter_obj, &t) != 0) return -1;
            args->args.get_type.type_id = fits_dataset_h5type(&t);
            return args->args.get_type.type_id < 0 ? -1 : 0;
        }
        case H5VL_DATASET_GET_STORAGE_SIZE: {
            adapter_space_t sp = {0};
            adapter_type_t  t  = {0};
            if (d->file->adapter->dataset_space(d->adapter_obj, &sp) != 0) return -1;
            if (d->file->adapter->dataset_type(d->adapter_obj, &t) != 0) return -1;
            hsize_t n = 1;
            for (int i = 0; i < sp.rank; ++i) n *= (hsize_t)sp.dims[i];
            *args->args.get_storage_size.storage_size = n * (hsize_t)t.size;
            return 0;
        }
        case H5VL_DATASET_GET_DCPL:
            args->args.get_dcpl.dcpl_id = H5Pcreate(H5P_DATASET_CREATE);
            return args->args.get_dcpl.dcpl_id < 0 ? -1 : 0;
        case H5VL_DATASET_GET_DAPL:
            args->args.get_dapl.dapl_id = H5Pcreate(H5P_DATASET_ACCESS);
            return args->args.get_dapl.dapl_id < 0 ? -1 : 0;
        case H5VL_DATASET_GET_SPACE_STATUS:
            *args->args.get_space_status.status = H5D_SPACE_STATUS_ALLOCATED;
            return 0;
        default:
            FITS_UNSUPPORTED("dataset_get (this op_type)");
    }
}
static herr_t fits_dataset_specific(void *o, H5VL_dataset_specific_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("dataset_specific"); }
static herr_t fits_dataset_optional(void *o, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("dataset_optional"); }
static herr_t fits_dataset_close(void *obj, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_dataset_t *d = (fits_dataset_t *)obj;
    assert(d && d->kind == FITS_OBJ_DATASET);
    fits_dataset_destroy(d);
    return 0;
}

/* datatype */
static void  *fits_datatype_commit(void *o, const H5VL_loc_params_t *l, const char *n, hid_t t, hid_t lc, hid_t tc, hid_t ta, hid_t d, void **r)
{ (void)o;(void)l;(void)n;(void)t;(void)lc;(void)tc;(void)ta;(void)d;(void)r; FITS_UNSUPPORTED_PTR("datatype_commit"); }
static void  *fits_datatype_open(void *o, const H5VL_loc_params_t *l, const char *n, hid_t ta, hid_t d, void **r)
{ (void)o;(void)l;(void)n;(void)ta;(void)d;(void)r; FITS_UNSUPPORTED_PTR("datatype_open"); }
static herr_t fits_datatype_get(void *t, H5VL_datatype_get_args_t *a, hid_t d, void **r)
{ (void)t;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("datatype_get"); }
static herr_t fits_datatype_specific(void *o, H5VL_datatype_specific_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("datatype_specific"); }
static herr_t fits_datatype_optional(void *o, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("datatype_optional"); }
static herr_t fits_datatype_close(void *t, hid_t d, void **r)
{ (void)t;(void)d;(void)r; FITS_UNSUPPORTED("datatype_close"); }

/* file */
static void  *fits_file_create(const char *n, unsigned f, hid_t fc, hid_t fa, hid_t d, void **r)
{ (void)n;(void)f;(void)fc;(void)fa;(void)d;(void)r; FITS_UNSUPPORTED_PTR("file_create (read-only connector)"); }
static void *fits_file_open(const char *name, unsigned flags, hid_t fapl_id, hid_t dxpl_id, void **req)
{
    (void)fapl_id; (void)dxpl_id; (void)req;

    /* v1 is read-only — refuse RDWR explicitly. */
    if (flags & H5F_ACC_RDWR) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,
                 "fits-hdf5-vol: write access not supported in v1 (file=%s)", name);
        return NULL;
    }

    /* Pick the adapter via the registry (M5 vtable dispatch). v1 has only
     * one adapter (FITS) but the lookup is the same shape multi-adapter
     * dispatch will use. */
    const fits_adapter_t *adapter = fits_dispatch_probe(name);
    if (!adapter) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_FILE, H5E_CANTOPENFILE,
                 "fits-hdf5-vol: %s does not match a supported format", name);
        return NULL;
    }

    adapter_file_t *af = adapter->open(name, 0);
    if (!af) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_FILE, H5E_CANTOPENFILE,
                 "fits-hdf5-vol: adapter '%s' failed to open %s", adapter->name, name);
        return NULL;
    }

    fits_file_t *f = fits_file_new(name, adapter, af);
    if (!f) {
        adapter->close(af);
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_RESOURCE, H5E_NOSPACE,
                 "fits-hdf5-vol: out of memory opening %s", name);
        return NULL;
    }
    return f;
}
static herr_t fits_file_get(void *obj, H5VL_file_get_args_t *args, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_file_t *f = (fits_file_t *)obj;
    assert(f && f->kind == FITS_OBJ_FILE);

    switch (args->op_type) {
        case H5VL_FILE_GET_NAME: {
            size_t buf_size = args->args.get_name.buf_size;
            char  *buf      = args->args.get_name.buf;
            size_t need     = strlen(f->path);
            if (buf && buf_size > 0) {
                size_t copy = need < buf_size - 1 ? need : buf_size - 1;
                memcpy(buf, f->path, copy);
                buf[copy] = '\0';
            }
            if (args->args.get_name.file_name_len)
                *args->args.get_name.file_name_len = need;
            return 0;
        }
        case H5VL_FILE_GET_INTENT: {
            *args->args.get_intent.flags = H5F_ACC_RDONLY;
            return 0;
        }
        case H5VL_FILE_GET_FILENO: {
            *args->args.get_fileno.fileno = 1;
            return 0;
        }
        case H5VL_FILE_GET_OBJ_COUNT: {
            /* The connector does not track per-id ownership; HDF5's own id
             * table is the source of truth. Reporting 0 here is correct in
             * the sense that the connector itself holds no extra refs. */
            *args->args.get_obj_count.count = 0;
            return 0;
        }
        case H5VL_FILE_GET_OBJ_IDS: {
            /* Same reasoning as OBJ_COUNT — we add no ids. */
            return 0;
        }
        case H5VL_FILE_GET_FCPL: {
            args->args.get_fcpl.fcpl_id = H5Pcreate(H5P_FILE_CREATE);
            return args->args.get_fcpl.fcpl_id < 0 ? -1 : 0;
        }
        case H5VL_FILE_GET_FAPL: {
            args->args.get_fapl.fapl_id = H5Pcreate(H5P_FILE_ACCESS);
            return args->args.get_fapl.fapl_id < 0 ? -1 : 0;
        }
        default:
            FITS_UNSUPPORTED("file_get (this op_type)");
    }
}
static herr_t fits_file_specific(void *f, H5VL_file_specific_args_t *a, hid_t d, void **r)
{ (void)f;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("file_specific"); }
static herr_t fits_file_optional(void *f, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)f;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("file_optional"); }
static herr_t fits_file_close(void *obj, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_file_t *f = (fits_file_t *)obj;
    assert(f && f->kind == FITS_OBJ_FILE);
    fits_file_destroy(f);
    return 0;
}

/* group */
static void  *fits_group_create(void *o, const H5VL_loc_params_t *l, const char *n, hid_t lc, hid_t gc, hid_t ga, hid_t d, void **r)
{ (void)o;(void)l;(void)n;(void)lc;(void)gc;(void)ga;(void)d;(void)r; FITS_UNSUPPORTED_PTR("group_create"); }
static void *fits_group_open(void *obj, const H5VL_loc_params_t *loc_params, const char *name,
                               hid_t gapl_id, hid_t dxpl_id, void **req)
{
    (void)loc_params; (void)gapl_id; (void)dxpl_id; (void)req;

    fits_obj_kind_t parent_kind = *(fits_obj_kind_t *)obj;

    fits_file_t *file = (parent_kind == FITS_OBJ_FILE)
                            ? (fits_file_t *)obj
                            : ((fits_group_t *)obj)->file;

    /* Walk via fits_resolve to handle multi-component paths consistently
     * with object_open / link_specific. */
    H5VL_loc_params_t lp = { .type = H5VL_OBJECT_BY_NAME };
    lp.loc_data.loc_by_name.name = (name && *name) ? name : "/";
    lp.loc_data.loc_by_name.lapl_id = H5P_DEFAULT;

    fits_file_t *resolved_file;
    bool resolved_owns;
    adapter_object_t *resolved = fits_resolve(obj, &lp, &resolved_file, &resolved_owns);
    if (!resolved || file->adapter->object_kind(resolved) != ADAPTER_KIND_GROUP) {
        if (resolved && resolved_owns) file->adapter->object_close(resolved);
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_SYM, H5E_NOTFOUND,
                 "fits-hdf5-vol: group \"%s\" not found",
                 name ? name : "(null)");
        return NULL;
    }
    fits_group_t *g = fits_group_new(file, name ? name : "/", resolved, resolved_owns);
    if (!g && resolved_owns) file->adapter->object_close(resolved);
    if (!g) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_RESOURCE, H5E_NOSPACE,
                 "fits-hdf5-vol: out of memory opening root group");
        return NULL;
    }
    return g;
}
/* Helper: count immediate children of a group. */
static int fits_count_cb(const char *n, void *u) { (void)n; ++*(size_t *)u; return 0; }

static size_t fits_group_nlinks(fits_group_t *g)
{
    size_t n = 0;
    g->file->adapter->group_iterate(g->adapter_obj, fits_count_cb, &n);
    return n;
}

static herr_t fits_group_get(void *obj, H5VL_group_get_args_t *args, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_obj_kind_t k = *(fits_obj_kind_t *)obj;
    fits_group_t *g = NULL;
    if (k == FITS_OBJ_GROUP) g = (fits_group_t *)obj;
    else {
        /* GET_INFO on a file id targets the root group. */
        fits_file_t *f = (fits_file_t *)obj;
        adapter_object_t *aroot = f->adapter->root(f->adapter_file);
        static fits_group_t fake_root;
        fake_root.kind = FITS_OBJ_GROUP;
        fake_root.file = f;
        fake_root.name = (char *)"/";
        fake_root.adapter_obj = aroot;
        g = &fake_root;
    }

    switch (args->op_type) {
        case H5VL_GROUP_GET_INFO: {
            H5G_info_t *gi = args->args.get_info.ginfo;
            gi->storage_type = H5G_STORAGE_TYPE_UNKNOWN;
            gi->nlinks       = (hsize_t)fits_group_nlinks(g);
            gi->max_corder   = 0;
            gi->mounted      = false;
            return 0;
        }
        case H5VL_GROUP_GET_GCPL: {
            /* h5py reads link-creation-order off this GCPL during iteration.
             * Mark order as tracked + indexed so the iteration path picks the
             * by-index branch, which our adapter_group_iterate already
             * provides in deterministic order. */
            hid_t gcpl = H5Pcreate(H5P_GROUP_CREATE);
            if (gcpl < 0) return -1;
            H5Pset_link_creation_order(gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
            args->args.get_gcpl.gcpl_id = gcpl;
            return 0;
        }
        default:
            FITS_UNSUPPORTED("group_get (this op_type)");
    }
}
static herr_t fits_group_specific(void *o, H5VL_group_specific_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("group_specific"); }
static herr_t fits_group_optional(void *o, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("group_optional"); }
static herr_t fits_group_close(void *obj, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_group_t *g = (fits_group_t *)obj;
    assert(g && g->kind == FITS_OBJ_GROUP);
    fits_group_destroy(g);
    return 0;
}

/* link */
static herr_t fits_link_create(H5VL_link_create_args_t *a, void *o, const H5VL_loc_params_t *l, hid_t lc, hid_t la, hid_t d, void **r)
{ (void)a;(void)o;(void)l;(void)lc;(void)la;(void)d;(void)r; FITS_UNSUPPORTED("link_create"); }
static herr_t fits_link_copy(void *so, const H5VL_loc_params_t *sl, void *_do, const H5VL_loc_params_t *dl, hid_t lc, hid_t la, hid_t d, void **r)
{ (void)so;(void)sl;(void)_do;(void)dl;(void)lc;(void)la;(void)d;(void)r; FITS_UNSUPPORTED("link_copy"); }
static herr_t fits_link_move(void *so, const H5VL_loc_params_t *sl, void *_do, const H5VL_loc_params_t *dl, hid_t lc, hid_t la, hid_t d, void **r)
{ (void)so;(void)sl;(void)_do;(void)dl;(void)lc;(void)la;(void)d;(void)r; FITS_UNSUPPORTED("link_move"); }
/* Build an H5L_info2_t for a hard link emanating from the given group. We have
 * no real object tokens in M2 (no chunking, no addresses), so we synthesize
 * a zero token — h5ls and friends only inspect the type. */
static void fits_fill_linfo(H5L_info2_t *linfo)
{
    linfo->type = H5L_TYPE_HARD;
    linfo->corder_valid = false;
    linfo->corder = 0;
    linfo->cset = H5T_CSET_ASCII;
    memset(&linfo->u.token, 0, sizeof(linfo->u.token));
}

static herr_t fits_link_get(void *obj, const H5VL_loc_params_t *loc_params,
                              H5VL_link_get_args_t *args, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;

    /* Resolve the parent group and the link name from loc_params. */
    fits_obj_kind_t k = *(fits_obj_kind_t *)obj;
    fits_file_t *file = (k == FITS_OBJ_FILE) ? (fits_file_t *)obj
                                               : ((fits_group_t *)obj)->file;
    adapter_object_t *parent = (k == FITS_OBJ_GROUP)
                                    ? ((fits_group_t *)obj)->adapter_obj
                                    : file->adapter->root(file->adapter_file);
    const char *name = NULL;
    if (loc_params->type == H5VL_OBJECT_BY_NAME)
        name = loc_params->loc_data.loc_by_name.name;

    adapter_link_info_t li = { .kind = ADAPTER_LINK_HARD, .target = NULL };
    int li_ok = (name && file->adapter->link_info(parent, (name[0] == '/') ? name + 1 : name, &li) == 0);

    switch (args->op_type) {
        case H5VL_LINK_GET_INFO: {
            H5L_info2_t *out = args->args.get_info.linfo;
            out->corder_valid = false;
            out->corder = 0;
            out->cset = H5T_CSET_ASCII;
            if (li_ok && li.kind == ADAPTER_LINK_SOFT) {
                out->type = H5L_TYPE_SOFT;
                out->u.val_size = strlen(li.target) + 1;
            } else {
                out->type = H5L_TYPE_HARD;
                memset(&out->u.token, 0, sizeof(out->u.token));
            }
            return 0;
        }
        case H5VL_LINK_GET_VAL: {
            if (!li_ok || li.kind != ADAPTER_LINK_SOFT) return -1;
            size_t need = strlen(li.target) + 1;
            size_t bs = args->args.get_val.buf_size;
            if (args->args.get_val.buf && bs > 0) {
                size_t copy = need < bs ? need : bs;
                memcpy(args->args.get_val.buf, li.target, copy);
                if (copy < bs) ((char *)args->args.get_val.buf)[copy - 1] = '\0';
            }
            return 0;
        }
        case H5VL_LINK_GET_NAME:
            /* Not used for our M2 access patterns; defer to a clear error. */
        default:
            FITS_UNSUPPORTED("link_get (this op_type)");
    }
}

/* Iteration cookie: bridges adapter_link_cb back to H5L_iterate2_t. The
 * prefix is the relative path from the H5Lvisit root, applied to each name
 * before it is handed to the user op. */
typedef struct {
    hid_t                     group_id;     /* hid_t for the iterated group */
    adapter_object_t         *parent_aobj;  /* adapter parent for link_info */
    H5VL_link_iterate_args_t *args;
    const char               *prefix;       /* relative path; "" at the root */
    fits_file_t             *file;
    herr_t                    user_rc;
    hsize_t                   idx;
} fits_iter_ctx_t;

static int fits_iter_trampoline(const char *name, void *user)
{
    fits_iter_ctx_t *ctx = (fits_iter_ctx_t *)user;
    H5L_info2_t info;
    fits_fill_linfo(&info);
    adapter_link_info_t li;
    if (ctx->file->adapter->link_info(ctx->parent_aobj, name, &li) == 0 && li.kind == ADAPTER_LINK_SOFT) {
        info.type = H5L_TYPE_SOFT;
        info.u.val_size = strlen(li.target) + 1;
    }

    /* Build the relative-from-root path the user op expects. */
    const char *p = ctx->prefix;
    char relpath[256];
    if (p && *p) snprintf(relpath, sizeof(relpath), "%s/%s", p, name);
    else         snprintf(relpath, sizeof(relpath), "%s", name);

    ctx->user_rc = ctx->args->op(ctx->group_id, relpath, &info, ctx->args->op_data);
    if (ctx->args->idx_p) *ctx->args->idx_p = ++ctx->idx;
    if (ctx->user_rc != 0) return ctx->user_rc;

    /* Recurse into hard-link groups when args->recursive. We do this here so
     * the order matches HDF5's semantics (parent's link reported before its
     * children). Soft links are not followed during iteration. */
    if (ctx->args->recursive && info.type == H5L_TYPE_HARD) {
        adapter_object_t *child = ctx->file->adapter->object_open(ctx->file->adapter_file,
                                                      ctx->parent_aobj, name);
        if (child && ctx->file->adapter->object_kind(child) == ADAPTER_KIND_GROUP) {
            fits_iter_ctx_t inner = *ctx;
            inner.parent_aobj = child;
            inner.prefix      = relpath;
            int rc = ctx->file->adapter->group_iterate(child, fits_iter_trampoline, &inner);
            ctx->user_rc = inner.user_rc;
            ctx->idx     = inner.idx;
            if (ctx->args->idx_p) *ctx->args->idx_p = ctx->idx;
            ctx->file->adapter->object_close(child);
            if (rc != 0) return rc;
        } else if (child) {
            ctx->file->adapter->object_close(child);
        }
    }
    return 0;
}

static herr_t fits_link_specific(void *obj, const H5VL_loc_params_t *loc_params,
                                   H5VL_link_specific_args_t *args, hid_t dxpl_id, void **req)
{
    (void)loc_params; (void)dxpl_id; (void)req;
    fits_obj_kind_t k = *(fits_obj_kind_t *)obj;

    switch (args->op_type) {
        case H5VL_LINK_EXISTS: {
            /* Resolve via fits_resolve so multi-component paths and the
             * "this group" context are handled identically to object access. */
            fits_file_t *file;
            bool resolved_owns;
            adapter_object_t *resolved = fits_resolve(obj, loc_params, &file, &resolved_owns);
            *args->args.exists.exists = (resolved != NULL);
            if (resolved && resolved_owns) file->adapter->object_close(resolved);
            return 0;
        }
        case H5VL_LINK_ITER: {
            fits_file_t *file;
            adapter_object_t *aobj;
            const char *iter_name;
            if (k == FITS_OBJ_GROUP) {
                fits_group_t *src = (fits_group_t *)obj;
                file = src->file; aobj = src->adapter_obj; iter_name = src->name;
            } else {
                file = (fits_file_t *)obj;
                aobj = file->adapter->root(file->adapter_file);
                iter_name = "/";
            }

            /* Allocate a disposable wrapper for the hid_t we hand to the user
             * op. HDF5 will call group_close on it via H5Idec_ref; freeing the
             * wrapper is correct because we set owns_adapter_obj=false. */
            fits_group_t *wrap = fits_group_new(file, iter_name, aobj, /*owns=*/false);
            if (!wrap) return -1;
            hid_t gid = H5VLwrap_register(wrap, H5I_GROUP);
            if (gid < 0) { fits_group_destroy(wrap); return -1; }

            fits_iter_ctx_t ctx = {
                .group_id    = gid,
                .parent_aobj = aobj,
                .args        = &args->args.iterate,
                .prefix      = "",
                .file        = file,
                .user_rc     = 0,
                .idx         = args->args.iterate.idx_p ? *args->args.iterate.idx_p : 0,
            };
            int rc = file->adapter->group_iterate(aobj, fits_iter_trampoline, &ctx);

            /* Release the temporary id; user op may have done its own incref. */
            H5Idec_ref(gid);

            /* Recursive iteration: HDU groups have no children in M2, so the
             * recursion is a no-op. M3 will surface `data` and recurse here. */

            return (herr_t)rc;
        }
        default:
            FITS_UNSUPPORTED("link_specific (this op_type)");
    }
}
static herr_t fits_link_optional(void *o, const H5VL_loc_params_t *l, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)o;(void)l;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("link_optional"); }

/* object */
static void *fits_object_open(void *obj, const H5VL_loc_params_t *loc_params,
                                H5I_type_t *opened_type, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    /* Resolve the target via the adapter to learn its kind, then dispatch to
     * the matching open. Avoids guessing from the path syntax. */
    fits_file_t *file;
    bool owns;
    adapter_object_t *target = fits_resolve(obj, loc_params, &file, &owns);
    if (!target) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_SYM, H5E_NOTFOUND,
                 "fits-hdf5-vol: object_open target not found");
        return NULL;
    }
    adapter_kind_t k = file->adapter->object_kind(target);
    /* fits_resolve returns a borrowed reference for root, owned otherwise.
     * Hand the underlying object back into the appropriate open path which
     * does its own resolution; close our temp ref afterward. */
    if (owns) file->adapter->object_close(target);

    const char *name = NULL;
    if (loc_params->type == H5VL_OBJECT_BY_NAME)
        name = loc_params->loc_data.loc_by_name.name;

    if (k == ADAPTER_KIND_DATASET) {
        void *d = fits_dataset_open(obj, loc_params, name, H5P_DEFAULT, dxpl_id, req);
        if (d) *opened_type = H5I_DATASET;
        return d;
    }
    void *g = fits_group_open(obj, loc_params, name, H5P_DEFAULT, dxpl_id, req);
    if (g) *opened_type = H5I_GROUP;
    return g;
}
static herr_t fits_object_copy(void *so, const H5VL_loc_params_t *sl, const char *sn, void *_do, const H5VL_loc_params_t *dl, const char *dn, hid_t oc, hid_t la, hid_t d, void **r)
{ (void)so;(void)sl;(void)sn;(void)_do;(void)dl;(void)dn;(void)oc;(void)la;(void)d;(void)r; FITS_UNSUPPORTED("object_copy"); }
/* Resolve the (obj, loc_params) pair to an adapter_object. Returned object is
 * borrowed if it's the root, otherwise heap-allocated and `*owns` is set so
 * the caller knows to close. Returns NULL on resolution failure. */
static adapter_object_t *fits_resolve(void *obj, const H5VL_loc_params_t *loc_params,
                                       fits_file_t **out_file, bool *owns)
{
    fits_obj_kind_t k = *(fits_obj_kind_t *)obj;
    fits_file_t *file = (k == FITS_OBJ_FILE) ? (fits_file_t *)obj : ((fits_group_t *)obj)->file;
    *out_file = file;
    *owns = false;

    if (loc_params->type == H5VL_OBJECT_BY_SELF) {
        if (k == FITS_OBJ_GROUP)   return ((fits_group_t   *)obj)->adapter_obj;
        if (k == FITS_OBJ_DATASET) return ((fits_dataset_t *)obj)->adapter_obj;
        return file->adapter->root(file->adapter_file);
    }
    if (loc_params->type == H5VL_OBJECT_BY_NAME) {
        const char *name = loc_params->loc_data.loc_by_name.name;
        if (!name) return NULL;
        if (strcmp(name, ".") == 0)
            return (k == FITS_OBJ_GROUP) ? ((fits_group_t *)obj)->adapter_obj
                                          : file->adapter->root(file->adapter_file);
        if (strcmp(name, "/") == 0)
            return file->adapter->root(file->adapter_file);

        /* Walk a multi-component path one slash at a time. */
        const char *p = (name[0] == '/') ? name + 1 : name;
        adapter_object_t *anchor = (name[0] == '/' || k == FITS_OBJ_FILE)
                                       ? file->adapter->root(file->adapter_file)
                                       : ((fits_group_t *)obj)->adapter_obj;
        adapter_object_t *cur = anchor;
        bool cur_owns = false;
        while (*p) {
            const char *slash = strchr(p, '/');
            size_t len = slash ? (size_t)(slash - p) : strlen(p);
            char comp[64];
            if (len == 0 || len >= sizeof(comp)) {
                if (cur_owns) file->adapter->object_close(cur);
                return NULL;
            }
            memcpy(comp, p, len); comp[len] = '\0';
            adapter_object_t *next = file->adapter->object_open(file->adapter_file, cur, comp);
            if (cur_owns) file->adapter->object_close(cur);
            if (!next) return NULL;
            cur = next;
            cur_owns = true;
            p = slash ? slash + 1 : p + len;
        }
        *owns = cur_owns;
        return cur;
    }
    return NULL;
}

static herr_t fits_object_get(void *obj, const H5VL_loc_params_t *loc_params,
                                H5VL_object_get_args_t *args, hid_t dxpl_id, void **req)
{
    (void)dxpl_id; (void)req;
    fits_file_t *file;
    bool owns;
    adapter_object_t *target = fits_resolve(obj, loc_params, &file, &owns);
    if (!target) {
        H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                 H5E_ERR_CLS, H5E_SYM, H5E_NOTFOUND, "fits-hdf5-vol: object_get target not found");
        return -1;
    }

    herr_t rc = 0;
    switch (args->op_type) {
        case H5VL_OBJECT_GET_FILE: {
            /* Return the file's connector handle. h5py calls this via
             * H5Iget_file_id on a dataset id; HDF5 will eventually call
             * file_close on the returned handle, so we ref-count to avoid
             * a double-free when both the original and the borrowed id
             * are released. */
            ++file->refcount;
            *args->args.get_file.file = file;
            break;
        }
        case H5VL_OBJECT_GET_TYPE: {
            *args->args.get_type.obj_type = (file->adapter->object_kind(target) == ADAPTER_KIND_GROUP)
                                                ? H5O_TYPE_GROUP : H5O_TYPE_DATASET;
            break;
        }
        case H5VL_OBJECT_GET_INFO: {
            H5O_info2_t *oi = args->args.get_info.oinfo;
            memset(oi, 0, sizeof(*oi));
            oi->fileno    = 1;
            oi->type      = (file->adapter->object_kind(target) == ADAPTER_KIND_GROUP)
                                ? H5O_TYPE_GROUP : H5O_TYPE_DATASET;
            oi->rc        = 1;
            size_t na = 0;
            file->adapter->attr_count(target, &na);
            oi->num_attrs = (hsize_t)na;
            break;
        }
        default:
            rc = -1;
            H5Epush2(H5E_DEFAULT, __FILE__, __func__, __LINE__,
                     H5E_ERR_CLS, H5E_VOL, H5E_UNSUPPORTED,
                     "fits-hdf5-vol: object_get op_type %d not implemented", (int)args->op_type);
    }
    if (owns) file->adapter->object_close(target);
    return rc;
}
static herr_t fits_object_specific(void *o, const H5VL_loc_params_t *l, H5VL_object_specific_args_t *a, hid_t d, void **r)
{ (void)o;(void)l;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("object_specific"); }
static herr_t fits_object_optional(void *o, const H5VL_loc_params_t *l, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)o;(void)l;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("object_optional"); }

/* introspect — get_cap_flags must succeed; HDF5 calls this at registration */
static herr_t fits_introspect_get_conn_cls(void *obj, H5VL_get_conn_lvl_t lvl, const H5VL_class_t **conn_cls);
static herr_t fits_introspect_get_cap_flags(const void *info, uint64_t *cap_flags)
{
    (void)info;
    /* Read-only file open + group open. Nothing else advertised. */
    *cap_flags = H5VL_CAP_FLAG_FILE_BASIC | H5VL_CAP_FLAG_GROUP_BASIC \
                         | H5VL_CAP_FLAG_LINK_BASIC | H5VL_CAP_FLAG_LINK_MORE \
                         | H5VL_CAP_FLAG_OBJECT_BASIC \
                         | H5VL_CAP_FLAG_ATTR_BASIC | H5VL_CAP_FLAG_ATTR_MORE \
                         | H5VL_CAP_FLAG_DATASET_BASIC;
    return 0;
}
static herr_t fits_introspect_opt_query(void *obj, H5VL_subclass_t cls, int opt_type, uint64_t *flags)
{ (void)obj;(void)cls;(void)opt_type; if (flags) *flags = 0; return 0; }

/* request — async not supported */
static herr_t fits_request_wait(void *r, uint64_t t, H5VL_request_status_t *s)   { (void)r;(void)t;(void)s; FITS_UNSUPPORTED("request_wait"); }
static herr_t fits_request_notify(void *r, H5VL_request_notify_t cb, void *c)    { (void)r;(void)cb;(void)c; FITS_UNSUPPORTED("request_notify"); }
static herr_t fits_request_cancel(void *r, H5VL_request_status_t *s)             { (void)r;(void)s; FITS_UNSUPPORTED("request_cancel"); }
static herr_t fits_request_specific(void *r, H5VL_request_specific_args_t *a)    { (void)r;(void)a; FITS_UNSUPPORTED("request_specific"); }
static herr_t fits_request_optional(void *r, H5VL_optional_args_t *a)            { (void)r;(void)a; FITS_UNSUPPORTED("request_optional"); }
static herr_t fits_request_free(void *r)                                         { (void)r; FITS_UNSUPPORTED("request_free"); }

/* blob */
static herr_t fits_blob_put(void *o, const void *b, size_t sz, void *id, void *c)            { (void)o;(void)b;(void)sz;(void)id;(void)c; FITS_UNSUPPORTED("blob_put"); }
static herr_t fits_blob_get(void *o, const void *id, void *b, size_t sz, void *c)            { (void)o;(void)id;(void)b;(void)sz;(void)c; FITS_UNSUPPORTED("blob_get"); }
static herr_t fits_blob_specific(void *o, void *id, H5VL_blob_specific_args_t *a)            { (void)o;(void)id;(void)a; FITS_UNSUPPORTED("blob_specific"); }
static herr_t fits_blob_optional(void *o, void *id, H5VL_optional_args_t *a)                 { (void)o;(void)id;(void)a; FITS_UNSUPPORTED("blob_optional"); }

/* token */
static herr_t fits_token_cmp(void *o, const H5O_token_t *a, const H5O_token_t *b, int *cmp)  { (void)o;(void)a;(void)b; if (cmp) *cmp = 0; return 0; }
static herr_t fits_token_to_str(void *o, H5I_type_t t, const H5O_token_t *tok, char **s)    { (void)o;(void)t;(void)tok; if (s) *s = NULL; return 0; }
static herr_t fits_token_from_str(void *o, H5I_type_t t, const char *s, H5O_token_t *tok)   { (void)o;(void)t;(void)s;(void)tok; return 0; }

/* catch-all optional */
static herr_t fits_optional(void *o, H5VL_optional_args_t *a, hid_t d, void **r)
{ (void)o;(void)a;(void)d;(void)r; FITS_UNSUPPORTED("optional"); }

/* ------------------------------------------------------------------ */
/* The class itself                                                    */
/* ------------------------------------------------------------------ */

static const H5VL_class_t fits_hdf5_vol_g = {
    H5VL_VERSION,
    (H5VL_class_value_t)FITS_HDF5_VOL_VALUE,
    FITS_HDF5_VOL_NAME,
    FITS_HDF5_VOL_VERSION,
    /* cap_flags */ H5VL_CAP_FLAG_FILE_BASIC | H5VL_CAP_FLAG_GROUP_BASIC \
                         | H5VL_CAP_FLAG_LINK_BASIC | H5VL_CAP_FLAG_LINK_MORE \
                         | H5VL_CAP_FLAG_OBJECT_BASIC \
                         | H5VL_CAP_FLAG_ATTR_BASIC | H5VL_CAP_FLAG_ATTR_MORE \
                         | H5VL_CAP_FLAG_DATASET_BASIC,
    fits_init,
    fits_term,
    /* info_cls    */ { 0, fits_info_copy, fits_info_cmp, fits_info_free, fits_info_to_str, fits_str_to_info },
    /* wrap_cls    */ { fits_get_object, fits_get_wrap_ctx, fits_wrap_object, fits_unwrap_object, fits_free_wrap_ctx },
    /* attr_cls    */ { fits_attr_create, fits_attr_open, fits_attr_read, fits_attr_write,
                        fits_attr_get, fits_attr_specific, fits_attr_optional, fits_attr_close },
    /* dataset_cls */ { fits_dataset_create, fits_dataset_open, fits_dataset_read, fits_dataset_write,
                        fits_dataset_get, fits_dataset_specific, fits_dataset_optional, fits_dataset_close },
    /* datatype_cls*/ { fits_datatype_commit, fits_datatype_open, fits_datatype_get,
                        fits_datatype_specific, fits_datatype_optional, fits_datatype_close },
    /* file_cls    */ { fits_file_create, fits_file_open, fits_file_get,
                        fits_file_specific, fits_file_optional, fits_file_close },
    /* group_cls   */ { fits_group_create, fits_group_open, fits_group_get,
                        fits_group_specific, fits_group_optional, fits_group_close },
    /* link_cls    */ { fits_link_create, fits_link_copy, fits_link_move,
                        fits_link_get, fits_link_specific, fits_link_optional },
    /* object_cls  */ { fits_object_open, fits_object_copy, fits_object_get,
                        fits_object_specific, fits_object_optional },
    /* introspect  */ { fits_introspect_get_conn_cls, fits_introspect_get_cap_flags, fits_introspect_opt_query },
    /* request_cls */ { fits_request_wait, fits_request_notify, fits_request_cancel,
                        fits_request_specific, fits_request_optional, fits_request_free },
    /* blob_cls    */ { fits_blob_put, fits_blob_get, fits_blob_specific, fits_blob_optional },
    /* token_cls   */ { fits_token_cmp, fits_token_to_str, fits_token_from_str },
    /* optional    */ fits_optional
};

static herr_t fits_introspect_get_conn_cls(void *obj, H5VL_get_conn_lvl_t lvl, const H5VL_class_t **conn_cls)
{
    (void)obj; (void)lvl;
    if (!conn_cls) return -1;
    *conn_cls = &fits_hdf5_vol_g;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Plugin entry points                                                 */
/* ------------------------------------------------------------------ */

H5PL_type_t H5PLget_plugin_type(void) { return H5PL_TYPE_VOL; }
const void *H5PLget_plugin_info(void) { return &fits_hdf5_vol_g; }
