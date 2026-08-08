#!/usr/bin/env bash
# Post-link gate for ARM .llext extension builds — the standalone analog of
# the device's llext symbol resolution and region checks. Catches at build
# time the whole class of "compiles fine, silently fails to load on device"
# failures.
#
#   check-llext.sh --nm <nm> --readelf <readelf> --allowed <file> <name.llext>
#
# Checks (mirroring zephyr/subsys/llext/llext_load.c llext_map_sections):
#  1. Undefined symbols: every one must be in the allowed list (the supported
#     subset of what the firmware exports). Anything else — sinf (no libm
#     on-target), __aeabi_* (no libgcc helpers exported: 64-bit division,
#     soft-float) — would fail llext symbol resolution at load time.
#  2. Region layout, the loader's model: sections are classified into regions
#     (text = executable, data = writable PROGBITS, rodata = other alloc
#     PROGBITS, export = .exported_sym, init/fini/preinit arrays), each
#     region's file span is the min..max over its sections, and region spans
#     must not overlap ("Region 0 ELF file range ... overlaps with 1" is the
#     on-device rejection this predicts, e.g. from COMDAT text interleaving
#     with data in a C++ object that skipped `ld -r`). Additionally at most
#     ONE allocatable NOBITS section is allowed — the loader hard-rejects
#     multiple ("Multiple SHT_NOBITS sections are not supported", -ENOTSUP),
#     which a C++ function-local static in an inline function can trigger
#     via a COMDAT .bss._Z* section.
#     Plus: .exported_sym present, nonzero, a multiple of 8.
#  3. Size: total SHF_ALLOC bytes (the loader copies/allocates all alloc
#     sections, including .bss, into the llext heap) against
#     CONFIG_LLEXT_HEAP_SIZE. The limit is read from heap-limit.txt next to
#     this script (stamped by package-sdk.sh from the board .conf), falling
#     back to grepping the board .conf directly when running from the
#     monorepo source tree — never a hardcoded copy.
#
# Parsing note: readelf -S -W output is processed in plain bash ($((16#...)))
# rather than awk — the "[ Nr]" column splits inconsistently for awk, and
# strtonum() is gawk-only (ubuntu runners default to mawk).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

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

# --- resolve the llext heap limit (single source: the board .conf) ----------
if [ -f "$SCRIPT_DIR/heap-limit.txt" ]; then
    HEAP_LIMIT="$(grep -o '^[0-9]*' "$SCRIPT_DIR/heap-limit.txt" | head -1)"
else
    board_conf="$SCRIPT_DIR/../../boards/rgb_sunglasses_proto0_nrf5340_cpuapp.conf"
    if [ -f "$board_conf" ]; then
        kb="$(grep -o '^CONFIG_LLEXT_HEAP_SIZE=[0-9]*' "$board_conf" | grep -o '[0-9]*$')"
        HEAP_LIMIT=$((kb * 1024))
    else
        echo "error: no heap-limit.txt beside check-llext.sh and no board .conf to derive it from" >&2
        exit 2
    fi
fi
if [ -z "$HEAP_LIMIT" ] || [ "$HEAP_LIMIT" -le 0 ]; then
    echo "error: could not determine the llext heap limit" >&2
    exit 2
fi

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
    echo "  Extensions may only call the symbols in $(basename "$ALLOWED")." >&2
    echo "  Common causes: libm calls (sinf/cosf - no math library on-target)," >&2
    echo "  64-bit division or soft-float arithmetic (__aeabi_* libgcc helpers)." >&2
    echo "  Use integer math or single-precision float expressions the FPU handles inline." >&2
    fail=1
fi

# --- 2. region layout (the loader's model) -----------------------------------
# Each relevant readelf -S -W line, after stripping the "[ Nr]" prefix, is:
#   Name Type Addr Off Size ES Flg Lk Inf Al
exported_size=0
alloc_total=0
nobits_count=0
nobits_names=""
# Per-region merged file spans: "<min_off> <max_end>" (empty = no sections).
declare -A region_min region_max

