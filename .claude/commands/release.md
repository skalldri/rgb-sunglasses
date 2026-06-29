---
description: Cut a versioned release of the firmware, the companion app, or both — bump version, tag, push, and attach curated release notes
allowed-tools: Bash(git:*), Bash(gh:*), Read, Write, Edit, AskUserQuestion
---

Cut a new release. This monorepo has three independent release tracks:

| Track | Tag convention | How it builds |
|---|---|---|
| Firmware (fw) | `fw-vX.Y.Z` | CI workflow `release.yaml` on push |
| App | `app-vX.Y.Z` | CI workflow `app-release.yml` on push |
| MCUboot bootloader | `mcuboot-vX.Y.Z` | **Local build + manual upload** (no CI workflow) |

For **Firmware** and **App**, the CI workflow is the source of truth for the version — it is injected at build time from the tag, so no in-repo version files need editing.

For **MCUboot**, the version is stored in `fw/sysbuild/mcuboot/VERSION` and must be bumped in-repo before building and tagging. The packaged binary is uploaded directly to the release as an asset.

Follow this process exactly. Do not push any tag until the user has approved the version and the release notes.

---

## 1. Choose what to release

Use `AskUserQuestion` to ask whether this is a **Firmware**, **App**, **MCUboot Bootloader**, or **Both** (fw+app)
release, unless the user already said so in their request.

---

## Firmware and App release process

### 2a. Pre-flight checks

- Confirm the working tree is clean (`git status -sb`). If not, stop and tell the user.
- `git fetch origin` and confirm local `main` matches `origin/main` (releases are
  cut from the tip of `main`). If they differ, stop and ask.
- Record the commit to tag: `git rev-parse --short HEAD`.

### 3a. Determine the version bump (per component)

For each component being released:

- Find the latest existing tag (use git's own semver sort — `--sort=-version:refname`
  — rather than piping through `sort -V`, which isn't available everywhere):
  - Firmware: `git tag --list 'fw-v*' --sort=-version:refname | head -1`
  - App: `git tag --list 'app-v*' --sort=-version:refname | head -1`
- Gather the commits that touch the component's directory:
  - **If a prior tag exists** — only the commits since it:
    - Firmware: `git log <lastTag>..HEAD --oneline -- fw/`
    - App: `git log <lastTag>..HEAD --oneline -- app/`
  - **If no prior tag exists** (first release for that component) — the next version
    is `1.0.0`, and the changelog covers the full history of that directory (drop the
    `<lastTag>..` range so the command is still runnable):
    - Firmware: `git log --oneline -- fw/`
    - App: `git log --oneline -- app/`
    You can still confirm major/minor/patch below, but `1.0.0` is the expected answer.
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

### 4a. Draft curated release notes

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

### 5a. Get explicit approval

Pushing tags creates **public, hard-to-reverse** GitHub releases. Show the user the
exact tag(s), the target commit, and the notes, and ask for a clear go-ahead before
proceeding.

### 6a. Tag and push

For each component (firmware first if releasing both):

```bash
git tag fw-v<version>  <commit> && git push origin fw-v<version>
git tag app-v<version> <commit> && git push origin app-v<version>
```

### 7a. Watch CI, then attach the curated notes

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

### 8a. Report

Give the user the release URLs (`gh release view <tag> --json url`), the attached
artifacts, and note that the app's in-app GitHub-releases auto-update check will now
match devices against the new firmware tag.

---

## MCUboot bootloader release process

MCUboot releases are fully manual — there is no CI workflow. The version is bumped
in-repo, the binary is built locally, packaged with `fw/tools/package_mcuboot.py`,
and uploaded directly to a GitHub release. Only Proto0 is supported.

### 2b. Pre-flight checks

- Confirm the working tree is clean (`git status -sb`). If not, stop and tell the user.
- `git fetch origin` and confirm local `main` matches `origin/main`. If they differ, stop and ask.
- Read the current version: `cat fw/sysbuild/mcuboot/VERSION`

### 3b. Determine the version bump

- Find the latest MCUboot tag: `git tag --list 'mcuboot-v*' --sort=-version:refname | head -1`
- Gather commits affecting MCUboot since the last tag:
  - If a prior tag exists: `git log <lastTag>..HEAD --oneline -- fw/sysbuild/mcuboot/ fw/mcuboot_hooks/`
  - If no prior tag (first release): `git log --oneline -- fw/sysbuild/mcuboot/ fw/mcuboot_hooks/`
- Suggest a bump (same semver intent rules as above), then ask the user to confirm
  with `AskUserQuestion`.

### 4b. Bump the version in-repo

Edit `fw/sysbuild/mcuboot/VERSION` to set the new `VERSION_MAJOR`, `VERSION_MINOR`,
and `PATCHLEVEL` values. Commit and push to `main`:

```bash
git add fw/sysbuild/mcuboot/VERSION
git commit -m "Bump MCUboot version to X.Y.Z"
git push origin main
```

### 5b. Build the firmware

A clean build is required so the new version is embedded in the MCUboot binary:

```bash
west build --build-dir /workspaces/rgb-sunglasses/fw/build /workspaces/rgb-sunglasses/fw
```

Verify the version was picked up:
```bash
grep -E "MAJOR|MINOR|PATCH" fw/build/mcuboot/zephyr/include/generated/version.h
```

### 6b. Package the binary

```bash
python3 fw/tools/package_mcuboot.py \
  --input  fw/build/mcuboot/zephyr/zephyr.bin \
  --output mcuboot-X.Y.Z-proto0.bin \
  --major  X --minor Y --revision Z
```

The output file name must follow the pattern `mcuboot-<version>-proto0.bin` so the
companion app can identify it by board variant.

### 7b. Draft release notes

Write concise notes covering what changed in this MCUboot version:
- What bug was fixed / what feature was added
- Any special flashing instructions (e.g. "requires Prepare Device step in app")
- Reference the asset: `mcuboot-X.Y.Z-proto0.bin`

Show the notes to the user for review.

### 8b. Get explicit approval

Show the user:
- Tag: `mcuboot-vX.Y.Z`
- Target commit (tip of `main` after the version bump commit)
- The packaged binary name and its CRC32 (printed by `package_mcuboot.py`)
- The draft release notes

Ask for a clear go-ahead before creating the release.

### 9b. Tag, create the release, and upload the binary

```bash
# Tag the version-bump commit
git tag mcuboot-vX.Y.Z && git push origin mcuboot-vX.Y.Z

# Create the release and attach the binary in one step
gh release create mcuboot-vX.Y.Z \
  --title "MCUboot Bootloader vX.Y.Z" \
  --notes-file <notes-file> \
  mcuboot-X.Y.Z-proto0.bin
```

Verify: `gh release view mcuboot-vX.Y.Z --json assets`

### 10b. Report

Give the user the release URL, confirm the asset is attached, and note that the app's
bootloader-update flow will need to be pointed at this release asset to use it (the
automatic download path for MCUboot packages is not yet wired up in the app).

Clean up the local binary: `rm mcuboot-X.Y.Z-proto0.bin`
