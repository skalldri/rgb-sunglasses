---
name: release
description: Cut a versioned release of the firmware, the companion app, or both — bump version, tag, push, and attach curated release notes
allowed-tools: Bash(git:*), Bash(gh:*), Read, Write, AskUserQuestion
---

Cut a new release. Releases in this monorepo are **tag-triggered**: the tag is the
authoritative version source — CI injects it at build time, so **no in-repo version
files need editing** for any release track.

| Track | Tag convention | CI workflow | Artifact |
|---|---|---|---|
| Firmware | `fw-vX.Y.Z` | `release.yaml` | `dfu_application_proto0.zip`, `dfu_application_dk.zip` |
| App | `app-vX.Y.Z` | `app-release.yml` | `rgb-sunglasses-<version>.apk` + iOS build on TestFlight + Google Play AAB (track from the `PLAY_TRACK` repo variable, default `internal`) |
| MCUboot bootloader | `mcuboot-vX.Y.Z` | `mcuboot-release.yaml` | `mcuboot-<version>-proto0.bin` |

> **Malformed-tag hazard — always tag a full three-part `X.Y.Z`.** `release.yaml`
> and `mcuboot-release.yaml` trigger on loose globs (`fw-v*`/`mcuboot-v*`): a
> two-part `fw-v1.2` WILL fire and stamp a broken VERSION file (empty
> `PATCHLEVEL`). `app-release.yml` requires strict `app-v[0-9]+.[0-9]+.[0-9]+`,
> so a malformed `app-v1.2` silently triggers nothing — the opposite failure mode.

