#!/usr/bin/env python3
"""M2.12/2.20 h5py smoke.

Verifies the path: h5py opens a FITS file through sciio-vol via env-var
auto-load, opens a named group by path, and reads an attribute.

NOT verified: h5py.Group.__iter__ / f.keys(). Reason — h5py 3.16 was built
with H5_USE_110_API which causes its iteration to call H5Literate_by_name1
(v1 API). HDF5 itself rejects v1 iteration on non-native VOL connectors with
"H5Literate_by_name1 is only meant to be used with the native VOL connector".
This is an h5py limitation upstream, tracked separately.

The C-level test_link_iter already verifies the v2 iterate path works.

Returns 77 (CMake SKIP_RETURN_CODE) if h5py isn't installed.
"""
import os
import sys

try:
    import h5py
except ImportError:
    print("SKIP: h5py not installed", flush=True)
    sys.exit(77)

if "SCIIO_FIXTURES_DIR" not in os.environ:
    print("FAIL: SCIIO_FIXTURES_DIR env var not set", file=sys.stderr)
    sys.exit(1)

path = os.path.join(os.environ["SCIIO_FIXTURES_DIR"], "image_2d.fits")

with h5py.File(path, "r") as f:
    # Path-based access does NOT require iteration — uses object_open by name.
    g = f["HDU0"]

    # Attribute readback through h5py.
    assert int(g.attrs["NAXIS"])  == 2,  f"NAXIS={g.attrs['NAXIS']}"
    assert int(g.attrs["NAXIS1"]) == 4,  f"NAXIS1={g.attrs['NAXIS1']}"
    assert int(g.attrs["NAXIS2"]) == 3,  f"NAXIS2={g.attrs['NAXIS2']}"
    assert int(g.attrs["MYINT"])  == 42, f"MYINT={g.attrs['MYINT']}"

    mystr = g.attrs["MYSTR"]
    if isinstance(mystr, bytes):
        mystr = mystr.decode()
    assert mystr == "regression-fixture", f"MYSTR={mystr!r}"

print(f"OK: h5py {h5py.__version__} (HDF5 {h5py.version.hdf5_version}) "
      f"opens FITS via sciio-vol and reads attributes by path")
sys.exit(0)
