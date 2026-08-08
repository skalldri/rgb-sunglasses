#!/usr/bin/env bash
# One-time (idempotent) setup for the extension simulator: npm deps, wasm
# toolchain, TypeScript build, and a first build of the example extensions.
set -euo pipefail
SIM_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "== toolchain =="
"$SIM_DIR/scripts/install-toolchain.sh"

echo "== npm ci =="
(cd "$SIM_DIR" && npm ci --silent)

echo "== tsc =="
(cd "$SIM_DIR" && npx tsc -p tsconfig.node.json)

echo "== build extensions + audio_dsp =="
"$SIM_DIR/build-extensions.sh"

echo "setup complete — try: fw/sim/rgbx-sim run cpptest --scenario silence --seconds 3"
