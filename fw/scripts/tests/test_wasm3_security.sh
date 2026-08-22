#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fw_root="$(cd "$script_dir/../.." && pwd)"
test_tmp="$(mktemp -d "${TMPDIR:-/tmp}/rgbx-wasm3-security.XXXXXX")"
trap 'rm -rf "$test_tmp"' EXIT

compiler="${CC:-cc}"
sources=(
  m3_bind.c
  m3_code.c
  m3_compile.c
  m3_core.c
  m3_env.c
  m3_exec.c
  m3_function.c
  m3_info.c
  m3_module.c
  m3_parse.c
  m3_validate.c
)
source_paths=()
for source in "${sources[@]}"; do
  source_paths+=("$fw_root/third_party/wasm3/source/$source")
done

"$compiler" \
  -std=c11 -O1 -g -fno-omit-frame-pointer \
  -fsanitize=address,undefined \
  -Dd_m3HasFloat=0 \
  -Dd_m3CascadedOpcodes=0 \
  -Dd_m3VerboseErrorMessages=0 \
  -Dd_m3RecordBacktraces=0 \
  -Dd_m3MaxFunctionStackHeight=128 \
  -I"$fw_root/third_party/wasm3/source" \
  -I"$fw_root/tests/extensions/wasm_mvp_runtime/src" \
  "$script_dir/wasm3_security_harness.c" \
  "${source_paths[@]}" \
  -o "$test_tmp/wasm3-security"

ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$test_tmp/wasm3-security"

