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
#   hw-lock.sh hold <resource...> [--steal] [--reason TEXT]
#                                 [--wait SECONDS] [--poll-interval SECONDS]
#   hw-lock.sh release <resource...> [--force]
#   hw-lock.sh release --all [--force]
#   hw-lock.sh status [resource...]
#   hw-lock.sh check <resource>
#   hw-lock.sh waiters <resource>
#   hw-lock.sh note-metro-pid <resource> <pid>  (internal -- see launch-app.sh)
#
# Resources: board (dev board + J-Link, used together), app (the one physical
# Android phone). See .claude/skills/hw-lock/SKILL.md for the full write-up.
#
# `hold` is the *only* way to take a lock -- there is deliberately no bare
# "acquire and forget" subcommand. It always runs as a long-lived, foregrounded
# process (launch it via the `Monitor` tool with `persistent: true`) tracking
# its own pid, and holds the lock for exactly as long as it keeps running --
# no timer, no hardware-state signal (USB de-enumeration, etc.) ever releases
# it early. If another session is waiting, `hold` only ever *tells you* (a
# printed notice, surfaced as a Monitor event even if this session is
# otherwise idle) -- it never auto-evicts. You decide when to actually
# release, by stopping the `hold` task (its own exit-trap releases) or by
# running `release ... --force` yourself.
#
# Re-running `hold` from a session that ALREADY holds the resource ADOPTS it:
# the new process takes over as the tracked pid/heartbeat and reports success
# immediately (it does not refuse, and does not futilely --wait on itself -- a
# session can never be waiting on a lock it is itself holding). This is the
# recovery path when a prior `hold` task died or was lost (e.g. context reset)
# but the lock record persists: just run `hold` again. Exclusion is per-session,
# so a second in-session hold is never a real conflict; the superseded sibling's
# release is pid-guarded so its later exit can't yank the lock from the adopter.
# A DIFFERENT session still conflicts exactly as before (fail-fast, or FIFO-queue
# with --wait).
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
# Resources this cmd_hold invocation ADOPTED (took over an existing same-session
# hold) rather than freshly acquired -- see try_acquire_one. Governs the "adopted"
# messaging and whether kill_orphaned_metro runs for 'app'.
declare -A ADOPTED=()
# For each adopted resource, the tracked pid we displaced -- so an all-or-nothing
# rollback (a later resource in the same hold conflicts) can hand authority back
# to the sibling instead of deleting a lock the session legitimately held.
declare -A ADOPTED_PREV_PID=()

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

