#!/usr/bin/env python3
"""Drift checker for the agent-facing docs (CLAUDE.md files, .claude/rules, skills, agents).

The agent docs are this project's persistent memory, and they rot in predictable ways: a file
they cite is renamed, a path-scoped rule's glob stops matching anything after a refactor, a
quoted section reference outlives the heading it named, or an always-loaded file quietly grows
back to 100 KB. Each check below catches one of those. See the root CLAUDE.md "Memory policy"
for the layout it enforces.

Usage:
    python3 scripts/check-agent-docs.py            # errors fail, warnings print
    python3 scripts/check-agent-docs.py --strict   # warnings fail too
    python3 scripts/check-agent-docs.py --verbose  # also list each rules glob's match count

Stdlib only (CI runs Python 3.11, where PurePath.full_match does not exist yet).
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --- Budgets --------------------------------------------------------------------------------
# The three CLAUDE.md files are loaded whole (root: every session; fw/app: on the first Read in
# that subtree), so they are held to Claude Code's "under 200 lines" guidance plus a byte cap —
# a line cap alone lets a 200-line file carry 60 KB of 4,000-character lines.
CLAUDE_MD_FILES = ("CLAUDE.md", "fw/CLAUDE.md", "app/CLAUDE.md")
CLAUDE_MD_MAX_LINES = 200
CLAUDE_MD_MAX_BYTES = 16 * 1024
CLAUDE_MD_MAX_LINE_CHARS = 400
RULE_MAX_LINES = 250
RULE_MAX_BYTES = 24 * 1024
# fw-logging.md co-loads with nearly every firmware .cpp Read, so it gets a tighter budget.
RULE_OVERRIDES_MAX_LINES = {".claude/rules/fw-logging.md": 60}
REFERENCE_MAX_LINES = 400
GLOB_MANY_MATCHES = 400

RULES_DIR = ".claude/rules"
SKILLS_DIR = ".claude/skills"
AGENTS_DIR = ".claude/agents"
INCIDENTS = "docs/agent-incidents.md"

# Headings other tooling cites by exact text (hook denial messages, the fw-code-reviewer agent,
# scripts). Renaming one silently breaks its citers, so they are pinned here.
REQUIRED_ANCHORS = {
    "CLAUDE.md": [
        "Memory policy",
        "Session startup",
        "NEVER write unverified commands or data into hardware parts",
        "Hardware locking",
        "Worktree isolation",
        "Git workflow",
        "Process management",
        "Task routing",
    ],
    "fw/CLAUDE.md": [
        "Commenting rules",
        "Coding rules",
        "SYS_INIT ordering for early registration",
        "Known non-blocking build warnings",
    ],
    "app/CLAUDE.md": ["State Updates with Optimistic UI"],
}

# Skills that are deliberately not task-routed (session-start probes).
ROUTING_EXEMPT_SKILLS = {"check-hardware", "check-software"}
# Built-in / harness slash commands an agent doc may name.
BUILTIN_COMMANDS = {
    "mcp", "memory", "context", "config", "hooks", "help", "clear", "compact", "init",
    "review", "agents", "permissions", "status", "model", "fast", "loop", "run", "simplify",
    "code-review", "security-review", "verify",
}

# Path prefixes that are generated, device-side, or outside the repo — never checked.
SKIP_PATH_PREFIXES = (
    "fw/build/", "build/", "fw/twister-out", "twister-out", "app/android/", "app/ios/",
    "node_modules/", "app/node_modules/", ".claude/worktrees/",
)
ALLOW_MISSING_MARK = "agent-docs: allow-missing"

GLOB_ALLOWED = re.compile(r"^[A-Za-z0-9_./*?{},-]+$")


@dataclass
class Finding:
    level: str  # "error" | "warning"
    code: str
    path: str
    line: int
    msg: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: [{self.code}] {self.level}: {self.msg}"


# --- Glob matching ----------------------------------------------------------------------------


def expand_braces(pattern: str) -> list[str]:
    """Expand the first {a,b} group recursively. No nesting support needed here."""
    m = re.search(r"\{([^{}]*)\}", pattern)
    if not m:
        return [pattern]
    out: list[str] = []
    for alt in m.group(1).split(","):
        out.extend(expand_braces(pattern[: m.start()] + alt + pattern[m.end():]))
    return out


def glob_to_regex(pattern: str) -> re.Pattern[str]:
    """gitignore-ish glob: ** crosses directories (and matches zero of them), * and ? do not."""
    i, out = 0, []
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("^" + "".join(out) + "$")


def glob_matches(pattern: str, files: list[str]) -> list[str]:
    regs = [glob_to_regex(p) for p in expand_braces(pattern)]
    return [f for f in files if any(r.match(f) for r in regs)]


# --- Markdown helpers -------------------------------------------------------------------------


def normalize_anchor(text: str) -> str:
    text = text.replace("`", "").replace("*", "").replace("—", "-").replace("–", "-")
    text = re.sub(r"\s+", " ", text).strip().lower()
    # A quote may elide the tail of a long heading ("USB Flash Disk (`/NAND:` ...)").
    for ell in ("...", "…"):
        if ell in text:
            text = text[: text.index(ell)]
    text = text.rstrip(". ")
    # A reference often quotes a heading up to (but not including) its parenthetical.
    if text.count("(") > text.count(")"):
        text = text[: text.rfind("(")].rstrip()
    return text


def github_slug(heading: str) -> str:
    s = heading.strip().lower()
    s = re.sub(r"[^\w\- ]", "", s)
    return s.replace(" ", "-")


def strip_fences(lines: list[str]) -> list[tuple[int, str]]:
    """(1-based line number, text) for lines outside fenced code blocks."""
    out, fenced = [], False
    for n, line in enumerate(lines, 1):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            continue
        if not fenced:
            out.append((n, line))
    return out


HEADING_RE = re.compile(r"^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$")
BOLD_LEAD_RE = re.compile(r"^\s*(?:[-*]|\d+\.)?\s*\*\*(.+?)\*\*")


def anchors_of(text: str) -> tuple[list[str], set[str]]:
    """(normalized headings + bold leads, heading slugs)."""
    anchors, slugs = [], set()
    for _, line in strip_fences(text.splitlines()):
        m = HEADING_RE.match(line)
        if m:
            anchors.append(normalize_anchor(m.group(2)))
            slugs.add(github_slug(m.group(2).replace("`", "")))
            continue
        m = BOLD_LEAD_RE.match(line)
        if m:
            anchors.append(normalize_anchor(m.group(1)))
    return anchors, slugs


def parse_frontmatter(text: str) -> tuple[list[str] | None, str | None]:
    """Restricted form: '---' / 'paths:' / '  - "glob"' ... / '---'. Returns (globs, error)."""
    lines = text.splitlines()
    if not lines or lines[0].strip() != "---":
        return None, "missing YAML frontmatter (a rule without `paths:` loads every session)"
    try:
        end = lines.index("---", 1)
    except ValueError:
        return None, "unterminated frontmatter"
    body = lines[1:end]
    if not body or body[0].strip() != "paths:":
        return None, "frontmatter must start with `paths:`"
    globs = []
    for raw in body[1:]:
        m = re.match(r'^\s+-\s+"([^"]+)"\s*$', raw)
        if not m:
            return None, f'paths entries must be `  - "glob"` (double-quoted), got: {raw!r}'
        globs.append(m.group(1))
    if not globs:
        return None, "empty `paths:` list"
    return globs, None


# --- Repo -------------------------------------------------------------------------------------


@dataclass
class Repo:
    root: Path
    files: list[str] = field(default_factory=list)

    @classmethod
    def from_git(cls, root: Path) -> "Repo":
        out = subprocess.run(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
            cwd=root, check=True, capture_output=True,
        ).stdout.decode()
        files = sorted({f for f in out.split("\0") if f and (root / f).exists()})
        return cls(root, files)

    def read(self, rel: str) -> str:
        return (self.root / rel).read_text(encoding="utf-8", errors="replace")

    def exists(self, rel: str) -> bool:
        rel = rel.rstrip("/")
        if rel in self._fileset:
            return True
        prefix = rel + "/"
        return any(f.startswith(prefix) for f in self.files)

    def __post_init__(self) -> None:
        self._fileset = set(self.files)
        self.top_level = {f.split("/", 1)[0] for f in self.files}

    def agent_docs(self) -> list[str]:
        docs = [f for f in CLAUDE_MD_FILES if f in self._fileset]
        for f in self.files:
            if f.endswith(".md") and (
                f.startswith(RULES_DIR + "/") or f.startswith(SKILLS_DIR + "/")
                or f.startswith(AGENTS_DIR + "/")
            ):
                docs.append(f)
        if INCIDENTS in self._fileset:
            docs.append(INCIDENTS)
        return docs

    def rules(self) -> list[str]:
        return [f for f in self.files if f.startswith(RULES_DIR + "/") and f.endswith(".md")]


# --- Checks -----------------------------------------------------------------------------------


class Checker:
    def __init__(self, repo: Repo, verbose: bool = False):
        self.repo = repo
        self.verbose = verbose
        self.findings: list[Finding] = []
        self._anchor_cache: dict[str, tuple[list[str], set[str]]] = {}

    def err(self, code, path, line, msg):
        self.findings.append(Finding("error", code, path, line, msg))

    def warn(self, code, path, line, msg):
        self.findings.append(Finding("warning", code, path, line, msg))

    def anchors(self, rel: str) -> tuple[list[str], set[str]]:
        if rel not in self._anchor_cache:
            self._anchor_cache[rel] = anchors_of(self.repo.read(rel))
        return self._anchor_cache[rel]

    def run(self) -> list[Finding]:
        self.c1_budgets()
        self.c2_c3_rules()
        self.c4_orphans()
        self.c5_routing()
        self.c6_paths()
        self.c7_section_refs()
        self.c8_required_anchors()
        self.c10_line_refs()
        self.c11_nested_claude()
        return self.findings

    # C1 ---------------------------------------------------------------------------------------
    def c1_budgets(self):
        for rel in CLAUDE_MD_FILES:
            if rel not in self.repo._fileset:
                continue
            self._budget(rel, CLAUDE_MD_MAX_LINES, CLAUDE_MD_MAX_BYTES, CLAUDE_MD_MAX_LINE_CHARS)
        for rel in self.repo.rules():
            self._budget(rel, RULE_OVERRIDES_MAX_LINES.get(rel, RULE_MAX_LINES), RULE_MAX_BYTES, None)
        for rel in self.repo.files:
            if rel.startswith(SKILLS_DIR + "/") and "/references/" in rel and rel.endswith(".md"):
                self._budget(rel, REFERENCE_MAX_LINES, None, None)

    def _budget(self, rel, max_lines, max_bytes, max_line_chars):
        text = self.repo.read(rel)
        lines = text.splitlines()
        if len(lines) > max_lines:
            self.err("C1", rel, 1, f"{len(lines)} lines > budget {max_lines}")
        size = len(text.encode())
        if max_bytes and size > max_bytes:
            self.err("C1", rel, 1, f"{size} bytes > budget {max_bytes}")
        if max_line_chars:
            for n, line in strip_fences(lines):
                if len(line) > max_line_chars:
                    self.err("C1", rel, n, f"line is {len(line)} chars > {max_line_chars}")

    # C2 + C3 ----------------------------------------------------------------------------------
    def c2_c3_rules(self):
        for rel in self.repo.rules():
            globs, error = parse_frontmatter(self.repo.read(rel))
            if error:
                self.err("C2", rel, 1, error)
                continue
            for g in globs:
                if not GLOB_ALLOWED.match(g) or g.startswith(("/", "./")) or ".." in g:
                    self.err("C3", rel, 1, f"glob {g!r} uses characters outside "
                             "[A-Za-z0-9_./*?{},-] or a relative/absolute prefix "
                             "(step over Expo route dirs like (tabs) with **)")
                    continue
                hits = glob_matches(g, self.repo.files)
                if self.verbose:
                    print(f"  {rel}: {g!r} -> {len(hits)} files")
                if not hits:
                    self.err("C3", rel, 1, f"glob {g!r} matches no tracked file "
                             "(renamed code? update the rule's paths)")
                elif len(hits) > GLOB_MANY_MATCHES:
                    self.warn("C3", rel, 1, f"glob {g!r} matches {len(hits)} files (over-broad?)")

    # C4 ---------------------------------------------------------------------------------------
    def c4_orphans(self):
        index_text = "\n".join(self.repo.read(f) for f in CLAUDE_MD_FILES if f in self.repo._fileset)
        for rel in self.repo.rules():
            if rel not in index_text:
                self.err("C4", rel, 1, "rules file is not listed in any CLAUDE.md index "
                         "(an unlisted rule cannot be found by hand or by a subagent)")
        for rel in self.repo.files:
            if rel.startswith(SKILLS_DIR + "/") and "/references/" in rel:
                skill_dir = rel.split("/references/")[0]
                skill_md = skill_dir + "/SKILL.md"
                name = rel.split("/references/")[1]
                if skill_md in self.repo._fileset and name not in self.repo.read(skill_md):
                    self.err("C4", rel, 1, f"reference is not linked from {skill_md}")
        for rel in self.repo.files:
            if rel.startswith(SKILLS_DIR + "/") and rel.endswith("/SKILL.md"):
                dirname = rel.split("/")[2]
                text = self.repo.read(rel)
                m = re.search(r"^name:\s*(\S+)", text, re.M)
                if not m or m.group(1).strip("\"'") != dirname:
                    self.err("C4", rel, 1, f"frontmatter `name` must equal directory {dirname!r}")
                if not re.search(r"^description:\s*\S", text, re.M):
                    self.err("C4", rel, 1, "frontmatter `description` is empty")

    # C5 ---------------------------------------------------------------------------------------
    def skills(self) -> set[str]:
        return {f.split("/")[2] for f in self.repo.files
                if f.startswith(SKILLS_DIR + "/") and f.endswith("/SKILL.md")}

    def c5_routing(self):
        if "CLAUDE.md" not in self.repo._fileset:
            return
        text = self.repo.read("CLAUDE.md")
        m = re.search(r"^## Task routing\s*$(.*?)(?=^## |\Z)", text, re.M | re.S)
        if not m:
            return  # C8 reports the missing heading
        start_line = text[: m.start()].count("\n") + 1
        routed = set(re.findall(r"(?<![\w/.`-])/([a-z][a-z0-9-]+)\b(?![/.\w])", m.group(1)))
        skills = self.skills()
        for s in sorted(skills - routed - ROUTING_EXEMPT_SKILLS):
            self.err("C5", "CLAUDE.md", start_line, f"skill /{s} is missing from the Task routing table")
        for s in sorted(routed - skills - BUILTIN_COMMANDS):
            self.err("C5", "CLAUDE.md", start_line, f"Task routing names /{s}, which is not a skill")

    # C6 ---------------------------------------------------------------------------------------
    SPAN_RE = re.compile(r"`([^`\n]+)`")
    LINK_RE = re.compile(r"\]\(([^)\s]+)\)")

    def c6_paths(self):
        for rel in self.repo.agent_docs():
            for n, line in strip_fences(self.repo.read(rel).splitlines()):
                if ALLOW_MISSING_MARK in line:
                    continue
                cands = self.SPAN_RE.findall(line) + self.LINK_RE.findall(line)
                for cand in cands:
                    path = self._as_repo_path(cand)
                    if path and not self.repo.exists(path):
                        self.err("C6", rel, n, f"`{cand}` does not exist in the repo")

    def _as_repo_path(self, cand: str) -> str | None:
        c = cand.strip()
        if re.search(r"\s|[*?{}<>$~…]|\.\.\.|^\w+://|^/|^#", c):
            return None
        c = re.sub(r"#.*$", "", c)
        c = re.sub(r":\d+(-\d+)?$", "", c)
        c = c.rstrip("/").rstrip(".,;:")
        if "/" not in c and c not in ("CLAUDE.md", "README.md"):
            return None
        first = c.split("/", 1)[0]
        if first not in self.repo.top_level:
            return None
        if any(c.startswith(p.rstrip("/")) for p in SKIP_PATH_PREFIXES):
            return None
        return c

    # C7 ---------------------------------------------------------------------------------------
    DOC_REF_RE = re.compile(
        r"(?P<root>root\s+)?`?(?P<doc>(?:fw/|app/)?CLAUDE\.md|\.claude/(?:rules|skills|agents)/[\w./-]+\.md"
        r"|docs/agent-incidents\.md)`?(?:'s)?,?\s*(?:§\s*)?"
        r"(?:\"(?P<q1>[^\"\n]{3,})\"|“(?P<q2>[^”\n]{3,})”|`#{2,4}\s+(?P<h>[^`\n]+)`|#(?P<slug>[a-z0-9_-]+))"
    )
    # docs/plans/ is dated history; the checker and its tests quote refs as test data.
    C7_SKIP_PREFIXES = ("docs/plans/", "app/node_modules/", "fw/tests/fixtures", "app/patches/",
                        "scripts/check-agent-docs.py", "scripts/tests/test_check_agent_docs.py")

    def c7_section_refs(self):
        for rel in self.repo.files:
            if rel.startswith(self.C7_SKIP_PREFIXES) or rel in (".gitignore",):
                continue
            if not rel.endswith((".md", ".py", ".sh", ".c", ".cpp", ".h", ".ts", ".tsx", ".js",
                                 ".yml", ".yaml", ".conf", ".txt", ".cmake", ".json")) \
                    and os.path.basename(rel) not in ("Kconfig", "CMakeLists.txt"):
                continue
            try:
                text = self.repo.read(rel)
            except OSError:
                continue
            if "CLAUDE.md" not in text and ".claude/" not in text and INCIDENTS not in text:
                continue
            for n, line in enumerate(text.splitlines(), 1):
                for m in self.DOC_REF_RE.finditer(line):
                    self._check_ref(rel, n, m)

    def _resolve_doc(self, referrer: str, doc: str, forced_root: bool) -> str:
        if doc != "CLAUDE.md" or forced_root:
            return doc
        # A bare CLAUDE.md means the nearest ancestor one, as Claude Code would load it.
        parts = referrer.split("/")[:-1]
        while parts:
            cand = "/".join(parts) + "/CLAUDE.md"
            if cand in self.repo._fileset:
                return cand
            parts.pop()
        return "CLAUDE.md"

    def _check_ref(self, rel, n, m):
        target = self._resolve_doc(rel, m.group("doc"), bool(m.group("root")))
        if target not in self.repo._fileset:
            self.err("C7", rel, n, f"references {target}, which does not exist")
            return
        anchors, slugs = self.anchors(target)
        if m.group("slug"):
            if m.group("slug") not in slugs:
                self.err("C7", rel, n, f"{target}#{m.group('slug')}: no heading with that slug")
            return
        quoted = m.group("q1") or m.group("q2") or m.group("h")
        want = normalize_anchor(quoted)
        if not want or any(a.startswith(want) for a in anchors):
            return
        # A quote that merely paraphrases (not a heading) is only suspicious if the text is absent.
        body = normalize_anchor(self.repo.read(target))
        if want in body:
            self.warn("C7", rel, n, f'"{quoted}" is in {target} but is not a heading or bold lead')
        else:
            self.err("C7", rel, n, f'"{quoted}" does not name a section of {target}')

    # C8 ---------------------------------------------------------------------------------------
    def c8_required_anchors(self):
        for doc, required in REQUIRED_ANCHORS.items():
            if doc not in self.repo._fileset:
                continue
            anchors, _ = self.anchors(doc)
            for want in required:
                w = normalize_anchor(want)
                if not any(a.startswith(w) for a in anchors):
                    self.err("C8", doc, 1, f'required heading "{want}" is missing '
                             "(hooks, agents or scripts cite it by name)")

    # C10 --------------------------------------------------------------------------------------
    def c10_line_refs(self):
        rx = re.compile(r"\b[\w./-]+\.(?:c|cpp|h|ts|tsx|js|py|sh):\d+\b")
        for rel in self.repo.agent_docs():
            for n, line in strip_fences(self.repo.read(rel).splitlines()):
                for hit in rx.findall(line):
                    self.warn("C10", rel, n, f"line-number reference `{hit}` rots; cite a grep anchor")

    # C11 --------------------------------------------------------------------------------------
    def c11_nested_claude(self):
        for rel in self.repo.files:
            if "/.claude/" in "/" + rel and not rel.startswith(".claude/"):
                self.err("C11", rel, 1, "nested .claude/ directory — agents are launched from the "
                         "repo root only (root CLAUDE.md \"Session startup\")")
            if os.path.basename(rel) == "CLAUDE.local.md":
                self.err("C11", rel, 1, "CLAUDE.local.md is personal; do not commit it")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--strict", action="store_true", help="warnings fail too")
    ap.add_argument("--verbose", action="store_true", help="print per-glob match counts")
    ap.add_argument("--root", default=None, help="repo root (default: git toplevel)")
    args = ap.parse_args(argv)

    root = Path(args.root) if args.root else Path(subprocess.run(
        ["git", "rev-parse", "--show-toplevel"], check=True, capture_output=True, text=True
    ).stdout.strip())
    findings = Checker(Repo.from_git(root), verbose=args.verbose).run()
    gh = os.environ.get("GITHUB_ACTIONS") == "true"
    for f in findings:
        print(f.render())
        if gh:
            print(f"::{f.level} file={f.path},line={f.line}::[{f.code}] {f.msg}")
    errors = sum(f.level == "error" for f in findings)
    warnings = len(findings) - errors
    print(f"check-agent-docs: {errors} error(s), {warnings} warning(s)")
    return 1 if errors or (args.strict and warnings) else 0


if __name__ == "__main__":
    sys.exit(main())