**Asset filenames and tag prefixes are a cross-component API.** The in-app updater
(`app/services/github-releases.ts`) filters release lists by the
`fw-v`/`app-v`/`mcuboot-v` tag prefixes — never `/releases/latest`, which returns
the newest release of ANY track and broke the update check (PRs #55/#57) — and
matches firmware zips by `proto0`/`dk` substring (`findAssetForBoard`) and the APK
by `.apk` suffix (`findApkAsset`). Renaming assets breaks the in-app updater.

The in-repo `fw/sysbuild/mcuboot/VERSION` is set to `0.0.0` so dev builds always
report a lower version than any official release, prompting users to upgrade.

**iOS build numbers are permanently consumed by TestFlight.** The app release's
`ios-testflight` job derives `ios.buildNumber` from the tag with the same formula
as the Android versionCode (`MAJOR*10000 + MINOR*100 + PATCH`). App Store Connect
rejects any re-upload of an already-used (version, buildNumber) pair — even if
the build was expired or deleted. So once the "Upload to TestFlight" step of a
tag's run has **succeeded**, that tag can never be re-released: fix forward and
bump the patch version. Delete-and-re-push is only safe when the iOS job failed
*before* its upload step. The workflow's `version` job enforces the collision
guard: manual `workflow_dispatch` builds must use 1–9999 and tags must be
≥ 1.0.0 (tag-derived numbers are always ≥ 10000), so the two ranges can't meet.

**Google Play versionCodes are consumed the same way** (every uploaded code is
burned forever, drafts included) — the same tag-derived number is the Android
versionCode, so the identical fix-forward rule applies once the `play` job's
upload step has succeeded. The `version` job additionally enforces MINOR/PATCH
< 100, since the formula collides past that (`1.0.100` = `1.1.0`).

Follow this process exactly. Do not push any tag until the user has approved the
version and the release notes.

---

## 1. Choose what to release

Use `AskUserQuestion` to ask whether this is a **Firmware**, **App**, **MCUboot
Bootloader**, or **Both** (fw+app) release, unless the user already said so.

---

## 2. Pre-flight checks

- Confirm the working tree is clean (`git status -sb`). If not, stop and tell the user.
- `git fetch origin` and confirm local `main` matches `origin/main` (releases are
  cut from the tip of `main`). If they differ, stop and ask.
- Record the commit to tag: `git rev-parse --short HEAD`.

---

## 3. Determine the version bump (per component)

For each component being released:

- Find the latest existing tag (use git's own semver sort — `--sort=-version:refname`):
  - Firmware:  `git tag --list 'fw-v*'       --sort=-version:refname | head -1`
  - App:       `git tag --list 'app-v*'      --sort=-version:refname | head -1`
  - MCUboot:   `git tag --list 'mcuboot-v*'  --sort=-version:refname | head -1`
- Gather the commits that touch the component since the last tag:
  - **If a prior tag exists:**
    - Firmware:  `git log <lastTag>..HEAD --oneline -- fw/`
    - App:       `git log <lastTag>..HEAD --oneline -- app/`
    - MCUboot:   `git log <lastTag>..HEAD --oneline -- fw/sysbuild/mcuboot/ fw/mcuboot_hooks/`
  - **If no prior tag** (first release): version is `1.0.0`; drop the range from the
    log command so it still runs.
- **Suggest** a bump using semver intent inferred from those commits:
  - **major** — breaking changes (MCUboot ABI changes, BLE GATT layout changes that
    break existing pairings, removed characteristics, protocol changes).
  - **minor** — new user-facing features, new characteristics, backward-compatible additions.
  - **patch** — bug fixes, performance, refactors, docs only.
- Ask the user to confirm with `AskUserQuestion`, showing your suggestion first
  (labeled "Recommended") and the computed next version for each option.

---

## 4. Draft curated release notes

Write notes to a scratchpad file, grounded in commits since the last tag. Keep the
structure used for v1.0.0:

- One-paragraph summary.
- Release artifacts list.
- Grouped highlights (Display/animations, Bluetooth/control, Hardware/platform for
  firmware; MCUboot changes for MCUboot; Device control, Firmware updates, Connection,
  Polish for the app).

**App notes must end with a Download section** referencing the QR asset the workflow
generates (the curated notes overwrite the CI body, so the QR must be included) —
use the exact markdown template in `references/app-notes-download.md`.

**App releases additionally need a ≤500-character Google Play "what's new" summary.**
It ships as the **annotated tag message** (see step 6) — the Play upload runs during
CI, before the curated GitHub notes are attached, so the tag message is the only
channel that reaches Play (500 chars is Play's hard limit; CI truncates). A
lightweight (unannotated) app tag falls back to a generic "Bug fixes and
improvements" line. Draft this summary alongside the notes and show both to the user.

Show the drafted notes to the user for review.

---

## 5. Get explicit approval

Pushing tags creates **public, hard-to-reverse** GitHub releases. Show the user the
exact tag(s), the target commit, and the notes, and ask for a clear go-ahead before
proceeding.

---

## 6. Tag and push

For each component (firmware first if releasing both):

```bash
git tag fw-v<version>       <commit> && git push origin fw-v<version>
git tag -a app-v<version>   <commit> -m "<play whats-new summary>" && git push origin app-v<version>
git tag mcuboot-v<version>  <commit> && git push origin mcuboot-v<version>
```

The app tag is **annotated** (`-a -m`): its message becomes the Google Play
"what's new" text (step 4). Firmware/MCUboot tags stay lightweight.

---

## 7. Watch CI, then attach the curated notes

- Find the triggered runs: `gh run list --limit 5`. `gh run list` has no duration
  field — compute elapsed time from `startedAt`/`updatedAt`
  (`gh run list --json workflowName,status,startedAt,updatedAt`).
- Watch each to completion: `gh run watch <id> --exit-status`. Observed durations,
  as of 2026-07 — re-verify: firmware `release.yaml` ~9–13 min, MCUboot
  `mcuboot-release.yaml` ~9 min, app `app-release.yml` ~19–21 min for the Android
  job. The firmware job runs its two pristine NCS builds sequentially (DK first,
  then proto0), so a proto0-only build failure surfaces only after the DK build
  finishes, ~5 min in.
- The app release runs **five** jobs: `test` and `version` (a fast ubuntu job
  that derives the shared version/build number and validates the collision
  guards), then `release` (Android APK, ubuntu), `ios-testflight` (self-hosted
  Mac), and `play` (Google Play AAB, ubuntu) in parallel — no publish job gates
  another. `ios-testflight` shows as *skipped* when the
  `TESTFLIGHT_PUBLISH_ENABLED` repo variable isn't `true` (the intentional
  pause switch), and `play` when `PLAY_PUBLISH_ENABLED` isn't `true` —
  expected before the Play Console bootstrap (see
  `app/docs/play-publishing.md`). Neither skip is a failure. The iOS job may
  sit
  queued behind an in-flight `app-ios-ci.yml` run
  (single Mac runner; the merge to `main` that preceded the tag typically
  triggers one). After the iOS job goes green, Apple keeps "Processing" the
  build for ~5–30 min before it reaches internal testers.
- If a run fails, read its logs (`gh run view <id> --log-failed`), report to the
  user, and **do not** edit release notes. Delete and re-push the tag after a fix:
  `git push --delete origin <tag> && git tag -d <tag>`
  **Exception:** if the app tag's TestFlight upload OR Play upload already
  succeeded, do NOT re-push that tag — both stores permanently consume build
  numbers (see the rules above); bump patch instead. A failed `play` or
  `ios-testflight` job that died *before* its upload step burns nothing and can
  be re-run alone: `gh run rerun <run-id> --job <job-id>`.
- On success, verify assets: `gh release view <tag> --json assets`
- Replace the CI-generated body with curated notes and set the title:

```bash
gh release edit fw-v<version>      --title "Firmware v<version>"           --notes-file <fw-notes>
gh release edit app-v<version>     --title "Companion App v<version>"      --notes-file <app-notes>
gh release edit mcuboot-v<version> --title "MCUboot Bootloader v<version>" --notes-file <mcuboot-notes>
```

---

## 8. Report

Give the user the release URLs (`gh release view <tag> --json url`), the attached
artifacts, and note:
- The app's in-app GitHub-releases auto-update check will match devices against the
  new firmware tag.
- The iOS build reaches TestFlight internal testers automatically once Apple-side
  processing completes (~5–30 min after the `ios-testflight` job finishes).
- For app releases with Play publishing enabled: the AAB landed on the Play track
  named by `PLAY_TRACK` with status from `PLAY_RELEASE_STATUS` (default `draft`,
  which needs manual publishing in the Play Console). Curated GitHub notes do
  **not** propagate to Play — Play shows the annotated tag message.
- The MCUboot release asset `mcuboot-<version>-proto0.bin` is now available for
  manual selection in the app's "Bootloader Update" section.
