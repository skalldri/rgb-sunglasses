#!/usr/bin/env bash
# Post-link gate for ARM .llext extension builds — the standalone analog of
# the device's llext symbol resolution and region checks. Catches at build
# time the whole class of "compiles fine, silently fails to load on device"
# failures.
#
#   check-llext.sh --nm <nm> --readelf <readelf> --allowed <file> <name.llext>
#
# Three checks:
#  1. Undefined symbols: every one must be in the allowed list (the complete
#     set the firmware exports to extensions). Anything else — sinf (no libm
#     on-target), __aeabi_* (no libgcc helpers exported: 64-bit division,
#     soft-float) — would fail llext symbol resolution at load time.
#  2. Section layout: .exported_sym present and well-formed, and allocatable
#     PROGBITS file offsets strictly non-overlapping — the precondition for
#     the on-device loader's region-overlap check ("Region 0 ELF file range
#     ... overlaps with 1"). ld -r ordering is an implementation behavior,
#     so this stays a permanent guard rather than a one-time verification.
#  3. Size: total SHF_ALLOC bytes (the loader copies/allocates all alloc
#     sections, including .bss, into the llext heap) against
#     CONFIG_LLEXT_HEAP_SIZE = 24 KB.
#
# Parsing note: readelf -S -W output is processed in plain bash ($((16#...)))
# rather than awk — the "[ Nr]" column splits inconsistently for awk, and
# strtonum() is gawk-only (ubuntu runners default to mawk).

set -euo pipefail

NM=""
READELF=""
ALLOWED=""
LLEXT=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --nm) NM="$2"; shift 2 ;;
        --readelf) READELF="$2"; shift 2 ;;
        --allowed) ALLOWED="$2"; shift 2 ;;
        *) LLEXT="$1"; shift ;;
    esac
done

if [ -z "$NM" ] || [ -z "$READELF" ] || [ -z "$ALLOWED" ] || [ -z "$LLEXT" ]; then
    echo "usage: check-llext.sh --nm <nm> --readelf <readelf> --allowed <file> <name.llext>" >&2
    exit 2
fi

HEAP_LIMIT=24576  # CONFIG_LLEXT_HEAP_SIZE (KB -> bytes) on proto0
fail=0

# --- 1. undefined-symbol gate ------------------------------------------------
bad=""
while read -r sym; do
    [ -n "$sym" ] || continue
    if ! grep -qx "$sym" <(grep -v '^#' "$ALLOWED"); then
        bad="$bad $sym"
    fi
done < <("$NM" -u "$LLEXT" | awk '{print $NF}')
if [ -n "$bad" ]; then
    echo "$LLEXT: undefined symbol(s) the device does not export:$bad" >&2
    echo "  The firmware only exports the symbols in $(basename "$ALLOWED") to extensions." >&2
    echo "  Common causes: libm calls (sinf/cosf - no math library on-target)," >&2
    echo "  64-bit division or soft-float arithmetic (__aeabi_* libgcc helpers)." >&2
    echo "  Use integer math or single-precision float expressions the FPU handles inline." >&2
    fail=1
fi

# --- 2 + 3. section layout and heap fit --------------------------------------
# Each relevant readelf -S -W line, after stripping the "[ Nr]" prefix, is:
#   Name Type Addr Off Size ES Flg Lk Inf Al
exported_size=0
alloc_total=0
ranges=""  # "off size name" per alloc PROGBITS section, newline-separated
while read -r name type _addr off size _es flg _rest; do
    case "$type" in PROGBITS|NOBITS|INIT_ARRAY|FINI_ARRAY|PREINIT_ARRAY) ;; *) continue ;; esac
    size_dec=$((16#$size))
    if [ "$name" = ".exported_sym" ]; then
        exported_size=$size_dec
    fi
    case "$flg" in *A*) ;; *) continue ;; esac
    alloc_total=$((alloc_total + size_dec))
    if [ "$type" != "NOBITS" ]; then
        ranges+="$((16#$off)) $size_dec $name"$'\n'
    fi
done < <("$READELF" -S -W "$LLEXT" | sed -n 's/^ *\[ *[0-9]*\] *//p')

if [ "$exported_size" -eq 0 ]; then
    echo "$LLEXT: missing or empty .exported_sym section (no EXPORT_SYMBOL entries survived the link)" >&2
    fail=1
elif [ $((exported_size % 8)) -ne 0 ]; then
    echo "$LLEXT: .exported_sym size $exported_size is not a multiple of 8 (struct llext_const_symbol layout mismatch)" >&2
    fail=1
fi

prev_end=0
prev_name=""
while read -r off size name; do
    [ -n "$off" ] || continue
    if [ "$off" -lt "$prev_end" ]; then
        echo "$LLEXT: section $name (file offset $off) overlaps $prev_name (ends at $prev_end) - the on-device loader will reject this file" >&2
        fail=1
    fi
    if [ $((off + size)) -gt "$prev_end" ]; then
        prev_end=$((off + size))
        prev_name=$name
    fi
done < <(printf '%s' "$ranges" | sort -n)

if [ "$alloc_total" -gt "$HEAP_LIMIT" ]; then
    echo "$LLEXT: allocatable sections total $alloc_total bytes > llext heap ($HEAP_LIMIT bytes, CONFIG_LLEXT_HEAP_SIZE)" >&2
    fail=1
elif [ "$alloc_total" -ge $((HEAP_LIMIT * 8 / 10)) ]; then
    echo "$LLEXT: warning: allocatable sections total $alloc_total bytes (>= 80% of the $HEAP_LIMIT-byte llext heap)" >&2
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "$LLEXT: OK (alloc $alloc_total bytes, $((exported_size / 8)) exported symbols)"
