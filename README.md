# FITS-HDF5-VOL (sciio-vol)

A terminal HDF5 Virtual Object Layer (VOL) connector that lets unmodified
HDF5 applications read **FITS** files through the standard HDF5 API.
The library / connector is named `sciio-vol`; this repository
(`grc-iit/FITS-HDF5-VOL`) is its source home.

**Status:** v1.0.0-rc — M1 through M6 complete. Read-only. 64 ctest cases
pass against synthetic fixtures, the astropy public test corpus, and the
local NRAO `ftt4b` corpus. The C-level subset is also clean under
`-fsanitize=address,undefined` with leak detection enabled. The same
`libsciio_vol.so` runs unchanged on HDF5 1.14.x and 2.1.x. A permanent
VOL connector value from The HDF Group is pending; the current `510` is
provisional and may change before the v1.0.0 tag.

**Target HDF5:** ≥ 1.14.3 (built and validated against 2.1.x).
**Hard dep:** CFITSIO ≥ 4.0 (system install via pkg-config).
**License:** see [LICENSE](LICENSE).

## Quick start

```bash
# 1. Install deps (Ubuntu / Debian; HDF5 ≥ 1.14.3 from your distro is fine).
sudo apt install libcfitsio-dev libhdf5-dev cmake gcc

# 2. Build.
cmake -S . -B build
cmake --build build -j$(nproc)

# 3. Point HDF5 at the connector and read any FITS file with stock tools.
export HDF5_PLUGIN_PATH=$PWD/build
export HDF5_VOL_CONNECTOR=sciio
h5ls -r path/to/any.fits        # FITS now looks like HDF5
```