# Idempotently sets/overwrites a single "key=value" line in resource $1's
# info file on top of whatever write_meta already wrote (currently just
# metro_pid, set after the initial acquire -- see cmd_note_metro_pid).
# Strips any pre-existing line for the same key first, so a relaunch under
# the same still-held lock (stop Metro, rerun launch-app.sh) updates the
# value in place rather than leaving stale duplicate lines meta_get's
# "grep -m1" could pick the wrong one of. Writes to a temp file in the same
# directory then mv's it into place -- same-filesystem mv is atomic, so a
# concurrent reader (cmd_status, cmd_release, etc.) never observes a
# half-written file.
set_meta_field() {
    local resource="$1" key="$2" value="$3" dir tmp
    dir="$(lock_dir_for "$resource")"
    tmp="$(mktemp "$dir/info.XXXXXX")"
    { grep -v "^$key=" "$dir/info" 2>/dev/null || true; echo "$key=$value"; } > "$tmp"
    mv -f "$tmp" "$dir/info"
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

# Sets CONFLICT_INFO on failure. Returns: 0 = newly acquired (or, under
# FRESH_ONLY=1, ADOPTED an existing same-session hold -- ADOPTED[resource] is set
# to tell the two apart), 1 = conflict (a DIFFERENT session holds it), 2 = already
# held by this session (idempotent no-op; only when FRESH_ONLY is unset).
try_acquire_one() {
    local resource="$1"
    local dir
    dir="$(lock_dir_for "$resource")"
    maybe_reclaim_stale "$resource"

    if [ -d "$dir" ]; then
        if [ "$(meta_get "$dir/info" session_id)" = "$HOLDER_ID" ]; then
            if [ "${FRESH_ONLY:-0}" != "1" ]; then
                return 2
            fi
            # FRESH_ONLY=1 (a `hold`) but THIS session already holds it. Because
            # maybe_reclaim_stale() above already reclaimed the lock if its tracked
            # pid were dead, reaching here means a still-LIVE sibling `hold` in this
            # same session owns it. Don't refuse (the old behavior, which then made
            # --wait spin futilely -- the waiter and the holder are the same session,
            # so it could never clear). Instead ADOPT: rewrite the tracked pid to
            # ours so THIS process becomes the authoritative holder/heartbeat. Safe
            # because exclusion is per-session -- the session already had exclusive
            # access, so a second hold within it is never a cross-agent conflict.
            # The superseded sibling's release is pid-guarded (see hold_release_all),
            # so its eventual exit won't pull the lock out from under us. This is what
            # lets an agent re-run `hold` to recover after its heartbeat task died
            # without its release trap firing (e.g. a hard kill that survived
            # stale-reclaim only because a sibling kept the pid alive).
            ADOPTED["$resource"]=1
            ADOPTED_PREV_PID["$resource"]="$(meta_get "$dir/info" pid)"
            set_meta_field "$resource" pid "$PID_ARG"
            [ -n "$REASON" ] && set_meta_field "$resource" reason "$REASON"
            return 0
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

    # Lost a race between the -d check and mkdir: another process created it
    # first. If that process is our own session (a sibling `hold` starting at the
    # same instant), adopt it exactly like the already-existing case above;
    # otherwise it's a genuine cross-session conflict.
    if [ "$(meta_get "$dir/info" session_id)" = "$HOLDER_ID" ] && [ "${FRESH_ONLY:-0}" = "1" ]; then
        ADOPTED["$resource"]=1
        ADOPTED_PREV_PID["$resource"]="$(meta_get "$dir/info" pid)"
        set_meta_field "$resource" pid "$PID_ARG"
        [ -n "$REASON" ] && set_meta_field "$resource" reason "$REASON"
        return 0
    fi
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
                # A conflict now only ever means a DIFFERENT session holds it --
                # a same-session hold is adopted (return 0) above, not refused.
                LAST_CONFLICT_RESOURCE="$r"
                LAST_CONFLICT_MSG="$(describe_conflict "$CONFLICT_INFO")"
                # Roll back only what THIS call did: delete freshly-created locks,
                # but hand adopted ones back to the sibling we displaced (deleting
                # them would release a lock the session legitimately still holds).
                local r2
                for r2 in "${newly_acquired[@]}"; do
                    if [ -n "${ADOPTED[$r2]:-}" ]; then
                        set_meta_field "$r2" pid "${ADOPTED_PREV_PID[$r2]}"
                        unset "ADOPTED[$r2]" "ADOPTED_PREV_PID[$r2]"
                    else
                        rm -rf "$(lock_dir_for "$r2")"
                    fi
                done
                return 1
                ;;
        esac
    done
    return 0
}

# Attempts to acquire every resource in RESOURCES (global array, set by the
# caller), blocking up to $1 seconds via the FIFO wait queue if the first
# attempt conflicts (0 = don't wait, fail immediately). Assumes
# REASON/PID_ARG/STEAL/FRESH_ONLY are already set by the caller, exactly as
# try_acquire_all expects. Returns 0 on success, 1 on conflict/timeout, with
# LAST_CONFLICT_RESOURCE/LAST_CONFLICT_MSG set on failure exactly as before.
# Does not print anything on success or failure -- callers own their own
# messages. On success, always cleans up this call's own FIFO ticket(s)
# immediately (register_ticket's entries are only otherwise removed by the
# EXIT trap below or by another session's dead-pid reaping in
# valid_tickets_for -- neither fires on a successful acquire, so a caller
# that queued via --wait and then itself polls `waiters` on the same
# resource would otherwise see its own leftover ticket and mistake itself
# for a waiter).
acquire_resources_blocking() {
    local wait_seconds="$1" poll_interval="$2"

    if try_acquire_all; then
        remove_my_tickets
        return 0
    fi

    if [ "$wait_seconds" -eq 0 ]; then
        return 1
    fi

    # --wait: FIFO queue, not a race. Register a ticket per requested resource
    # and only ever attempt the real (all-or-nothing) acquire once we're the
    # oldest still-valid waiter on *every* one of them -- otherwise we just
    # keep sleeping even if the resource happens to be free, so we never jump
    # ahead of whoever arrived before us. This also preserves the no-deadlock
    # property: we still never hold a partial set between polls, since
    # try_acquire_all itself rolls back on any conflict as before.
    local r
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
            remove_my_tickets
            return 0
        fi

        local now
        now=$(date +%s)
        if [ "$now" -ge "$deadline" ]; then
            return 1
        fi
        if [ "$now" -ge "$next_log" ]; then
            log "Still waiting -- $(( deadline - now ))s left...$(queue_summary)"
            next_log=$(( now + 30 ))
        fi
    done
}

# Kills any Metro/expo processes (and any lingering launch-app.sh wrapper)
# still running. Called only once, right after cmd_hold confirms exclusive
# ownership of the 'app' resource -- so by construction, anything found here
# is necessarily an orphan (a legitimate concurrent holder would have made
# our acquire above conflict instead), most likely left behind by a session
# that was hard-killed or crashed without its exit trap running. Never
# pkill/killall (banned repo-wide, see root CLAUDE.md "Process management")
# -- pgrep for exact PIDs, then a targeted kill per PID, SIGTERM with a short
# grace period, then SIGKILL for stragglers.
#
# pgrep -f matches a process's FULL command line, which includes THIS
# process's own --reason text -- a hold launched with e.g.
# `--reason "... launch-app.sh ..."` would otherwise match and kill itself
# (hit for real during testing: a --reason mentioning "launch-app.sh"
# self-matched and killed the hold process running this very function).
# Exclude our own pid unconditionally, and exclude any matched pid whose own
# command line contains "hw-lock.sh" -- a real Metro/expo/launch-app.sh
# process never does, so this is a safe, principled filter, not a special
# case just for $$ (it also protects against a *different* concurrent
# hw-lock.sh invocation's --reason text causing a cross-match).
kill_orphaned_metro() {
    local candidates pid cmdline pids=()
    candidates="$( { pgrep -f 'expo run:android'; pgrep -f 'launch-app\.sh'; } 2>/dev/null | sort -u || true)"
    [ -n "$candidates" ] || return 0

    for pid in $candidates; do
        [ "$pid" = "$$" ] && continue
        cmdline="$(ps -o cmd= -p "$pid" 2>/dev/null || true)"
        case "$cmdline" in
            *hw-lock.sh*) continue ;;
        esac
        pids+=("$pid")
    done
    [ ${#pids[@]} -gt 0 ] || return 0

    log "Found orphaned Metro/launch-app.sh process(es) still running (pid(s): ${pids[*]}) -- killing before considering this hold established."
    for pid in "${pids[@]}"; do
        kill -TERM "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in "${pids[@]}"; do
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    done
}

# Self-release trap for cmd_hold -- fires on clean exit, kill, or crash.
# Never triggered by the waiter-poll loop in cmd_hold itself (see below).
# --force is required and safe here: we ARE the tracked pid for every resource we
# still hold, so this is a deliberate self-release, not a cross-session override.
#
# Pid guard: only release a resource if its lock STILL records our pid. If a later
# same-session `hold` adopted it (rewrote the tracked pid to its own -- see
# try_acquire_one), that newer hold is now authoritative and owns the release;
# releasing here would pull the lock out from under it. Skipping is safe because
# the adopter's own trap will release when it exits.
hold_release_all() {
    [ "$RELEASED_ONCE" = 1 ] && return
    RELEASED_ONCE=1
    local r tracked
    for r in "${RESOURCES[@]}"; do
        tracked="$(meta_get "$(lock_dir_for "$r")/info" pid 2>/dev/null || true)"
        if [ -n "$tracked" ] && [ "$tracked" != "$$" ]; then
            continue
        fi
        "$0" release "$r" --force >/dev/null 2>&1 || true
    done
}

# The only subcommand capable of taking a lock. Always meant to be launched
# as a long-lived foreground process (via the `Monitor` tool, persistent:
# true) -- it holds the requested resources for exactly as long as it keeps
# running, and releases them the moment it stops, however it stops. It never
# releases early on its own: if another session is waiting, it only ever
# prints a notice (a `Monitor` event, reaching this session even if otherwise
# idle) -- holding a lock means exclusive access until *you* decide to let
# it go, never on a timer or because a hardware surface (e.g. the J-Link
# de-enumerating mid-flash) went quiet.
cmd_hold() {
    local steal=0 reason="" wait_seconds=0 acquire_poll_interval=5 resources=()
    while [ $# -gt 0 ]; do
        case "$1" in
            --steal) steal=1; shift ;;
            --reason) reason="${2:-}"; shift 2 ;;
            --wait) wait_seconds="${2:-}"; shift 2 ;;
            --poll-interval) acquire_poll_interval="${2:-}"; shift 2 ;;
            -*) usage_err "unknown flag $1" ;;
            *) resources+=("$1"); shift ;;
        esac
    done
    [ ${#resources[@]} -gt 0 ] || usage_err "hold requires at least one resource (${VALID_RESOURCES[*]})"
    case "$wait_seconds" in ''|*[!0-9]*) usage_err "--wait requires a non-negative integer number of seconds" ;; esac
    case "$acquire_poll_interval" in ''|*[!0-9]*|0) usage_err "--poll-interval requires a positive integer number of seconds" ;; esac

    local seen=() r
    for r in "${resources[@]}"; do
        validate_resource "$r"
        case " ${seen[*]:-} " in *" $r "*) ;; *) seen+=("$r") ;; esac
    done
    RESOURCES=("${seen[@]}")

    mkdir -p "$LOCK_ROOT"
    REASON="$reason"
    PID_ARG="$$"
    STEAL="$steal"
    FRESH_ONLY=1

    if ! acquire_resources_blocking "$wait_seconds" "$acquire_poll_interval"; then
        echo "[!] Cannot hold '$LAST_CONFLICT_RESOURCE': $LAST_CONFLICT_MSG" >&2
        exit 1
    fi

    echo "Holding: ${RESOURCES[*]} (session $HOLDER_ID, pid $$)"
    if [ "${#ADOPTED[@]}" -gt 0 ]; then
        echo "Adopted existing same-session hold(s) for: ${!ADOPTED[*]} (now tracked by pid $$; a prior heartbeat in this session already held them). This is the authoritative hold now -- stopping it releases."
    fi

    for r in "${RESOURCES[@]}"; do
        # Skip the Metro-orphan sweep for an ADOPTED 'app' hold: the sibling we
        # took over already ran it and may have a legitimate Metro running (tracked
        # via metro_pid); re-sweeping could kill it. Only a FRESH app acquire means
        # nothing legitimate can be running yet, so anything found is a true orphan.
        if [ "$r" = "app" ] && [ -z "${ADOPTED[app]:-}" ]; then
            kill_orphaned_metro
            break
        fi
    done

    RELEASED_ONCE=0
    trap hold_release_all EXIT INT TERM

    # Silent in steady state -- only ever prints when the waiter-present
    # state actually changes, plus an infrequent re-notify if the same
    # waiter is still around, so a multi-hour hold stays well under
    # Monitor's "too many events" auto-stop threshold.
    local notified=0 last_notify=0
    while true; do
        sleep "$(( 15 + (RANDOM % 5) ))"

        local total=0
        for r in "${RESOURCES[@]}"; do
            total=$(( total + $(cmd_waiters "$r") ))
        done

        local now
        now=$(date +%s)
        if [ "$total" -gt 0 ]; then
            if [ "$notified" = 0 ] || [ $(( now - last_notify )) -ge 600 ]; then
                echo "WAITER_PRESENT: $total agent(s) waiting for ${RESOURCES[*]} -- this hold is NOT released automatically. Release when you can: scripts/hw-lock.sh release ${RESOURCES[*]} --force (or stop this task, which releases automatically)."
                notified=1
                last_notify=$now
            fi
        else
            notified=0
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
        # `pid` here is always the `hold` process's own $$ (set internally by
        # cmd_hold via PID_ARG -- there is no caller-supplied --pid flag; an
        # earlier design where launch-app.sh set its own pid directly was
        # replaced when the hold-only architecture made `hold` the sole
        # lock-taking path). Releasing the lock dir while that pid is still
        # alive desyncs the lock from reality: the resource stays in use (the
        # `hold` task itself, and for 'app', its separately-tracked Metro
        # process below) but looks FREE to every other session, so someone
        # else can acquire and collide with it. Require --force here too,
        # same as the cross-session override above, so this can't happen by
        # an unnoticed bare `release`.
        local pid reason
        pid="$(meta_get "$dir/info" pid)"
        reason="$(meta_get "$dir/info" reason)"
        if [ -n "$pid" ] && [ "$force" != 1 ] && kill -0 "$pid" 2>/dev/null; then
            echo "[!] '$r' is still actively held by a live process (pid $pid${reason:+, $reason}) -- releasing now would desync this lock from actual hardware usage while that process keeps running. Stop the owning process instead (it releases the lock automatically on exit), or pass --force to release anyway." >&2
            failed=1
            continue
        fi
        # 'app' locks may separately track the actual Metro/expo process via
        # metro_pid (see cmd_note_metro_pid) -- distinct from `pid` above,
        # which is always the `hold` process's own pid. Same-session release
        # (including hold's own self-release trap) always stops it, so
        # releasing the lock reliably means Metro has quit too. Cross-session
        # --force deliberately does NOT kill another session's Metro -- see
        # the reasoning in scripts/hw-lock.sh's plan/PR history: --force has
        # never killed a tracked process for any resource, only overridden
        # lock bookkeeping, and the acquire-time kill_orphaned_metro sweep
        # already cleans up anything left behind the moment anyone next
        # holds 'app', so no real collision window is left open either way.
        local metro_pid
        metro_pid="$(meta_get "$dir/info" metro_pid || true)"
        if [ -n "$metro_pid" ] && kill -0 "$metro_pid" 2>/dev/null; then
            if [ "$sid" = "$HOLDER_ID" ]; then
                log "Stopping Metro (pid $metro_pid) tracked by this session's '$r' hold before releasing."
                kill -TERM "$metro_pid" 2>/dev/null || true
                sleep 2
                kill -0 "$metro_pid" 2>/dev/null && kill -KILL "$metro_pid" 2>/dev/null || true
            else
                echo "[!] '$r' still has a separately-tracked Metro process (pid $metro_pid) running under the now-released session ($sid) -- a cross-session --force release does not kill another session's Metro. It will be cleaned up automatically the next time '$r' is acquired (kill_orphaned_metro), or stop it yourself now: kill -TERM $metro_pid" >&2
            fi
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

