#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Stuart Alldritt
#
# Regression suite for the cache handling in the two pinned-toolchain
# resolvers, fw/sdk/scripts/install-arm-toolchain.sh and
# fw/sim/scripts/install-toolchain.sh.
#
#   fw/sdk/tests/test-toolchain-resolver.sh
#
# Both resolvers re-extract their toolchain from a pin-verified archive on
# every call, so more than one resolve can be installing a tree at the same
# cache path at the same time: the arm and the wasm configure of one build, two
# builds, or a rebuild that starts while an earlier tree cannot be removed.
# The cases below cover what that has to survive:
#
#   concurrent      Several resolves at once. Every one must succeed and leave
#                   exactly one usable tree, never a half-removed one and never
#                   a tree nested inside the tree it was replacing.
#   straggler       A swap over a tree rm -rf cannot remove, plus an aside left
#                   by a run killed mid-swap. Note that an open file descriptor
#                   is NOT what obstructs rm on POSIX: unlinking a file some
#                   other process still has open works fine. The real
#                   obstructions are a directory the user cannot write (what
#                   the permissions case below simulates), an immutable flag,
#                   a network filesystem that renamed a still-open file into
#                   place under the tree, and another process creating entries
#                   under the tree while rm walks it.
#   hijacked swap   A foreign tree appears at the destination at the instant
#                   the resolver renames its own tree in, so the rename nests.
#                   The resolve must fail closed, hand out no path, and reap
#                   the aside it no longer owns. This is the accidental-race
#                   shape the post-swap check is for; a racer that reads the
#                   staged nonce is outside the resolvers' threat model, which
#                   treats the cache as a same-user working area.
#   lock deleted    The lock removed in a tight loop while resolves run.
#                   Resolves may fail; none may succeed against a tree it did
#                   not install.
#   lock hygiene    A non-directory at the lock path, and owner files holding
#                   values that must never be handed to kill or treated as
#                   proof the owner is gone.
#
# The suite never downloads a distribution archive and never touches the
# developer's real cache. For each resolver it copies the script into a scratch
# tree, repoints every pinned digest at a small synthetic archive with the same
# internal layout, and runs it against a scratch XDG_CACHE_HOME with a curl
# stand-in that serves that fixture. The resolver's own digest verification
# still runs on every resolve; only the value it is pinned to belongs to the
# fixture. Nothing in the shipped scripts is test-aware.
#
# Tunables (defaults are sized so the races reproduce reliably on a laptop and
# in CI):
#   RGBX_TEST_WORKERS         concurrent resolves per round (default 8)
#   RGBX_TEST_ROUNDS          concurrent rounds (default 3)
#   RGBX_TEST_FIXTURE_FILES   filler files in the fixture tree (default 1500)

set -euo pipefail

WORKERS="${RGBX_TEST_WORKERS:-8}"
ROUNDS="${RGBX_TEST_ROUNDS:-3}"
FIXTURE_FILES="${RGBX_TEST_FIXTURE_FILES:-1500}"

# Nothing here should ever wait on a lock for long. A wedge has to fail the
# suite in minutes rather than sit on the resolver's real budget for half an
# hour; the lock cases below shorten it further still.
export RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS=120

TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$TESTS_DIR/../../.." && pwd)"
ARM_RESOLVER="$REPO_ROOT/fw/sdk/scripts/install-arm-toolchain.sh"
WASI_RESOLVER="$REPO_ROOT/fw/sim/scripts/install-toolchain.sh"

scratch="$(mktemp -d "${TMPDIR:-/tmp}/rgbx-resolver-test.XXXXXX")"
cleanup() {
    # The straggler cases deliberately leave obstructed directories behind.
    if command -v chflags >/dev/null 2>&1; then
        chflags -R nouchg "$scratch" 2>/dev/null || true
    fi
    if command -v chattr >/dev/null 2>&1; then
        chattr -R -i "$scratch" 2>/dev/null || true
    fi
    chmod -R u+rwX "$scratch" 2>/dev/null || true
    rm -rf "$scratch"
}
trap cleanup EXIT

failures=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1" >&2; failures=$((failures + 1)); }
skip() { echo "  skip $1"; }

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

