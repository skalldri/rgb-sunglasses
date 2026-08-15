---
name: pr-review-watch
description: "Stand up a session that watches GitHub for new PRs and pushes to open PRs, and automatically code-reviews each one with a parallel agent that posts findings to the PR. Use for unattended/continuous PR review, babysitting a stack of in-flight PRs, or any 'review PRs as they come in' request."
allowed-tools: Bash(scripts/pr-watch.sh *), Bash(gh:*)
---

Continuous PR review: a `Monitor` watches GitHub, and every event spawns a
review agent that posts its findings to the PR. The agents run in parallel, in
isolated worktrees, so a burst of PRs is reviewed concurrently instead of
queueing behind whichever review is in flight.

This skill is the workflow. `scripts/pr-watch.sh` is the watcher; `/code-review`
does the actual reviewing. Neither needs hardware, so **no `hw-lock` is
required** — but see § 6 before letting a review touch the board.

## 1. Arm the watcher

```
Monitor(command: "scripts/pr-watch.sh", description: "new PRs + pushes on <repo>", persistent: true)
```

It baselines on the PRs open right now and then emits one line per event:

```
NEW PR #383 by skalldri [fix-380-fatfs-reentrant]: fw: serialize FatFS access (#380)
UPDATED PR #377 by skalldri [capture-audio-sidecar]: sound: ... (head 57545a20 -> 6ad8b144)
```

Flags: `--repo OWNER/NAME`, `--poll SECONDS` (default 60), `--state-dir DIR`.
`--self-test` exercises the state machine offline — run it after any edit.

**Already-open PRs never fire.** Baselining is what stops the arming of the
monitor from kicking off a review of every open PR at once. If the user wants
the existing ones reviewed, that is a separate, explicit pass — ask, then review
them by number.

## 2. Fan out one agent per event

Do **not** run `/code-review` inline in the main session — a high-effort review
takes minutes, and while it runs every other event queues behind it. Reviews
arriving faster than they are consumed is the normal failure mode here; it was
observed with 5 PRs and ~4 pushes landing inside one review's runtime.

For each event, spawn an agent with `isolation: "worktree"`:

```
Agent(subagent_type: "general-purpose", isolation: "worktree",
      name: "review-<N>", description: "Review PR <N>", prompt: <template below>)
```

`isolation: "worktree"` is **required, not an optimization.** `/code-review`
checks out the PR branch to read it. Two agents doing that in one checkout
corrupt each other's reads and can strand the tree on the wrong branch — which
is exactly what root `CLAUDE.md` § "Worktree isolation" forbids.

### Prompt template

```
Review GitHub PR #<N> in <owner>/<repo> and post the findings to the PR.

1. Invoke the skill: Skill(skill="code-review", args="high <N> --comment").
   Follow it fully — it resolves the target, reviews, and posts inline comments.
2. Verify every comment ROUND-TRIPPED, not just that the API returned 201:
     gh api --paginate repos/<owner>/<repo>/pulls/<N>/comments \
       --jq '.[] | select(.body | test("^\\s*@\\S+\\s*$")) | "STALE \(.id) \(.body)"'
   Any output means a comment body was posted as a literal @path — repair it
   with PATCH (see root CLAUDE.md § "GitHub PR review comments via gh api").
   Note --paginate: these endpoints cut off at 30 comments.
3. If the review found nothing, post a short summary with `gh pr comment <N>`
   so the PR still carries a record of the pass.
<prior-round context — see § 3>

Constraints: do NOT modify the working tree, commit, or push. Do NOT touch
hardware (no serial, adb/execbro, flashing, or hw-locks). Restore any checkout.

Report back: head SHA reviewed, each finding as file:line + one sentence, which
prior findings are now fixed, and confirmation the comments are live.
```

## 3. Re-reviews need the prior round in the prompt

An `UPDATED` event usually means the author pushed a fix **for your last
review**. A fresh agent knows nothing about that and will happily re-report
findings that are already fixed — noisy, and it erodes trust in the reviews.

Always include in the prompt: what you raised last round, what has since been
fixed, and anything deliberately not to re-raise (e.g. a known issue tracked
elsewhere). Ask the agent to state explicitly which prior findings it confirmed
fixed — that is the most useful part of a re-review, and it forces the agent to
actually check rather than assume the commit message.

Ask for confirmation *by inspection*: "the commit says it fixed X" is not
evidence. Verified arithmetic or a re-read of the changed lines is.

## 4. Coalesce; never chase a stale head

Events can outrun reviews. When several are queued:

- Review each PR **once, at its current head**. Skip superseded SHAs — a review
  of a head the author has already replaced is wasted.
- Prefer never-reviewed PRs over re-reviews.
- Snapshot heads before fanning out:
  `gh pr list --state open --json number,headRefOid,title`

## 5. Cost knobs

A high-effort review per push is expensive, and an active author can push every
few minutes. Offer these rather than silently burning the budget:

| Knob | How |
| ---- | --- |
| Fewer reviews per burst | `--poll 600` — a longer cycle is also a longer debounce |
| Cheaper re-reviews | `medium` effort for `UPDATED`, `high` for `NEW` |
| Narrow the scope | watch one label/author; filter in the `gh pr list` query |

## 6. Pitfalls

- **Never key the trigger on `updatedAt`.** It bumps on every comment,
  *including the review comments this workflow posts*, so the watcher
  re-triggers on its own output and loops forever. `headRefOid` only changes
  on a push. The script is already correct; the danger is "improving" it.
- **`-f body=@file` posts the literal string `@file`.** Root `CLAUDE.md`
  § "GitHub PR review comments via `gh api`" has the full trap and the fix.
  It bit this workflow on 2026-08-15 (PR #377, 5 empty comments), which is why
  step 2 of the template is mandatory rather than advisory.
- **Comment listings paginate at 30.** Without `--paginate`, a PR several
  rounds deep reports a false "my comment did not land".
- **A pending draft review blocks all new review comments** with a 422. Never
  touch the user's pending review; fall back to `gh pr comment` with permalinks.
- **A PR opened and closed inside one poll interval is never seen.** Acceptable
  for review purposes; know it before claiming full coverage.
- **Re-arming re-baselines.** Pushes that landed while the monitor was down are
  absorbed silently. After restarting it, diff against the SHAs you actually
  reviewed if that gap matters.
- **Deleting a posted comment is a real, visible action.** If an agent decides
  a finding is out of scope after posting, have it say so — do not let comment
  deletions happen silently.
- **Hardware stays out of it.** Reviews are read-only by construction. If a
  finding genuinely needs on-device confirmation, that is a separate task under
  the `board`/`app` locks (`/flash-and-verify`, `/e2e-test`) — never something
  a review agent does on its own.

## 7. Stopping

`TaskStop` the monitor task. Nothing else to clean up: state lives under
`$TMPDIR`, and agent worktrees are removed automatically when unchanged.
