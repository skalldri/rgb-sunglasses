---
name: pr-review-watch
description: "Stand up a session that watches GitHub for new PRs and pushes to open PRs, and automatically code-reviews each one with a parallel agent that posts findings to the PR. Use for unattended/continuous PR review, babysitting a stack of in-flight PRs, or any 'review PRs as they come in' request."
allowed-tools: Monitor, Agent, Skill, TaskStop, Bash(scripts/pr-watch.sh *), Bash(gh:*)
---

Continuous PR review: a `Monitor` watches GitHub, and every event spawns a
review agent that posts its findings to the PR. The agents run in parallel, in
isolated worktrees, so a burst of PRs is reviewed concurrently instead of
queueing behind whichever review is in flight.

This skill is the workflow. `scripts/pr-watch.sh` is the watcher; `/code-review`
does the actual reviewing. Neither drives hardware.

**That does not mean you can skip the hardware locks.** The `PreToolUse` guard
matches on command *text*, so a read-only command that merely contains `adb`,
`mcumgr`, `JLinkExe`, etc. is denied without the lock — grepping a hook file or
a doc that mentions them is enough to trip it (root `CLAUDE.md` § "Hardware
locking" documents this; it was hit while reviewing this very skill). Reviews of
firmware PRs routinely read such files. Either avoid the tokens (Read/Grep tools
rather than shell), or take the lock deliberately — do not tell an agent that no
lock is ever needed.

## 1. Arm the watcher

```
Monitor(command: "scripts/pr-watch.sh", description: "new PRs + pushes on <repo>", persistent: true)
# default poll/debounce is 15 min; use --poll 60 only if you want every push separately
```

It baselines on the PRs open right now and then emits one line per event:

```
NEW PR #383 by skalldri [fix-380-fatfs-reentrant]: fw: serialize FatFS access (#380)
UPDATED PR #377 by skalldri [capture-audio-sidecar]: sound: ... (head 57545a20 -> 6ad8b144)
```

Flags: `--repo OWNER/NAME`, `--poll SECONDS` (default 900 = 15 min; the cycle IS the debounce), `--state-dir DIR`,
`--limit N` (default 100 open PRs; it warns rather than truncating silently),
`--skip-drafts` (defer drafts, then fire `READY` when one is marked ready for
review). `--self-test` exercises the state machine offline — run it after any
edit; it covers the burst, revert, open-then-fixup, draft→ready, and
closed-then-reopened cases.

One watcher per repo: the state files are read-modify-written non-atomically,
so a second instance would interleave and lose events. The script takes an
atomic `mkdir` lock on its state dir and reclaims it from a dead pid.

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
      name: "review-<N>-r<round>", description: "Review PR <N>", prompt: <template below>)
