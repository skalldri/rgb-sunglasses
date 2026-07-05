#!/usr/bin/env bash
# Coordinates exclusive "checkout" access to shared physical hardware (the one
# dev board + J-Link, and the one Android phone) across multiple Claude Code
# agents each working in their own git worktree of this repo.
#
# Lock storage lives under $(git rev-parse --git-common-dir)/hardware-locks/ --
# a single shared path resolvable identically from every worktree, since it's
# anchored inside the repo's one shared .git directory rather than any
# particular worktree's own path.
#
# Usage:
#   hw-lock.sh acquire <resource...> [--steal] [--reason TEXT] [--pid N] [--fresh-only]
#                                    [--wait SECONDS] [--poll-interval SECONDS]
#   hw-lock.sh release <resource...> [--force]
#   hw-lock.sh release --all [--force]
#   hw-lock.sh status [resource...]
#   hw-lock.sh check <resource>
#
# Resources: board (dev board + J-Link, used together), app (the one physical
# Android phone). See .claude/skills/hw-lock/SKILL.md for the full write-up.
#
# --wait queues fairly (FIFO), it does not just retry-and-race: each waiter
# registers a ticket per requested resource under that resource's queue/ dir
# (a fixed-width nanosecond-timestamp filename, so lexical sort == arrival
# order), and only attempts the actual acquire once its own ticket is the
# oldest still-valid one on *every* requested resource. A ticket whose owning
# process has died is reaped (skipped) wherever it's encountered, so a crashed
# waiter can never permanently jam the line for those behind it.
set -euo pipefail

VALID_RESOURCES=(board app)
declare -A MY_TICKETS=()

if ! WORKTREE_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"; then
    echo "[!] hw-lock.sh must be run from inside a git worktree of this repo." >&2
    exit 1
fi