# Real pathname expansion rather than a pattern string, so a scratch path
# containing [ or ] is matched literally. Prints each existing match.
glob_matches() {
    local pattern_dir="$1" pattern_tail="$2" candidate
    for candidate in "$pattern_dir"/$pattern_tail; do
        [ -e "$candidate" ] && printf '%s\n' "$candidate"
    done
    return 0
}

# Read a pinned scalar out of a resolver, so this suite never carries a second
# copy of a version the resolver owns.
pin_value() {
    local script="$1" name="$2" line
    line="$(grep -m1 "^$name=" "$script" || true)"
    if [ -z "$line" ]; then
        echo "error: $script has no $name pin" >&2
        exit 1
    fi
    echo "$line" | sed -E "s/^$name=\"([^\"]*)\".*/\1/"
}

# Copy a resolver and repoint every one of its pinned per-host digests at the
# fixture, so whichever host runs this suite verifies the fixture's bytes.
repin_resolver() {
    local src="$1" dst="$2" prefix="$3" digest="$4" replaced
    sed -E "s/^(${prefix}[A-Z0-9_]+=)\"[0-9a-f]{64}\"/\1\"$digest\"/" "$src" > "$dst"
    chmod +x "$dst"
    replaced="$(grep -c "\"$digest\"" "$dst" || true)"
    if [ "$replaced" -lt 2 ]; then
        echo "error: repinning $src replaced $replaced digests; the pin format changed" >&2
        exit 1
    fi
}

# A curl stand-in that serves the fixture archive instead of reaching the
# network, so the suite is hermetic and a resolve that unexpectedly decides to
# download still gets bytes that match the pin it was given.
write_curl_stub() {
    local dir="$1"
    mkdir -p "$dir"
    cat > "$dir/curl" <<'STUB'
#!/bin/sh
out=""
prev=""
for arg in "$@"; do
    if [ "$prev" = "-o" ]; then
        out="$arg"
    fi
    prev="$arg"
done
if [ -z "$out" ]; then
    echo "curl stub: no -o argument in: $*" >&2
    exit 2
fi
cp "$RGBX_TEST_FIXTURE_ARCHIVE" "$out"
echo "fetch" >> "$RGBX_TEST_CURL_LOG"
STUB
    chmod +x "$dir/curl"
}

# An mv stand-in that makes a foreign tree appear at the destination just
# before the rename that installs the toolchain, which is the race the
# resolver's post-swap check exists to catch. Only that one rename is
# hijacked: the destination is always the last argument.
write_mv_shim() {
    local dir="$1"
    mkdir -p "$dir"
    cat > "$dir/mv" <<'SHIM'
#!/bin/sh
dest=""
for arg in "$@"; do
    dest="$arg"
done
if [ "$dest" = "$RGBX_TEST_TREE" ]; then
    mkdir -p "$RGBX_TEST_TREE/$(dirname "$RGBX_TEST_COMPILER")"
    printf '#!/bin/sh\necho PWNED\n' > "$RGBX_TEST_TREE/$RGBX_TEST_COMPILER"
    chmod +x "$RGBX_TEST_TREE/$RGBX_TEST_COMPILER"
fi
exec /bin/mv "$@"
SHIM
    chmod +x "$dir/mv"
}

# Build a toolchain-shaped directory: the one binary the resolver looks for,
# plus enough filler that removing the tree takes long enough for a competing
# resolve to land inside the window.
build_fixture_tree() {
    local root="$1" compiler="$2" i
    mkdir -p "$root/$(dirname "$compiler")" "$root/lib"
    printf '#!/bin/sh\necho "fixture toolchain"\n' > "$root/$compiler"
    chmod +x "$root/$compiler"
    for i in $(seq 1 "$FIXTURE_FILES"); do
        : > "$root/lib/filler-$i"
    done
}

# Assert the cache holds exactly one usable toolchain tree.
assert_tree_sane() {
    local what="$1" tree="$2" compiler="$3" nested_glob="$4" nested
    if [ ! -x "$tree/$compiler" ]; then
        fail "$what: $tree/$compiler is missing or not executable"
        return 1
    fi
    nested="$(glob_matches "$tree" "$nested_glob")"
    if [ -n "$nested" ]; then
        fail "$what: the resolved tree contains a nested toolchain tree: $nested"
        return 1
    fi
    return 0
}

