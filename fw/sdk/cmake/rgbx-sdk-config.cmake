# rgbx-sdk — CMake entry point for standalone rgbx extension builds.
#
# Consumed by include() of this file's absolute path AFTER project(), with
# one of the SDK's toolchain files (cmake/toolchains/arm-llext.cmake or
# wasm.cmake) passed at configure time. See the rgbx-extension-template
# repo's CMakeLists.txt for the canonical consumption pattern.
#
# PUBLIC CONTRACT — keep append-only. The monorepo's registry CI builds every
# community extension against the *release's* SDK using exactly this surface,
# so a breaking change here breaks extensions that were green yesterday:
#   rgbx_add_extension(<name> SOURCES <single .c or .cpp>)
#   RGBX_TARGET            ("arm" | "wasm", set by the toolchain file)
#   RGBX_SDK_SOURCE_DIR    (consumer-side override: use a local SDK tree)
#   RGBX_STRICT_TOOLCHAIN  (make toolchain-pin deviations fatal; CI sets it)

get_filename_component(_RGBX_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED RGBX_TARGET)
    message(FATAL_ERROR "rgbx-sdk: RGBX_TARGET is not set — configure with one of the SDK's toolchain files (cmake/toolchains/arm-llext.cmake or wasm.cmake)")
endif()

# Read RGBX_STRICT_TOOLCHAIN unconditionally: the toolchain files only touch
# it on a pin mismatch, and CMake would otherwise warn "manually-specified
# variable not used" on every fully-pinned CI configure.
if(RGBX_STRICT_TOOLCHAIN)
    message(STATUS "rgbx-sdk: strict toolchain mode (pin deviations are fatal)")
endif()

# The pinned -O level in the toolchain files is part of the reproducibility
# contract; a CMAKE_BUILD_TYPE would append its own -O after ours and
# silently change codegen (and thereby the undefined-symbol gate result).
if(CMAKE_BUILD_TYPE)
    message(WARNING "rgbx-sdk: CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' appends optimization flags after the SDK's pinned -O2; unset it for reproducible builds")
endif()

if(RGBX_TARGET STREQUAL "wasm")
    find_program(RGBX_NODE node)
    if(NOT RGBX_NODE)
        message(FATAL_ERROR "rgbx-sdk: node not found — Node.js (>= 20) is required to run the wasm module gate (check-wasm.mjs)")
    endif()
endif()