```

Include the round in the name. A bare `review-<N>` collides the second time the
same PR is reviewed, and the newer agent takes the name — which silently
redirects any follow-up `SendMessage` (e.g. resuming an interrupted review)
to the wrong one.

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
   To POST, prefer `gh pr comment --body-file` or `gh api --input <json>`. The
   `-f body="$(cat ...)"` form is correct but is sometimes refused by the
   PreToolUse guard, and `-f body=@file` posts the literal string.
3. If the review found nothing, post a short summary with `gh pr comment <N>`
   so the PR still carries a record of the pass.
4. Every agent AND the PR author post under the SAME GitHub account, so `user`
   identifies nobody. Before treating a comment as a duplicate of your own,
   check `in_reply_to_id` — if it is set, it is a reply to an earlier finding,
   usually the author's. Delete nothing without that check.
5. If this push adds or changes any test, state explicitly whether that test
   would FAIL if the behaviour it claims to pin were reverted, and how you
   determined it. Do not accept "a test was added" as coverage.
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

## 4. Review a stacked pair as a unit

When PR B is based on PR A rather than `main` (this repo stacks routinely —
#381 on #378, #383 on #382), two things go wrong if you review them separately:

- **Diff the merge base, not `main`.** A base that is itself being pushed to
  makes an agent report the *base's* changes as the PR's. Tell the agent the
  base branch explicitly and have it report which merge base it used.
- **A defect fixed by the sibling gets re-reported every round.** #378 carries a
  render/display phase slip that only the stacked #381 fixes; two independent
  agents found it, posted it, and had to delete their own comments. State the
  sibling and the issue number in the prompt.

The higher-value review is often of the *pair*: #382 adds a diagnostic behind a
`K_FOREVER` lock, #383 serializes FatFS on top of it, and only together do they
make that diagnostic unrunnable during the fault it diagnoses. Neither PR shows
that alone.

## 5. Triage before you spawn — most pushes do not need a review

A high-effort review costs minutes and real tokens. Three cheap local checks
decide it, and in one session they correctly skipped roughly half the events:

**Is it a rebase?** On a stacked PR, compare the PR-only diff at both heads.
Identical patch-id means this PR's own content did not move and the previous
review still stands — post a short note saying so instead of re-reviewing.

```bash
mb=$(git merge-base <base-pr-head> <this-pr-head>)
git diff "$mb" <this-pr-head> | git patch-id --stable
```

This fires constantly on stacked branches, and the file list will not tell you:
a rebase shows the *base's* changed files, so a pure rebase can look like a
21-file, +907/−64 change. Reviewing that attributes the sibling's work to this
PR — confident, detailed, and entirely wrong.

**Did anything relevant change?** `gh api repos/O/R/compare/<old>...<new>` with
a file list answers "does this push touch the thing the open finding is about?"
A push that adds 20 lines of docstring to the test you said cannot work has not
changed the answer.

**Is a review already in flight?** If the delta since the in-flight head is
small and touches no test, let it finish and check the delta against its
conclusions afterwards. Restarting means two agents reviewing overlapping
states and reporting contradictory "fixed / not fixed" verdicts.

Otherwise: review each PR **once, at its current head**, skip superseded SHAs,
and prefer never-reviewed PRs over re-reviews. Snapshot heads before fanning
out with `gh pr list --state open --json number,headRefOid,title`.

## 6. Know when to stop

The loop does not self-terminate, and an author fixing findings as fast as they
arrive will keep it running indefinitely. Stop a PR's reviews when:

- **The finding stops changing.** If three consecutive rounds re-derive the same
  structural conclusion, further rounds cost a full review to learn nothing.
  Say what would change your mind — "review again when the test can observe a
  return value" — and stop until that lands. One PR reached round 14 with the
  same finding open since round 8; rounds 9–13 each re-proved it.
- **You are reviewing your own work.** One or two adversarial passes on a PR
  this session authored is genuinely valuable — in one session they found two
  HIGH defects the author had missed, including a review-storm bug. Past that
  it degenerates into reviewing fixes to findings from reviews of fixes. Cap it
  at two.
- **Each round's fixes reliably introduce the next round's findings.** That is a
  signal the code lacks the tests to be changed safely, not that the reviews are
  working. The useful output becomes "add a test of this shape", not finding
  number 47.

Say the cost out loud when you notice it. The user cannot see how many agents
are running or what they are re-deriving.

## 7. Cost knobs

A high-effort review per push is expensive, and an active author can push every
few minutes. Offer these rather than silently burning the budget:

| Knob | How |
| ---- | --- |
| Fewer reviews per burst | raise `--poll` — the cycle IS the debounce; 900 is the default for this reason |
| Cheaper re-reviews | `medium` effort for `UPDATED`, `high` for `NEW` |
| Narrow the scope | watch one label/author; filter in the `gh pr list` query |

## 8. Pitfalls

- **Never key the trigger on `updatedAt`.** It bumps on every comment,
  *including the review comments this workflow posts*, so the watcher
  re-triggers on its own output and loops forever. `headRefOid` only changes
  when the branch moves. The danger is "improving" this back.
- **`headRefOid` is not a perfect proxy for author intent**, and the script
  accepts that. An "Update branch" / merge-queue sync moves the SHA with no
  authored change (one wasted review); a base-branch retarget changes the whole
  diff with no SHA move (one missed review). Watch for retargets by hand on a
  stacked series; neither case justifies the `updatedAt` loop.
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
  deletions happen silently. And never instruct a delete unconditionally: say
  "if X, delete it", because your premise about what a comment is may be wrong.
  Twice in one session a comment that looked like a duplicated finding was the
  author's reply; only the conditional phrasing stopped it being destroyed.
- **`isolation: "worktree"` does not survive a resume.** An agent resumed with
  `SendMessage` after dying can come back in the PARENT checkout rather than
  its own worktree. That makes the prompt's "do not modify the working tree,
  restore any checkout" constraint load-bearing rather than belt-and-braces.
  Tell agents that if they find themselves on someone else's branch they
  should LEAVE IT — a well-meaning "restore" yanks an active branch out from
  under whoever is working there.
- **Ask for the test-revert verdict every round** (step 5 of the template).
  Across one session it found four tests that accompanied a fix and would have
  passed with that fix reverted — a soak gate, a suite whose subject was not
  compiled into the binary, vacuous counter asserts, and a clamp test whose
  assertion could not distinguish 1 step from 3000. In the last case the blind
  spot demonstrably let a real bug survive several rounds. It is the highest
  yield question in the template.
- **Hardware stays out of it.** Reviews are read-only by construction. If a
  finding genuinely needs on-device confirmation, that is a separate task under
  the `board`/`app` locks (`/flash-and-verify`, `/e2e-test`) — never something
  a review agent does on its own.

## 9. Stopping

`TaskStop` the monitor task. Nothing else to clean up: state lives under
`$TMPDIR`, and agent worktrees are removed automatically when unchanged.
