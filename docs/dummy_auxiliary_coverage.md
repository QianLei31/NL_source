# Dummy auxiliary GUI coverage

## Status and boundary

Final status (2026-10-02 UTC): **PASS, 187 assertions, zero failures** after two reproduced updater defects were fixed. Two complete final runs passed: `../visual-qa-preview2/dummy-auxiliary-version-final.log` and `../visual-qa-preview2/dummy-auxiliary-version-repeat.log`. The actual updater UI made 21 loopback HTTP requests while managed acquisition Dummy remained connected and its frame count advanced. Both pending-request lifetime subprocesses passed again in this complete run; the earlier dedicated lifetime repeat passed **10/10 per case (20 total)** in `../visual-qa-preview2/dummy-auxiliary-lifetime-repeat.log`. Test: `tests/dummy_auxiliary_ui_e2e.cpp`; target: `ccv2_dummy_auxiliary_ui_e2e`.

The acquisition Dummy does not implement the remote updater or licensing protocol. These auxiliary paths therefore use an additional **in-process loopback HTTP fixture** while actual `CommandCenterMainWindow`, `SettingsPanel`, `UpdateClient`, and `LicenseDialog` code runs. Managed Dummy acquisition is active throughout updater UI checks, and frame progression is asserted afterward.

The fixture is an endpoint, not a forwarding proxy: every received request is parsed, recorded, and answered locally. No production service is contacted. `UpdateClient`'s actual `QNetworkAccessManager` receives a test-only loopback proxy. The stack-local manager inside `LicenseClient` uses a scoped `QNetworkProxy::setApplicationProxy` in the isolated test executable; its prior process-local value is restored afterward. No operating-system proxy, production endpoint, signature verification, license policy, or security setting is changed.

Configuration and app-data paths use a fresh temporary directory. The license editor contains an explicitly invalid synthetic key; its device fingerprint is a synthetic 64-character test value stored only in that temporary directory. No real user key/cache/credential is read or used. No private key or signed allow response is created. License rejection tests assert that no authorization cache is written.

## Cases and oracles

| Case | Actual GUI/service path | Required oracle | Status |
|---|---|---|---|
| AUX01 | First settings icon click, checking state, duplicate disabled click, release response | Exact GET path, product/version/channel query, user agent; exactly one result; version/notes and successful UI state | PASS |
| AUX02 | Actual Open Download Page button | `QDesktopServices` registered URL handler captures exact trusted URL; no browser or external site opened | PASS |
| AUX03 | Settings close and reopen | No second automatic check in same session | PASS |
| AUX04 | Manual no-update check, preview-to-final and next-preview updates | Current version/no update; stable 7.0.0 and preview.2 offered over shipped preview.1/date | PASS |
| AUX05 | HTTP 404 | Reachable service/no published version message, no false offline error | PASS |
| AUX06 | HTTP 503, malformed JSON, array JSON, missing version | Explicit real parser errors and UI retry restored | PASS |
| AUX07 | Version aliases, release aliases, notes, explicit update flag, numeric comparison | Real parser metadata and 12-row preview/stable/date/fallback comparison table | PASS |
| AUX08 | File/script/data/HTTP/untrusted host URLs; trusted GitHub subdomains | Unsafe inputs replaced by built-in HTTPS release URL before actual open click; exact captured safe target | PASS |
| AUX09 | Full five-second timeout; close/reopen pending settings; refused local proxy; retry | One result only; error visible; retry works; Dummy frame count advances | PASS |
| AUX10 | Re-check while previous network reply is pending | Isolated subprocess completes normally, old request canceled silently, only replacement result emitted | PASS |
| AUX11 | Destroy updater while network reply is pending | Isolated subprocess completes normally without a spurious result | PASS |
| AUX12 | Missing offline cache and explicit license grace boundary dates | No offline authorization; grace through 2026-12-21 and required from 2026-12-22, unchanged policy | PASS |
| AUX13 | Empty/whitespace license and fingerprint Copy button | Local empty-key failure without request; exact clipboard value; previous text restored | PASS |
| AUX14 | Repeated invalid key; HTTP rejection/server failure, refused loopback transport; malformed, mismatched and unsigned response | Exact synthetic POST protocol; specific error visible; never accepted; retry enabled; no cache written | PASS |
| AUX15 | Full eight-second verification timeout | Actual progress visible, verify disabled, no cancel control offered; failure and successful error-retry flow | PASS |
| AUX16 | Exit after failure, Escape, window close | Real dialogs finish Rejected and become hidden | PASS |

