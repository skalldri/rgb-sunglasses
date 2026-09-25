# GitHub PR review comments via `gh api`

The traps in posting and replying to PR review comments from an agent session. This is the
single source of truth; `/submit-pr` and `/pr-review-watch` link here. (Cloud sessions
without the `gh` CLI use the GitHub MCP tools instead; the pending-review and pagination
rules apply to those too.)

- While this account has a **pending (draft) review** on a PR, the API rejects ALL new
  review comments from it with 422 "user can only have one pending review per pull
  request" — both `POST /pulls/<n>/reviews` and standalone `POST /pulls/<n>/comments`
  (standalone line comments are single-comment reviews internally). Never touch or submit
  the user's pending review; fall back to regular PR comments (`gh pr comment`) with
  `https://github.com/<owner>/<repo>/blob/<sha>/<path>#L<line>` permalinks, which render
  the referenced snippet inline.
- Once a review is submitted, reply to its line-comment threads with
  `gh api repos/<owner>/<repo>/pulls/<n>/comments/<comment_id>/replies -f body=...`.
- **`-f body=@file` does NOT read the file — it posts the literal string `@file`.**
  `-f`/`--raw-field` is verbatim; only `-F`/`--field` interprets a leading `@` as a file
  path. Writing a long comment to a temp file and passing `-f body=@/tmp/c1.md` (a
  natural-looking move, and how `curl` behaves) silently publishes a comment whose entire
  body is `@/tmp/c1.md` — it happened on PR #377
  (`docs/agent-incidents.md#2026-08-15-gh-api-bodyfile-posted-literal-paths`). Prefer the
  unambiguous form, which works regardless of flag semantics:

  ```bash
  gh api -X POST repos/<owner>/<repo>/pulls/<n>/comments -f body="$(cat /tmp/c1.md)" ...
  ```

  The temp file is still the right way to carry markdown — heredocs and inline quoting
  mangle backticks and `$` in code snippets. It is only the `@` hand-off that is broken.
- **Always verify a posted comment round-trips.** The POST returns 201 with a valid comment
  object either way, so the failure is invisible without an explicit check. After posting,
  re-read the bodies and assert none is a bare `@path`:

  ```bash
  gh api --paginate repos/<owner>/<repo>/pulls/<n>/comments \
    --jq '.[] | select(.body | test("^\\s*@\\S+\\s*$")) | "STALE \(.id) \(.body)"'
  ```

  Repair in place with
  `gh api -X PATCH repos/<owner>/<repo>/pulls/comments/<comment_id> -f body="$(cat …)"` —
  no need to delete and repost, which would lose thread replies.
- Comment listing endpoints **paginate at 30**. A PR with several review rounds silently
  truncates, so a "did my comment land?" check without `--paginate` can report a false
  negative.
