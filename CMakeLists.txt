cmake_minimum_required(VERSION 3.18)
project(sciio_vol
    VERSION 0.0.1
    DESCRIPTION "Terminal HDF5 VOL connector for foreign scientific formats (FITS first)"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Sanitizer build option (M6.1). Use:
#   cmake -B build-asan -DSCIIO_SANITIZE=ON
option(SCIIO_SANITIZE "Build with -fsanitize=address,undefined" OFF)
if(SCIIO_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

# Don't try to rewrite RPATH on install — keep the build-tree RUNPATH so the
# installed lib still finds HDF5/CFITSIO via LD_LIBRARY_PATH or system paths.
# Avoids a CMake "RPATH_CHANGE could not write new RPATH" failure when the
# install prefix differs from the build prefix.
set(CMAKE_SKIP_INSTALL_RPATH TRUE)

find_package(HDF5 2.1.1 REQUIRED CONFIG NAMES hdf5)
message(STATUS "HDF5 version: ${HDF5_VERSION}")
message(STATUS "HDF5 cmake config: ${HDF5_DIR}")

# --- CFITSIO (required, system install) ---
find_package(PkgConfig REQUIRED)
pkg_check_modules(CFITSIO REQUIRED IMPORTED_TARGET cfitsio)
message(STATUS "CFITSIO version: ${CFITSIO_VERSION}")

add_library(sciio_vol SHARED
    src/sciio_vol_connector.c
    src/registry.c
    adapters/fits/fits_adapter.c
)

target_include_directories(sciio_vol
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sciio_vol PRIVATE hdf5-shared PkgConfig::CFITSIO)

target_compile_options(sciio_vol PRIVATE
    -Wall -Wextra -Wpedantic -Wno-unused-parameter
)

set_target_properties(sciio_vol PROPERTIES
    OUTPUT_NAME sciio_vol
    PREFIX "lib"
)
# Skip RPATH rewriting on install — the user controls LD_LIBRARY_PATH /
# HDF5_PLUGIN_PATH at runtime. Avoids a CMake quirk where the RUNPATH
# rewrite fails when install destination differs from build prefix.
set_target_properties(sciio_vol PROPERTIES SKIP_INSTALL_RPATH TRUE)

# --- Install rules ---
include(GNUInstallDirs)

install(TARGETS sciio_vol
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/include/sciio/adapter.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/sciio/sciio_vol.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sciio
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sciio-vol.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/sciio-vol.pc
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sciio-vol.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# --- Test fixtures ---
add_executable(build_fixtures tests/fixtures/build_fixtures.c)
target_link_libraries(build_fixtures PRIVATE PkgConfig::CFITSIO)

set(SCIIO_FIXTURES_DIR ${CMAKE_BINARY_DIR}/fixtures)
file(MAKE_DIRECTORY ${SCIIO_FIXTURES_DIR})
set(SCIIO_FIXTURES
    ${SCIIO_FIXTURES_DIR}/primary_only.fits
    ${SCIIO_FIXTURES_DIR}/image_2d.fits
    ${SCIIO_FIXTURES_DIR}/multi_hdu.fits
    ${SCIIO_FIXTURES_DIR}/edge_keywords.fits
    ${SCIIO_FIXTURES_DIR}/complex_keyword.fits
    ${SCIIO_FIXTURES_DIR}/bitpix_matrix.fits
    ${SCIIO_FIXTURES_DIR}/unsigned_bzero.fits
    ${SCIIO_FIXTURES_DIR}/scaled_image.fits
    ${SCIIO_FIXTURES_DIR}/table_multidim.fits
    ${SCIIO_FIXTURES_DIR}/table_vlen.fits
)
add_custom_command(
    OUTPUT  ${SCIIO_FIXTURES}
    COMMAND build_fixtures ${SCIIO_FIXTURES_DIR}
    DEPENDS build_fixtures tests/fixtures/build_fixtures.c
    COMMENT "Generating FITS test fixtures"
)
add_custom_target(fixtures ALL DEPENDS ${SCIIO_FIXTURES})

# --- Real-FITS corpus (downloaded on demand at configure time) ---
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchCorpus.cmake)

# --- Tests ---
enable_testing()

add_executable(test_register tests/integration/test_register.c)
target_include_directories(test_register PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_register PRIVATE hdf5-shared sciio_vol)
add_test(NAME register COMMAND test_register)
set_tests_properties(register PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_open_close tests/integration/test_open_close.c)
target_include_directories(test_open_close PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_open_close PRIVATE hdf5-shared sciio_vol PkgConfig::CFITSIO)
add_test(NAME open_close COMMAND test_open_close)
set_tests_properties(open_close PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

# Env-var auto-load test: deliberately does NOT link sciio_vol — the connector
# must be picked up at runtime via HDF5_PLUGIN_PATH.
add_executable(test_envvar_load tests/integration/test_envvar_load.c)
target_link_libraries(test_envvar_load PRIVATE hdf5-shared PkgConfig::CFITSIO)
add_test(NAME envvar_load COMMAND test_envvar_load)
set_tests_properties(envvar_load PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR};HDF5_VOL_CONNECTOR=sciio"
)

add_executable(test_cfitsio_smoke tests/integration/test_cfitsio_smoke.c)
target_link_libraries(test_cfitsio_smoke PRIVATE PkgConfig::CFITSIO)
add_test(NAME cfitsio_smoke COMMAND test_cfitsio_smoke)

add_executable(test_fits_adapter tests/integration/test_fits_adapter.c)
target_include_directories(test_fits_adapter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_fits_adapter PRIVATE sciio_vol PkgConfig::CFITSIO)
add_test(NAME fits_adapter COMMAND test_fits_adapter)

add_executable(test_link_iter tests/integration/test_link_iter.c)
target_include_directories(test_link_iter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_link_iter PRIVATE sciio_vol hdf5-shared PkgConfig::CFITSIO)
add_test(NAME link_iter COMMAND test_link_iter)
set_tests_properties(link_iter PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_attr_read tests/integration/test_attr_read.c)
target_include_directories(test_attr_read PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_attr_read PRIVATE sciio_vol hdf5-shared PkgConfig::CFITSIO)
add_test(NAME attr_read COMMAND test_attr_read)
set_tests_properties(attr_read PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_attr_edge tests/integration/test_attr_edge.c)
target_include_directories(test_attr_edge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_attr_edge PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_attr_edge PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_attr_edge fixtures)
add_test(NAME attr_edge COMMAND test_attr_edge)
set_tests_properties(attr_edge PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_random_groups_reject tests/integration/test_random_groups_reject.c)
target_include_directories(test_random_groups_reject PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_random_groups_reject PRIVATE sciio_vol hdf5-shared PkgConfig::CFITSIO)
add_test(NAME random_groups_reject COMMAND test_random_groups_reject)
set_tests_properties(random_groups_reject PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_complex_attr tests/integration/test_complex_attr.c)
target_include_directories(test_complex_attr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_complex_attr PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_complex_attr PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_complex_attr fixtures)
add_test(NAME complex_attr COMMAND test_complex_attr)
set_tests_properties(complex_attr PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_concurrent_open tests/integration/test_concurrent_open.c)
target_include_directories(test_concurrent_open PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_concurrent_open PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_concurrent_open PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_concurrent_open fixtures)
add_test(NAME concurrent_open COMMAND test_concurrent_open)
set_tests_properties(concurrent_open PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_extname_rules tests/integration/test_extname_rules.c)
target_include_directories(test_extname_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_extname_rules PRIVATE sciio_vol hdf5-shared PkgConfig::CFITSIO)
add_test(NAME extname_rules COMMAND test_extname_rules)
set_tests_properties(extname_rules PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_file_intent tests/integration/test_file_intent.c)
target_include_directories(test_file_intent PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_file_intent PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_file_intent PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_file_intent fixtures)
add_test(NAME file_intent COMMAND test_file_intent)
set_tests_properties(file_intent PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_non_fits_rejected tests/integration/test_non_fits_rejected.c)
target_include_directories(test_non_fits_rejected PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_non_fits_rejected PRIVATE sciio_vol hdf5-shared)
add_test(NAME non_fits_rejected COMMAND test_non_fits_rejected)
set_tests_properties(non_fits_rejected PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_rdwr_rejected tests/integration/test_rdwr_rejected.c)
target_include_directories(test_rdwr_rejected PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_rdwr_rejected PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_rdwr_rejected PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_rdwr_rejected fixtures)
add_test(NAME rdwr_rejected COMMAND test_rdwr_rejected)
set_tests_properties(rdwr_rejected PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_extname_link tests/integration/test_extname_link.c)
target_include_directories(test_extname_link PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_extname_link PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_extname_link PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_extname_link fixtures)
add_test(NAME extname_link COMMAND test_extname_link)
set_tests_properties(extname_link PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_raw_header tests/integration/test_raw_header.c)
target_include_directories(test_raw_header PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_raw_header PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_raw_header PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_raw_header fixtures)
add_test(NAME raw_header COMMAND test_raw_header)
set_tests_properties(raw_header PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_scaled_image tests/integration/test_scaled_image.c)
target_include_directories(test_scaled_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_scaled_image PRIVATE sciio_vol hdf5-shared m)
target_compile_definitions(test_scaled_image PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_scaled_image fixtures)
add_test(NAME scaled_image COMMAND test_scaled_image)
set_tests_properties(scaled_image PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_unsigned_bzero tests/integration/test_unsigned_bzero.c)
target_include_directories(test_unsigned_bzero PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_unsigned_bzero PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_unsigned_bzero PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_unsigned_bzero fixtures)
add_test(NAME unsigned_bzero COMMAND test_unsigned_bzero)
set_tests_properties(unsigned_bzero PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_bitpix_matrix tests/integration/test_bitpix_matrix.c)
target_include_directories(test_bitpix_matrix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_bitpix_matrix PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_bitpix_matrix PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_bitpix_matrix fixtures)
add_test(NAME bitpix_matrix COMMAND test_bitpix_matrix)
set_tests_properties(bitpix_matrix PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_table_rowview tests/integration/test_table_rowview.c)
target_include_directories(test_table_rowview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_table_rowview PRIVATE sciio_vol hdf5-shared)
add_test(NAME table_rowview COMMAND test_table_rowview)
set_tests_properties(table_rowview PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_table_vlen tests/integration/test_table_vlen.c)
target_include_directories(test_table_vlen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_table_vlen PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_table_vlen PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_table_vlen fixtures)
add_test(NAME table_vlen COMMAND test_table_vlen)
set_tests_properties(table_vlen PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_table_multidim tests/integration/test_table_multidim.c)
target_include_directories(test_table_multidim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_table_multidim PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_table_multidim PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_table_multidim fixtures)
add_test(NAME table_multidim COMMAND test_table_multidim)
set_tests_properties(table_multidim PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_column_units tests/integration/test_column_units.c)
target_include_directories(test_column_units PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_column_units PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_column_units PRIVATE SCIIO_CORPUS_DIR="${SCIIO_CORPUS_DIR}")
add_test(NAME column_units COMMAND test_column_units)
set_tests_properties(column_units PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_column_diff tests/integration/test_column_diff.c)
target_include_directories(test_column_diff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_column_diff PRIVATE sciio_vol hdf5-shared PkgConfig::CFITSIO)
target_compile_definitions(test_column_diff PRIVATE
    SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}"
    SCIIO_CORPUS_DIR="${SCIIO_CORPUS_DIR}")
add_test(NAME column_diff COMMAND test_column_diff)
set_tests_properties(column_diff PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(perf_smoke tests/integration/perf_smoke.c)
target_include_directories(perf_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(perf_smoke PRIVATE sciio_vol hdf5-shared PkgConfig::CFITSIO)
# Not gated as ctest — perf is reported for human eyes.

add_executable(fuzz_smoke tests/integration/fuzz_smoke.c)
target_include_directories(fuzz_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fuzz_smoke PRIVATE sciio_vol)
# Not gated as ctest — invoke manually with the corpus dir.

add_executable(test_zimage_reject tests/integration/test_zimage_reject.c)
target_include_directories(test_zimage_reject PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_zimage_reject PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_zimage_reject PRIVATE SCIIO_CORPUS_DIR="${SCIIO_CORPUS_DIR}")
add_test(NAME zimage_reject COMMAND test_zimage_reject)
set_tests_properties(zimage_reject PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_point_selection tests/integration/test_point_selection.c)
target_include_directories(test_point_selection PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_point_selection PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_point_selection PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_point_selection fixtures)
add_test(NAME point_selection COMMAND test_point_selection)
set_tests_properties(point_selection PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_hyperslab tests/integration/test_hyperslab.c)
target_include_directories(test_hyperslab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_hyperslab PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_hyperslab PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_hyperslab fixtures)
add_test(NAME hyperslab COMMAND test_hyperslab)
set_tests_properties(hyperslab PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_dataset_read_short tests/integration/test_dataset_read_short.c)
target_include_directories(test_dataset_read_short PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_dataset_read_short PRIVATE sciio_vol hdf5-shared)
target_compile_definitions(test_dataset_read_short PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_dataset_read_short fixtures)
add_test(NAME dataset_read_short COMMAND test_dataset_read_short)
set_tests_properties(dataset_read_short PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

add_executable(test_diff_image_2d tests/integration/test_diff_image_2d.c)
target_include_directories(test_diff_image_2d PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_diff_image_2d PRIVATE sciio_vol hdf5-shared m)
target_compile_definitions(test_diff_image_2d PRIVATE SCIIO_FIXTURES_DIR="${SCIIO_FIXTURES_DIR}")
add_dependencies(test_diff_image_2d fixtures)
add_test(NAME diff_image_2d COMMAND test_diff_image_2d)
set_tests_properties(diff_image_2d PROPERTIES
    ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
)

# h5py smoke. Prefer the project venv if present; otherwise fall back to the
# system python3 (which usually lacks h5py — the test SKIPs).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python3)
    set(PYTHON3 ${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python3)
else()
    find_program(PYTHON3 python3)
endif()
if(PYTHON3)
    add_test(NAME h5py_smoke
        COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_h5py_smoke.py
    )
    set_tests_properties(h5py_smoke PROPERTIES
        ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR};HDF5_VOL_CONNECTOR=sciio;LD_LIBRARY_PATH=${HDF5_DIR}/../lib;SCIIO_FIXTURES_DIR=${SCIIO_FIXTURES_DIR}"
        SKIP_RETURN_CODE 77
    )
endif()

# Local FITS corpus from ~/fits-tests/ftt4b (FITS test suite shipped with the
# FITS Standard documentation). One ctest case per file. Path is configurable.
set(SCIIO_FTT4B_DIR "$ENV{HOME}/fits-tests/ftt4b" CACHE PATH
    "Local copy of the NRAO ftt4b FITS test suite")

# Round-trip demonstration tools (sit in tools/, not part of libsciio_vol):
#   fits_to_h5      — read FITS via sciio-vol, write native HDF5
#   h5_to_fits      — read native HDF5, write FITS via CFITSIO (no sciio-vol)
#   fits_compare    — byte-compare two FITS files HDU-by-HDU
add_executable(fits_to_h5   tools/fits_to_h5.c)
target_link_libraries(fits_to_h5   PRIVATE hdf5-shared)
add_executable(h5_to_fits   tools/h5_to_fits.c)
target_link_libraries(h5_to_fits   PRIVATE hdf5-shared PkgConfig::CFITSIO)
add_executable(fits_compare tools/fits_compare.c)
target_link_libraries(fits_compare PRIVATE PkgConfig::CFITSIO)

# Real-FITS corpus smoke (skipped per file if download didn't succeed).
add_executable(test_corpus_smoke tests/integration/test_corpus_smoke.c)
target_include_directories(test_corpus_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(test_corpus_smoke PRIVATE sciio_vol hdf5-shared)

list(LENGTH SCIIO_CORPUS_FILES _n)
math(EXPR _last "${_n} - 1")
foreach(i RANGE ${_last})
    list(GET SCIIO_CORPUS_FILES   ${i} _fname)
    list(GET SCIIO_CORPUS_BEHAVIOR ${i} _behavior)
    string(REPLACE "." "_" _safe ${_fname})
    if(_behavior STREQUAL "reject")
        add_test(NAME corpus_${_safe}
            COMMAND test_corpus_smoke ${SCIIO_CORPUS_DIR}/${_fname} reject
        )
    else()
        add_test(NAME corpus_${_safe}
            COMMAND test_corpus_smoke ${SCIIO_CORPUS_DIR}/${_fname}
        )
    endif()
    set_tests_properties(corpus_${_safe} PROPERTIES
        ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
    )
endforeach()

# Round-trip tests: FITS → HDF5 → restored FITS, byte-compared.
# Excludes files that promote to float64 via BSCALE/BZERO: the FITS encoding
# can't be losslessly recovered from physical-value floats.
set(_RT_RUNNER ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_roundtrip.sh)
set(_RT_HDF5_LIB ${HDF5_DIR}/../lib)
set(_RT_ENV
    "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR};LD_LIBRARY_PATH=${_RT_HDF5_LIB}"
)

# Baseline: deterministic in-tree fixture.
add_test(NAME roundtrip_image_2d
    COMMAND bash ${_RT_RUNNER}
            $<TARGET_FILE:fits_to_h5>
            $<TARGET_FILE:h5_to_fits>
            $<TARGET_FILE:fits_compare>
            ${SCIIO_FIXTURES_DIR}/image_2d.fits
            ${CMAKE_BINARY_DIR}/roundtrip_image_2d
)
set_tests_properties(roundtrip_image_2d PROPERTIES ENVIRONMENT "${_RT_ENV}")

# ftt4b: only files known to round-trip bit-exact (no BSCALE/BZERO promotion,
# no tables, no compression). Files that promote to float64 are correctly
# preserved-by-physical-value but the BITPIX changes — that is,
# not a regression to test against.
if(EXISTS ${SCIIO_FTT4B_DIR})
    foreach(_stem file007 file010 file011 file015 file016)
        if(EXISTS ${SCIIO_FTT4B_DIR}/${_stem}.fits)
            add_test(NAME roundtrip_${_stem}
                COMMAND bash ${_RT_RUNNER}
                        $<TARGET_FILE:fits_to_h5>
                        $<TARGET_FILE:h5_to_fits>
                        $<TARGET_FILE:fits_compare>
                        ${SCIIO_FTT4B_DIR}/${_stem}.fits
                        ${CMAKE_BINARY_DIR}/roundtrip_${_stem}
            )
            set_tests_properties(roundtrip_${_stem} PROPERTIES ENVIRONMENT "${_RT_ENV}")
        endif()
    endforeach()
endif()

# Local ftt4b corpus: one test per file. file004 (CFITSIO parse failure) and
# file009 (NRAO Random Groups UV data, rejected by M2.9) are expected to fail
# H5Fopen; everything else must traverse cleanly.
if(EXISTS ${SCIIO_FTT4B_DIR}/file005.fits)
    add_executable(test_dup_keyword tests/integration/test_dup_keyword.c)
    target_include_directories(test_dup_keyword PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(test_dup_keyword PRIVATE sciio_vol hdf5-shared)
    add_test(NAME dup_keyword COMMAND test_dup_keyword)
    set_tests_properties(dup_keyword PROPERTIES
        ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
    )
endif()

if(EXISTS ${SCIIO_FTT4B_DIR})
    file(GLOB _ftt4b_files RELATIVE ${SCIIO_FTT4B_DIR} ${SCIIO_FTT4B_DIR}/*.fits)
    list(SORT _ftt4b_files)
    set(_ftt4b_reject "file004.fits" "file009.fits")
    list(LENGTH _ftt4b_files _ftt4b_count)
    message(STATUS "ftt4b corpus: ${_ftt4b_count} file(s) under ${SCIIO_FTT4B_DIR}")
    foreach(_fname ${_ftt4b_files})
        get_filename_component(_stem ${_fname} NAME_WE)
        list(FIND _ftt4b_reject ${_fname} _is_reject)
        if(_is_reject GREATER -1)
            add_test(NAME ftt4b_${_stem}
                COMMAND test_corpus_smoke ${SCIIO_FTT4B_DIR}/${_fname} reject
            )
        else()
            add_test(NAME ftt4b_${_stem}
                COMMAND test_corpus_smoke ${SCIIO_FTT4B_DIR}/${_fname}
            )
        endif()
        set_tests_properties(ftt4b_${_stem} PROPERTIES
            ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR}"
        )
    endforeach()
endif()

# Golden h5ls -r tests, one per fixture.
set(SCIIO_H5LS ${HDF5_DIR}/../bin/h5ls)
foreach(fname primary_only image_2d multi_hdu edge_keywords table_multidim table_vlen)
    add_test(NAME h5ls_golden_${fname}
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/run_h5ls_golden.sh
                ${SCIIO_H5LS}
                ${SCIIO_FIXTURES_DIR}/${fname}.fits
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/${fname}.h5ls.txt
    )
    set_tests_properties(h5ls_golden_${fname} PROPERTIES
        ENVIRONMENT "HDF5_PLUGIN_PATH=${CMAKE_BINARY_DIR};HDF5_VOL_CONNECTOR=sciio;LD_LIBRARY_PATH=${HDF5_DIR}/../lib"
    )
endforeach()
