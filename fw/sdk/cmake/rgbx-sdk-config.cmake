# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stuart Alldritt
# rgbx-sdk — CMake entry point for standalone rgbx extension builds.
#
# Consumed by include() of this file's absolute path AFTER project(), with
# one of the SDK's toolchain files (cmake/toolchains/arm-llext.cmake,
# wasm.cmake, or rgbx-v2.cmake) passed at configure time. See the rgbx-extension-template
# repo's CMakeLists.txt for the canonical consumption pattern.
#
# PUBLIC CONTRACT — keep append-only. The monorepo's registry CI builds every
# community extension against the *release's* SDK using exactly this surface,
# so a breaking change here breaks extensions that were green yesterday:
#   rgbx_add_extension(<name> SOURCES <single .c or .cpp> [MANIFEST <json>])
#   RGBX_TARGET            ("arm" | "wasm" | "rgbx-v2", set by the toolchain file)
#   RGBX_SDK_SOURCE_DIR    (consumer-side override: use a local SDK tree)
#   RGBX_STRICT_TOOLCHAIN  (make toolchain-pin deviations fatal; CI sets it)
#
# "Append-only" governs the CMake surface above — the names and arguments a
# consumer's CMakeLists.txt spells out. It does NOT extend to the compile/link
# flags applied underneath, and those are not frozen: TIGHTENING THEM IS A
# BREAKING, VERSIONED EVENT. A registry extension pinned at a `rev` compiles
# against whatever the *release's* SDK passes, so a newly-fatal diagnostic can
# stop it building with no commit of its own — the extension did not change,
# the gate did. Precedent: issue #351 added -Wl,--fatal-warnings to the wasm
# link, which turns a hand-written `extern "C" int printk(...)` from a silently
# trapping module into a link error (rgbx-mask-eyes was exactly that case).
# That was deliberate — it is the only automated signature check wasm has, and
# the alternative is shipping modules that trap on first call. But it belongs
# in release notes as a breaking change, and a registry extension that stops
# building after an SDK bump should be checked against this list before its
# author is told the breakage is theirs.

get_filename_component(_RGBX_SDK_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED RGBX_TARGET)
    message(FATAL_ERROR "rgbx-sdk: RGBX_TARGET is not set; configure with one of the SDK's toolchain files")
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

if(RGBX_TARGET STREQUAL "wasm" OR RGBX_TARGET STREQUAL "rgbx-v2")
    find_program(RGBX_NODE node)
    if(NOT RGBX_NODE)
        message(FATAL_ERROR "rgbx-sdk: node not found — Node.js (>= 20) is required to run the wasm module gate (check-wasm.mjs)")
    endif()
    # ...and it has to be new enough to RUN it. check-wasm.mjs uses top-level
    # await, so an older node fails the wasm link with a bare SyntaxError
    # pointing into this SDK tree, naming neither node nor a version — the arm
    # target having built fine first, so it reads like the SDK shipped broken
    # JS (rgbx-extension-template#5, reported against Ubuntu 22.04's v12.22.9).
    # Deferring the check to link time is what made it unreadable; do it here.
    execute_process(COMMAND "${RGBX_NODE}" --version
                    OUTPUT_VARIABLE _rgbx_node_version
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                    RESULT_VARIABLE _rgbx_node_rc)
    if(NOT _rgbx_node_rc EQUAL 0)
        message(FATAL_ERROR "rgbx-sdk: '${RGBX_NODE} --version' failed (exit ${_rgbx_node_rc})")
    endif()
    if(NOT _rgbx_node_version MATCHES "^v([0-9]+)\\.")
        message(FATAL_ERROR "rgbx-sdk: unexpected 'node --version' output from '${RGBX_NODE}': '${_rgbx_node_version}'")
    endif()
    # Read CMAKE_MATCH_1 only under the confirmed match above — a later
    # regex anywhere in this scope would otherwise decide the version.
    if(CMAKE_MATCH_1 LESS 20)
        message(FATAL_ERROR "rgbx-sdk: node ${_rgbx_node_version} at '${RGBX_NODE}' is too old — "
            "Node.js >= 20 is required to run the wasm module gate (check-wasm.mjs uses "
            "top-level await). Install a newer Node (e.g. nvm install 22), then re-configure "
            "with -URGBX_NODE: RGBX_NODE is cached, so an existing build tree keeps using the "
            "old interpreter.")
    endif()
