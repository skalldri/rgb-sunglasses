# Toolchain file for building rgbx extensions to WebAssembly for the
# hardware-free simulator (https://rgb-sunglasses.autom8ed.com/sim/).
# Mirrors fw/sim/build-extensions.sh flag-for-flag.
#
# Toolchain resolution: $WASI_SDK_PATH env override, else the pinned wasi-sdk
# is auto-installed by scripts/install-wasi-sdk.sh (instant no-op when
# cached; re-executed on every try_compile).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

# Reactor modules have no _start; a try_compile executable link would fail.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

execute_process(
    COMMAND "${CMAKE_CURRENT_LIST_DIR}/../../scripts/install-wasi-sdk.sh"
    OUTPUT_VARIABLE _rgbx_wasi_root
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _rgbx_wasi_rc)
if(NOT _rgbx_wasi_rc EQUAL 0)
    message(FATAL_ERROR "rgbx-sdk: failed to resolve/install wasi-sdk (install-wasi-sdk.sh exited ${_rgbx_wasi_rc})")
endif()

set(CMAKE_C_COMPILER "${_rgbx_wasi_root}/bin/clang")
set(CMAKE_CXX_COMPILER "${_rgbx_wasi_root}/bin/clang++")
set(CMAKE_EXECUTABLE_SUFFIX ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_C ".wasm")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".wasm")

# -Wno-null-conversion is load-bearing for C++: clang (unlike the device's
# GCC) warns on rgbx_animation.h's `NULL ? NULL : params` macro expansion;
# the header is part of the ABI contract and stays as-is (see
# fw/sim/build-extensions.sh).
set(CMAKE_C_FLAGS_INIT "-O2 -g")
set(CMAKE_CXX_FLAGS_INIT "-O2 -g -std=c++23 -fno-exceptions -fno-rtti -Wno-null-conversion")

set(RGBX_TARGET "wasm" CACHE STRING "rgbx extension build target")

# Version check against the pin (wasi-sdk roots carry a VERSION file).
set(_rgbx_wasi_expected "33.0")
set(_rgbx_wasi_actual "")
if(EXISTS "${_rgbx_wasi_root}/VERSION")
    file(STRINGS "${_rgbx_wasi_root}/VERSION" _rgbx_wasi_version_lines LIMIT_COUNT 1)
    list(GET _rgbx_wasi_version_lines 0 _rgbx_wasi_actual)
endif()
if(NOT _rgbx_wasi_actual MATCHES "${_rgbx_wasi_expected}" AND NOT _rgbx_wasi_root MATCHES "${_rgbx_wasi_expected}")
    if(RGBX_STRICT_TOOLCHAIN)
        message(FATAL_ERROR "rgbx-sdk: strict toolchain mode requires wasi-sdk ${_rgbx_wasi_expected}; found '${_rgbx_wasi_actual}' at ${_rgbx_wasi_root}")
    elseif(NOT _RGBX_WASI_TOOLCHAIN_WARNED)
        message(WARNING "rgbx-sdk: wasi-sdk at ${_rgbx_wasi_root} does not look like the pinned ${_rgbx_wasi_expected}")
        set(_RGBX_WASI_TOOLCHAIN_WARNED ON CACHE INTERNAL "")
    endif()
endif()
