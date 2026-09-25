"""
Tests for scripts/check-agent-docs.py, the drift checker for CLAUDE.md files, path-scoped
.claude/rules, skills and agents. Most tests build a synthetic Repo from an in-memory file
map, so each check can be exercised in isolation; the last test runs the checker against the
real repository, which is the property CI actually cares about.

Run:  pytest scripts/tests/
"""
import importlib.util
import os
import sys
from pathlib import Path

import pytest

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", ".."))

# Hyphenated file name -> load by path. Registering in sys.modules first is required: the
# module defines dataclasses, which look their module up while being created.
_spec = importlib.util.spec_from_file_location(
    "check_agent_docs", os.path.join(_HERE, "..", "check-agent-docs.py"))
cad = importlib.util.module_from_spec(_spec)
sys.modules["check_agent_docs"] = cad
_spec.loader.exec_module(cad)


def make_repo(tmp_path: Path, files: dict[str, str]):
    for rel, text in files.items():
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    return cad.Repo(tmp_path, sorted(files))


def codes(repo, level="error"):
    return sorted({f.code for f in cad.Checker(repo).run() if f.level == level})


def findings(repo, code):
    return [f for f in cad.Checker(repo).run() if f.code == code]


ROOT_OK = """# Root
## Memory policy
Rules live in `.claude/rules/fw-x.md`.
## Session startup
## NEVER write unverified commands or data into hardware parts
## Hardware locking
## Worktree isolation
## Git workflow
## Process management
## Task routing
| Task | Skill |
|---|---|
| Do a thing | /do-thing |
"""
SKILL = "---\nname: do-thing\ndescription: does a thing\n---\nBody.\n"
RULE = '---\npaths:\n  - "fw/src/**/*.cpp"\n---\n# X rule\n'


def base_files(**extra):
    files = {
        "CLAUDE.md": ROOT_OK,
        ".claude/skills/do-thing/SKILL.md": SKILL,
        ".claude/rules/fw-x.md": RULE,
        "fw/src/a/b.cpp": "int x;\n",
    }
    files.update(extra)
    return files


# --- glob semantics ----------------------------------------------------------------------------


@pytest.mark.parametrize("pattern,path,expected", [
    ("fw/**/Kconfig", "fw/Kconfig", True),          # ** matches zero directories
    ("fw/**/Kconfig", "fw/a/b/Kconfig", True),
    ("fw/*.conf", "fw/a/prj.conf", False),          # * does not cross /
    ("fw/src/power.*", "fw/src/power.cpp", True),
    ("fw/src/extension_{bt,mgmt}.cpp", "fw/src/extension_mgmt.cpp", True),
    ("fw/src/extension_{bt,mgmt}.cpp", "fw/src/extension_host.cpp", False),
    ("app/app/**/bluetooth.tsx", "app/app/(tabs)/bluetooth.tsx", True),
    ("fw/src/led_?.h", "fw/src/led_a.h", True),
])
def test_glob_semantics(pattern, path, expected):
    assert bool(cad.glob_matches(pattern, [path])) is expected


def test_clean_synthetic_repo_passes(tmp_path):
    assert codes(make_repo(tmp_path, base_files())) == []


# --- C1 budgets --------------------------------------------------------------------------------


def test_claude_md_line_budget(tmp_path):
    big = ROOT_OK + "x\n" * 300
    assert "C1" in codes(make_repo(tmp_path, base_files(**{"CLAUDE.md": big})))


def test_claude_md_long_line_but_not_in_code_fence(tmp_path):
    long_in_fence = ROOT_OK + "```\n" + "y" * 500 + "\n```\n"
    assert "C1" not in codes(make_repo(tmp_path, base_files(**{"CLAUDE.md": long_in_fence})))
    long_prose = ROOT_OK + "y" * 500 + "\n"
    assert "C1" in codes(make_repo(tmp_path, base_files(**{"CLAUDE.md": long_prose})))


# --- C2 / C3 rules frontmatter and globs ---------------------------------------------------------


@pytest.mark.parametrize("text", [
    "# no frontmatter\n",
    "---\npaths:\n---\n",                            # empty list
    "---\npaths:\n  - fw/src/**/*.cpp\n---\n",       # unquoted
    "---\nglobs:\n  - \"fw/**\"\n---\n",             # wrong key
])
def test_bad_frontmatter(tmp_path, text):
    assert "C2" in codes(make_repo(tmp_path, base_files(**{".claude/rules/fw-x.md": text})))


def test_glob_matching_nothing_is_an_error(tmp_path):
    rule = '---\npaths:\n  - "fw/src/renamed.cpp"\n---\n'
    assert "C3" in codes(make_repo(tmp_path, base_files(**{".claude/rules/fw-x.md": rule})))


@pytest.mark.parametrize("glob", ["app/app/(tabs)/x.tsx", "app/[id].tsx", "./fw/**", "/fw/**"])
def test_hazardous_glob_characters_rejected(tmp_path, glob):
    rule = f'---\npaths:\n  - "{glob}"\n---\n'
    assert "C3" in codes(make_repo(tmp_path, base_files(**{".claude/rules/fw-x.md": rule})))


# --- C4 orphans ----------------------------------------------------------------------------------


def test_rule_not_indexed_is_orphan(tmp_path):
    files = base_files(**{".claude/rules/fw-y.md": RULE})
    assert any(f.path == ".claude/rules/fw-y.md" for f in findings(make_repo(tmp_path, files), "C4"))


