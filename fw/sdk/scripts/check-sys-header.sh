#!/usr/bin/env bash
# Assert that <rgbx/rgbx_sys.h> declares every symbol allowed-symbols.txt
# sanctions, with the RIGHT prototype — the third member of the lockstep set
# that already ties allowed-symbols.txt to extension_exports.c.
#
#   check-sys-header.sh [--cc <compiler>] [--include-dir <dir>] [--allowed <file>]
#
# Why a compile and not a grep: the header deliberately delegates the standard
# functions to <string.h>/<math.h> rather than re-declaring them (see the
# rationale in rgbx_sys.h). A text check could only see what the header spells
# out; this checks what an extension author actually gets after including it.
#
# Mechanism: for each sanctioned symbol, emit a function pointer initialized
# from the symbol at its expected type, and compile with -Werror.
#
#   typedef void chk_t_printk(const char *, ...);
#   chk_t_printk *const chk_printk = printk;
#
# An undeclared symbol is an error; a symbol declared with any other signature
# is an incompatible-pointer error. Both name the symbol, which is the whole
# point — this is the same class of mistake the header exists to prevent, so
# it must not be able to hide in the header either.
#
# The "no more, no less" half: the PROTOTYPES table below must cover the allow
# list exactly, checked in both directions. A symbol added to
# allowed-symbols.txt with no table row fails here, and so does a table row
# that is no longer sanctioned. A symbol declared but not exported is the same
# trap as one exported but undeclared.
#
# Compiler: any C compiler will do — the header's content is target-agnostic
# and every toolchain in play (picolibc, wasi-libc, host glibc) declares the
# standard subset identically. Defaults to `cc`; sdk-ci.yml additionally runs
# it under the two toolchains that actually build extensions.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

CC_BIN="${CC:-cc}"
INCLUDE_DIR="$SCRIPT_DIR/../../include"
ALLOWED="$SCRIPT_DIR/../arm/allowed-symbols.txt"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --cc) CC_BIN="$2"; shift 2 ;;
        --include-dir) INCLUDE_DIR="$2"; shift 2 ;;
        --allowed) ALLOWED="$2"; shift 2 ;;
        *)
            echo "usage: check-sys-header.sh [--cc <compiler>] [--include-dir <dir>] [--allowed <file>]" >&2
            exit 2
            ;;
    esac
done

if [ ! -f "$ALLOWED" ]; then
    echo "error: no allowed-symbols list at $ALLOWED" >&2
    exit 2
fi
if [ ! -f "$INCLUDE_DIR/rgbx/rgbx_sys.h" ]; then
    echo "error: no rgbx/rgbx_sys.h under $INCLUDE_DIR" >&2
    exit 2
fi

# The expected prototype of every sanctioned symbol: "<name>|<ret>|<params>".
# Sources: C99 7.21 (string) and 7.12 (math) for the standard subset,
# zephyr/include/zephyr/sys/printk.h for printk/vprintk.
#
# The __aeabi_* entries on the allow list are the one deliberate omission:
# they are ARM run-time ABI routines the compiler emits on the extension's
# behalf, with multi-register return conventions C cannot express and which
# nothing calls by name. rgbx_sys.h explains the same exclusion — keep the two
# in step. The `excluded()` predicate below is what keeps them off this table
# without tripping the coverage check.
PROTOTYPES="$(cat <<'EOF'
strcpy|char *|(char *, const char *)
strncpy|char *|(char *, const char *, size_t)
strlen|size_t|(const char *)
strcmp|int|(const char *, const char *)
strncmp|int|(const char *, const char *, size_t)
memcmp|int|(const void *, const void *, size_t)
memcpy|void *|(void *, const void *, size_t)
memset|void *|(void *, int, size_t)
memmove|void *|(void *, const void *, size_t)
printk|void|(const char *, ...)
vprintk|void|(const char *, va_list)
sinf|float|(float)
cosf|float|(float)
tanf|float|(float)
atan2f|float|(float, float)
sqrtf|float|(float)
expf|float|(float)
logf|float|(float)
powf|float|(float, float)
fmodf|float|(float, float)
floorf|float|(float)
ceilf|float|(float)
roundf|float|(float)
EOF
)"

excluded() {
    case "$1" in __aeabi_*) return 0 ;; *) return 1 ;; esac
}

# Sanctioned symbols, comments and blanks stripped.
allowed_syms="$(grep -v -e '^#' -e '^$' "$ALLOWED")"

# --- coverage, both directions ----------------------------------------------
missing=""
for sym in $allowed_syms; do
    excluded "$sym" && continue
    if ! grep -q "^$sym|" <<<"$PROTOTYPES"; then
        missing="$missing $sym"
    fi
done
if [ -n "$missing" ]; then
    echo "allowed-symbols.txt sanctions symbol(s) with no expected prototype in $(basename "$0"):$missing" >&2
    echo "  Add a row to the PROTOTYPES table AND make sure <rgbx/rgbx_sys.h> declares it," >&2
    echo "  or extension authors are back to hand-writing prototypes (issue #351)." >&2
    exit 1
fi

stale=""
while IFS='|' read -r sym _ret _params; do
    [ -n "$sym" ] || continue
    if ! grep -qx "$sym" <<<"$allowed_syms"; then
        stale="$stale $sym"
    fi
done <<<"$PROTOTYPES"
if [ -n "$stale" ]; then
    echo "$(basename "$0") expects prototype(s) for symbol(s) no longer in $(basename "$ALLOWED"):$stale" >&2
    echo "  Drop the stale row(s) from the PROTOTYPES table." >&2
    exit 1
fi

# --- the compile ------------------------------------------------------------
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
src="$tmp/check.c"

{
    echo '/* Generated by check-sys-header.sh from allowed-symbols.txt. */'
    echo '#include <rgbx/rgbx_sys.h>'
} > "$src"

checked=0
while IFS='|' read -r sym ret params; do
    [ -n "$sym" ] || continue
    # External linkage deliberately (no `static`): a file-scope const with
    # internal linkage that nothing reads trips -Wunused-const-variable.
    printf 'typedef %s chk_t_%s%s;\nchk_t_%s *const chk_%s = %s;\n' \
        "$ret" "$sym" "$params" "$sym" "$sym" "$sym" >> "$src"
    checked=$((checked + 1))
done <<<"$PROTOTYPES"

if ! "$CC_BIN" -c "$src" -o "$tmp/check.o" -I "$INCLUDE_DIR" -Wall -Wextra -Werror 2>"$tmp/err"; then
    echo "<rgbx/rgbx_sys.h> does not declare the sanctioned symbols with the expected prototypes:" >&2
    cat "$tmp/err" >&2
    echo "  Each error names the symbol whose declaration disagrees. A wrong prototype here is" >&2
    echo "  exactly the trap the header exists to remove — fix the header, not this check." >&2
    exit 1
fi

n_excluded=0
for sym in $allowed_syms; do
    excluded "$sym" && n_excluded=$((n_excluded + 1))
done

echo "OK: <rgbx/rgbx_sys.h> declares all $checked checkable allowed symbols correctly ($n_excluded __aeabi_* helpers excluded by design)"
