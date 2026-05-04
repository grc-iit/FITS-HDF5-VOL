# Changelog

All notable changes to sciio-vol. Format roughly follows
[Keep a Changelog](https://keepachangelog.com/) with one section per
milestone of `sciio-vol-plan.md`.

## [Unreleased] — v1.0.0-rc

Hardened. Ready for v1.0.0 once a real VOL connector value is registered with
The HDF Group (currently `510`, provisional).

### Added (M6 — hardening)
- `SCIIO_SANITIZE=ON` CMake option enabling `-fsanitize=address,undefined`.
- 57/57 C-level ctest cases pass under ASan + UBSan with leak detection on.
- `tests/integration/perf_smoke.c` — measures sciio-vol overhead vs. direct
  CFITSIO. Within plan §8.3's 10% budget on real-archive image sizes.
- `tests/integration/fuzz_smoke.c` — deterministic mutation harness on the
  FITS header parse path; 1000-iteration ASan run completes without crashes.
- `H5VL` ABI compatibility canary: same `libsciio_vol.so` built against
  HDF5 2.1.1 loads and runs end-to-end under HDF5 1.14.3 (`H5VL_VERSION=3`,
  `sizeof(H5VL_class_t)=632` confirmed identical across both lines).

## [0.5.0] — M5 — API freeze

### Added
- `sciio_adapter_t` vtable (function-pointer table). Adapters export a
  single `const sciio_adapter_t` instance; the connector dispatches every
  call through it. Replaces the previous global-function API.
- `include/sciio/registry.h` + `src/registry.c` — compile-time adapter
  registry, `sciio_dispatch_probe()` picks the highest-confidence match.
- `SCIIO_ADAPTER_API_VERSION_MAJOR/MINOR` macros + semver policy.
- `docs/format-adapter-api.md` — canonical reference for adapter authors.
  Includes paper-review walkthroughs for DICOM and GRIB.
- `cmake --install` lays down `libsciio_vol.so`, `include/sciio/*.h`, and
  `lib/pkgconfig/sciio-vol.pc`.

### Changed
- FITS adapter functions all `static`; only `sciio_fits_adapter` exported.

### Verified
- DICOM and GRIB paper reviews completed; the abstract shape supports both
  without further API changes.

## [0.4.0] — M4 — Table HDUs

### Added
- ASCII Tables (`XTENSION='TABLE'`) and Binary Tables (`XTENSION='BINTABLE'`)
  surface as `/HDUn/columns/<TTYPE>` per-column 1-D datasets plus a
  `/HDUn/table` compound row-view dataset (plan §7.3).
- `TUNITn` → `units` attribute on each per-column dataset.
- `TDIMn` multi-dimensional cells map to HDF5 array datatypes
  (`H5T_ARRAY[d1,d2,…]`).
- Variable-length array columns (`TFORM 'P'/'Q'`) map to `H5T_VLEN`.
- 7-of-7 columns of `ftt4b/file001.fits` (ESO 1984 ASCII galaxy catalog)
  byte-match direct CFITSIO readback.

### Constraints
- VLEN columns are excluded from the row-view compound element type
  (per-column `/columns/<vlen>` reads still work).
- Vlen-string columns (`TFORM 'PA'`) hidden with logged warning.

## [0.3.0] — M3 — Image dataset reads

### Added
- `H5Dopen2` / `H5Dread` on `/HDUn/data` for every BITPIX (8/16/32/64/-32/-64).
- BZERO unsigned-int convention: `BSCALE=1, BZERO=2^15/2^31/2^63` →
  `uint16/uint32/uint64`.
- General BSCALE/BZERO rescale → `float64` per plan §7.2.
- Hyperslab selections (`start`/`count`/`stride`).
- Point selections.
- Tile-compressed image HDUs (`ZIMAGE=T`) surface for introspection;
  `H5Dread` rejected with v2-deferred error per plan §7.6.
- Round-trip tooling: `tools/{fits_to_h5,h5_to_fits,fits_compare}` produce
  bit-exact pixel reproduction on real archive files.

### Verified
- 14 image HDUs across the corpus read pixel-by-pixel via the connector.
- `roundtrip_*` ctest cases assert bit-exact pixels via SHA256.

## [0.2.0] — M2 — Headers, groups, attributes

### Added
- Each FITS HDU surfaces as `/HDU0`, `/HDU1`, ….
- `EXTNAME` keyword surfaces as a soft link `/<EXTNAME>` → `/HDUn` when
  valid + unique. Validation rules: empty / contains `/` / shadows
  `HDU<digits>` / duplicates → dropped with logged warning.
- Header keyword → typed attribute mapping covering the entire plan §7.5
  table: int / float / bool / string / complex / `COMMENT` / `HISTORY` /
  `HIERARCH` / `CONTINUE` long strings / `__raw_header__` byte-exact card
  array.
- Random Groups primary HDUs rejected with the documented diagnostic.
- `H5Lvisit2` recursive iteration with relative-path prefixes.
- Real-archive corpus integrated: 5 astropy public files, 16 NRAO ftt4b
  files, hand-crafted edge fixtures.

## [0.1.0] — M1 — Skeleton

### Added
- CMake build producing `libsciio_vol.so`.
- Terminal VOL connector that registers via
  `H5VLregister_connector_by_name("sciio")`.
- `H5Fopen` / `H5Gopen("/")` round-trip; capability flags set conservatively.
- `HDF5_VOL_CONNECTOR=sciio` + `HDF5_PLUGIN_PATH` auto-load verified.

## Unsupported in v1 (out of scope per plan §8.2)

- Writing FITS — sciio-vol is read-only by design.
- Tile-compressed image data reads (deferred to v2).
- WCS interpretation (keywords surfaced verbatim).
- Parallel / MPI-IO.
- Object stores (S3, Swift).
- DICOM / GRIB / NetCDF-3 adapters (Format-Adapter API designed for them;
  only FITS shipped in v1).

## Pre-v1.0 blockers

- VOL connector value `510` is **provisional**. Register a permanent value
  with The HDF Group before tagging v1.0.0.