GIT_COMMON_DIR_RAW="$(git rev-parse --git-common-dir)"
case "$GIT_COMMON_DIR_RAW" in
    /*) GIT_COMMON_DIR="$GIT_COMMON_DIR_RAW" ;;
    *)  GIT_COMMON_DIR="$WORKTREE_ROOT/$GIT_COMMON_DIR_RAW" ;;
esac
LOCK_ROOT="$GIT_COMMON_DIR/hardware-locks"
BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")"

HOLDER_ID="${CLAUDE_CODE_SESSION_ID:-${CLAUDE_CODE_BRIDGE_SESSION_ID:-${HW_LOCK_HOLDER_ID:-$(id -un)@$(hostname)}}}"

log() { echo "[hw-lock] $*" >&2; }
usage_err() { echo "[!] $*" >&2; exit 2; }

validate_resource() {
    local r="$1" v
    for v in "${VALID_RESOURCES[@]}"; do [ "$r" = "$v" ] && return 0; done
    usage_err "Unknown resource '$r'. Valid resources: ${VALID_RESOURCES[*]}"
}

meta_get() { grep -m1 "^$2=" "$1" 2>/dev/null | cut -d= -f2-; }

# The lock "held" marker lives in its own subdirectory, separate from the
# resource's queue/ (see below) -- so releasing or reclaiming a lock never
# touches (and can never accidentally wipe out) other agents' pending wait
# tickets for that same resource, which sit alongside it, not inside it.
lock_dir_for() { echo "$LOCK_ROOT/$1/held"; }

write_meta() {
    local resource="$1" reason="$2" pid="$3"
    {
        echo "session_id=$HOLDER_ID"
        echo "bridge_session_id=${CLAUDE_CODE_BRIDGE_SESSION_ID:-}"
        echo "worktree=$WORKTREE_ROOT"
        echo "branch=$BRANCH"
        echo "hostname=$(hostname)"
        echo "acquired_at=$(date +%s)"
        echo "acquired_at_human=$(date -Is)"
        echo "pid=$pid"
        echo "reason=$reason"
    } > "$(lock_dir_for "$resource")/info"
}

# Reclaims (deletes) the lock dir for $1 if it's stale: metadata missing, its
# recorded worktree no longer exists, or its recorded pid is no longer alive.
maybe_reclaim_stale() {
    local resource="$1"
    local dir
    dir="$(lock_dir_for "$resource")"
    [ -d "$dir" ] || return 0

    if [ ! -f "$dir/info" ]; then
        log "'$resource' lock dir has no metadata -- treating as stale, reclaiming."
        rm -rf "$dir"
        return 0
    fi

    local wt pid
    wt="$(meta_get "$dir/info" worktree)"
    pid="$(meta_get "$dir/info" pid)"

    if [ -z "$wt" ] || [ ! -d "$wt" ]; then
        log "Stale lock cleared for '$resource' (held by $(meta_get "$dir/info" session_id) since $(meta_get "$dir/info" acquired_at_human); worktree '$wt' no longer exists)."
        rm -rf "$dir"
        return 0
    fi

    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
        log "Stale lock cleared for '$resource' (held by $(meta_get "$dir/info" session_id) since $(meta_get "$dir/info" acquired_at_human); tracked pid $pid is no longer running)."
        rm -rf "$dir"
        return 0
    fi
}

describe_conflict() {
    local f="$1"
    echo "held by $(meta_get "$f" session_id) in worktree $(meta_get "$f" worktree) (branch $(meta_get "$f" branch), since $(meta_get "$f" acquired_at_human))"
}

# --- FIFO waiting queue (per resource) -------------------------------------

queue_dir_for() { echo "$LOCK_ROOT/$1/queue"; }

sanitize_for_filename() { printf '%s' "$1" | tr -c 'A-Za-z0-9._-' '_'; }

# Registers a waiting ticket for resource $1, sets TICKET_PATH. Filename is
# <19-digit nanosecond timestamp>-<holder>-<pid>.ticket -- fixed-width numeric
# prefix so a plain lexical `sort` gives arrival order.
register_ticket() {
    local resource="$1" qdir name path
    qdir="$(queue_dir_for "$resource")"
    mkdir -p "$qdir"
    name="$(date +%s%N)-$(sanitize_for_filename "$HOLDER_ID")-$$.ticket"
    path="$qdir/$name"
    {
        echo "session_id=$HOLDER_ID"
        echo "pid=$$"
        echo "worktree=$WORKTREE_ROOT"
        echo "queued_at_human=$(date -Is)"
    } > "$path"
    TICKET_PATH="$path"
}

# Lists still-valid ticket basenames for resource $1, oldest first, reaping
# (deleting) any ticket whose owning pid is no longer running as it goes --
# so a crashed waiter can't permanently block the line for those behind it.
valid_tickets_for() {
    local resource="$1" qdir f pid
    qdir="$(queue_dir_for "$resource")"
    [ -d "$qdir" ] || return 0
    for f in "$qdir"/*.ticket; do
        [ -e "$f" ] || continue
        pid="$(meta_get "$f" pid)"
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
            rm -f "$f"
            continue
        fi
        basename "$f"
    done | sort
}

# True iff our own ticket is the oldest still-valid one for resource $1.
is_my_turn() {
    local resource="$1" mine head
    mine="$(basename "${MY_TICKETS[$resource]}")"
    head="$(valid_tickets_for "$resource" | head -n1)"
    [ "$head" = "$mine" ]
}

# " board: #2/3 app: #1/1" -- our position and queue depth per resource.
queue_summary() {
    local r line="" tickets total pos
    for r in "${RESOURCES[@]}"; do
        tickets="$(valid_tickets_for "$r")"
        total=$(printf '%s\n' "$tickets" | grep -c . || true)
        pos=$(printf '%s\n' "$tickets" | grep -n -F -x "$(basename "${MY_TICKETS[$r]}")" | cut -d: -f1)
        line+=" $r: #${pos:-?}/${total}"
    done
    echo "$line"
}

remove_my_tickets() {
    local r
    for r in "${!MY_TICKETS[@]}"; do rm -f "${MY_TICKETS[$r]}"; done
}

# --- Core acquire/release ---------------------------------------------------

# Sets CONFLICT_INFO on failure. Returns: 0 = newly acquired, 1 = conflict,
# 2 = already held by this session (idempotent no-op), unless FRESH_ONLY=1, in
# which case an already-held-by-us lock is also treated as a conflict (1).
try_acquire_one() {
    local resource="$1"
    local dir
    dir="$(lock_dir_for "$resource")"
    maybe_reclaim_stale "$resource"

    if [ -d "$dir" ]; then
        if [ "$(meta_get "$dir/info" session_id)" = "$HOLDER_ID" ] && [ "${FRESH_ONLY:-0}" != "1" ]; then
            return 2
        fi
        CONFLICT_INFO="$dir/info"
        return 1
    fi

    # -p on the parent (the resource's namespace dir, which may also hold a
    # queue/ subdir) is harmless/idempotent under concurrent callers; the
    # final path component is a plain (non -p) mkdir, which is what's
    # actually atomic and race-safe.
    mkdir -p "$(dirname "$dir")"
    if mkdir "$dir" 2>/dev/null; then
        write_meta "$resource" "$REASON" "$PID_ARG"
        return 0
    fi

    # Lost a race between the -d check and mkdir.
    CONFLICT_INFO="$dir/info"
    return 1
}

# Attempts every resource in RESOURCES (global array) exactly once, in order.
# All-or-nothing: any conflict rolls back only what *this call* freshly
# acquired before returning 1. On conflict, sets LAST_CONFLICT_RESOURCE and
# LAST_CONFLICT_MSG for the caller to report/log. Returns 0 if every resource
# ends up held by this session (freshly acquired or already-held, per
# FRESH_ONLY semantics in try_acquire_one).
try_acquire_all() {
    local newly_acquired=() r rc

    for r in "${RESOURCES[@]}"; do
        if [ "${STEAL:-0}" = 1 ] && [ -d "$(lock_dir_for "$r")" ]; then
            log "WARNING: --steal forcibly reclaiming '$r' from $(describe_conflict "$(lock_dir_for "$r")/info")"
            rm -rf "$(lock_dir_for "$r")"
        fi

        rc=0
        try_acquire_one "$r" || rc=$?
        case $rc in
            0) newly_acquired+=("$r") ;;
            2) : ;;
            1)
                LAST_CONFLICT_RESOURCE="$r"
                if [ "${FRESH_ONLY:-0}" = "1" ] && [ "$(meta_get "$CONFLICT_INFO" session_id)" = "$HOLDER_ID" ]; then
                    LAST_CONFLICT_MSG="already held by this same session ($(describe_conflict "$CONFLICT_INFO")) -- refusing to start a second instance"
                else
                    LAST_CONFLICT_MSG="$(describe_conflict "$CONFLICT_INFO")"
                fi
                local r2
                for r2 in "${newly_acquired[@]}"; do rm -rf "$(lock_dir_for "$r2")"; done
                return 1
                ;;
        esac
    done
    return 0
}

cmd_acquire() {
    local steal=0 reason="" pid="" wait_seconds=0 poll_interval=5 resources=()
    FRESH_ONLY=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --steal) steal=1; shift ;;
            --reason) reason="${2:-}"; shift 2 ;;
            --pid) pid="${2:-}"; shift 2 ;;
            --fresh-only) FRESH_ONLY=1; shift ;;
            --wait) wait_seconds="${2:-}"; shift 2 ;;
            --poll-interval) poll_interval="${2:-}"; shift 2 ;;
            -*) usage_err "unknown flag $1" ;;
            *) resources+=("$1"); shift ;;
        esac
    done
    [ ${#resources[@]} -gt 0 ] || usage_err "acquire requires at least one resource (${VALID_RESOURCES[*]})"
    case "$wait_seconds" in ''|*[!0-9]*) usage_err "--wait requires a non-negative integer number of seconds" ;; esac
    case "$poll_interval" in ''|*[!0-9]*|0) usage_err "--poll-interval requires a positive integer number of seconds" ;; esac

    local seen=() r
    for r in "${resources[@]}"; do
        validate_resource "$r"
        case " ${seen[*]:-} " in *" $r "*) ;; *) seen+=("$r") ;; esac
    done
    RESOURCES=("${seen[@]}")

    mkdir -p "$LOCK_ROOT"
    REASON="$reason"
    PID_ARG="$pid"
    STEAL="$steal"

    if try_acquire_all; then
        echo "Acquired: ${RESOURCES[*]} (session $HOLDER_ID)"
        return 0
    fi

    if [ "$wait_seconds" -eq 0 ]; then
        echo "[!] Cannot acquire '$LAST_CONFLICT_RESOURCE': $LAST_CONFLICT_MSG" >&2
        exit 1
    fi

    # --wait: FIFO queue, not a race. Register a ticket per requested resource
    # and only ever attempt the real (all-or-nothing) acquire once we're the
    # oldest still-valid waiter on *every* one of them -- otherwise we just
    # keep sleeping even if the resource happens to be free, so we never jump
    # ahead of whoever arrived before us. This also preserves the no-deadlock
    # property: we still never hold a partial set between polls, since
    # try_acquire_all itself rolls back on any conflict as before.
    for r in "${RESOURCES[@]}"; do
        register_ticket "$r"
        MY_TICKETS["$r"]="$TICKET_PATH"
    done
    trap remove_my_tickets EXIT INT TERM

    local deadline=$(( $(date +%s) + wait_seconds ))
    local next_log=$(( $(date +%s) + 30 ))
    log "Waiting up to ${wait_seconds}s for '$LAST_CONFLICT_RESOURCE' ($LAST_CONFLICT_MSG) --$(queue_summary)"

    while true; do
        sleep "$(( poll_interval + (RANDOM % 3) ))"

        local my_turn=1
        for r in "${RESOURCES[@]}"; do
            is_my_turn "$r" || my_turn=0
        done

        if [ "$my_turn" = 1 ] && try_acquire_all; then
            echo "Acquired: ${RESOURCES[*]} (session $HOLDER_ID, after waiting)"
            return 0
        fi

        local now
        now=$(date +%s)
        if [ "$now" -ge "$deadline" ]; then
            echo "[!] Timed out after ${wait_seconds}s waiting to acquire '$LAST_CONFLICT_RESOURCE': $LAST_CONFLICT_MSG" >&2
            exit 1
        fi
        if [ "$now" -ge "$next_log" ]; then
            log "Still waiting -- $(( deadline - now ))s left...$(queue_summary)"
            next_log=$(( now + 30 ))
        fi
    done
}

cmd_release() {
    local force=0 all=0 resources=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --force) force=1; shift ;;
            --all) all=1; shift ;;
            -*) usage_err "unknown flag $1" ;;
            *) resources+=("$1"); shift ;;
        esac
    done

    if [ "$all" = 1 ]; then
        resources=()
        local rr
        for rr in "${VALID_RESOURCES[@]}"; do
            [ -d "$(lock_dir_for "$rr")" ] && resources+=("$rr")
        done
    fi
    [ ${#resources[@]} -gt 0 ] || usage_err "release requires at least one resource or --all"

    local failed=0 r
    for r in "${resources[@]}"; do
        validate_resource "$r"
        local dir
        dir="$(lock_dir_for "$r")"
        if [ ! -d "$dir" ]; then
            echo "'$r' already free."
            continue
        fi
        local sid
        sid="$(meta_get "$dir/info" session_id)"
        if [ "$sid" != "$HOLDER_ID" ] && [ "$force" != 1 ]; then
            echo "[!] '$r' is held by $sid (worktree $(meta_get "$dir/info" worktree)), not this session. Use --force to override." >&2
            failed=1
            continue
        fi
        # A lock acquired with --pid (e.g. launch-app.sh) tracks the process
        # actually using the resource. Releasing the lock dir while that pid
        # is still alive desyncs the lock from reality: the resource stays in
        # use (Metro/expo still running, board still open) but looks FREE to
        # every other session, so someone else can acquire and collide with
        # it. Require --force here too, same as the cross-session override
        # above, so this can't happen by an unnoticed bare `release`.
        local pid reason
        pid="$(meta_get "$dir/info" pid)"
        reason="$(meta_get "$dir/info" reason)"
        if [ -n "$pid" ] && [ "$force" != 1 ] && kill -0 "$pid" 2>/dev/null; then
            echo "[!] '$r' is still actively held by a live process (pid $pid${reason:+, $reason}) -- releasing now would desync this lock from actual hardware usage while that process keeps running. Stop the owning process instead (it releases the lock automatically on exit), or pass --force to release anyway." >&2
            failed=1
            continue
        fi
        rm -rf "$dir"
        echo "Released '$r'"
    done
    [ "$failed" = 0 ]
}

cmd_status() {
    local resources=("$@")
    [ ${#resources[@]} -gt 0 ] || resources=("${VALID_RESOURCES[@]}")
    local r
    for r in "${resources[@]}"; do
        validate_resource "$r"
        local dir
        dir="$(lock_dir_for "$r")"
        local waiting
        waiting=$(valid_tickets_for "$r" | grep -c . || true)
        local waiting_suffix=""
        [ "$waiting" -gt 0 ] && waiting_suffix=" ($waiting waiting)"
        if [ ! -d "$dir" ]; then
            echo "$r: FREE$waiting_suffix"
            continue
        fi
        local sid wt br host acq_h acq_e pid reason now age stale=""
        sid="$(meta_get "$dir/info" session_id)"
        wt="$(meta_get "$dir/info" worktree)"
        br="$(meta_get "$dir/info" branch)"
        host="$(meta_get "$dir/info" hostname)"
        acq_h="$(meta_get "$dir/info" acquired_at_human)"
        acq_e="$(meta_get "$dir/info" acquired_at)"
        pid="$(meta_get "$dir/info" pid)"
        reason="$(meta_get "$dir/info" reason)"
        now=$(date +%s)
        age=$(( now - ${acq_e:-$now} ))
        if [ -z "$wt" ] || [ ! -d "$wt" ]; then
            stale=" [STALE -- worktree missing, will auto-reclaim on next acquire]"
        elif [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
            stale=" [STALE -- tracked pid $pid is no longer running, will auto-reclaim on next acquire]"
        fi
        echo "$r: HELD by $sid on $host (branch $br, worktree $wt${pid:+, pid $pid}) since $acq_h (${age}s ago)${stale}${reason:+ -- $reason}$waiting_suffix"
    done
}

cmd_check() {
    [ $# -eq 1 ] || usage_err "check requires exactly one resource"
    local r="$1"
    validate_resource "$r"
    maybe_reclaim_stale "$r"
    local dir
    dir="$(lock_dir_for "$r")"
    if [ ! -d "$dir" ]; then
        echo "'$r' is not held." >&2
        return 1
    fi
    if [ "$(meta_get "$dir/info" session_id)" = "$HOLDER_ID" ]; then
        return 0
    fi
    echo "'$r' is held by $(meta_get "$dir/info" session_id), not this session." >&2
    return 1
}

case "${1:-}" in
    acquire) shift; cmd_acquire "$@" ;;
    release) shift; cmd_release "$@" ;;
    status)  shift; cmd_status "$@" ;;
    check)   shift; cmd_check "$@" ;;
    *) usage_err "usage: hw-lock.sh {acquire|release|status|check} ..." ;;
esac
