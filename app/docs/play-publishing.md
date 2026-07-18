# Google Play publishing

Every `app-vX.Y.Z` tag push runs three parallel publish jobs in
`.github/workflows/app-release.yml` (fed by the shared `version` job, whose
build number is both the Android versionCode and the iOS buildNumber):

- `release` — signed **APK** attached to a GitHub release (with QR code),
  consumed by the in-app self-updater.
- `ios-testflight` — signed iOS build uploaded to TestFlight (Mac runner).
- `play` — builds a Play-variant **AAB** and uploads it to Google Play via
  `r0adkll/upload-google-play`. Gated on the `PLAY_PUBLISH_ENABLED` repo
  variable, so it is inert (skipped) until the one-time bootstrap below is done.

Both Android jobs build through the composite action
`.github/actions/build-signed-android`, so the APK and AAB cannot drift apart
in how they are built.

The Play build differs from the GitHub APK on purpose (Play's Device & Network
Abuse policy forbids self-updating outside Play, and `REQUEST_INSTALL_PACKAGES`
is a restricted permission):

| | GitHub APK build | Play AAB build |
|---|---|---|
| `expo.extra.distribution` | *(unset)* | `"play"` (stamped by CI) |
| `REQUEST_INSTALL_PACKAGES` | declared | removed by CI |
| In-app APK self-update | enabled | disabled (`APP_SELF_UPDATE_SUPPORTED` in `app/services/app-update.ts`) |
| Updates delivered by | in-app GitHub-release download | Google Play itself |

## Signing lineage — why the CI keystore is the Play app signing key

At Play App Signing enrollment the existing CI release keystore (the
`KEYSTORE_BASE64` secret) was uploaded as the **app signing key** (not
Google-generated), and the same keystore serves as the upload key. Because of
that, Play-delivered APKs and the GitHub sideload APK carry the **same
signature** — one install lineage, so users can switch between channels (or
sideload a newer GitHub build over a Play install) without uninstalling.

Consequences:

