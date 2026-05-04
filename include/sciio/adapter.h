/*
 * sciio-vol Format-Adapter API.
 *
 * The connector calls every adapter through a vtable: each adapter exports a
 * single `const sciio_adapter_t` instance. The connector iterates a registry
 * of these on H5Fopen, runs probe(), picks the highest-confidence match, and
 * dispatches every subsequent call through that adapter's function pointers.
 *
 * v1 ships exactly one adapter (FITS). The vtable is stable surface so DICOM
 * / GRIB / NetCDF-3 adapters can be added in v2 without changing the
 * connector or any HDF5 application that uses sciio-vol.
 *
 * Conventions:
 *   - All handles are opaque to the connector (struct fwd-decls).
 *   - Every per-file/per-object call takes a handle the adapter created;
 *     no global adapter state.
 *   - Status convention: 0 on success, -1 on failure (status int).
 *     Allocating calls return NULL on failure.
 *   - Caller-owned strings on input; adapter copies if it needs to retain.
 *   - The root object is owned by the file and must NOT be passed to
 *     object_close. All other adapter_object_t* are caller-owned.
 *   - For H5T_VLEN reads, each row's hvl_t.p is malloc'd by the adapter; the
 *     HDF5 caller reclaims via H5Treclaim.
 *   - For variable-length string attributes, the caller frees via
 *     adapter->free_string. For rank-1 string arrays, free each element.
 */

#ifndef SCIIO_ADAPTER_H
#define SCIIO_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on every breaking change to this header. Additive changes (new
 * function-pointer slots appended to sciio_adapter_t) increment the minor;
 * struct-layout-breaking changes increment the major. */
#define SCIIO_ADAPTER_API_VERSION_MAJOR 1
#define SCIIO_ADAPTER_API_VERSION_MINOR 0

/* ------------------------------------------------------------------ */
/* Type system                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    ADAPTER_KIND_GROUP,
    ADAPTER_KIND_DATASET
} adapter_kind_t;

typedef enum {
    ADAPTER_T_INT,
    ADAPTER_T_UINT,
    ADAPTER_T_FLOAT,
    ADAPTER_T_BOOL,
    ADAPTER_T_STRING,    /* fixed-length when size>0; vlen UTF-8 when size==0 */
    ADAPTER_T_COMPLEX,   /* compound { float re; float im; } in 2× size */
    ADAPTER_T_OPAQUE,    /* fixed-size byte blob, no interpretation */
    ADAPTER_T_COMPOUND   /* see adapter_type_t.extra → adapter_compound_info_t */
} adapter_type_class_t;

typedef struct {
    adapter_type_class_t cls;
    size_t               size;        /* bytes per element (STRING vlen: 0) */
    /* Element-shape extension. When array_rank>0, each dataset/attr element
     * is an HDF5 array datatype of this shape over the base scalar type
     * (FITS: TFORM repeat>1 + optional TDIMn). Mutually exclusive with
     * is_vlen=1 in v1. */
    int                  array_rank;
    uint64_t             array_dims[8];
    /* Variable-length flag. When set, each element is an HDF5 vlen of the
     * base scalar type (FITS: TFORM 'P'/'Q'). */
    int                  is_vlen;
    /* When cls == ADAPTER_T_COMPOUND, points to an adapter_compound_info_t
     * owned by the adapter and valid for the file's lifetime. */
    const void          *extra;
} adapter_type_t;

typedef struct {
    int      rank;            /* 0 = scalar */
    uint64_t dims[8];
} adapter_space_t;

typedef struct {
    const char     *name;     /* owned by adapter; valid until object closed */
    adapter_type_t  type;
    adapter_space_t space;
} adapter_attr_info_t;

typedef struct {
    const char     *name;     /* owned by adapter */
    adapter_type_t  type;
    size_t          offset;   /* byte offset within compound row */
} adapter_compound_member_t;

typedef struct {
    int    n_members;
    size_t row_size;
    const adapter_compound_member_t *members;
} adapter_compound_info_t;

/* ------------------------------------------------------------------ */
/* Opaque handles                                                      */
/* ------------------------------------------------------------------ */

typedef struct adapter_file_s   adapter_file_t;
typedef struct adapter_object_s adapter_object_t;

