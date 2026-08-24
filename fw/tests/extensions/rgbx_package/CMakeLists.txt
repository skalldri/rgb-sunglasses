# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rgbx_package_tests)

set(FW_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

target_sources(app PRIVATE
  src/main.cpp
  ${FW_ROOT}/src/extensions/rgbx_package.cpp
  ${FW_ROOT}/src/extensions/rgbx_package_psa.cpp
  ${FW_ROOT}/src/extensions/rgbx_staged_package.cpp
)

target_include_directories(app PRIVATE ${FW_ROOT}/src ${FW_ROOT}/include)

# The release-package test asserts the parsed source digest equals the SHA-256
# of the exact source the SDK consumer compiled into that package. Compute it
# from the committed source at build time so the expectation cannot drift from
# the source, and hand it to the test as a hex string.
set(_rgbx_v2_test_source ${FW_ROOT}/sdk/tests/consumer/rgbx_v2_test.c)
file(SHA256 ${_rgbx_v2_test_source} _rgbx_v2_test_source_sha256)
target_compile_definitions(app PRIVATE
  RGBX_V2_TEST_SOURCE_SHA256="${_rgbx_v2_test_source_sha256}")
