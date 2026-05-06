#!/usr/bin/env python3
"""
Download real open-access astronomy FITS files for connector testing.

Sources (all open-access, no login required):
  - NASA SkyView (HEASARC): multi-survey image cutouts
  - CDS VizieR via astroquery: catalog FITS binary tables
  - Astropy project public test data

Usage:
    pip install requests astroquery
    python3 tools/download_test_data.py

Files land in:
    tests/astronomy_data/images/   — image FITS files
    tests/astronomy_data/tables/   — table FITS files

Re-running is safe: existing files are skipped.
"""
import os, sys, time, warnings
from pathlib import Path

import requests
warnings.filterwarnings("ignore")

DEST    = Path(__file__).parent.parent / "tests" / "astronomy_data"
IMG_DIR = DEST / "images"
TBL_DIR = DEST / "tables"
IMG_DIR.mkdir(parents=True, exist_ok=True)
TBL_DIR.mkdir(parents=True, exist_ok=True)


# ── helpers ────────────────────────────────────────────────────────────────────

def dl(url, dst, session, label=""):
    """Download url → dst, skip if already present, return True on success."""
    if dst.exists() and dst.stat().st_size > 0:
        print(f"  SKIP {dst.name}")
        return True
    try:
        r = session.get(url, timeout=30, stream=True)
        r.raise_for_status()
        with open(dst, "wb") as f:
            for chunk in r.iter_content(65536):
                f.write(chunk)
        size = dst.stat().st_size
        if size == 0:
            dst.unlink()
            print(f"  EMPTY {dst.name}: {label}")
            return False
        print(f"  OK  {dst.name:55s} {size//1024:6d} KB  {label}")
        return True
    except Exception as e:
        print(f"  FAIL {dst.name}: {e}")
        if dst.exists():
            dst.unlink()
        return False


# ── Section 1: SkyView image cutouts ──────────────────────────────────────────
# 25 sky targets × 3 surveys = 75 image FITS files

TARGETS = [
    ("83.8221,-5.3911",   "Orion_Nebula"),
    ("10.6847,41.2690",   "Andromeda_M31"),
    ("187.706,12.3911",   "Virgo_A_M87"),
    ("266.417,-29.008",   "Galactic_Center"),
    ("201.365,-43.019",   "Centaurus_A"),
    ("202.470,47.1952",   "M51_Whirlpool"),
    ("148.969,69.6797",   "M81_Bodes"),
    ("148.888,69.0653",   "M82_Cigar"),
    ("56.750,24.1167",    "Pleiades"),
    ("83.700,-5.900",     "Orion_Belt"),
    ("290.663,14.4900",   "M27_Dumbbell"),
    ("315.017,43.0000",   "Cygnus_X1"),
    ("310.358,45.2804",   "North_America_Neb"),
    ("5.5755,-72.8006",   "SMC"),
    ("80.894,-69.756",    "LMC"),
    ("285.267,-4.0000",   "Eagle_M16"),
    ("101.288,-16.716",   "Rosette_Nebula"),
    ("23.462,30.6597",    "Perseus_Cluster"),
    ("229.702,26.5900",   "Serpens_Cloud"),
    ("239.600,-26.400",   "Lupus_Cloud"),
    ("277.500,-2.5000",   "M16_Vicinity"),
    ("83.629,-5.5455",    "Horsehead_Neb"),
    ("82.800,-5.4000",    "Orion_Sword"),
    ("83.000,-6.0000",    "Orion_South"),
    ("84.000,-5.0000",    "Orion_North"),
]

SURVEYS = [
    ("DSS",           "DSS_optical"),
    ("2MASS-J",       "2MASS_J_band"),
    ("RASS-Cnt-Hard", "ROSAT_RASS_hard"),
]

def skyview_url(pos, survey, size=0.5, pixels=256):
    return (
        "https://skyview.gsfc.nasa.gov/cgi-bin/images?"
        f"Survey={survey}&coordinates=J2000&position={pos}"
        f"&size={size}&pixels={pixels}&Return=FITS"
    )


# ── Section 2: VizieR catalog tables via astroquery ───────────────────────────
# astroquery.vizier returns proper FITS binary tables (not VOTable XML).

