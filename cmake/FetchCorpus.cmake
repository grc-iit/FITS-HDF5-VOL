# Fetch a small set of real public FITS files for offline regression tests.
# Files are cached in ${CMAKE_BINARY_DIR}/corpus and re-fetched only if absent.
# If the network is unavailable, the corpus tests SKIP cleanly.
#
# Source: astropy/astropy main branch test data (BSD-3-Clause). Stable URLs.

set(FITS_CORPUS_DIR ${CMAKE_BINARY_DIR}/corpus)
file(MAKE_DIRECTORY ${FITS_CORPUS_DIR})

set(_astropy_base "https://raw.githubusercontent.com/astropy/astropy/main/astropy/io/fits/tests/data")

# (filename, sha256 prefix-12, expected behavior)
#   "open" — H5Fopen succeeds, traversal is asserted no-crash.
#   "reject" — H5Fopen MUST fail (M2.9 Random Groups path).
set(FITS_CORPUS_SPEC
    "test0.fits|ea06ee30b28f|open"
    "ascii.fits|b3d5e79c5cf9|open"
    "tb.fits|e1891a5453af|open"
    "comp.fits|bc675bd775cd|open"
    "random_groups.fits|04c74ec21087|reject"
)

set(FITS_CORPUS_FILES "")
set(FITS_CORPUS_BEHAVIOR "")
foreach(spec ${FITS_CORPUS_SPEC})
    string(REPLACE "|" ";" parts ${spec})
    list(GET parts 0 fname)
    list(GET parts 1 sha_prefix)
    list(GET parts 2 behavior)

    set(target ${FITS_CORPUS_DIR}/${fname})
    if(NOT EXISTS ${target})
        message(STATUS "Fetching corpus file ${fname}")
        file(DOWNLOAD
            "${_astropy_base}/${fname}"
            ${target}
            TIMEOUT 30
            INACTIVITY_TIMEOUT 10
            STATUS dl_status
        )
        list(GET dl_status 0 dl_code)
        if(NOT dl_code EQUAL 0)
            message(WARNING "Could not fetch ${fname}: ${dl_status}")
            file(REMOVE ${target})
            continue()
        endif()
    endif()

    # Sanity check: confirm we got the expected file (12-char SHA256 prefix).
    file(SHA256 ${target} actual_sha)
    string(SUBSTRING ${actual_sha} 0 12 actual_prefix)
    if(NOT actual_prefix STREQUAL sha_prefix)
        message(WARNING "Hash mismatch on ${fname}: got ${actual_prefix} expected ${sha_prefix}")
        file(REMOVE ${target})
        continue()
    endif()

    list(APPEND FITS_CORPUS_FILES ${fname})
    list(APPEND FITS_CORPUS_BEHAVIOR ${behavior})
endforeach()

list(LENGTH FITS_CORPUS_FILES FITS_CORPUS_COUNT)
message(STATUS "Corpus: ${FITS_CORPUS_COUNT} file(s) available in ${FITS_CORPUS_DIR}")