# A pid that is certainly not running: start a process, let it exit, reuse its
# number. Reuse within the life of one test run is not a practical concern.
dead_pid() {
    local pid
    ( exit 0 ) &
    pid=$!
    wait "$pid" 2>/dev/null || true
    printf '%s' "$pid"
}

# Whether this host can make a file the current user cannot remove. Used for
# the leftover-aside case; a permissions obstruction does not qualify because
# the resolver is expected to lift that one itself.
IMMUTABLE_SET=""
IMMUTABLE_CLEAR=""
detect_immutable_support() {
    local probe="$scratch/immutable-probe" set_cmd clear_cmd
    for set_cmd in "chflags uchg" "chattr +i"; do
        case "$set_cmd" in
            "chflags uchg") clear_cmd="chflags nouchg" ;;
            *) clear_cmd="chattr -i" ;;
        esac
        command -v "${set_cmd%% *}" >/dev/null 2>&1 || continue
        rm -rf "$probe" 2>/dev/null || true
        mkdir -p "$probe/d"
        : > "$probe/d/f"
        if ! $set_cmd "$probe/d/f" 2>/dev/null; then
            rm -rf "$probe" 2>/dev/null || true
            continue
        fi
        if rm -rf "$probe" 2>/dev/null; then
            continue
        fi
        $clear_cmd "$probe/d/f" 2>/dev/null || true
        rm -rf "$probe" 2>/dev/null || true
        IMMUTABLE_SET="$set_cmd"
        IMMUTABLE_CLEAR="$clear_cmd"
        return 0
    done
    return 1
}

# --- the cases, shared by both resolvers -----------------------------------

case_concurrent() {
    local label="$1" resolver="$2" tree="$3" compiler="$4" nested_glob="$5"
    local round worker pids status out expected leftover ok=1

    # Warm the cache so every timed round starts from the state that provokes
    # the race: a tree already in place that each resolve has to replace.
    if ! expected="$("$resolver")"; then
        fail "$label concurrent: the warm-up resolve failed"
        return
    fi
    if [ "$expected" != "$tree" ]; then
        fail "$label concurrent: resolver printed '$expected', expected '$tree'"
        return
    fi

    for round in $(seq 1 "$ROUNDS"); do
        pids=()
        for worker in $(seq 1 "$WORKERS"); do
            "$resolver" > "$scratch/$label.out.$worker" 2> "$scratch/$label.err.$worker" &
            pids+=("$!")
        done
        for worker in $(seq 1 "$WORKERS"); do
            status=0
            wait "${pids[$((worker - 1))]}" || status=$?
            if [ "$status" -ne 0 ]; then
                fail "$label concurrent: round $round worker $worker exited $status"
                sed 's/^/        /' "$scratch/$label.err.$worker" >&2
                ok=0
            fi
            out="$(cat "$scratch/$label.out.$worker")"
            if [ "$out" != "$tree" ]; then
                fail "$label concurrent: round $round worker $worker printed '$out'"
                ok=0
            fi
        done
        assert_tree_sane "$label concurrent round $round" \
            "$tree" "$compiler" "$nested_glob" || ok=0
        if [ "$ok" -eq 0 ]; then
            return
        fi
    done

    leftover="$(glob_matches "$(dirname "$tree")" "$(basename "$tree").old.*")"
    if [ -n "$leftover" ]; then
        fail "$label concurrent: aside directories were not cleaned up: $leftover"
        return
    fi
    pass "$label concurrent: $((ROUNDS * WORKERS)) overlapping resolves left one usable tree"
}