VIZIER_CATALOGS = [
    ("I/355/gaiadr3",         "Gaia_DR3"),
    ("II/246/out",            "2MASS_PSC"),
    ("I/345/gaia2",           "Gaia_DR2"),
    ("I/259/tyc2",            "Tycho2_stars"),
    ("I/311/hip2",            "Hipparcos_2007"),
    ("VII/118/ngc2000",       "NGC_2000_catalog"),
    ("I/350/gaiaedr3",        "Gaia_eDR3"),
    ("IV/38/tic",             "TESS_Input_Catalog"),
    ("I/322A/out",            "UCAC4_catalog"),
    ("II/328/allwise",        "AllWISE"),
    ("V/50/catalog",          "Bright_Star_Catalog"),
    ("I/337/gaia",            "Gaia_DR1"),
    ("II/336/apass9",         "APASS_DR9"),
    ("V/147/sdss12",          "SDSS_DR12"),
]


# ── Section 3: Astropy public test data ───────────────────────────────────────

ASTROPY_BASE  = "https://data.astropy.org"
ASTROPY_FILES = [
    ("/tutorials/FITS-images/HorseHead.fits",     "img", "Horsehead_astropy"),
    ("/tutorials/FITS-cubes/L1448_13CO.fits",     "img", "L1448_13CO_cube"),
    ("/tutorials/FITS-images/M13_blue_0001.fits", "img", "M13_blue_1"),
    ("/tutorials/FITS-images/M13_blue_0002.fits", "img", "M13_blue_2"),
    ("/tutorials/FITS-images/M13_blue_0003.fits", "img", "M13_blue_3"),
    ("/tutorials/FITS-tables/chandra_events.fits","tbl", "Chandra_events"),
    ("/photometry/M6707HH.fits",                  "tbl", "photometry_M6707HH"),
]


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    s = requests.Session()
    s.headers["User-Agent"] = "fits-hdf5-vol-test/1.0 (open science)"
    ok = fail = 0

    # ── SkyView images ────────────────────────────────────────────────────────
    print(f"\n=== SkyView images ({len(TARGETS)} targets × {len(SURVEYS)} surveys) ===")
    for pos, name in TARGETS:
        for survey_id, survey_label in SURVEYS:
            dst = IMG_DIR / f"{name}_{survey_label}.fits"
            r = dl(skyview_url(pos, survey_id), dst, s, f"{survey_label} @ {name}")
            ok += r; fail += not r
            time.sleep(0.25)

    # ── VizieR tables ─────────────────────────────────────────────────────────
    print(f"\n=== VizieR catalog tables ({len(VIZIER_CATALOGS)} catalogs via astroquery) ===")
    try:
        from astroquery.vizier import Vizier
    except ImportError:
        print("  astroquery not installed — skipping tables (pip install astroquery)")
    else:
        v = Vizier(row_limit=300, timeout=60)
        for catalog, label in VIZIER_CATALOGS:
            dst = TBL_DIR / f"{label}.fits"
            if dst.exists() and dst.stat().st_size > 0:
                print(f"  SKIP {dst.name}")
                ok += 1
                continue
            try:
                result = v.get_catalogs(catalog)
                if not result or len(result[0]) == 0:
                    print(f"  EMPTY {label}: no rows returned")
                    fail += 1
                    continue
                result[0].write(str(dst), format="fits", overwrite=True)
                size = dst.stat().st_size
                print(f"  OK  {dst.name:45s} {len(result[0]):5d} rows  {size//1024:5d} KB")
                ok += 1
            except Exception as e:
                print(f"  FAIL {label}: {e}")
                fail += 1

    # ── Astropy public files ──────────────────────────────────────────────────
    print(f"\n=== Astropy public test data ({len(ASTROPY_FILES)} files) ===")
    for path, kind, label in ASTROPY_FILES:
        dst = (IMG_DIR if kind == "img" else TBL_DIR) / f"{label}.fits"
        r = dl(ASTROPY_BASE + path, dst, s, label)
        ok += r; fail += not r
        time.sleep(0.3)

    # ── summary ───────────────────────────────────────────────────────────────
    imgs = list(IMG_DIR.glob("*.fits"))
    tbls = list(TBL_DIR.glob("*.fits"))
    print(f"\n{'='*60}")
    print(f"  Downloads:  {ok} ok   {fail} failed")
    print(f"  Images:     {len(imgs)}  in tests/astronomy_data/images/")
    print(f"  Tables:     {len(tbls)}  in tests/astronomy_data/tables/")
    print(f"  Total FITS: {len(imgs) + len(tbls)}")
    print(f"\nRun ctest to exercise all files:")
    print(f"  ctest --test-dir build -L astro")


if __name__ == "__main__":
    main()
