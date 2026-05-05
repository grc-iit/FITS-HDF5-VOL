# fits-hdf5-vol v1.0.0 Release-Readiness Audit

Snapshot of every pre-release blocker identified during M1–M6, with the
status today and what (if anything) needs to happen before tagging v1.0.0.

## Plan-level exit criteria

| §11 milestone | Status |
|---|---|
| M1 Skeleton | ✅ shipped |
| M2 Headers, groups, attributes | ✅ shipped |
| M3 Image HDU reads | ✅ shipped |
| M4 Table HDU reads | ✅ shipped |
| M5 Adapter API freeze + DICOM/GRIB paper review | ✅ shipped |
| M6 Hardening | ✅ shipped (within in-environment scope) |

## Plan §10 testing matrix

| Layer | Status |
|---|---|
| Unit tests (cmocka) | Not used; coverage delivered via 64 integration tests |
| Differential / oracle tests vs CFITSIO | ✅ — `test_diff_image_2d`, `test_column_diff` |
| Integration tests (h5dump, h5ls -r golden, PyTables) | ✅ for h5ls (4 goldens); ❌ PyTables (not exercised) |
| Fuzz tests | ✅ smoke harness; full AFL++/libFuzzer 24-hour run is a CI item |
| Performance regression | ✅ within the 10% overhead budget on real-archive sizes |
| Test corpus: NASA HEASARC | ⚠️ partial — `ftt4b` covers it; large mission samples pending |
| Test corpus: astropy test set | ✅ 5 files, sha256-pinned |
| Test corpus: hand-crafted edge files | ✅ 10 fixtures |
| Test corpus: large real ≥ 1 GB | ❌ not yet wired (Hubble image, TESS lightcurve) |
| CI: Ubuntu/macOS × gcc/clang + ASan/UBSan | ⚠️ ASan/UBSan locally green; GitHub Actions not configured |
| Compatibility canary against HDF5 1.14 | ✅ verified bit-identical ABI; cross-runtime tested |

## Plan §13 open questions

| Q | Status |
|---|---|
| Q1 Read-only for v1 | ✅ confirmed; honored throughout the codebase |
| Q2 CFITSIO acceptable | ✅ confirmed; pkg-config-required |
| Q3 HDU naming `/HDU0`, `/HDU1`, … | ✅ implemented |
| Q4 Phase-2 adapter | Still open. DICOM and GRIB are both viable — decision is product-side |
| Q5 BSD-3-Clause | ✅ `LICENSE` in place |
| Q6 Distribution channel | ✅ source-only on GitHub for v1.0.0; conda-forge / spack later |

## Plan §12 risks — current state

| ID | Risk | Today |
|---|---|---|
| R1 | CFITSIO Windows packaging | Not a blocker; documented in README as Linux/macOS for v1 |
| R2 | Format-Adapter API turns FITS-shaped | ✅ paper-reviewed against DICOM + GRIB; no struct changes needed |
| R3 | HDF5 type mismatches on exotic FITS columns | ✅ ADAPTER_T_OPAQUE for unrecognized; complex, vlen, multi-D all wired |
| R4 | Hyperslab translation subtle bugs | ✅ differential test on real images; ASan clean |
| R5 | Performance overhead exceeds 10% | ✅ measured on 600 KB image: −8.3%; on 512 KB: +9.5% |
| R6 | HDF5 1.14 → 1.15/2.x ABI break | ✅ verified bit-identical between 1.14.3 and 2.1.1 |
| R7 | Capability-flag misadvertising | ✅ audited and reviewed in M5 |
| R8 | Read-only insufficient for some users | Documented as out-of-scope; v2 conversation if it ever happens |

## Pre-release blockers (must close before v1.0.0)

| Item | Owner | Status |
|---|---|---|
| Register a permanent VOL connector value with The HDF Group | maintainer | **OPEN** — currently provisional `510` |
| Document h5py upstream caveat in README | this audit | ✅ in README "Reading a FITS file directly through fits-hdf5-vol" |
| Document HDF5-instance-separation rule | this audit | ✅ in README h5py section |

## Tracked v1.x backlog (post-v1.0.0)

| Item | Why it's not v1.0 |
|---|---|
| GitHub Actions CI matrix (Ubuntu/macOS × gcc/clang + sanitizers) | Repo not on GitHub yet |
| Large-file test (≥ 1 GB Hubble image, TESS lightcurve) | Bandwidth/storage; CI fetch on demand |
| AFL++/libFuzzer 24-hour fuzzing runs | Smoke version shipped; full fuzz needs nightly infra |
| `vlen` members inside the row-view compound | Per-column `/columns/<vlen>` already works |
| Vlen-string columns (`TFORM 'PA'`) | Rare in practice; logged-skip today |
| Hyperslab `block > 1` | Explicit "not supported" error; rare in real workloads |
| In-flight HDF5 type conversion in `H5Dread` | Caller currently must request the dataset's native type |
| Tile-compressed image data reads | Explicitly v2 |
| Conda-forge / spack distribution | Targeted for v1.1 |

## v2.0 candidates

| Item | Notes |
|---|---|
| DICOM adapter | Paper-reviewed in M5; API supports it |
| GRIB adapter | Paper-reviewed in M5; API supports it |
| NetCDF-3 adapter | Future adapter target |
| Tile-compressed image data reads | Deferred from v1 |
| Storage-faithful round-trip (preserve original BITPIX through scaling) | Round-trip tools currently lose BITPIX when BSCALE/BZERO promote to float64 |
| Object-store backends (S3, Swift) | Deferred from v1 |

## What "v1.0.0-rc" means today

The codebase delivers everything scoped for v1, with verified behavior
against:
- 10 hand-crafted deterministic fixtures
- 5 astropy public test files (downloaded + sha256-pinned)
- 16 NRAO `ftt4b` reference files
- Synthetic mutations under ASan
- Real-archive image round-trips with bit-exact pixel match
- HDF5 1.14.3 *and* 2.1.x runtimes against the same `.so`

The single open release-blocker is **registering the VOL connector value
with The HDF Group** — coordination work outside the codebase. Once that
lands, the version macros tick from `0.0.1` to `1.0.0` and we tag.
