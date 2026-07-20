# Patch-coverage extraction pipeline

Referenced from `/submit-pr` step 4. Computes **changed-line** (patch) coverage:
of the C/C++ **lines actually added/modified on this branch**, what fraction is
executed by the Twister suite. This is the same metric Codecov's `codecov/patch`
check enforces — and the gate to match is **> 70%**.

**Do not** measure whole-file coverage (`lcov --extract <file>` + `lcov
--summary`). That reports the coverage of *every* line in the changed files,
including untouched old code, so a small well-tested change to a large
already-covered file reads as ~90% even when the *new* lines are barely tested.
That false pass is exactly what this pipeline replaced (a PR measured 87.7%
whole-file while Codecov's patch coverage was 40.7% and failing).

Run from the repo root, after step 2 produced `fw/twister-out/coverage.info`:

```bash
python3 - fw/twister-out/coverage.info <<'PY'
import re, subprocess, sys, collections
info = sys.argv[1]

# 1. Added executable lines per changed C/C++ source file, from the diff vs main.
diff = subprocess.run(
    ["git", "diff", "--unified=0", "origin/main...HEAD", "--", "fw/src"],
    capture_output=True, text=True).stdout
added = collections.defaultdict(set)   # path -> {added line numbers}
cur = None; newln = 0
for l in diff.splitlines():
    if l.startswith("+++ b/"):
        cur = l[6:]
    elif l.startswith("@@"):
        newln = int(re.search(r'\+(\d+)', l).group(1))
    elif l.startswith("+") and not l.startswith("+++"):
        if cur and cur.endswith((".c", ".cpp")):
            added[cur].add(newln)
        newln += 1
    elif not l.startswith("-"):
        newln += 1

# 2. lcov per-line hit counts (DA:<line>,<hits>) for those files.
hits = {}
cur = None
for l in open(info, encoding="utf-8", errors="replace"):
    l = l.strip()
    if l.startswith("SF:"):
        p = l[3:]; cur = next((a for a in added if p.endswith(a)), None)
    elif l.startswith("DA:") and cur is not None:
        ln, h = l[3:].split(",")[:2]
        hits.setdefault(cur, {})[int(ln)] = int(h)

# 3. Patch coverage = covered / total, over added lines that are executable
#    (an added line only counts if lcov emitted a DA record for it — comments,
#    braces and declarations have none and are correctly ignored).
tot = cov = 0
for path, lns in sorted(added.items()):
    h = hits.get(path, {})
    exe = [ln for ln in lns if ln in h]
    c = sum(1 for ln in exe if h[ln] > 0)
    tot += len(exe); cov += c
    miss = sorted(ln for ln in exe if h[ln] == 0)
    print(f"{path}: {c}/{len(exe)} added lines covered"
          + (f"  MISSING: {miss}" if miss else ""))
pct = 100 * cov / tot if tot else 0.0
print(f"\nPATCH COVERAGE: {cov}/{tot} = {pct:.1f}%")
sys.exit(0 if (tot == 0 or pct > 70.0) else 1)
PY
```

The script prints per-file coverage (naming the exact uncovered line numbers so
you know what to test), a `PATCH COVERAGE: <cov>/<tot> = <pct>%` summary line,
and **exits non-zero when the percentage is ≤ 70%** — so `/submit-pr` step 4 can
gate on its exit status directly.

Notes:

- **`tot == 0` (no executable added lines)** — the change is comments, headers,
  declarations, or non-`fw/src` files only. The script passes (exit 0); there is
  nothing to cover. (A change that adds a whole new *file* never compiled into a
  test still shows up here as its executable lines all MISSING — that is the
  0%-coverage case, and it fails.)
- The uncovered line numbers it prints are the actionable output: add tests that
  exercise those branches (`/add-fw-test`), re-run step 2, re-run this.
- Waiving the gate needs explicit user approval **and** a follow-up issue, both
  recorded in the PR body (SKILL.md step 4; precedent: PR #82 / issue #83) —
  never waive silently.