- **Never rotate or regenerate this keystore casually.** Rotating the upload
  key (possible via Play's key-upgrade flow) would keep Play working but break
  signature continuity with GitHub APKs — the two channels would become
  incompatible lineages, exactly what the enrollment choice avoided.
- A keystore leak is correspondingly serious: it is the app signing key, not
  just a resettable upload key. Guard the GitHub secrets.

## versionCode scheme

Tag builds: `versionCode = MAJOR*10000 + MINOR*100 + PATCH` — derived once in
the workflow's `version` job (the same number is the iOS buildNumber).
Constraints, enforced there:

- Tags must be ≥ `1.0.0` and MINOR/PATCH must stay **< 100** (else collisions:
  `1.0.100` = `1.1.0`), so tag-derived codes are always ≥ 10000.
- Manual `workflow_dispatch` builds must use build numbers **1–9999**, so they
  can never collide with a tag-derived code.
- Play permanently consumes every uploaded versionCode, **including drafts**.
  A same-version tag re-push after a successful Play upload fails with a
  duplicate-versionCode error — recover by bumping patch. Dry-runs burn their
  manual-range codes too (bump the build number per attempt).

## One-time bootstrap checklist

Do these in order; the API upload only works after the final steps.

1. **Register a Google Play developer account** ($25 one-time; identity
   verification can take days). Note: a *personal* account created after
   Nov 2023 must run a closed test with **12+ testers opted in for 14
   consecutive days** before it can get production access — internal/closed
   tracks work immediately. Organization accounts are exempt.
2. **Create the app** in Play Console: name "RGB Sunglasses", type *App*,
   *Free* (irreversible: paid→free only). The package name
   `com.autom8ed.rgbsunglassesapp` is locked in by the first bundle upload.
3. **Play App Signing — enroll the existing key (do NOT accept the
   Google-generated default):** on the first-release app-signing step choose
   *Use an existing key* → *Export and upload a key from Java keystore*,
   download Google's PEPK tool, and run:

   ```bash
   java -jar pepk.jar --keystore=release.keystore --alias=<KEY_ALIAS> \
        --output=encrypted_key.zip --encryptionkey=<hex key from the Console page>
   ```

   using the same keystore/alias as the CI secrets, then upload the zip.
4. **Build and manually upload the first AAB** (the API cannot create an app's
   first release). From `app/` with the CI keystore decoded locally:

   ```bash
   npm ci
   jq '.expo.version="0.1.0" | .expo.android.versionCode=100
       | .expo.extra.distribution="play"
       | .expo.android.permissions -= ["android.permission.REQUEST_INSTALL_PACKAGES"]' \
       app.json > app.json.tmp && mv app.json.tmp app.json   # do NOT commit
   npx expo prebuild --platform android --clean
   cd android && ./gradlew bundleRelease \
       -Pandroid.injected.signing.store.file=<path>/release.keystore \
       -Pandroid.injected.signing.store.password=... \
       -Pandroid.injected.signing.key.alias=... \
       -Pandroid.injected.signing.key.password=...
   ```

   Upload `android/app/build/outputs/bundle/release/app-release.aab` to
   **Internal testing** in the Console (version `0.1.0`/code 100 sorts below
   every real release). Create an internal tester email list and note the
   opt-in link. Revert `app.json` afterwards.
5. **Store listing**: short (≤80) + full (≤4000) descriptions, 512×512 icon,
   1024×500 feature graphic, ≥2 phone screenshots, **privacy policy URL**
   (host via GitHub Pages), category, contact email.
6. **App content declarations** (Policy → App content): data safety form (BLE
   permissions — `BLUETOOTH_SCAN`/`BLUETOOTH_CONNECT`/`ACCESS_FINE_LOCATION`;
   nothing leaves the device except anonymous GitHub API reads), ads
   declaration (none — verify the merged manifest has no
   `com.google.android.gms.permission.AD_ID`), content rating questionnaire,
   target audience (not child-directed), and an app-access note for reviewers:
   "all screens reachable without login; BLE features require the RGB
   Sunglasses hardware peripheral". Optional easing: `neverForLocation: true`
   in the `react-native-ble-plx` plugin config in `app.json` can drop the
   location permission on API 31+ — verify plugin support before relying on it.
7. **Service account**: in Google Cloud, enable the **Google Play Android
   Developer API** on a project, create a service account (no GCP roles
   needed) and a JSON key. In Play Console → *Users and permissions*, invite
   the service-account email with app-level permissions **Release to testing
   tracks** + **Manage testing tracks and edit tester lists** (add *Release to
   production* later when flipping tracks). Permissions can take up to 24 h to
   propagate — early API calls may 401/403.
8. **GitHub configuration** (repo settings):

   | Kind | Name | Value |
   |---|---|---|
   | Secret | `PLAY_SERVICE_ACCOUNT_JSON` | raw contents of the service-account JSON key |
   | Secret (existing) | `KEYSTORE_BASE64`, `STORE_PASSWORD`, `KEY_ALIAS`, `KEY_PASSWORD` | reused unchanged |
   | Variable | `PLAY_PUBLISH_ENABLED` | `true` (leave unset to keep the `play` job skipped) |
   | Variable | `PLAY_TRACK` | `alpha` — the built-in **Closed testing** track (current default; falls back to `internal` only if the variable is unset) |
   | Variable | `PLAY_RELEASE_STATUS` | defaults to `draft` when unset (fail-safe: a never-published app **rejects** `completed` releases). Set to `completed` once the app is first published and you want tag releases to go live automatically |

## Foreground-service declaration (connectedDevice)

The app declares a `connectedDevice`-typed foreground service (issue #124: it keeps
the BLE connection alive while the app is backgrounded — notifee's service, retyped
by `plugins/withBleForegroundService.js`, permissions `FOREGROUND_SERVICE` +
`FOREGROUND_SERVICE_CONNECTED_DEVICE` in `app.json`). Google Play requires a
**Foreground service permissions declaration** for every typed FGS (Play Console →
*App content* → *Foreground service permissions*): pick the `CONNECTED_DEVICE` type,
describe the use case ("maintains the Bluetooth LE connection to the user's RGB
Sunglasses so animation changes apply instantly while the app is backgrounded"), and
attach a short screen-recording of connect → HOME → notification visible → control
still works. Submissions without the declaration are rejected at review time — do
this before the first Play upload that contains the FGS.

Do NOT add `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` to "fix" OEM kills — it is a
Play-restricted permission with its own (rarely granted) declaration. The in-app
guidance for aggressive OEMs (OxygenOS "advanced optimization") is to exempt the app
manually: Settings → Battery → the app → *Don't optimize*.

## Dry-run procedure

Trigger the `App Release` workflow via **workflow_dispatch** with
`target = play`, a version like `0.90.0` (`0.9x.y` by convention, so it reads
as a test build in the Console), and a build number in the manual range, e.g.
`9000` (1–9999 is enforced, so a dry-run can never burn a tag-derived
versionCode). Dispatch runs build only the selected channel — the GitHub
`release` job and the TestFlight job are skipped — and force the Play upload to
**draft** status, so a test can never replace a live release. Verify in the
Play Console: the draft appears on the track named by `PLAY_TRACK` (currently
`alpha` / **Closed testing**), the what's-new text is right, and *App bundle
explorer* shows the expected signing certificate. Each attempt burns its build
number — bump to `9001`, `9002`, … per retry. (Play permanently consumes every
versionCode, drafts included, so pick a low number deliberately rather than a
high one — e.g. the first Closed-testing draft used version `0.2.2` / code
`202`.)

## Promote-to-production runbook

1. First real releases: keep `PLAY_RELEASE_STATUS=draft`, publish each draft
   manually in the Console. Sanity-check the shared lineage once: sideload the
   GitHub APK of a version, then install the same version from the Play opt-in
   link (or vice versa) — it must update in place without an uninstall.
2. Once trusted: set `PLAY_RELEASE_STATUS=completed` — releases on the
   `PLAY_TRACK` track (currently `alpha` / Closed testing) go live to testers
   automatically on every tag.
3. Production: after the store listing is approved (and, for personal
   accounts, the 12-tester/14-day closed-test requirement is met — this is why
   `PLAY_TRACK` now defaults to the closed `alpha` track), grant the
   service account *Release to production* and set `PLAY_TRACK=production`.
   Until then, promote closed testing → production manually in the Console.

## Release-notes flow ("what's new")

Play's listing text comes from the **annotated tag message** (`git tag -a
app-vX.Y.Z -m "<summary>"`), truncated to Play's 500-char limit by CI. The
curated GitHub release notes are attached *after* CI finishes, so they never
reach Play. Lightweight tags fall back to a generic "Bug fixes and
improvements" line. See `.claude/skills/release/SKILL.md` for the full release
process.