# rgbx_add_extension(<name> SOURCES <tu>)
#
# One extension = one translation unit (matching the device build and the
# on-device single-TU convention). Produces:
#   arm:  <build>/<name>.llext   (compile -> mandatory `ld -r` partial link
#          -> check-llext.sh gate: undefined symbols vs the device's export
#          table, section layout, 24 KB llext heap fit)
#   wasm: <build>/<name>.wasm    (TU + sim shims, reactor model, rgbx exports
#          -> check-wasm.mjs gate: zero imports + required exports)
function(rgbx_add_extension name)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})
    list(LENGTH ARG_SOURCES _n_sources)
    if(NOT _n_sources EQUAL 1)
        message(FATAL_ERROR "rgbx_add_extension(${name}): exactly one source file required (one extension = one translation unit); got ${_n_sources}")
    endif()
    list(GET ARG_SOURCES 0 _src)
    get_filename_component(_src "${_src}" ABSOLUTE)

    # A C++ TU in a C-only project() yields an EMPTY object library and a
    # baffling "ld: no input files" — fail at configure time instead.
    get_property(_langs GLOBAL PROPERTY ENABLED_LANGUAGES)
    if(_src MATCHES "\\.(cpp|cc|cxx)$" AND NOT "CXX" IN_LIST _langs)
        message(FATAL_ERROR "rgbx_add_extension(${name}): ${_src} is C++ but the project() does not enable CXX — use project(<name> C CXX)")
    endif()

    if(RGBX_TARGET STREQUAL "arm")
        add_library(${name}_obj OBJECT "${_src}")
        target_include_directories(${name}_obj PRIVATE "${_RGBX_SDK_ROOT}/include")
        target_include_directories(${name}_obj SYSTEM PRIVATE "${_RGBX_SDK_ROOT}/arm/shim/include")
        target_compile_options(${name}_obj PRIVATE -Wall -Wextra)

        # The partial link is mandatory for C++ (COMDAT group sections
        # otherwise interleave with .data/.bss file offsets and the on-device
        # loader rejects the file); applied uniformly, matching
        # fw/extensions/build.sh. The gate runs in the same command chain so
        # a gate failure fails the build.
        set(_llext "${CMAKE_CURRENT_BINARY_DIR}/${name}.llext")
        add_custom_command(
            OUTPUT "${_llext}"
            COMMAND "${CMAKE_LINKER}" -r $<TARGET_OBJECTS:${name}_obj> -o "${_llext}"
            COMMAND bash "${_RGBX_SDK_ROOT}/arm/check-llext.sh"
                    --nm "${CMAKE_NM}" --readelf "${CMAKE_READELF}"
                    --allowed "${_RGBX_SDK_ROOT}/arm/allowed-symbols.txt"
                    "${_llext}"
            DEPENDS ${name}_obj $<TARGET_OBJECTS:${name}_obj>
            COMMENT "Linking (ld -r) and gating ${name}.llext"
            VERBATIM
            COMMAND_EXPAND_LISTS)
        add_custom_target(${name}_llext ALL DEPENDS "${_llext}")
    elseif(RGBX_TARGET STREQUAL "wasm")
        # The wasm side only exists in a packaged SDK tree (package-sdk.sh
        # copies the sim shims in); fail with the actual cause rather than a
        # missing-source error from the generator.
        if(NOT EXISTS "${_RGBX_SDK_ROOT}/wasm/shim/sim_shim.c")
            message(FATAL_ERROR "rgbx-sdk: '${_RGBX_SDK_ROOT}' has no wasm/shim/ — the wasm target needs a packaged SDK tree (a package-sdk.sh output), not fw/sdk/ from the source tree")
        endif()
        # The sim shims compile as C alongside the extension TU:
        # sim_shim.c (self-contained printk -> exported log buffer, keeps the
        # module import-free) and abi_offsets.c (static_asserts the struct
        # layout against the header — ABI drift fails the build).
        add_executable(${name}
            "${_src}"
            "${_RGBX_SDK_ROOT}/wasm/shim/sim_shim.c"
            "${_RGBX_SDK_ROOT}/wasm/shim/abi_offsets.c")
        target_include_directories(${name} PRIVATE "${_RGBX_SDK_ROOT}/include")
        target_include_directories(${name} SYSTEM PRIVATE "${_RGBX_SDK_ROOT}/wasm/shim/include")
        target_compile_options(${name} PRIVATE -Wall -Wextra)
        # -mexec-model=reactor is link-only; export visibility comes from the
        # linker (--export-if-defined tolerates absent optional exports).
        # The export surface is single-sourced in wasm/shim/rgbx-exports.txt
        # (shared with fw/sim/build-extensions.sh).
        file(STRINGS "${_RGBX_SDK_ROOT}/wasm/shim/rgbx-exports.txt" _rgbx_export_lines)
        set(_rgbx_export_flags "")
        foreach(_line IN LISTS _rgbx_export_lines)
            string(STRIP "${_line}" _line)
            if(_line STREQUAL "" OR _line MATCHES "^#")
                continue()
            endif()
            list(APPEND _rgbx_export_flags "-Wl,--export-if-defined=${_line}")
        endforeach()
        target_link_options(${name} PRIVATE -mexec-model=reactor ${_rgbx_export_flags})
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${RGBX_NODE}" "${_RGBX_SDK_ROOT}/wasm/check-wasm.mjs" "$<TARGET_FILE:${name}>"
            COMMENT "Gating ${name}.wasm (zero imports + required exports)"
            VERBATIM)
    else()
        message(FATAL_ERROR "rgbx-sdk: unknown RGBX_TARGET '${RGBX_TARGET}' (expected 'arm' or 'wasm')")
    endif()
endfunction()
