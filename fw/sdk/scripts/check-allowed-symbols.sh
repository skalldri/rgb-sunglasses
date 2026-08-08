#!/usr/bin/env bash
# Assert that every symbol the SDK's allowed-symbols.txt sanctions is
# actually exported by the built firmware — i.e. allowed ⊆ built exports.
#
#   check-allowed-symbols.sh <zephyr.elf> <allowed-symbols.txt>
#
# Every base-image EXPORT_SYMBOL(x) emits a symbol named __llext_sym_x, so
# the built ELF's symbol table IS the device's llext export surface. If this
# check fails, the SDK gate would pass an extension that then fails symbol
# resolution at load time on the device — the exact silent-failure class the
# gate exists to prevent. Fix by either exporting the symbol in firmware or
# removing it from allowed-symbols.txt.
#
# Deliberately one-directional: the firmware exporting MORE than the list
# (z_impl_* syscall implementations, arch internals) is by design — the list
# is the curated extension ABI, the export table is an implementation detail.
#
# NM defaults to host nm: GNU binutils reads foreign-ELF symbol tables fine;
# override with NM=<path> if needed.

set -euo pipefail

ELF="${1:-}"
ALLOWED="${2:-}"
if [ -z "$ELF" ] || [ -z "$ALLOWED" ]; then
    echo "usage: check-allowed-symbols.sh <zephyr.elf> <allowed-symbols.txt>" >&2
    exit 2
fi
NM="${NM:-nm}"

exports="$("$NM" "$ELF" | grep -o '__llext_sym_[A-Za-z0-9_]*' | sed 's/^__llext_sym_//' | sort -u)"
if [ -z "$exports" ]; then
    echo "error: no __llext_sym_* symbols in $ELF — is this a CONFIG_LLEXT firmware image?" >&2
    exit 2
fi

missing=""
while read -r sym; do
    case "$sym" in ''|'#'*) continue ;; esac
    if ! grep -qx "$sym" <<<"$exports"; then
        missing="$missing $sym"
    fi
done < <(cat "$ALLOWED")

if [ -n "$missing" ]; then
    echo "allowed-symbols.txt sanctions symbol(s) the built firmware does NOT export:$missing" >&2
    echo "  An extension calling these would pass the SDK gate but fail llext load on-device." >&2
    echo "  Export them in firmware (EXPORT_SYMBOL) or remove them from $(basename "$ALLOWED")." >&2
    exit 1
fi

count="$(grep -cv -e '^#' -e '^$' "$ALLOWED" || true)"
echo "OK: all $count allowed symbols are present in the firmware export table"
