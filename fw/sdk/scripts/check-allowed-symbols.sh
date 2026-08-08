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

# Reverse direction for the one file whose exports exist FOR extensions:
# every EXPORT_SYMBOL in extension_exports.c must be mirrored in the allowed
# list, or the new export is silently unusable (the SDK gate keeps rejecting
# it even though the device would resolve it). The firmware's OTHER exports
# (z_impl_* syscall internals etc.) are deliberately not mirrored — this
# check is scoped to the extension-facing file only.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
EXPORTS_SRC="$SCRIPT_DIR/../../src/extensions/extension_exports.c"
if [ -f "$EXPORTS_SRC" ]; then
    unmirrored=""
    while read -r sym; do
        [ -n "$sym" ] || continue
        if ! grep -qx "$sym" <(grep -v '^#' "$ALLOWED"); then
            unmirrored="$unmirrored $sym"
        fi
    done < <(grep -o '^EXPORT_SYMBOL([A-Za-z0-9_]*)' "$EXPORTS_SRC" | sed 's/EXPORT_SYMBOL(\(.*\))/\1/')
    if [ -n "$unmirrored" ]; then
        echo "extension_exports.c exports symbol(s) missing from $(basename "$ALLOWED"):$unmirrored" >&2
        echo "  The two files must move in lockstep (see the invariant note in each) —" >&2
        echo "  without the mirror line the SDK gate rejects extensions calling the new export." >&2
        exit 1
    fi
fi

count="$(grep -cv -e '^#' -e '^$' "$ALLOWED" || true)"
echo "OK: all $count allowed symbols are present in the firmware export table"