If your distro HDF5 is too old, see [Building HDF5 from source](#optional-building-hdf5-from-source).

## Capabilities at a glance

What an unmodified HDF5 app can do today against any FITS file:

- `H5Fopen` (read-only). Random Groups and non-FITS files cleanly rejected.
- Walk the HDU tree as `/HDU0`, `/HDU1`, … (`H5Lvisit2`, `h5ls -r`).
  `EXTNAME` keywords surface as soft links `/<EXTNAME>` → `/HDUn`.
- Read every header keyword as a typed HDF5 attribute (int / float / bool /
  string / complex / `COMMENT` / `HISTORY` / `HIERARCH` / `CONTINUE`,
  plus `__raw_header__` byte-exact card array). `H5Aiterate2`, `H5Aread` on
  any keyword, `TUNITn` exposed as `units` on per-column datasets.
- Read every image HDU's pixels: all BITPIX (8/16/32/64/-32/-64), the BZERO
  unsigned-int convention (uint16/32/64), general BSCALE/BZERO rescale
  (→ float64), hyperslab and point selections.
- Read every table HDU (ASCII + binary). Per-column view at
  `/HDUn/columns/<TTYPE>` and a row-view compound at `/HDUn/table`.
  Variable-length columns map to `H5T_VLEN`; `TDIMn` multi-D cells map to
  `H5T_ARRAY`.
- Tile-compressed image HDUs surface for introspection; `H5Dread` on them
  fails with a clear v2-deferred error.

What's deferred:
- Writing FITS — out of scope for v1. Sciio-vol is read-only by design.
  See `tools/h5_to_fits.c` for a separate one-way HDF5→FITS converter.
- Vlen members inside the compound row view (per-column vlen still works).
- Vlen-string columns (TFORM 'PA').
- WCS interpretation (keywords surfaced verbatim; geometry is the app's job).

## Build

### Prerequisites

```bash
# Ubuntu / Debian:
sudo apt install libcfitsio-dev libhdf5-dev cmake gcc
```

### Build sciio-vol

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Outputs: `build/libsciio_vol.so` plus the demo utilities (`fits_to_h5`,
`h5_to_fits`, `fits_compare`) under `build/`. The `.c` sources for the
utilities live in `tools/`; the binaries land in `build/` after the build.

### Smoke-test the install

```bash
ctest --test-dir build
# Expect: 100% tests passed, 64 / 64
```

### Optional: Building HDF5 from source

Only needed if your distro HDF5 is older than 1.14.3, or you want to
exercise sciio-vol against HDF5 2.1.x:

```bash
git clone https://github.com/HDFGroup/hdf5
git -C hdf5 checkout 2.1.1
cmake -S hdf5 -B hdf5/build \
    -DCMAKE_INSTALL_PREFIX=$HOME/opt/hdf5-2.1 \
    -DBUILD_SHARED_LIBS=ON \
    -DHDF5_BUILD_TOOLS=ON \
    -DBUILD_TESTING=OFF
cmake --build hdf5/build -j$(nproc)
cmake --install hdf5/build

# Then build sciio-vol against it:
cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/opt/hdf5-2.1
cmake --build build -j$(nproc)
LD_LIBRARY_PATH=$HOME/opt/hdf5-2.1/lib ctest --test-dir build
```

## Load the connector

Two ways. **Environment-variable form** — zero source change to your app:

```bash
export HDF5_PLUGIN_PATH=$PWD/build
export HDF5_VOL_CONNECTOR=sciio
# If you built HDF5 from source: export LD_LIBRARY_PATH=$HOME/opt/hdf5-2.1/lib

# Now any HDF5 program reads FITS as if it were HDF5:
h5ls -r some.fits
h5dump -A some.fits
```

**Programmatic FAPL form** — if you only want sciio-vol on specific files:

```c
hid_t vol = H5VLregister_connector_by_name("sciio", H5P_DEFAULT);
hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
H5Pset_vol(fapl, vol, NULL);
hid_t fid = H5Fopen("obs.fits", H5F_ACC_RDONLY, fapl);
```

## End-to-end example: convert a FITS file to HDF5 and back

`tools/` ships three small utilities. **`fits_to_h5`** uses sciio-vol on the
input side and the native HDF5 VOL on the output side. **`h5_to_fits`** does
the reverse direction with CFITSIO directly (sciio-vol is read-only). 
**`fits_compare`** does a byte-by-byte HDU-pixel comparison via CFITSIO.

A complete round-trip on a small public FITS file (the astropy
`HorseHead.fits`, 2-D BITPIX=16):

```bash
# Set up the environment once.
export HDF5_PLUGIN_PATH=$PWD/build

mkdir -p roundtrip
curl -L -o roundtrip/file007.fits \
    https://github.com/astropy/astropy-data/raw/main/galactic_center/gc_2mass_k.fits
# (any real FITS file works; substitute your own path if you have one.)

# 1. FITS → native HDF5 (via sciio-vol).
./build/fits_to_h5 roundtrip/file007.fits roundtrip/file007.h5

# 2. HDF5 → restored FITS (via CFITSIO).
./build/h5_to_fits roundtrip/file007.h5    roundtrip/restored_fits_from_hdf5.fits

# 3. Verify pixels are bit-exact.
./build/fits_compare roundtrip/file007.fits roundtrip/restored_fits_from_hdf5.fits
# → HDU0: rank 2|2  bitpix 16|16  dims [ 512 512 ] | [ 512 512 ]  ✓ pixels match (524288 bytes)
```

The intermediate `roundtrip/file007.h5` is a **native HDF5 file**. Inspect
it with stock HDF5 tools — sciio-vol is not needed:

```bash
h5ls -r roundtrip/file007.h5
# /                        Group
# /HDU0                    Group
# /HDU0/data               Dataset {512, 512}
```

Read it from Python with h5py:

```python
import h5py
with h5py.File("roundtrip/file007.h5", "r") as f:
    data = f["HDU0/data"][...]              # NumPy int16, shape (512, 512)
    bitpix = int(f["HDU0"].attrs["BITPIX"]) # 16
```

## Reading a FITS file directly through sciio-vol (no conversion)

You can skip the conversion entirely and treat FITS as live HDF5:

```python
import os, h5py
os.environ["HDF5_PLUGIN_PATH"]    = "/path/to/FITS-HDF5-VOL/build"
os.environ["HDF5_VOL_CONNECTOR"]  = "sciio"
# If you built HDF5 from source, also set LD_LIBRARY_PATH to its lib dir.

with h5py.File("some.fits", "r") as f:
    pixels = f["HDU0/data"][...]
    iden   = f["HDU1/columns/IDEN."][...]   # if HDU1 is a table
    naxis2 = int(f["HDU0"].attrs["NAXIS2"])
```

**h5py limitation (upstream):** h5py 3.16 builds with `H5_USE_110_API`,
which makes `f.keys()` route through the v1 link-iterate API that HDF5
forbids on non-native VOLs. Path-based access (`f["HDU0/data"]`) works
fine; only directory-style iteration is blocked. The same h5py needs to
be built from source against the same HDF5 install
(`HDF5_DIR=$HOME/opt/hdf5-2.1 pip install --no-binary=h5py h5py`),
otherwise its bundled HDF5 instance can't see hid_ts our connector
issues against your HDF5 instance.

## Repository layout

```
FITS-HDF5-VOL/
├── src/sciio_vol_connector.c     VOL callback layer
├── adapters/fits/fits_adapter.c  FITS adapter (CFITSIO-backed)
├── include/sciio/                Public headers (adapter.h, sciio_vol.h)
├── tools/                        Demo utility sources (fits_to_h5.c, h5_to_fits.c, fits_compare.c)
├── tests/
│   ├── integration/              C tests + h5py smoke + golden runner script
│   ├── fixtures/                 build_fixtures.c (10 deterministic FITS files)
│   └── golden/                   *.h5ls.txt golden outputs
├── cmake/FetchCorpus.cmake       Pinned astropy public test files
├── docs/                         Format-adapter API and release notes
└── CMakeLists.txt
```

## Further reading

- [`docs/format-adapter-api.md`](docs/format-adapter-api.md) — adapter
  interface for adding new scientific file formats.
- [`docs/release-readiness.md`](docs/release-readiness.md) — release
  checklist and validation status.
- [`CHANGELOG.md`](CHANGELOG.md) — version history.
