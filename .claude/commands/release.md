---
description: Cut a versioned release of the firmware, the companion app, or both — bump version, tag, push, and attach curated release notes
allowed-tools: Bash(git:*), Bash(gh:*), Read, Write, AskUserQuestion
---

Cut a new release. Releases in this monorepo are **tag-triggered**: the tag is the
authoritative version source — CI injects it at build time, so **no in-repo version
files need editing** for any release track.

| Track | Tag convention | CI workflow | Artifact |
|---|---|---|---|
| Firmware | `fw-vX.Y.Z` | `release.yaml` | `dfu_application_proto0.zip`, `dfu_application_dk.zip` |
| App | `app-vX.Y.Z` | `app-release.yml` | `rgb-sunglasses-<version>.apk` |
| MCUboot bootloader | `mcuboot-vX.Y.Z` | `mcuboot-release.yaml` | `mcuboot-<version>-proto0.bin` |

The in-repo `fw/sysbuild/mcuboot/VERSION` is set to `0.0.0` so dev builds always
report a lower version than any official release, prompting users to upgrade.

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
generates (the curated notes overwrite the CI body, so the QR must be included):

```markdown
## 📱 Install on Android

Scan to download the APK:

![Download QR code](https://github.com/skalldri/rgb-sunglasses/releases/download/app-v<version>/rgb-sunglasses-<version>-qr.png)

[Direct download: rgb-sunglasses-<version>.apk](https://github.com/skalldri/rgb-sunglasses/releases/download/app-v<version>/rgb-sunglasses-<version>.apk)
```

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
git tag app-v<version>      <commit> && git push origin app-v<version>
git tag mcuboot-v<version>  <commit> && git push origin mcuboot-v<version>
```

---

## 7. Watch CI, then attach the curated notes

- Find the triggered runs: `gh run list --limit 5`.
- Watch each to completion: `gh run watch <id> --exit-status`
  - Firmware (`release.yaml`): ~15–25 min (two pristine NCS builds)
  - MCUboot (`mcuboot-release.yaml`): ~15–25 min (one pristine NCS build, proto0 only)
  - App (`app-release.yml`): faster
- If a run fails, read its logs (`gh run view <id> --log-failed`), report to the
  user, and **do not** edit release notes. Delete and re-push the tag after a fix:
  `git push --delete origin <tag> && git tag -d <tag>`
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
- The MCUboot release asset `mcuboot-<version>-proto0.bin` is now available for
  manual selection in the app's "Bootloader Update" section.
