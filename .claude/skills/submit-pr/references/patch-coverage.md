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

# 1. Added executable lines per changed C/C++ file, from the diff vs main.
#    Headers INCLUDED: inline/template code in a .h compiled into a test app is
#    lcov-instrumented, and codecov/patch counts it (issue #245 / PR #244).
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
        if cur and cur.endswith((".c", ".cpp", ".h", ".hpp")):
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
#
#    A changed file with NO SF: record at all is a file no native_sim suite
#    compiles (audio_config.cpp, sound.cpp, the BT adapters — issue #83). lcov
#    cannot see any of its lines, so silently dropping them from the
#    denominator overstates the result. Those files are listed separately
#    with a rough count of their code-looking added lines; the printed
#    percentage covers only what lcov measured, and the summary says so.
seen = set()
for l in open(info, encoding="utf-8", errors="replace"):
    if l.startswith("SF:"):
        p = l[3:].strip(); m = next((a for a in added if p.endswith(a)), None)
        if m: seen.add(m)
tot = cov = 0; unmeasured = {}
for path, lns in sorted(added.items()):
    if path not in seen:
        src = open(path, encoding="utf-8", errors="replace").read().splitlines()
        code = [ln for ln in sorted(lns) if ln - 1 < len(src)
                and src[ln - 1].strip() not in ("", "{", "}", "};")
                and not src[ln - 1].strip().startswith(("//", "/*", "*", "#include"))]
        unmeasured[path] = len(code)
        continue
    h = hits.get(path, {})
    exe = [ln for ln in lns if ln in h]
    c = sum(1 for ln in exe if h[ln] > 0)
    tot += len(exe); cov += c
    miss = sorted(ln for ln in exe if h[ln] == 0)
    print(f"{path}: {c}/{len(exe)} added lines covered"
          + (f"  MISSING: {miss}" if miss else ""))
for path, n in sorted(unmeasured.items()):
    print(f"{path}: UNMEASURED — no lcov record (not compiled by any native_sim "
          f"suite, or declarations only); ~{n} code-looking added lines; issue #83")
pct = 100 * cov / tot if tot else 0.0
print(f"\nPATCH COVERAGE: {cov}/{tot} = {pct:.1f}% of the lines lcov could see"
      + (f"; {sum(unmeasured.values())} added lines in {len(unmeasured)} "
         f"uncompiled file(s) are NOT in that figure" if unmeasured else ""))
sys.exit(0 if (tot == 0 or pct > 70.0) else 1)
PY
```

Report the result in the PR body the way the script prints it: "N/M = X% of
the lines lcov could see; <files> unmeasured (#83)" — never a bare "100%" when
the UNMEASURED line printed anything. Those additions still need on-device
verification, and the body should say what covered them.

The script prints per-file coverage (naming the exact uncovered line numbers so
you know what to test), a `PATCH COVERAGE: <cov>/<tot> = <pct>%` summary line,
and **exits non-zero when the percentage is ≤ 70%** — so `/submit-pr` step 4 can
gate on its exit status directly.

Notes:

- **`tot == 0` (no executable added lines)** — the change is comments,
  declarations, or non-`fw/src` files only. The script passes (exit 0); there is
  nothing to cover. Do NOT read "it's only a header" as tot==0 territory:
  inline/template code in a `.h` that any test app compiles IS instrumented and
  counted — by this script and by Codecov alike (issue #245). (A change that
  adds a whole new *file* never compiled into a test still shows up here as its
  executable lines all MISSING — that is the 0%-coverage case, and it fails.)
- The uncovered line numbers it prints are the actionable output: add tests that
  exercise those branches (`/add-fw-test`), re-run step 2, re-run this.
- Waiving the gate needs explicit user approval **and** a follow-up issue, both
  recorded in the PR body (SKILL.md step 4; precedent: PR #82 / issue #83) —
  never waive silently.
