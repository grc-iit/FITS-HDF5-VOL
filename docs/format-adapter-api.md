# fits-hdf5-vol Format-Adapter API

Public surface: `include/fits_hdf5/adapter.h`. This is the abstract contract a
format adapter implements. The connector calls every adapter through a
**vtable** (`fits_adapter_t`). v1 ships a single FITS adapter; the API
shape is co-designed against DICOM and GRIB so that adding either later
won't require a connector change.

This document is the canonical reference for adapter authors.

---

## 1. Architecture

```
HDF5 application
       │  (HDF5 public API, unchanged)
       ▼
HDF5 library
       │  (VOL callbacks)
       ▼
fits-hdf5-vol connector  (src/fits_hdf5_vol_connector.c)
       │
       │  Dispatch:
       │    1. On H5Fopen, walk the registry, call each adapter's probe(),
       │       pick the highest-confidence match.
       │    2. Stash the chosen adapter pointer on the connector's per-file
       │       state. Every subsequent call dispatches through it.
       │
       ▼
adapter vtable        (fits_adapter_t — defined in adapter.h)
       │
       ▼
Adapter implementation: probe / open / close / root / object_open /
                        object_close / object_kind / group_iterate /
                        link_info / attr_count / attr_info_by_idx /
                        attr_read_by_name / free_string / dataset_space /
                        dataset_type / dataset_read
       │
       ▼
Underlying format library  (CFITSIO for FITS; DCMTK for DICOM; eccodes for GRIB)
```

## 2. Versioning

```c
#define FITS_ADAPTER_API_VERSION_MAJOR 1
#define FITS_ADAPTER_API_VERSION_MINOR 0
```

- **Major version** bumps on layout-breaking changes to `fits_adapter_t`
  (insertion in the middle, deletion, signature change). Adapters built
  against an older major are refused at registration time.
- **Minor version** bumps when new function pointers are appended to the
  end of `fits_adapter_t`. Existing adapters keep working; the connector
  checks for NULL before calling new slots.
- All other typedefs (`adapter_type_t`, `adapter_space_t`, etc.) are
  governed by the same version.

Adapters initialize:

```c
const fits_adapter_t my_adapter = {
    .name              = "my-format",
    .api_version_major = FITS_ADAPTER_API_VERSION_MAJOR,
    .api_version_minor = FITS_ADAPTER_API_VERSION_MINOR,
    /* function pointers ... */
};
```

## 3. The vtable

Every adapter exports exactly one `const fits_adapter_t` and is added to
`include/fits_hdf5/registry.h` (v1 has compile-time registration; v2 may add
runtime registration without breaking existing adapters).

### Lifecycle

| Slot | Purpose |
|---|---|
| `int probe(const char *path, adapter_probe_result_t *out)` | Read up to 16 KiB to decide whether `path` is this adapter's format. `out->confidence` ∈ [0, 100]; 0 means "not my format". On I/O error returns -1. Adapters MUST NOT throw on non-matching files; that's the dispatch path. |
| `adapter_file_t *open(const char *path, unsigned flags)` | Open `path` read-only (`flags == 0`; other values reserved). Returns NULL on failure. Adapter is free to define its own concrete `struct adapter_file_s`. |
| `void close(adapter_file_t *f)` | Releases everything `open` allocated, including all `adapter_object_t` handles still considered "borrowed" (e.g. the root). |

### Object navigation

| Slot | Purpose |
|---|---|
| `adapter_object_t *root(adapter_file_t *f)` | The root group. **Owned by the file** — caller must NOT pass it to `object_close`. |
| `adapter_object_t *object_open(adapter_file_t *f, adapter_object_t *parent, const char *name)` | Open a single-component child of `parent` by name. Caller closes via `object_close`. NULL if not found. |
| `void object_close(adapter_object_t *o)` | Free a non-root object. |
| `adapter_kind_t object_kind(const adapter_object_t *o)` | `ADAPTER_KIND_GROUP` or `ADAPTER_KIND_DATASET`. |
| `int group_iterate(adapter_object_t *g, adapter_link_cb cb, void *user)` | Yields immediate children of `g` in stable order. cb returns 0 to continue, non-zero to stop early. |
| `int link_info(adapter_object_t *parent, const char *name, adapter_link_info_t *out)` | Fill `out` for a child link. `kind` is HARD or SOFT; SOFT links have a `target` path string owned by the adapter, valid until `parent` closes. |

### Attributes

