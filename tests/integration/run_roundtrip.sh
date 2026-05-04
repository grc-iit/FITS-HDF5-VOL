#!/usr/bin/env bash
# Round-trip test runner: original.fits → intermediate.h5 → restored.fits,
# then byte-compare original vs restored. All artifacts go under a per-run
# temp directory beneath the build tree.
set -euo pipefail

FITS_TO_H5="$1"     # path to fits_to_h5 executable
H5_TO_FITS="$2"     # path to h5_to_fits executable
FITS_CMP="$3"       # path to fits_compare executable
SRC="$4"            # source .fits
WORK="$5"           # working dir (created if missing)

mkdir -p "$WORK"
H5="$WORK/intermediate.h5"
RESTORED="$WORK/restored_fits_from_hdf5.fits"
rm -f "$H5" "$RESTORED"

if ! "$FITS_TO_H5"  "$SRC"  "$H5";       then echo "FAIL fits_to_h5";  exit 1; fi
if ! "$H5_TO_FITS"  "$H5"   "$RESTORED"; then echo "FAIL h5_to_fits";  exit 1; fi
if ! "$FITS_CMP"    "$SRC"  "$RESTORED"; then echo "FAIL fits_compare"; exit 1; fi
echo "OK round-trip: $(basename "$SRC")"