def test_reference_must_be_linked_from_skill(tmp_path):
    files = base_files(**{".claude/skills/do-thing/references/extra.md": "x\n"})
    assert "C4" in codes(make_repo(tmp_path, files))
    files[".claude/skills/do-thing/SKILL.md"] = SKILL + "See references/extra.md.\n"
    assert "C4" not in codes(make_repo(tmp_path, files))


def test_skill_name_must_match_dir(tmp_path):
    files = base_files(**{".claude/skills/do-thing/SKILL.md": SKILL.replace("do-thing", "other")})
    assert "C4" in codes(make_repo(tmp_path, files))


# --- C5 routing ----------------------------------------------------------------------------------


def test_unrouted_skill(tmp_path):
    files = base_files(**{".claude/skills/new-one/SKILL.md": SKILL.replace("do-thing", "new-one")})
    assert "C5" in codes(make_repo(tmp_path, files))


def test_routing_names_missing_skill(tmp_path):
    files = base_files(**{"CLAUDE.md": ROOT_OK + "| Ghost | /ghost-skill |\n"})
    assert "C5" in codes(make_repo(tmp_path, files))


# --- C6 paths ------------------------------------------------------------------------------------


def test_missing_backticked_path(tmp_path):
    files = base_files(**{"CLAUDE.md": ROOT_OK + "See `fw/src/gone.cpp`.\n"})
    assert "C6" in codes(make_repo(tmp_path, files))


@pytest.mark.parametrize("span", [
    "fw/build/fw/zephyr/autoconf.h",   # build output
    "fw/src/*.cpp",                    # glob
    "fw/<board>.conf",                 # placeholder
    "/dev/ttyACM0",                    # absolute
    "src/relative.cpp",                # not repo-rooted
    "fw/src/a/b.cpp:12",               # line suffix on an existing file
    "https://example.com/fw/x",        # URL
])
def test_path_skip_classes(tmp_path, span):
    files = base_files(**{"CLAUDE.md": ROOT_OK + f"See `{span}`.\n"})
    assert "C6" not in codes(make_repo(tmp_path, files))


def test_allow_missing_marker(tmp_path):
    line = "Writes `fw/sim/out.json`. <!-- agent-docs: allow-missing -->\n"
    assert "C6" not in codes(make_repo(tmp_path, base_files(**{"CLAUDE.md": ROOT_OK + line})))


# --- C7 section references -----------------------------------------------------------------------


def test_section_ref_resolves_by_prefix(tmp_path):
    files = base_files(**{"fw/src/a/b.cpp": '// see root CLAUDE.md "Hardware lock"\n'})
    assert "C7" not in codes(make_repo(tmp_path, files))


def test_section_ref_to_bold_lead(tmp_path):
    fw = "# fw\n## Coding rules\n- **Never do flash I/O from a cooperative thread** — ...\n"
    files = base_files(**{
        "fw/CLAUDE.md": fw + "## Commenting rules\n## SYS_INIT ordering for early registration\n"
                              "## Known non-blocking build warnings\n",
        "fw/src/a/b.cpp": '// fw/CLAUDE.md "Never do flash I/O"\n',
    })
    assert "C7" not in codes(make_repo(tmp_path, files))


def test_broken_section_ref(tmp_path):
    files = base_files(**{"fw/src/a/b.cpp": '// see root CLAUDE.md "No such section"\n'})
    assert "C7" in codes(make_repo(tmp_path, files))


def test_bare_claude_md_resolves_to_nearest_ancestor(tmp_path):
    app = "# app\n## State Updates with Optimistic UI\n## Launching\n"
    files = base_files(**{
        "app/CLAUDE.md": app,
        "app/hooks/x.ts": '// CLAUDE.md "Launching"\n',   # would fail against root
    })
    assert "C7" not in codes(make_repo(tmp_path, files))


def test_heading_slug_ref(tmp_path):
    files = base_files(**{
        "docs/agent-incidents.md": "# Incidents\n## 2026-07-05 TPS25750 GO2P wedge\n",
        "fw/src/a/b.cpp": "// docs/agent-incidents.md#2026-07-05-tps25750-go2p-wedge\n",
    })
    assert "C7" not in codes(make_repo(tmp_path, files))
    files["fw/src/a/b.cpp"] = "// docs/agent-incidents.md#nope\n"
    assert "C7" in codes(make_repo(tmp_path, files))


def test_quoted_ellipsis_ref(tmp_path):
    root = ROOT_OK + "## USB Flash Disk (`/NAND:` — GLIM/animation assets)\n"
    files = base_files(**{"CLAUDE.md": root,
                          "fw/src/a/b.cpp": '// CLAUDE.md "USB Flash Disk (`/NAND:` ...)"\n'})
    assert "C7" not in codes(make_repo(tmp_path, files))


# --- C8 / C11 ------------------------------------------------------------------------------------


def test_required_anchor_missing(tmp_path):
    files = base_files(**{"CLAUDE.md": ROOT_OK.replace("## Git workflow\n", "")})
    assert "C8" in codes(make_repo(tmp_path, files))


def test_nested_claude_dir_rejected(tmp_path):
    files = base_files(**{"fw/.claude/settings.json": "{}\n"})
    assert "C11" in codes(make_repo(tmp_path, files))
    files = base_files(**{"app/CLAUDE.local.md": "x\n"})
    assert "C11" in codes(make_repo(tmp_path, files))


# --- the real repository -------------------------------------------------------------------------


def test_real_repo_has_no_errors():
    repo = cad.Repo.from_git(Path(_REPO))
    errors = [f.render() for f in cad.Checker(repo).run() if f.level == "error"]
    assert errors == [], "\n".join(errors)