case_straggler_permissions() {
    local label="$1" resolver="$2" tree="$3" compiler="$4" nested_glob="$5"
    local stale_aside="$tree.old.stale-from-a-killed-run" leftover

    if ! "$resolver" >/dev/null; then
        fail "$label straggler: the warm-up resolve failed"
        return
    fi

    # A directory rm -rf cannot empty because the current user cannot write it.
    # The resolver is expected to lift this one and reap the aside.
    if [ "$(id -u)" -ne 0 ]; then
        mkdir -p "$tree/unwritable"
        : > "$tree/unwritable/entry"
        chmod 500 "$tree/unwritable"
    fi
    # An aside a previous run was killed before removing. Backdated, because
    # another resolve's aside is only swept once it is old enough that it
    # cannot belong to a swap still in progress.
    mkdir -p "$stale_aside/leftover"
    : > "$stale_aside/leftover/file"
    touch -t 200001010000 "$stale_aside"

    if ! "$resolver" > "$scratch/$label.strag.out" 2> "$scratch/$label.strag.err"; then
        fail "$label straggler: the resolve failed"
        sed 's/^/        /' "$scratch/$label.strag.err" >&2
        return
    fi
    assert_tree_sane "$label straggler" "$tree" "$compiler" "$nested_glob" || return
    if [ -e "$stale_aside" ]; then
        fail "$label straggler: the stale aside directory was not swept: $stale_aside"
        return
    fi
    leftover="$(glob_matches "$(dirname "$tree")" "$(basename "$tree").old.*")"
    if [ -n "$leftover" ]; then
        fail "$label straggler: a permissions obstruction should have been lifted, not leaked: $leftover"
        return
    fi
    pass "$label straggler: resolve survived an unwritable tree and swept both asides"
}

case_unremovable_aside() {
    local label="$1" resolver="$2" tree="$3" compiler="$4" nested_glob="$5"
    local i leftover count aside

    if [ -z "$IMMUTABLE_SET" ]; then
        skip "$label unremovable aside: this host cannot make a file the owner may not remove"
        return
    fi
    if ! "$resolver" >/dev/null; then
        fail "$label unremovable aside: the warm-up resolve failed"
        return
    fi

    mkdir -p "$tree/blocked"
    : > "$tree/blocked/entry"
    $IMMUTABLE_SET "$tree/blocked/entry"

    # The resolve that has to move that tree aside cannot then remove it. It
    # must still succeed, and it must say what it left behind.
    if ! "$resolver" > "$scratch/$label.u1.out" 2> "$scratch/$label.u1.err"; then
        fail "$label unremovable aside: the resolve over an unremovable tree failed"
        sed 's/^/        /' "$scratch/$label.u1.err" >&2
        return
    fi
    if ! grep -q "could not remove the replaced" "$scratch/$label.u1.err"; then
        fail "$label unremovable aside: no warning was printed for the leftover"
        return
    fi
    leftover="$(glob_matches "$(dirname "$tree")" "$(basename "$tree").old.*")"
    if [ -z "$leftover" ]; then
        fail "$label unremovable aside: expected one leftover aside, found none"
        return
    fi
    aside="$(printf '%s\n' "$leftover" | head -1)"

    # Four more resolves. The leftover belongs to an earlier run and is young,
    # so it is left alone rather than fought over, and no new leftover appears.
    for i in 2 3 4 5; do
        if ! "$resolver" >/dev/null 2> "$scratch/$label.u$i.err"; then
            fail "$label unremovable aside: resolve $i failed"
            sed 's/^/        /' "$scratch/$label.u$i.err" >&2
            return
        fi
    done
    count="$(glob_matches "$(dirname "$tree")" "$(basename "$tree").old.*" | wc -l | tr -d ' ')"
    if [ "$count" != "1" ]; then
        fail "$label unremovable aside: five resolves left $count asides, expected 1"
        return
    fi

    # Once the leftover is older than the staleness threshold the sweep retries
    # it, and says again that it could not remove it.
    touch -t 200001010000 "$aside"
    if ! "$resolver" >/dev/null 2> "$scratch/$label.u6.err"; then
        fail "$label unremovable aside: the resolve that should retry the sweep failed"
        return
    fi
    if ! grep -q "could not remove the replaced" "$scratch/$label.u6.err"; then
        fail "$label unremovable aside: the aged leftover was not retried and reported"
        return
    fi

    # Lift the obstruction and the next resolve reaps it.
    $IMMUTABLE_CLEAR "$aside/blocked/entry" 2>/dev/null || true
    touch -t 200001010000 "$aside"
    if ! "$resolver" >/dev/null 2> "$scratch/$label.u7.err"; then
        fail "$label unremovable aside: the reaping resolve failed"
        return
    fi
    leftover="$(glob_matches "$(dirname "$tree")" "$(basename "$tree").old.*")"
    if [ -n "$leftover" ]; then
        fail "$label unremovable aside: the unblocked aside was not reaped: $leftover"
        return
    fi
    pass "$label unremovable aside: reported every leftover and reaped it once unblocked"
}