| Slot | Purpose |
|---|---|
| `int attr_count(adapter_object_t *o, size_t *out)` | Number of attributes attached to `o`. |
| `int attr_info_by_idx(adapter_object_t *o, size_t idx, adapter_attr_info_t *out)` | Name + type + space for the `idx`-th attribute. `name` is owned by the adapter and valid until `o` closes. |
| `int attr_read_by_name(adapter_object_t *o, const char *name, void *dst)` | Read the named attribute into `dst`. Layout depends on type — see §4 below. |
| `void free_string(char *s)` | Free a single string buffer returned via `attr_read_by_name`. |

### Datasets

| Slot | Purpose |
|---|---|
| `int dataset_space(adapter_object_t *ds, adapter_space_t *out)` | Rank + dims, in HDF5 (C) order. |
| `int dataset_type(adapter_object_t *ds, adapter_type_t *out)` | Element type. May set `array_rank>0` (HDF5 array element), `is_vlen=1` (HDF5 vlen element), or `cls=ADAPTER_T_COMPOUND` with `extra` pointing at an `adapter_compound_info_t`. |
| `int dataset_read(adapter_object_t *ds, const uint64_t *start, const uint64_t *count, const uint64_t *stride, const uint64_t *block, void *dst)` | Hyperslab read. `stride`/`block` may be NULL meaning 1. Coordinates in HDF5 (C) order. For VLEN element types, `dst` is `count[0]` HDF5 `hvl_t` descriptors; the adapter mallocs `.p`, the HDF5 caller reclaims via `H5Treclaim`. |

## 4. Type system

```c
typedef enum {
    ADAPTER_T_INT,        /* signed integer */
    ADAPTER_T_UINT,       /* unsigned integer */
    ADAPTER_T_FLOAT,      /* IEEE-754 float */
    ADAPTER_T_BOOL,       /* 1 byte, 0 or 1 */
    ADAPTER_T_STRING,     /* fixed-length when size>0; vlen UTF-8 when size==0 */
    ADAPTER_T_COMPLEX,    /* compound { float|double re; ... im; } */
    ADAPTER_T_OPAQUE,     /* fixed-size byte blob, no interpretation */
    ADAPTER_T_COMPOUND    /* extra → adapter_compound_info_t */
} adapter_type_class_t;
```

Plus shape modifiers:
- `array_rank>0`: each element is itself an HDF5 array of shape `array_dims[]` over the base scalar.
- `is_vlen=1`: each element is an HDF5 vlen of the base scalar. Mutually exclusive with `array_rank>0` in v1.

### Read buffer layouts

| Element type | `dst` shape per element |
|---|---|
| INT / UINT / FLOAT / BOOL / OPAQUE | `size` bytes |
| STRING fixed (`size>0`) | `size` chars, null-padded |
| STRING vlen (`size==0`) | `char *` (per element); freed via `free_string` |
| COMPLEX | `size` bytes (8 for f32-pair, 16 for f64-pair) |
| Array (`array_rank>0`) | product(`array_dims`) × base size |
| VLEN (`is_vlen=1`) | one `hvl_t` per element; `.p` malloc'd by adapter, reclaimed by HDF5 caller |
| COMPOUND | `extra->row_size` bytes; member layout per `extra->members[]` |

## 5. Conventions and contracts

### Status returns

- `int` slots: 0 success, -1 failure.
- Allocating slots return NULL on failure.
- On error, the adapter SHOULD log a single line to `stderr` describing what failed and skip the offending element rather than failing the whole open. Plan-level "no silent fallbacks" rule: every drop must be visible.

### String ownership

- Names returned via `adapter_attr_info_t` and `adapter_compound_member_t` are **owned by the adapter** and valid until the surrounding object is closed. Callers must not free them.
- String values returned via `attr_read_by_name` are **owned by the caller** and freed via `free_string`.
- For rank-1 string array attributes (e.g. FITS `__raw_header__`), `dst` is `char *[count]`; each element is independently `free_string`-able.

### Coordinate order

- `dataset_space` reports dims in **HDF5 C order** (last dim fastest-varying).
- `dataset_read` start/count/stride/block are in HDF5 C order.
- Adapters translate to their underlying convention internally (FITS uses Fortran order; the FITS adapter reverses axes at the CFITSIO boundary).

### Thread safety

- v1 makes no thread-safety guarantees. The connector serializes per-file operations through HDF5's own locking; adapters can assume single-threaded entry per file.
- Adapters MUST NOT keep global mutable state — every per-file/per-object call gets a handle the adapter created.