typedef struct {
    const char *name;        /* short adapter name, e.g. "fits"             */
    int         confidence;  /* 0..100; highest wins; 0 means "no match"     */
} adapter_probe_result_t;

typedef enum {
    ADAPTER_LINK_HARD,
    ADAPTER_LINK_SOFT
} adapter_link_kind_t;

typedef struct {
    adapter_link_kind_t kind;
    /* For SOFT links: target path string, owned by adapter, valid until
     * the parent group is closed. NULL for HARD links. */
    const char *target;
} adapter_link_info_t;

typedef int (*adapter_link_cb)(const char *name, void *user);

/* ------------------------------------------------------------------ */
/* The vtable                                                          */
/* ------------------------------------------------------------------ */

typedef struct sciio_adapter_s {
    /* Identity. `name` is short and lowercase, used in error messages and
     * in registry lookups. */
    const char *name;

    /* api_version_major must equal SCIIO_ADAPTER_API_VERSION_MAJOR at the
     * call site, otherwise the connector refuses to register the adapter. */
    unsigned    api_version_major;
    unsigned    api_version_minor;

    /* === Lifecycle ============================================== */

    /* Probe the file for a format match. May read up to 16 KiB. Returns 0
     * with out filled (confidence==0 means "not my format"). Returns -1 on
     * I/O error. The connector iterates adapters and picks highest
     * confidence > 0. */
    int   (*probe)(const char *path, adapter_probe_result_t *out);

    /* Open a file. flags currently must be 0 (read-only); other values are
     * reserved. Returns NULL on failure. */
    adapter_file_t *(*open)(const char *path, unsigned flags);

    void  (*close)(adapter_file_t *f);

    /* === Object navigation ===================================== */

    /* The root group, owned by the file. Must NOT be passed to object_close. */
    adapter_object_t *(*root)(adapter_file_t *f);

    /* Open a child of `parent` by name. Caller must close. NULL if absent. */
    adapter_object_t *(*object_open)(adapter_file_t   *f,
                                     adapter_object_t *parent,
                                     const char       *name);

    void (*object_close)(adapter_object_t *o);

    adapter_kind_t (*object_kind)(const adapter_object_t *o);

    int (*group_iterate)(adapter_object_t *group,
                         adapter_link_cb   cb,
                         void             *user);

    int (*link_info)(adapter_object_t       *parent,
                     const char             *name,
                     adapter_link_info_t    *out);

    /* === Attributes =========================================== */

    int (*attr_count)(adapter_object_t *o, size_t *out);

    int (*attr_info_by_idx)(adapter_object_t    *o,
                            size_t               idx,
                            adapter_attr_info_t *out);

    /* Read a named attribute. dst layout depends on type:
     *   - scalar numeric / bool        -> dst = pointer to one element
     *   - scalar vlen string           -> dst = char**, freed via free_string
     *   - rank-1 vlen string array     -> dst = char*[count], each freed
     *   - rank-1 fixed-length numeric  -> dst = contiguous element buffer
     */
    int (*attr_read_by_name)(adapter_object_t *o,
                             const char       *name,
                             void             *dst);

    /* Free a single string buffer returned via attr_read_by_name. */
    void (*free_string)(char *s);

    /* === Dataset I/O ========================================== */

    int (*dataset_space)(adapter_object_t *ds, adapter_space_t *out);
    int (*dataset_type )(adapter_object_t *ds, adapter_type_t  *out);

    /* Read a hyperslab. start/count have rank elements; stride and block
     * may be NULL meaning "1". Coordinates are in HDF5 (C) order — the
     * adapter translates to its underlying convention.
     *
     * For VLEN element types, dst is a buffer of `count[0]` HDF5 hvl_t
     * descriptors; each .p is malloc'd by the adapter and reclaimed by the
     * caller via H5Treclaim. */
    int (*dataset_read)(adapter_object_t *ds,
                        const uint64_t *start,
                        const uint64_t *count,
                        const uint64_t *stride,    /* may be NULL */
                        const uint64_t *block,     /* may be NULL */
                        void           *dst);

} sciio_adapter_t;

#ifdef __cplusplus
}
#endif
#endif /* SCIIO_ADAPTER_H */