case_hijacked_swap() {
    local label="$1" resolver="$2" tree="$3" compiler="$4"
    local shim="$scratch/$label-mvshim" out err status=0 leftover

    write_mv_shim "$shim"
    out="$scratch/$label.hijack.out"
    err="$scratch/$label.hijack.err"
    # Start from a tree in place, so the resolve really does move one aside and
    # the loser's cleanup of its own aside is exercised.
    if ! "$resolver" >/dev/null; then
        fail "$label hijacked swap: the warm-up resolve failed"
        return
    fi
    PATH="$shim:$PATH" RGBX_TEST_TREE="$tree" RGBX_TEST_COMPILER="$compiler" \
        "$resolver" > "$out" 2> "$err" || status=$?
    if [ "$status" -eq 0 ]; then
        fail "$label hijacked swap: the resolve succeeded although a foreign tree was installed under it"
        return
    fi
    if [ -s "$out" ]; then
        fail "$label hijacked swap: a path was printed anyway: $(cat "$out")"
        return
    fi
    if ! grep -q "replaced $tree while this resolve was installing it" "$err"; then
        fail "$label hijacked swap: the failure did not name the lost race"
        sed 's/^/        /' "$err" >&2
        return
    fi
    # The planted compiler is still sitting in the cache; what matters is that
    # no caller was pointed at it.
    if ! grep -q PWNED "$tree/$compiler" 2>/dev/null; then
        fail "$label hijacked swap: the shim did not actually plant a foreign tree"
        return
    fi
    # The tree this resolve moved aside belongs to nobody once it has lost, so
    # it must not be left for the age-based sweep to find later.
    leftover="$(glob_matches "$(dirname "$tree")" "$(basename "$tree").old.*")"
    if [ -n "$leftover" ]; then
        fail "$label hijacked swap: the loser left its aside behind: $leftover"
        return
    fi
    rm -rf "$tree"
    pass "$label hijacked swap: failed closed, printed no path, left no aside"
}

case_lock_wait_clamp() {
    local label="$1" resolver="$2" block probe input expected effective
    local probe_script="$scratch/$label.wait-probe.sh"
    # The override may only shorten the wait. Evaluate the shipped constant
    # block itself rather than sitting through a timeout to observe it.
    block="$(sed -n '/^LOCK_STALE_MINUTES=/,/^fi$/p' "$resolver")"
    if [ -z "$block" ]; then
        fail "$label lock wait: could not find the wait budget block in the resolver"
        return
    fi
    printf '%s\n' "$block" > "$probe_script"
    cat >> "$probe_script" <<'PROBE'
echo "$LOCK_WAIT_SECONDS"
PROBE
    for probe in "999999:2100" "60:60" "2100:2100" "0:2100" "abc:2100" ":2100"; do
        input="${probe%%:*}"
        expected="${probe##*:}"
        effective="$(env RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS="$input" bash "$probe_script")"
        if [ "$effective" != "$expected" ]; then
            fail "$label lock wait: override '$input' produced ${effective}s, expected ${expected}s"
            return
        fi
    done
    pass "$label lock wait: the override can only shorten the budget, never lengthen it"
}