## 6. Paper review

The API was reviewed against two non-FITS formats. Both were validated as
implementable against the current shape without further API changes; only
the dispatch layer (now in place) was missing.

### 6.1 DICOM (medical imaging)

A single DICOM file is one image plus a flat list of metadata tags.

- `probe(path)` reads the first 132 bytes; offset 128 should hold `"DICM"`.
  Confidence 100.
- `open(path)` parses the dataset header; populates an internal handle.
- A multi-file DICOM **study** is exposed by passing a directory path:
  `open` enumerates the directory, builds a `/study/series/instance`
  hierarchy synthesized from per-file `StudyInstanceUID` etc.
- The single-file case maps to `/pixel_data` (a dataset) plus per-tag
  attributes. Sequence (SQ) VR types map to nested groups, where each
  group element is a tag-attributed sub-record.
- VR types we don't recognize (e.g. proprietary extensions) map to
  `ADAPTER_T_OPAQUE` with the byte length CFITSIO doesn't have an
  equivalent for — confirms `OPAQUE` was the right slot to keep in the
  enum.
- DICOM sub-types beyond our scalar list (Person Name, Date-Time, etc.)
  surface as fixed-length strings.

**Result:** the existing API is sufficient. No struct or signature
changes required.

### 6.2 GRIB (meteorological forecast records)

A GRIB file is a flat record stream, no inherent hierarchy. Each record
has a parameter ID, level, time, etc.

- `probe(path)` reads the first 4 bytes; should be `"GRIB"`. Confidence
  100. (GRIB-1 vs. GRIB-2 distinguished at byte offset 7 inside `open`.)
- `open(path)` scans the entire file once, building an internal index of
  records.
- Hierarchy is **synthesized**: e.g. `/<param>/<level>/<time>/data`. The
  adapter chooses any structure it likes; the connector doesn't care.
- `group_iterate` yields synthesized child names.
- Dataset shape and type come from the GRIB record's grid descriptor and
  packing.
- Records that share `(param, level, time)` from different forecast
  members can be stacked along an extra axis as the adapter sees fit.

**Result:** the existing API is sufficient. The "no a priori HDU
enumeration" pattern fits naturally because nothing in the API requires
the adapter to describe its hierarchy at `open` time — `group_iterate`
is called lazily.

## 7. Adapter author quick-start

A minimal adapter skeleton:

```c
#include "fits_hdf5/adapter.h"

struct adapter_file_s   { /* your concrete file state */ };
struct adapter_object_s { /* your concrete object state */ };

static int          myfmt_probe (const char *path, adapter_probe_result_t *out) { /* … */ }
static adapter_file_t *myfmt_open(const char *path, unsigned flags)              { /* … */ }
static void         myfmt_close(adapter_file_t *f)                                { /* … */ }
/* … all other slots … */

const fits_adapter_t fits_myfmt_adapter = {
    .name              = "myfmt",
    .api_version_major = FITS_ADAPTER_API_VERSION_MAJOR,
    .api_version_minor = FITS_ADAPTER_API_VERSION_MINOR,
    .probe             = myfmt_probe,
    .open              = myfmt_open,
    .close             = myfmt_close,
    /* … */
};
```

Then in `include/fits_hdf5/registry.h` add:

```c
extern const fits_adapter_t fits_myfmt_adapter;
```

And in `src/registry.c`:

```c
static const fits_adapter_t *fits_registry[] = {
    &fits_adapter,
    &fits_myfmt_adapter,    /* ← */
    NULL
};
```

That's the entire integration surface.

## 8. Reference implementation

The FITS adapter under `adapters/fits/fits_adapter.c` is the canonical
reference. It exercises every slot, including the trickier paths:

- Variable-length array column reads (`H5T_VLEN`)
- Multi-dimensional cell reads (`H5T_ARRAY`)
- Compound row-view dataset (`H5T_COMPOUND`)
- BSCALE/BZERO unsigned-int convention and general rescale
- Soft links (FITS EXTNAME aliases)
- The `__raw_header__` rank-1 vlen-string attribute

Its `static` internal structure (`AO_ROOT`, `AO_HDU_GROUP`,
`AO_COLUMNS_GROUP`, `AO_IMAGE_DATA`, `AO_COLUMN_DATA`, `AO_TABLE_DATA`)
is purely an implementation detail and not part of the public API.
