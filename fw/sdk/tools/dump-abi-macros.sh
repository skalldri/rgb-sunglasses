#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Authoritative extractor for the RGBX ABI constants that the SDK manifest and
# the drift gate both depend on. It is the single source both tools read, so
# they cannot disagree about what a macro means.
#
#   dump-abi-macros.sh <include-dir>
#
# <include-dir> is the directory that contains rgbx/rgbx_v2.h and rgbx/rgbx_api.h
# (fw/include or a packaged SDK's include/). Output is stable "KIND KEY VALUE"
# lines, sorted, one per constant:
#
#   abi abiVersion <n>
#   abi rgbxV2AbiVersion <n>
#   cap <name> <bit>
#   policy <field> <value>
#
# Every value is produced by the host C compiler, not by a regex over the
# header text. That matters: a decoy `#define ... 8192` sitting in an #if 0
# block, or inside a comment, above the real define fools first-match text
# scraping but not the preprocessor, which reports only the definition the
# firmware actually compiles. Expression-valued macros (a frame's pixel count,
# the spans per tick) are likewise evaluated rather than copied verbatim. Each
# macro is additionally required to have exactly one active definition, so a
# genuine second `#define` (a real redefinition) is rejected rather than
# silently resolved to the last one.

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: dump-abi-macros.sh <include-dir>" >&2
    exit 2
fi
include_dir="$1"
cc="${CC:-cc}"

# manifest field -> ABI header macro. Primitive pins only; anything derived
# (pixel count, spans per tick) is evaluated below like any other macro so a
# derived value can never disagree with the primitives it comes from.
policy_fields=(
    "width RGBX_V2_WIDTH"
    "height RGBX_V2_HEIGHT"
    "pixelsPerSpan RGBX_V2_PIXELS_PER_SPAN"
    "maxParams RGBX_V2_MAX_PARAMS"
    "maxStringParams RGBX_V2_MAX_STRING_PARAMS"
    "stringParamSize RGBX_V2_STRING_PARAM_SIZE"
    "audioBandCount RGBX_V2_AUDIO_BAND_COUNT"
    "audioDisplayBucketCount RGBX_V2_AUDIO_DISPLAY_BUCKET_COUNT"
    "imuAxisCount RGBX_V2_IMU_AXIS_COUNT"
    "maxDiagnosticsPerTick RGBX_V2_DIAGNOSTIC_COUNT"
    "moduleMaxBytes RGBX_V2_MODULE_MAX_BYTES"
    "maxFunctions RGBX_V2_MAX_FUNCTIONS"
    "maxGlobals RGBX_V2_MAX_GLOBALS"
    "maxLocalsPerFunction RGBX_V2_MAX_LOCALS_PER_FUNCTION"
    "minImports RGBX_V2_MIN_IMPORTS"
    "maxImports RGBX_V2_MAX_IMPORTS"
    "maxParamCallsPerTick RGBX_V2_MAX_PARAM_CALLS_PER_TICK"
    "maxInputCallsPerTick RGBX_V2_MAX_INPUT_CALLS_PER_TICK"
    "sectionAllowedMask RGBX_V2_SECTION_ALLOWED_MASK"
    "sectionRequiredMask RGBX_V2_SECTION_REQUIRED_MASK"
)

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

printf '#include <rgbx/rgbx_api.h>\n#include <rgbx/rgbx_v2.h>\n' > "$tmp/probe.c"

# The set of active #define directives the preprocessor keeps (an #if 0 or a
# commented shadow is not among them). Used both to enforce single-definition
# and to discover the capability macros without hardcoding their names.
defs="$("$cc" -E -dD -I "$include_dir" "$tmp/probe.c")"

require_one_definition() {
    local macro="$1"
    local count
    count="$(printf '%s\n' "$defs" | grep -c "^#define ${macro} " || true)"
    if [ "$count" != 1 ]; then
        echo "error: macro ${macro} has ${count} active definitions (want exactly 1)" >&2
        exit 1
    fi
}

# Capability macros, discovered from the active defines, excluding the _ALL
# aggregate. Sorted for stable output.
cap_macros="$(printf '%s\n' "$defs" \
    | sed -n 's/^#define \(RGBX_V2_CAPABILITY_[A-Z][A-Z0-9]*\) .*/\1/p' \
    | grep -v '^RGBX_V2_CAPABILITY_ALL$' | sort -u)"
if [ -z "$cap_macros" ]; then
    echo "error: the ABI header declares no RGBX_V2_CAPABILITY_* permission bits" >&2
    exit 1
fi

# Build a program that prints each macro's evaluated value, and assert each is
# defined exactly once on the way.
{
    echo '#include <stdio.h>'
    echo '#include <rgbx/rgbx_api.h>'
    echo '#include <rgbx/rgbx_v2.h>'
    echo 'int main(void){'
} > "$tmp/dump.c"

emit() {
    # kind key macro
    require_one_definition "$3"
    printf 'printf("%s %s %%llu\\n",(unsigned long long)(%s));\n' "$1" "$2" "$3" >> "$tmp/dump.c"
}

require_one_definition RGBX_ABI_VERSION
require_one_definition RGBX_V2_ABI_VERSION
printf 'printf("abi abiVersion %%llu\\n",(unsigned long long)(RGBX_ABI_VERSION));\n' >> "$tmp/dump.c"
printf 'printf("abi rgbxV2AbiVersion %%llu\\n",(unsigned long long)(RGBX_V2_ABI_VERSION));\n' >> "$tmp/dump.c"

while IFS= read -r macro; do
    [ -n "$macro" ] || continue
    require_one_definition "$macro"
    name="$(printf '%s' "$macro" | sed 's/^RGBX_V2_CAPABILITY_//' | tr '[:upper:]' '[:lower:]')"
    printf 'printf("cap %s %%llu\\n",(unsigned long long)(%s));\n' "$name" "$macro" >> "$tmp/dump.c"
done <<< "$cap_macros"

for pair in "${policy_fields[@]}"; do
    field="${pair%% *}"
    macro="${pair##* }"
    require_one_definition "$macro"
    printf 'printf("policy %s %%llu\\n",(unsigned long long)(%s));\n' "$field" "$macro" >> "$tmp/dump.c"
done

echo 'return 0;}' >> "$tmp/dump.c"

"$cc" -I "$include_dir" -O0 -o "$tmp/dump" "$tmp/dump.c"
"$tmp/dump" | LC_ALL=C sort