while read -r name type _addr off size _es flg _rest; do
    case "$flg" in *A*) ;; *) continue ;; esac
    size_dec=$((16#$size))
    [ "$size_dec" -gt 0 ] || continue

    # Classify into the loader's llext_mem region (llext_map_sections).
    region=""
    if [ "$name" = ".exported_sym" ]; then
        region="export"
        exported_size=$size_dec
    else
        case "$type" in
            NOBITS)
                nobits_count=$((nobits_count + 1))
                nobits_names="$nobits_names $name"
                ;;
            PROGBITS)
                case "$flg" in
                    *X*) region="text" ;;
                    *W*) region="data" ;;
                    *) region="rodata" ;;
                esac
                ;;
            INIT_ARRAY) region="init" ;;
            FINI_ARRAY) region="fini" ;;
            PREINIT_ARRAY) region="preinit" ;;
            *) ;;  # unknown types are skipped by the loader too
        esac
    fi
    alloc_total=$((alloc_total + size_dec))

    if [ -n "$region" ]; then
        off_dec=$((16#$off))
        end=$((off_dec + size_dec))
        if [ -z "${region_min[$region]:-}" ] || [ "$off_dec" -lt "${region_min[$region]}" ]; then
            region_min[$region]=$off_dec
        fi
        if [ -z "${region_max[$region]:-}" ] || [ "$end" -gt "${region_max[$region]}" ]; then
            region_max[$region]=$end
        fi
    fi
done < <("$READELF" -S -W "$LLEXT" | sed -n 's/^ *\[ *[0-9]*\] *//p')

if [ "$exported_size" -eq 0 ]; then
    echo "$LLEXT: missing or empty .exported_sym section (no EXPORT_SYMBOL entries survived the link)" >&2
    fail=1
elif [ $((exported_size % 8)) -ne 0 ]; then
    echo "$LLEXT: .exported_sym size $exported_size is not a multiple of 8 (struct llext_const_symbol layout mismatch)" >&2
    fail=1
fi

if [ "$nobits_count" -gt 1 ]; then
    echo "$LLEXT: $nobits_count allocatable NOBITS sections ($nobits_names ) - the on-device loader rejects multiple bss sections (-ENOTSUP)" >&2
    echo "  Common cause in C++: a function-local static in an inline or template function emits a COMDAT .bss._Z* section." >&2
    fail=1
fi

# Pairwise overlap between merged region file spans (any shared byte counts,
# matching the loader's REGIONS_OVERLAP_ON).
regions=("${!region_min[@]}")
n=${#regions[@]}
for ((i = 0; i < n; i++)); do
    for ((j = i + 1; j < n; j++)); do
        a=${regions[$i]} b=${regions[$j]}
        if [ "${region_min[$a]}" -lt "${region_max[$b]}" ] && [ "${region_min[$b]}" -lt "${region_max[$a]}" ]; then
            echo "$LLEXT: region '$a' file span [${region_min[$a]},${region_max[$a]}) overlaps region '$b' [${region_min[$b]},${region_max[$b]}) - the on-device loader will reject this file" >&2
            echo "  (This is the merged-region overlap the mandatory 'ld -r' partial link exists to prevent.)" >&2
            fail=1
        fi
    done
done

# --- 3. llext heap fit -------------------------------------------------------
if [ "$alloc_total" -gt "$HEAP_LIMIT" ]; then
    echo "$LLEXT: allocatable sections total $alloc_total bytes > llext heap ($HEAP_LIMIT bytes, CONFIG_LLEXT_HEAP_SIZE)" >&2
    fail=1
elif [ "$alloc_total" -ge $((HEAP_LIMIT * 8 / 10)) ]; then
    echo "$LLEXT: warning: allocatable sections total $alloc_total bytes (>= 80% of the $HEAP_LIMIT-byte llext heap)" >&2
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi
echo "$LLEXT: OK (alloc $alloc_total bytes, $((exported_size / 8)) exported symbols, heap limit $HEAP_LIMIT)"
