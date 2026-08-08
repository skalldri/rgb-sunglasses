# Scratch consumer project for the rgbx-sdk — used by sdk-ci.yml and local
# verification. Builds the in-repo hello (C) and plasma (C++) extensions
# through the SDK's public contract, exercising the same pre-project()
# consumption shape as the rgbx-extension-template (minus the download step:
# the SDK tree is supplied via -DRGBX_SDK_SOURCE_DIR, pointing at an
# extracted package-sdk.sh output — NOT at fw/sdk/, which is only a source
# fragment of the assembled SDK).
#
#   cmake -S fw/sdk/tests/consumer -B <build> \
#         -DRGBX_SDK_SOURCE_DIR=<extracted-sdk> -DRGBX_TARGET=arm|wasm \
#         [-DRGBX_STRICT_TOOLCHAIN=ON]
#   cmake --build <build>

cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED RGBX_SDK_SOURCE_DIR)
    message(FATAL_ERROR "pass -DRGBX_SDK_SOURCE_DIR=<extracted rgbx-sdk tree>")
endif()
get_filename_component(RGBX_SDK_SOURCE_DIR "${RGBX_SDK_SOURCE_DIR}" ABSOLUTE)

# RGBX_TARGET selects the toolchain; the toolchain file re-asserts it.
if(NOT DEFINED RGBX_TARGET)
    message(FATAL_ERROR "pass -DRGBX_TARGET=arm or -DRGBX_TARGET=wasm")
endif()
if(RGBX_TARGET STREQUAL "arm")
    set(CMAKE_TOOLCHAIN_FILE "${RGBX_SDK_SOURCE_DIR}/cmake/toolchains/arm-llext.cmake")
else()
    set(CMAKE_TOOLCHAIN_FILE "${RGBX_SDK_SOURCE_DIR}/cmake/toolchains/wasm.cmake")
endif()

project(rgbx_sdk_consumer C CXX)

include("${RGBX_SDK_SOURCE_DIR}/cmake/rgbx-sdk-config.cmake")

get_filename_component(_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
rgbx_add_extension(hello SOURCES "${_repo_root}/fw/extensions/hello/hello.c")
rgbx_add_extension(plasma SOURCES "${_repo_root}/fw/extensions/plasma/plasma.cpp")
# Exercises the issue #295 export surface (libm, __aeabi_uldivmod, memmove)
# so the gate's ACCEPT side is proven, not just its reject side.
rgbx_add_extension(mathtest SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/mathtest.c")