case_lock_deleted() {
    local label="$1" resolver="$2" tree="$3" compiler="$4" nested_glob="$5"
    local lock="$6"
    local stop="$scratch/$label.stop-killer" killer worker pids status out
    local ok=1 wrong=0 succeeded=0

    if ! "$resolver" >/dev/null; then
        fail "$label lock deleted: the warm-up resolve failed"
        return
    fi
    rm -f "$stop"
    ( while [ ! -e "$stop" ]; do rm -rf "$lock" 2>/dev/null || true; done ) &
    killer=$!

    pids=()
    for worker in $(seq 1 "$WORKERS"); do
        "$resolver" > "$scratch/$label.lk.out.$worker" 2> "$scratch/$label.lk.err.$worker" &
        pids+=("$!")
    done
    for worker in $(seq 1 "$WORKERS"); do
        status=0
        wait "${pids[$((worker - 1))]}" || status=$?
        out="$(cat "$scratch/$label.lk.out.$worker")"
        if [ "$status" -eq 0 ]; then
            succeeded=$((succeeded + 1))
            if [ "$out" != "$tree" ]; then
                wrong=$((wrong + 1))
            fi
        elif [ -n "$out" ]; then
            wrong=$((wrong + 1))
        fi
    done
    : > "$stop"
    wait "$killer" 2>/dev/null || true

    if [ "$wrong" -ne 0 ]; then
        fail "$label lock deleted: $wrong resolve(s) produced a path they should not have"
        ok=0
    fi
    assert_tree_sane "$label lock deleted" "$tree" "$compiler" "$nested_glob" || ok=0
    [ "$ok" -eq 1 ] || return
    pass "$label lock deleted: $succeeded/$WORKERS resolves succeeded, none against a foreign tree"
}

case_lock_path_hygiene() {
    local label="$1" resolver="$2" lock="$3"
    local err="$scratch/$label.lockpath.err" status started elapsed

    rm -rf "$lock"
    : > "$lock"
    status=0
    started="$SECONDS"
    RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS=30 "$resolver" >/dev/null 2> "$err" || status=$?
    elapsed=$((SECONDS - started))
    rm -f "$lock"
    if [ "$status" -eq 0 ]; then
        fail "$label lock path: a regular file at the lock path did not stop the resolve"
        return
    fi
    if ! grep -q "exists and is not a directory" "$err"; then
        fail "$label lock path: no diagnostic naming the lock path"
        sed 's/^/        /' "$err" >&2
        return
    fi
    if [ "$elapsed" -ge 20 ]; then
        fail "$label lock path: waited ${elapsed}s instead of failing immediately"
        return
    fi

    ln -s /dev/null "$lock"
    status=0
    RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS=30 "$resolver" >/dev/null 2> "$err" || status=$?
    rm -f "$lock"
    if [ "$status" -eq 0 ] || ! grep -q "exists and is not a directory" "$err"; then
        fail "$label lock path: a symlink at the lock path was not rejected"
        return
    fi
    pass "$label lock path: a non-directory at the lock path fails closed at once"
}

case_lock_owner_content() {
    local label="$1" resolver="$2" lock="$3"
    local err="$scratch/$label.owner.err" status content

    # Values that must never be handed to kill and must never be read as proof
    # the owner is gone. A young lock carrying any of them stays locked.
    for content in "-1" "0" "not-a-pid" "" " 123" "$$"; do
        rm -rf "$lock"
        mkdir -p "$lock"
        printf '%s' "$content" > "$lock/owner"
        status=0
        RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS=2 "$resolver" >/dev/null 2> "$err" || status=$?
        if [ "$status" -eq 0 ]; then
            fail "$label lock owner: owner '$content' let a young lock be broken"
            rm -rf "$lock"
            return
        fi
        if ! grep -q "timed out after" "$err"; then
            fail "$label lock owner: owner '$content' did not produce the timeout diagnostic"
            sed 's/^/        /' "$err" >&2
            rm -rf "$lock"
            return
        fi
    done

    # The same unusable owner content on a lock older than the threshold is
    # broken by the age rule, so a wedged cache still recovers.
    rm -rf "$lock"
    mkdir -p "$lock"
    printf '%s' "-1" > "$lock/owner"
    touch -t 200001010000 "$lock"
    if ! RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS=10 "$resolver" >/dev/null 2> "$err"; then
        fail "$label lock owner: an aged lock with unusable owner content was not broken"
        sed 's/^/        /' "$err" >&2
        rm -rf "$lock"
        return
    fi

    # A dead owner is broken straight away, without waiting for the age rule.
    rm -rf "$lock"
    mkdir -p "$lock"
    printf '%s' "$(dead_pid)" > "$lock/owner"
    if ! RGBX_TOOLCHAIN_LOCK_WAIT_SECONDS=10 "$resolver" >/dev/null 2> "$err"; then
        fail "$label lock owner: a lock whose owner is gone was not broken"
        sed 's/^/        /' "$err" >&2
        rm -rf "$lock"
        return
    fi
    rm -rf "$lock"
    pass "$label lock owner: unusable owner content never breaks a live lock and never wedges one"
}

