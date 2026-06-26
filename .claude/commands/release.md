---
description: Cut a versioned release of the firmware, the companion app, or both — bump version, tag, push, and attach curated release notes
allowed-tools: Bash(git:*), Bash(gh:*), Read, Write, AskUserQuestion
---

Cut a new release. Releases in this monorepo are **tag-triggered**: pushing a
`fw-v<X.Y.Z>` tag runs `.github/workflows/release.yaml` (builds DK + Proto0 DFU
zips), and pushing an `app-v<X.Y.Z>` tag runs `.github/workflows/app-release.yml`
(builds a signed Android APK + download QR). The CI workflows are the source of
truth for the version — they inject it at build time from the tag, so **no
in-repo version files need editing**.

Follow this process exactly. Do not push any tag until the user has approved the
version and the release notes.

## 1. Choose what to release

Use `AskUserQuestion` to ask whether this is a **Firmware**, **App**, or **Both**
release, unless the user already said so in their request.

## 2. Pre-flight checks

- Confirm the working tree is clean (`git status -sb`). If not, stop and tell the user.
- `git fetch origin` and confirm local `main` matches `origin/main` (releases are
  cut from the tip of `main`). If they differ, stop and ask.
- Record the commit to tag: `git rev-parse --short HEAD`.

## 3. Determine the version bump (per component)

For each component being released:

- Find the latest existing tag:
  - Firmware: `git tag --list 'fw-v*' | sort -V | tail -1`
  - App: `git tag --list 'app-v*' | sort -V | tail -1`
  - If none exists, the first release is `1.0.0`.
- Gather the commits since that tag that touch the component's directory:
  - Firmware: `git log <lastTag>..HEAD --oneline -- fw/`
  - App: `git log <lastTag>..HEAD --oneline -- app/`
- **Suggest** a bump using semver intent inferred from those commits:
  - **major** — breaking/incompatible changes (BLE GATT layout changes that break
    older app↔firmware pairings, removed characteristics, protocol/wire-format
    changes, anything a commit flags as breaking).
  - **minor** — new user-facing features or animations, new characteristics,
    backward-compatible additions.
  - **patch** — bug fixes, performance, refactors, docs only.
- Then use `AskUserQuestion` to ask the user to confirm **major / minor / patch**,
  with your suggestion listed first and labeled "(Recommended)". Show the computed
  next version for each option (e.g. patch → 1.0.1, minor → 1.1.0, major → 2.0.0)
  and one line of reasoning for the suggestion.

## 4. Draft curated release notes

Write the notes to a scratchpad file per component, grounded in the commits since
the last tag (not invented). Keep the structure used for v1.0.0:

- A one-paragraph summary line.
- The release artifacts (firmware: `dfu_application_proto0.zip`,
  `dfu_application_dk.zip`; app: `rgb-sunglasses-<version>.apk`).
- Grouped highlights (Display/animations, Bluetooth/control, Hardware/platform for
  firmware; Device control, Firmware updates, Connection, Polish for the app).

**App notes must end with a Download section** that references the QR asset the
workflow generates, so the curated notes keep the QR when they overwrite the
CI-generated body in step 7. The QR and APK have predictable asset URLs:

```markdown
## 📱 Install on Android

Scan to download the APK:

![Download QR code](https://github.com/skalldri/rgb-sunglasses/releases/download/app-v<version>/rgb-sunglasses-<version>-qr.png)

[Direct download: rgb-sunglasses-<version>.apk](https://github.com/skalldri/rgb-sunglasses/releases/download/app-v<version>/rgb-sunglasses-<version>.apk)
```

Show the drafted notes to the user for review.

## 5. Get explicit approval

Pushing tags creates **public, hard-to-reverse** GitHub releases. Show the user the
exact tag(s), the target commit, and the notes, and ask for a clear go-ahead before
proceeding.

## 6. Tag and push

For each component (firmware first if releasing both):

```bash
git tag fw-v<version>  <commit> && git push origin fw-v<version>
git tag app-v<version> <commit> && git push origin app-v<version>
```

## 7. Watch CI, then attach the curated notes

- Find the runs: `gh run list --limit 5`. Watch each to completion:
  `gh run watch <id> --exit-status` (firmware is the slow one — two pristine NCS
  builds, ~15-25 min; run the watch in the background).
- If a run fails, read its logs (`gh run view <id> --log-failed`), report to the
  user, and do **not** proceed to edit notes. The tag can be deleted and re-pushed
  after a fix: `git push --delete origin <tag> && git tag -d <tag>`.
- On success, verify the release assets exist
  (`gh release view <tag> --json assets`), then replace the CI-generated body with
  the curated notes and set a title:

```bash
gh release edit fw-v<version>  --title "Firmware v<version>"      --notes-file <fw-notes>
gh release edit app-v<version> --title "Companion App v<version>" --notes-file <app-notes>
```

## 8. Report

Give the user the release URLs (`gh release view <tag> --json url`), the attached
artifacts, and note that the app's in-app GitHub-releases auto-update check will now
match devices against the new firmware tag.