endif()

# rgbx_add_extension(<name> SOURCES <tu> [MANIFEST <json>])
#
# One extension = one translation unit (matching the device build and the
# on-device single-TU convention). Produces:
#   arm:  <build>/<name>.llext   (compile -> mandatory `ld -r` partial link
#          -> objcopy --strip-debug -> check-llext.sh gate: undefined symbols
#          vs the device's export table, section layout, 24 KB llext heap
#          fit, no debug sections left)
#         <build>/<name>.llext.debug  (the unstripped partial link: same
#          code, plus DWARF — ships as a release asset next to the .llext so
#          a fault PC offset can be resolved with addr2line/gdb)
#   wasm: <build>/<name>.wasm    (TU + sim shims, reactor model, rgbx exports
#          -> check-wasm.mjs gate: zero imports + required exports)
#   rgbx-v2: <build>/<name>.wasm + <build>/<name>.rgbx (freestanding guest,
#          deterministic memoryless post-link, exact ABI gate, canonical package)
function(rgbx_add_extension name)
    cmake_parse_arguments(ARG "" "MANIFEST" "SOURCES" ${ARGN})
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
        #
        # The partial link lands in <name>.llext.debug (full DWARF, since the
        # toolchain compiles with -g) and the shipped <name>.llext is that
        # file with the debug sections stripped. The device loader only ever
        # reads SHF_ALLOC sections, so DWARF costs nothing at runtime — but it
        # was ~90% of the file (a 73 KB mask_eyes.llext carried 5.6 KB of
        # loadable code), i.e. ~90% of every BLE upload and of the NAND
        # footprint. --strip-debug (not --strip-all) keeps .symtab/.strtab:
        # the loader needs them for relocation and the gate needs them for
        # `nm -u`. The .debug sidecar is a first-class output so the release
        # workflow can attach it next to the .llext.
        set(_llext "${CMAKE_CURRENT_BINARY_DIR}/${name}.llext")
        set(_llext_debug "${_llext}.debug")
        add_custom_command(
            OUTPUT "${_llext}" "${_llext_debug}"
            COMMAND "${CMAKE_LINKER}" -r $<TARGET_OBJECTS:${name}_obj> -o "${_llext_debug}"
            COMMAND "${CMAKE_OBJCOPY}" --strip-debug "${_llext_debug}" "${_llext}"
            COMMAND bash "${_RGBX_SDK_ROOT}/arm/check-llext.sh"
                    --nm "${CMAKE_NM}" --readelf "${CMAKE_READELF}"
                    --allowed "${_RGBX_SDK_ROOT}/arm/allowed-symbols.txt"
                    "${_llext}"
            DEPENDS ${name}_obj $<TARGET_OBJECTS:${name}_obj>
            COMMENT "Linking (ld -r), stripping and gating ${name}.llext"
            VERBATIM
            COMMAND_EXPAND_LISTS)
        add_custom_target(${name}_llext ALL DEPENDS "${_llext}" "${_llext_debug}")
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
        # --fatal-warnings promotes wasm-ld's "function signature mismatch"
        # warning to a link error. wasm calls are typed by full signature, so
        # a mismatch is not a discarded return value: wasm-ld emits a stub
        # that traps with `RuntimeError: unreachable` on first call, having
        # only warned at build time. Issue #351. Keep this in step with
        # fw/sim/build-extensions.sh, which passes the same flag.
        target_link_options(${name} PRIVATE -mexec-model=reactor
                            -Wl,--fatal-warnings ${_rgbx_export_flags})
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND "${RGBX_NODE}" "${_RGBX_SDK_ROOT}/wasm/check-wasm.mjs" "$<TARGET_FILE:${name}>"
            COMMENT "Gating ${name}.wasm (zero imports + required exports)"
            VERBATIM)
    elseif(RGBX_TARGET STREQUAL "rgbx-v2")
        if(NOT ARG_MANIFEST)
            message(FATAL_ERROR "rgbx_add_extension(${name}): the rgbx-v2 target requires MANIFEST <json>")
        endif()
        get_filename_component(_manifest "${ARG_MANIFEST}" ABSOLUTE)
        if(NOT EXISTS "${_manifest}")
            message(FATAL_ERROR "rgbx_add_extension(${name}): manifest not found: ${_manifest}")
        endif()
        if(NOT EXISTS "${_RGBX_SDK_ROOT}/wasm-v2/prepare-rgbx-v2.mjs")
            message(FATAL_ERROR "rgbx-sdk: '${_RGBX_SDK_ROOT}' has no wasm-v2 tooling; use a packaged SDK from an RGBX v2 firmware release")
        endif()

        add_executable(${name}_raw "${_src}")
        set_target_properties(${name}_raw PROPERTIES OUTPUT_NAME "${name}.raw")
        target_include_directories(${name}_raw PRIVATE "${_RGBX_SDK_ROOT}/include")
        target_compile_options(${name}_raw PRIVATE -Wall -Wextra -Werror)
        target_link_options(${name}_raw PRIVATE
            -nostdlib
            -Wl,--no-entry
            -Wl,--allow-undefined
            -Wl,--fatal-warnings
            -Wl,--gc-sections
            -Wl,--compress-relocations
            -Wl,--strip-all
            -Wl,--export=rgbx_init
            -Wl,--export=rgbx_tick)

        set(_wasm "${CMAKE_CURRENT_BINARY_DIR}/${name}.wasm")
        set(_rgbx "${CMAKE_CURRENT_BINARY_DIR}/${name}.rgbx")
        # The gate and the packager both read the release's admission profile
        # and toolchain pins out of sdk-manifest.json, and both refuse to run
        # without it: neither tool keeps a fallback copy of a device limit.
        set(_sdk_manifest "${_RGBX_SDK_ROOT}/sdk-manifest.json")
        if(NOT EXISTS "${_sdk_manifest}")
            message(FATAL_ERROR "rgbx-sdk: '${_RGBX_SDK_ROOT}' has no sdk-manifest.json; use a packaged SDK from an RGBX v2 firmware release")
        endif()
        add_custom_command(
            OUTPUT "${_wasm}" "${_rgbx}"
            COMMAND "${RGBX_NODE}" "${_RGBX_SDK_ROOT}/wasm-v2/prepare-rgbx-v2.mjs"
                    "$<TARGET_FILE:${name}_raw>" "${_wasm}"
            COMMAND "${CMAKE_COMMAND}" -E env
                    "RGBX_SDK_MANIFEST=${_sdk_manifest}"
                    "${RGBX_NODE}" "${_RGBX_SDK_ROOT}/wasm-v2/check-rgbx-v2.mjs"
                    "${_wasm}"
            COMMAND "${CMAKE_COMMAND}" -E env
                    "RGBX_SDK_MANIFEST=${_sdk_manifest}"
                    "RGBX_SOURCE_FILE=${_src}"
                    "${RGBX_NODE}" "${_RGBX_SDK_ROOT}/wasm-v2/package-rgbx.mjs"
                    "${_manifest}" "${_wasm}" "${_rgbx}"
            DEPENDS ${name}_raw "${_src}" "${_manifest}" "${_sdk_manifest}"
                    "${_RGBX_SDK_ROOT}/wasm-v2/prepare-rgbx-v2.mjs"
                    "${_RGBX_SDK_ROOT}/wasm-v2/check-rgbx-v2.mjs"
                    "${_RGBX_SDK_ROOT}/wasm-v2/package-rgbx.mjs"
                    "${_RGBX_SDK_ROOT}/wasm-v2/rgbx-v2-policy.mjs"
            COMMENT "Preparing, gating, and packaging ${name}.rgbx"
            VERBATIM)
        add_custom_target(${name}_rgbx ALL DEPENDS "${_wasm}" "${_rgbx}")
    else()
        message(FATAL_ERROR "rgbx-sdk: unknown RGBX_TARGET '${RGBX_TARGET}'")
    endif()
endfunction()