# --- per-resolver setup ----------------------------------------------------

run_resolver_suite() {
    local label="$1" resolver_src="$2" pin_prefix="$3" tree_name="$4"
    local compiler="$5" nested_glob="$6" archive_name="$7" tar_flags="$8"
    local extracted_name="$9" lock_name="${10}"

    local home="$scratch/$label"
    local cache="$home/cache/rgb-sunglasses"
    local stage="$home/stage"
    local bin="$home/bin"
    mkdir -p "$cache" "$stage" "$bin"

    build_fixture_tree "$stage/$extracted_name" "$compiler"
    local archive="$home/$archive_name"
    tar "$tar_flags" "$archive" -C "$stage" "$extracted_name"
    local digest
    digest="$(sha256_file "$archive")"

    local resolver
    resolver="$home/$(basename "$resolver_src")"
    repin_resolver "$resolver_src" "$resolver" "$pin_prefix" "$digest"

    write_curl_stub "$bin"

    export XDG_CACHE_HOME="$home/cache"
    export RGBX_TEST_FIXTURE_ARCHIVE="$archive"
    export RGBX_TEST_CURL_LOG="$home/curl.log"
    export PATH="$bin:$PATH"
    # The resolvers' documented operator overrides must not leak in from the
    # developer's environment and short-circuit the resolve under test.
    unset RGBX_ARM_TOOLCHAIN_PATH WASI_SDK_PATH

    local tree="$cache/$tree_name"
    local lock="$cache/$lock_name"
    echo "$label ($tree_name):"
    case_concurrent "$label" "$resolver" "$tree" "$compiler" "$nested_glob"
    case_straggler_permissions "$label" "$resolver" "$tree" "$compiler" "$nested_glob"
    case_unremovable_aside "$label" "$resolver" "$tree" "$compiler" "$nested_glob"
    case_lock_deleted "$label" "$resolver" "$tree" "$compiler" "$nested_glob" "$lock"
    case_lock_path_hygiene "$label" "$resolver" "$lock"
    case_lock_owner_content "$label" "$resolver" "$lock"
    case_lock_wait_clamp "$label" "$resolver"
    case_hijacked_swap "$label" "$resolver" "$tree" "$compiler"

    PATH="${PATH#"$bin":}"
    unset XDG_CACHE_HOME RGBX_TEST_FIXTURE_ARCHIVE RGBX_TEST_CURL_LOG
}

detect_immutable_support || true

arm_version="$(pin_value "$ARM_RESOLVER" ARM_TOOLCHAIN_VERSION)"
run_resolver_suite arm "$ARM_RESOLVER" ARM_TOOLCHAIN_SHA256_ \
    "arm-gnu-toolchain-$arm_version" "bin/arm-none-eabi-gcc" \
    "arm-gnu-toolchain-*-arm-none-eabi" \
    "fixture.tar.xz" "-cJf" "arm-gnu-toolchain-$arm_version-fixture-arm-none-eabi" \
    "arm-gnu-toolchain.lock"

wasi_version="$(pin_value "$WASI_RESOLVER" WASI_SDK_VERSION)"
case "$(uname -m)" in
    x86_64) wasi_arch="x86_64" ;;
    arm64 | aarch64) wasi_arch="arm64" ;;
    *) echo "error: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac
case "$(uname -s)" in
    Linux) wasi_os="linux" ;;
    Darwin) wasi_os="macos" ;;
    *) echo "error: unsupported OS $(uname -s)" >&2; exit 1 ;;
esac
run_resolver_suite wasi "$WASI_RESOLVER" WASI_SDK_SHA256_ \
    "wasi-sdk-$wasi_version" "bin/clang" "wasi-sdk-*" \
    "fixture.tar.gz" "-czf" "wasi-sdk-$wasi_version-$wasi_arch-$wasi_os" \
    "wasi-sdk.lock"

if [ "$failures" -ne 0 ]; then
    echo "$failures toolchain resolver check(s) failed" >&2
    exit 1
fi
echo "toolchain resolver concurrency, straggler and lock checks passed"