## Reproduced defects and verification

1. **Pending updater abort crashed in replacement and destruction.** The real `QNetworkReply::abort()` synchronously emitted `finished`; `handleReply()` cleared `m_reply`, and the caller then dereferenced `m_reply->deleteLater()`. Both narrow child processes terminated with SIGSEGV/exit 139 before the fix (`dummy-auxiliary-lifetime-before-fix.log`). The initial complete suite recorded exactly those two failures (`dummy-auxiliary-initial.log`, 162 checks). `UpdateClient` now stops its timer and detaches/disconnects the reply before abort/deleteLater, suppressing stale completion on replacement/destruction. Both child modes passed 10/10 repeated runs and the final full suites.
2. **A final release was ranked below the shipped preview.** The real Settings check against `{"latest_version":"7.0.0"}` incorrectly reported no update for current `7.0.0-preview.1-20261002`, because numeric-only comparison treated preview/build-date digits as higher version fields. The pre-fix complete run had exactly one failure (`dummy-auxiliary-version-before-fix.log`, 175 checks). Comparison now recognizes only the repository's numeric stable / `-preview.N` forms with optional `v` and eight-digit display date, compares release core first, and ranks stable above preview at equal core. Same-kind preview/date, dated 6.x releases, and unrecognized-format numeric fallback remain unchanged. Actual parser/UI final-release and preview.2 checks and a 12-row comparison table pass.

Production edits are confined to `src/service/update_client.cpp`. No endpoint, update channel, license policy, key, signature, or permission setting changed. The combined terminal run passes 187 assertions; earlier smaller counts are retained as diagnostic history, not claimed as the latest suite.

## Explicit untested/blocked cases

- **BLOCKED: genuine signed successful online activation and signed authoritative rejection.** No authorized server fixture with a fresh Ed25519 signature bound to this product/device/key was supplied. Synthetic unsigned denial tests verify rejection; they are not successful licensing evidence.
- **BLOCKED: genuine signed offline-cache grace success/expiry/renewal and migration.** Those cases need an authorized signed cache/response fixture. Empty-cache failure is exercised; no signature or policy bypass is used.
- **N/A in ordinary application startup during the current release grace period:** the gated license dialog is not normally reached before 2026-12-22. The test directly instantiates the actual compiled dialog to exercise its negative paths; it does not change the date or startup policy.
- **N/A: a user-facing in-flight License verification Cancel button.** Production `QProgressDialog` explicitly removes that control. Exit/Escape/window-close tests cover the idle dialog, and the production timeout covers a held verification request. The suite does not programmatically force a hidden cancel action or claim that closing settings cancels a pending update.
- **BLOCKED/UNRUN: live server deployment behavior, TLS/proxy environment differences, real browser navigation, release download/installation, and real license entitlement.** No external service or browser is contacted by this suite. URL handling is capture-only.
- Updater JSON is unsigned over the existing production HTTP endpoint. This suite verifies the existing allow-listed HTTPS download-URL sanitation; it does not establish authenticity of remote version strings/notes.

## Reproduction

After the target is built with the existing Qt/OpenSSL environment:

```sh
source ../deps/env.sh
QT_QPA_PLATFORM=offscreen XDG_CACHE_HOME="$PWD/../deps/cache" ../build-preview2/ccv2_dummy_auxiliary_ui_e2e
QT_QPA_PLATFORM=offscreen XDG_CACHE_HOME="$PWD/../deps/cache" ../build-preview2/ccv2_dummy_auxiliary_ui_e2e --repeat-check-child
QT_QPA_PLATFORM=offscreen XDG_CACHE_HOME="$PWD/../deps/cache" ../build-preview2/ccv2_dummy_auxiliary_ui_e2e --destroy-pending-child
```

The two child modes retain narrow, real-network-reply lifetime reproducers. Full-suite subprocess isolation ensures a lifetime crash is recorded as a failed assertion rather than silently skipping later license tests.