# Records the pid of the Metro/expo process actually using 'app', as an
# additional field on that resource's already-held info file. Called by
# launch-app.sh with its own $$ right before it execs into
# `npx expo run:android` (exec keeps the same pid, so this *is* Metro's real
# pid from that point on). Refuses (caller must treat as fatal) unless the
# resource is currently held by this exact session, so a lock released or
# reclaimed in the gap between launch-app.sh's `check` and here can never
# have a stray metro_pid grafted onto whoever holds it next.
cmd_note_metro_pid() {
    [ $# -eq 2 ] || usage_err "note-metro-pid requires exactly two arguments: <resource> <pid>"
    local r="$1" pid="$2"
    validate_resource "$r"
    case "$pid" in ''|*[!0-9]*) usage_err "note-metro-pid requires a numeric pid" ;; esac
    local dir
    dir="$(lock_dir_for "$r")"
    if [ ! -d "$dir" ] || [ "$(meta_get "$dir/info" session_id)" != "$HOLDER_ID" ]; then
        echo "[!] '$r' is not held by this session -- refusing to note metro pid $pid." >&2
        return 1
    fi
    set_meta_field "$r" metro_pid "$pid"
    log "Recorded metro_pid=$pid for '$r'."
}

# Prints the number of other sessions currently queued (via --wait) for
# resource $1. Machine-readable counterpart to the "(N waiting)" suffix
# `status` already prints for humans -- meant for hooks/scripts that want to
# act on the count (e.g. nudging a holder to wrap up) without parsing prose.
cmd_waiters() {
    [ $# -eq 1 ] || usage_err "waiters requires exactly one resource"
    local r="$1"
    validate_resource "$r"
    valid_tickets_for "$r" | grep -c . || true
}

case "${1:-}" in
    hold)           shift; cmd_hold "$@" ;;
    release)        shift; cmd_release "$@" ;;
    status)         shift; cmd_status "$@" ;;
    check)          shift; cmd_check "$@" ;;
    waiters)        shift; cmd_waiters "$@" ;;
    note-metro-pid) shift; cmd_note_metro_pid "$@" ;;
    *) usage_err "usage: hw-lock.sh {hold|release|status|check|waiters|note-metro-pid} ..." ;;
esac
