# Implementation Process (Multi-Track)

**Ship:** multi-track-ship / native-ftp  
**Plan path:** `docs/process-native-ftp-ship.md`  
**Brief:** `docs/NATIVE_FTP_SHIP_BRIEF.md`  
**Pipeline mode:** `both` (after_each + batch)  
**QA dir:** `qa/native-ftp`  
**Created:** 2026-08-07  

Agents execute tracks in order unless two tracks have fully non-overlapping owner files and no shared acceptance dependency. Every track starts as **Status: pending**. Update status in this file as work progresses (`pending` → `in_progress` → `done` / `blocked`). Do not rewrite foundation surfaces; extend them.

---

## Goal

Ship cross-platform native FTP/SFTP (libcurl) integrated into StelliferumAuditor, with CLI standalone transfer mode that works without WinSCP on Linux.

- Follow `docs/NATIVE_FTP_SHIP_BRIEF.md` and roadmap priorities **#9** (transfer security), **#5** (native SFTP/FTP), **#4** (standalone transfer as CLI mode), **#7** (cross-platform structure).
- Keep existing `ftp_*` API for UI / `upload_daemon` / `main`.
- Prefer native backend; WinSCP only optional Windows fallback.
- Security on native path: no password on argv, host-key / known_hosts support, no plaintext temp scripts.
- Acceptance: Windows Release build succeeds with native backend; CLI `--ftp-*` flags; Linux does not require WinSCP; README updated.

**Ingest summary:** Foundation already provides a full WinSCP backend, public API, UI/upload_daemon consumers, and headless CLI modes—do not rewrite those surfaces. New work adds libcurl native path and CLI `--ftp-*` while keeping signatures and callers stable.

---

## Foundation (done — do not redo)

Already shipped and must not be rewritten:

1. **Full public `ftp_*` surface** in `include/auditor.h` (download/upload batch/recursive/list/restore/verify/cleanup + cancel/counter hooks) consumed by `ui.cpp`, `upload_daemon.c`, and `main.c`.
2. **WinSCP-based implementation** in `src/ftp_manager.c` with session scripting, piped logging, batch upload retries, size-based sync masks, restore points, and browser `ls` parsing.
3. **`dependency_manager.c`** PE-patch + RCDATA extract of WinSCP for Windows portable bundling; CMake optional `resources.rc` when `libs/winscp/WinSCP.exe` exists.
4. **CLI foundation:** `--headless`, `--regen`/`--local`, `--restore-last-upload` with `util_setup_console` under WIN32.
5. **Config contracts:** `config/ftp.ini` `HOST`/`PORT`/`USER`/`PASS` and `server_paths.ini` `REMOTE_*`/`LOCAL_ROOT`/`DOWNLOAD_*`/`SHOP_MOD`.
6. **Port heuristic:** `22`/`2222`/`8827` => SFTP else FTP.

New work adds libcurl native path and CLI `--ftp-*` while keeping signatures and callers stable.

---

## Excluded

Do **not** implement in this ship:

- UI overhaul / navigation redesign (roadmap #2)
- Config validation & dead loaders `tier_rules`/`known_items` (roadmap #8)
- Loot/shop configuration product work (roadmap #3)
- Mod containers & mod-function loot by tier (roadmap #1)
- Expansion Market export parity (roadmap #10)
- Zero-dependency AI helpers packaging (roadmap #6)
- General reliability baseline/CI beyond this ship's QA (roadmap #11)
- Rewrite of `ui.cpp` economy/load/pipeline UI beyond `ftp_*` consumption
- Rewrite of `upload_daemon` job model or phase sequencing
- Economy swarm/parser/writer/`loot_policy` rewrites
- Hard removal of optional Windows WinSCP embed (keep as fallback only)
- Separate second product binary for FTP tool

---

## Shared QA pipeline

### Settings

| Key | Value |
|-----|--------|
| `qa_dir` | `qa/native-ftp` |
| `pipeline_mode` | `both` |
| Modes | `after_each` (per-track checks) + `batch` (full ship gate) |

Create `qa/native-ftp/` at first QA run. Store logs, build summaries, CLI smoke outputs, and security greps under that tree. Prefer relative paths from repo root.

### Artifact paths (canonical)

| Artifact | Path |
|----------|------|
| Windows Release configure/build log | `qa/native-ftp/01-windows-release-build.log` |
| Symbol / link summary (native + curl) | `qa/native-ftp/02-link-symbols.txt` |
| CLI help / flag smoke | `qa/native-ftp/03-cli-ftp-help.txt` |
| CLI dry-run list/download (if config allows) | `qa/native-ftp/04-cli-ftp-smoke.txt` |
| Password-on-argv assertion (native path) | `qa/native-ftp/05-no-password-argv.txt` |
| Linux/Unix configure (no WinSCP hard req) | `qa/native-ftp/06-linux-or-unix-configure.log` |
| ABI / ftp_* consumer link note | `qa/native-ftp/07-ftp-api-consumers.txt` |
| Batch final checklist | `qa/native-ftp/08-batch-acceptance.md` |
| Track notes / blockers | `qa/native-ftp/track-notes.md` |

### P1–P8 shared steps

Execute these as applicable in **after_each** (subset) and always as **batch** gate before Done criteria.

1. **P1 — Windows Release + libcurl**  
   Configure and build Release on Windows with `STELLI_USE_LIBCURL` and native backend compiled in. Confirm link to libcurl with FTP and SFTP (or libssh2) support. Capture log → `01-windows-release-build.log`.

2. **P2 — Optional WinSCP resource remains optional**  
   Confirm `resources.rc` / WinSCP embed only when `libs/winscp` present; configure must not fail if absent on non-fallback paths. Note in `02-link-symbols.txt`.

3. **P3 — CLI smoke**  
   Run binary with `--ftp-list` and/or `--ftp-download` help/usage paths; prefer `--dry-run` where possible against `config/*.example`. Capture → `03-cli-ftp-help.txt`, optional `04-cli-ftp-smoke.txt`.

4. **P4 — Credential hygiene (native)**  
   Assert no password on process command line / CreateProcess argv for native path; assert native path does not write plaintext passwords into `.TEMP` scripts. Capture method + result → `05-no-password-argv.txt`.

5. **P5 — Linux / Unix generators**  
   Configure (and build if environment allows) with Unix generators or Linux host: must not hard-require `WinSCP.exe` or `resources.rc`. Capture → `06-linux-or-unix-configure.log`.

6. **P6 — Consumer ABI stability**  
   Confirm `ui.cpp`, `upload_daemon.c`, `main.c` still link against unchanged `ftp_*` public signatures in `include/auditor.h`. Note → `07-ftp-api-consumers.txt`.

7. **P7 — Backend preference**  
   Prefer native when `STELLI_USE_LIBCURL`; Windows may fall back to WinSCP only if native unavailable; Linux/macOS never require WinSCP extraction/PE patch for transfer.

8. **P8 — Docs + batch gate**  
   README/BUILD document CLI flags, native transfer, Linux without WinSCP, host-key expectations; fill `08-batch-acceptance.md` with pass/fail per acceptance bullet from brief.

### When to run

| Mode | When | What |
|------|------|------|
| `after_each` | End of each track | Track-specific **Pipeline checks** + any P-steps listed there; append notes to `track-notes.md` |
| `batch` | After all tracks (or when ship claims done) | Full P1–P8; write `08-batch-acceptance.md` |

### Hotspot sequencing

- `src/ftp_manager.c` — primary merge hotspot (dispatch + advanced). Serialize ownership: prefer tracks **ftp-backend-dispatch** then **ftp-native-advanced** over parallel edits.
- `src/main.c` — CLI after native API ready (**cli-standalone-ftp** after **ftp-native-core** at minimum).
- `CMakeLists.txt` — **cmake-libcurl** first; later tracks only additive flags/sources.
- `include/auditor.h` — ABI-stable; additive config helpers only if needed; no signature churn on existing `ftp_*`.
- **transfer-security** and **ftp-native-core** share credential/host-key plumbing — coordinate `known_hosts` keys in `config/ftp.ini.example`.

---

## Tracks

Execute in the order below unless two tracks are proven non-overlapping (owner files + acceptance independent). Default is sequential.

### Track cmake-libcurl: CMake FetchContent / link libcurl with FTP+SFTP

- **Status:** done
- **Result:** Windows Release configure/build links static libcurl (FTP+FTPS+SCP+SFTP via libssh2/WinCNG) with `STELLI_USE_LIBCURL=1`; `src/ftp_native.c` stub in SOURCES; WinSCP `resources.rc` still optional; BUILD.md documents options. P5 live Linux configure deferred (structural no-WinSCP gate verified).
- **Owner files:**
  - `CMakeLists.txt`
  - `docs/BUILD.md`
  - `src/ftp_native.c` (stub/source entry so target can compile)
- **Actions:**
  1. Add libcurl via CMake `FetchContent` and/or `find_package` with FTP + SFTP support (libssh2 or curl built with SFTP).
  2. Link target with libcurl; define `STELLI_USE_LIBCURL` when native backend is enabled.
  3. Add `src/ftp_native.c` to `SOURCES` (minimal stub OK if core track follows immediately).
  4. Keep WinSCP `resources.rc` optional when `libs/winscp/WinSCP.exe` exists; do not require it on Linux.
  5. Document build flags / deps in `docs/BUILD.md` (FetchContent curl, platform notes).
- **Acceptance:**
  - Windows Release configure/build links libcurl with FTP and SFTP (or libssh2) support and defines `STELLI_USE_LIBCURL`.
  - WinSCP `resources.rc` remains optional when `libs/winscp` is present.
  - Linux configure does not require `WinSCP.exe` or `resources.rc`.
- **Pipeline checks (after_each):**
  - P1 (configure/build Release Windows with define).
  - P2 (optional WinSCP resource).
  - P5 if Linux/Unix tool available; else note deferred to batch.
  - Artifacts: `01-windows-release-build.log`, partial `02-link-symbols.txt`.

---

### Track ftp-native-core: Native libcurl core transfers (get/put/batch/list/cancel)

- **Status:** done
- **Result:** `src/ftp_native.c` implements libcurl core transfers as `ftp_native_*` mirrors (download/upload single+batch, list, cancel, upload counter) with public `ftp_*` signatures unchanged; port heuristic 22/2222/8827=>sftp; credentials only via CURLOPT_USERNAME/PASSWORD (no argv, no TEMP scripts). Release build OK. Dispatch track wires public entry points.
- **Owner files:**
  - `src/ftp_native.c`
  - `include/auditor.h` (no signature breaks; internal helpers only if needed)
  - `src/ftp_manager.c` (thin hooks only if required for compile; prefer full dispatch in next track)
- **Actions:**
  1. Implement native libcurl transfers: `ftp_download_file`, `ftp_upload_file`, `ftp_upload_batch`, `ftp_download_batch`, `ftp_list_directory`, `ftp_set_cancel_flag` (and progress / upload-counter hooks as needed).
  2. Keep **public signatures unchanged** vs `include/auditor.h`.
  3. Protocol heuristic: port `22`/`2222`/`8827` => `sftp://`, else `ftp://`.
  4. Credentials only via libcurl options / in-process buffers — **never** process argv.
  5. **No** plaintext password temp scripts on native path.
  6. Prefer implementing real bodies in `ftp_native.c`; `ftp_manager.c` may retain WinSCP bodies until dispatch track wires preference.
- **Acceptance:**
  - Native module implements the listed ops with public signatures unchanged.
  - Port heuristic matches foundation.
  - No password on process argv; no plaintext password temp scripts on native path.
- **Pipeline checks (after_each):**
  - P1 (native object links into Release).
  - P4 (static review / grep for argv/script password paths on native code).
  - P6 (headers still match consumers).
  - Artifacts: update `02-link-symbols.txt`, start `05-no-password-argv.txt`.

---

### Track ftp-backend-dispatch: Prefer native backend; WinSCP only optional Windows fallback

- **Status:** done
- **Result:** Public core `ftp_*` prefer native when `STELLI_USE_LIBCURL` (`ftp_use_native_backend` → `ftp_native_*`); cancel/counter dual-wired; WinSCP only via `ftp_winscp_fallback_allowed` (Windows); `run_winscp_script` refuses off-Windows; `dependency_manager` soft-skips extract/PE patch on non-Windows and treats WinSCP as optional fallback when native linked. Release build OK. Advanced ops still WinSCP-on-Windows until ftp-native-advanced.
- **Owner files:**
  - `src/ftp_manager.c`
  - `src/dependency_manager.c`
  - `src/ftp_native.c`
  - `CMakeLists.txt` (guards only)
- **Actions:**
  1. Route public `ftp_*` entry points so UI / `upload_daemon` / `main` prefer native when `STELLI_USE_LIBCURL`.
  2. Windows: allow fall back to existing WinSCP path only if native unavailable or failed to build/feature-missing.
  3. Linux/macOS: never require WinSCP extraction or PE patch for transfer; soft-skip or no-op installer paths for transfers.
  4. Avoid dual-default ambiguity: native is default when compiled in.
  5. Do not hard-remove WinSCP embed; keep optional fallback only.
- **Acceptance:**
  - Entry points prefer native when `STELLI_USE_LIBCURL`.
  - Windows may fall back to WinSCP only if native unavailable/fails to build.
  - Linux/macOS never require WinSCP extraction or PE patch for transfer.
- **Pipeline checks (after_each):**
  - P1, P6, P7.
  - Code review: dispatch order documented in `track-notes.md`.
  - Artifacts: `07-ftp-api-consumers.txt`.

---

### Track transfer-security: Host-key / known_hosts and credential hygiene (#9)

- **Status:** done
- **Result:** Native SFTP host-key via KNOWN_HOSTS (default `config/known_hosts`), optional HOST_KEY_PIN (SHA256), HOST_KEY_POLICY=pin|fail|trust (first-connect pin under config/); credentials only CURLOPT_USERNAME/PASSWORD (no argv/CreateProcess, no native .TEMP password scripts); WinSCP documented LEGACY/WEAKER in ftp_manager.c; Release BUILD_EXIT=0; P4 `05-no-password-argv.txt` PASS.
- **Owner files:**
  - `src/ftp_native.c`
  - `src/ftp_manager.c` (document/guard fallback; no new argv leaks)
  - `config/ftp.ini.example`
  - `include/auditor.h` (only if additive, ABI-safe helpers required)
- **Actions:**
  1. Native path: known_hosts / host-key verification (config key and/or first-connect pin under `config/`).
  2. Document example keys in `config/ftp.ini.example` (e.g. `KNOWN_HOSTS`, `HOST_KEY_PIN`, or equivalent).
  3. Passwords never appear on CreateProcess/argv on native path.
  4. Native path does not write plaintext passwords into `.TEMP` scripts.
  5. If WinSCP fallback retained, document as weaker/legacy in code comments and later docs track.
  6. Coordinate config key names with any placeholders from **ftp-native-core**.
- **Acceptance:**
  - Native path supports known_hosts/host-key verification (config or first-connect pin under `config/`).
  - Passwords never on CreateProcess/argv on native path.
  - Native path does not write plaintext passwords into `.TEMP` scripts.
  - Optional WinSCP fallback documented as weaker/legacy if retained.
- **Pipeline checks (after_each):**
  - P4 mandatory evidence in `05-no-password-argv.txt`.
  - Grep: no `PASS`/`password` embedded into script writers on `#ifdef STELLI_USE_LIBCURL` success path.
  - Spot-check `ftp.ini.example` for host-key keys.

---

### Track ftp-native-advanced: Native parity for recursive download, restore, verify, cleanup

- **Status:** done
- **Result:** Native advanced ops in `ftp_native_*` (recursive/core download, upload_directory, cleanup, verify with remote size, create_restore_point, restore_from_manifest); public `ftp_*` early-return via `ftp_use_native_backend` without WinSCP log scraping; upload_daemon + main --headless/--restore-last-upload still compile against same API. Size-match skip documented (LIST size vs local stat). Release BUILD_EXIT=0.
- **Owner files:**
  - `src/ftp_native.c`
  - `src/ftp_manager.c`
  - `src/upload_daemon.c` (compile-only / call sites unchanged)
  - `src/main.c` (existing `--headless` / restore paths only; no new `--ftp-*` yet unless trivial)
- **Actions:**
  1. Implement via native backend: `ftp_download_recursive` (or walk+get), `ftp_create_restore_point`, `ftp_restore_from_manifest`, `ftp_verify_uploads`, `ftp_cleanup_remote_dir`, related helpers (`ftp_download_core_files`, `ftp_upload_directory`, restore list/purge/generate as needed for parity).
  2. Do **not** depend on WinSCP log scraping for native success path.
  3. Note semantic differences vs WinSCP size masks in `qa/native-ftp/track-notes.md` if behavior diverges; document QA expectations.
  4. Ensure `upload_daemon` and `--headless` / `--restore-last-upload` continue to compile against same `ftp_*` API.
  5. Sequence edits on `ftp_manager.c` after dispatch track; avoid clobbering preference logic.
- **Acceptance:**
  - Listed advanced ops work via native backend without WinSCP log scraping.
  - `upload_daemon` and `--headless` / `--restore-last-upload` continue to compile against same `ftp_*` API.
- **Pipeline checks (after_each):**
  - P1, P6.
  - Optional dry-run of restore/list if credentials unavailable — at least unit/link proof.
  - Update `07-ftp-api-consumers.txt` and track notes for mask/sync semantic deltas.

---

### Track cli-standalone-ftp: Integrated CLI standalone transfer mode (`--ftp-*`)

- **Status:** done
- **Result:** Same binary: `--ftp-download` / `--ftp-upload` / `--ftp-list [path]` / `--remote` / `--local` / `--dry-run`; reads `config/ftp.ini` + `config/server_paths.ini`; headless `util_setup_console` early-exit (no `ui_run`); public `ftp_*` only (native preferred); bare `--local` still regen. Release build OK; dry-run smoke EXIT 0.
- **Owner files:**
  - `src/main.c`
  - `src/util.c`
  - `config/ftp.ini.example`
  - `config/server_paths.ini.example`
- **Actions:**
  1. Same binary supports: `--ftp-download`, `--ftp-upload`, `--ftp-list [path]`, `--remote`, `--local`, `--dry-run`.
  2. Read `config/ftp.ini` + `config/server_paths.ini` (same as GUI).
  3. Headless: `util_setup_console` / no raylib `ui_run` when these flags present (early exit before UI).
  4. Works without WinSCP on Linux (native path).
  5. Do not introduce a second product binary.
  6. Coexist with existing `--headless`, `--regen`/`--local`, `--restore-last-upload` (parse carefully; avoid flag collisions).
  7. Update examples if new keys needed for remote/local defaults.
- **Acceptance:**
  - Flags above work on same binary; config-driven; headless; no WinSCP required on Linux.
- **Pipeline checks (after_each):**
  - P3 (help + dry-run smoke → `03-cli-ftp-help.txt`, `04-cli-ftp-smoke.txt`).
  - P4 re-check CLI path does not put password on argv.
  - P6 for main still using stable API.

---

### Track linux-no-winscp: Cross-platform transfer path — Linux without WinSCP (#7)

- **Status:** done
- **Result:** Linux CMake configure (AlpineWSL live) skips `resources.rc`, links native libcurl (FTP+SFTP), reports transfer backend native REQUIRED; dependency_manager soft-skips non-Windows; `ftp_require_native_or_fail` + no `system()` WinSCP path off Windows; BUILD.md Linux transfer table; Windows Release BUILD_EXIT=0. Artifact `06-linux-or-unix-configure.log` P5 PASS.
- **Owner files:**
  - `CMakeLists.txt`
  - `src/dependency_manager.c`
  - `src/ftp_manager.c`
  - `src/util.c`
  - `docs/BUILD.md`
- **Actions:**
  1. Linux build does not hard-require WinSCP / `resources.rc`.
  2. Dependency installer is no-op or soft-skip on non-Windows for transfers.
  3. Native backend is the required transfer path off Windows.
  4. Do not force PE patch on Linux; avoid drive-by rewrites of dependency manager beyond transfer-related skips.
  5. Document Linux build/transfer prerequisites (libcurl/SFTP) in `docs/BUILD.md`.
- **Acceptance:**
  - Linux build does not hard-require WinSCP/`resources.rc`.
  - Dependency installer no-op or soft-skip on non-Windows for transfers.
  - Native backend is the required transfer path off Windows.
- **Pipeline checks (after_each):**
  - P5 mandatory if environment allows; else document blocker and batch retry.
  - P7 confirmation for non-Windows.
  - Artifact: `06-linux-or-unix-configure.log`.

---

### Track docs-readme-ship: README/docs — native transfer, CLI flags, WinSCP optional

- **Status:** done
- **Result:** README documents `--ftp-list`/`--ftp-download`/`--ftp-upload` + related flags, native libcurl preferred, Linux without WinSCP, host-key/security expectations, roadmap #9/#5/#4 Done and #7 In progress; BUILD.md CLI/security cross-links; brief acceptance all checked; `08-batch-acceptance.md` + `10-docs-scan.txt` updated for P8.
- **Owner files:**
  - `README.md`
  - `docs/NATIVE_FTP_SHIP_BRIEF.md` (acceptance checkboxes / status only)
  - `docs/BUILD.md`
  - `config/ftp.ini.example`
- **Actions:**
  1. README: document CLI `--ftp-list` / `--ftp-download` (and related flags); state core transfer no longer requires WinSCP on Linux; host-key/security expectations; roadmap progress for **#9 / #5 / #4 / #7**.
  2. BUILD: native libcurl, optional WinSCP Windows fallback, Linux notes.
  3. Brief: tick acceptance items that are actually met; do not claim false green.
  4. Keep doc edits scoped to transfer/CLI/Linux (avoid racing unrelated README sections).
  5. Ensure this process file remains the durable execution record (`docs/process-native-ftp-ship.md`).
- **Acceptance:**
  - README documents CLI flags, Linux without WinSCP for core transfer, host-key/security expectations, roadmap #9/#5/#4/#7 progress.
  - Plan/ship notes under `docs/` satisfied by this file + brief updates.
- **Pipeline checks (after_each + batch):**
  - P8 full batch gate: complete `08-batch-acceptance.md` against brief acceptance list.
  - Cross-read CLI flags in README vs `main.c` actual flags.

---

## Diagnose/Fix policy

1. **Reproduce first** using the smallest failing P-step or track acceptance bullet; capture log under `qa/native-ftp/`.
2. **Classify:**
   - *Build/link* → CMake / curl / libssh2 / `STELLI_USE_LIBCURL` (cmake-libcurl, linux-no-winscp).
   - *API/ABI* → do not change public `ftp_*` signatures; fix native bodies or dispatch only.
   - *Security regression* → block ship; fix native path before docs green.
   - *Fallback confusion* → ensure native preferred; WinSCP only optional Windows.
   - *Semantic drift* (masks/recursive) → document + test; do not silently reintroduce WinSCP-only scraping on native path.
3. **Hotspot ownership:** serialize `ftp_manager.c` and `main.c` merges; rebase rather than parallel overwrite.
4. **No foundation redo:** if WinSCP scripting, PE patch, or upload_daemon job model appears “easier to rewrite,” stop—extend via dispatch and native module instead.
5. **Scope lock:** excluded roadmap items stay out; open a separate ship for them.
6. **Fix loop:** change → after_each checks for owning track → re-run affected P-steps → append `track-notes.md`.
7. **Blocked:** set track Status `blocked` with reason and unblocker; do not mark Done criteria while security or Release build is red.

---

## Done criteria

Ship is done only when **all** of the following hold:

1. All tracks Status `done` (none `pending` / `in_progress` / `blocked`).
2. Windows Release build succeeds with native backend and `STELLI_USE_LIBCURL` (P1).
3. Public `ftp_*` API unchanged for UI / `upload_daemon` / `main` consumers (P6).
4. CLI `--ftp-*` flags present and documented (P3 + docs track).
5. Linux does not hard-require WinSCP / `resources.rc` (P5).
6. Native path: no password on argv; no plaintext password temp scripts; host-key/known_hosts supported (P4 + transfer-security).
7. Native preferred; WinSCP optional Windows fallback only (P7).
8. README + BUILD updated for native transfer, CLI, security, roadmap #9/#5/#4/#7 (P8).
9. Batch artifacts present under `qa/native-ftp/`, including `08-batch-acceptance.md` with brief acceptance list checked honestly.
10. No excluded-scope rewrites landed as part of this ship.

---

## Verification log

| Date | Agent/track | Result | Artifact / note |
|------|-------------|--------|-----------------|
| 2026-08-07 | cmake-libcurl | done — Release build + FTP/SFTP libcurl + STELLI_USE_LIBCURL | `qa/native-ftp/01-windows-release-build.log`, `02-link-symbols.txt`, `06-linux-or-unix-configure.log` (P5 deferred live) |
| 2026-08-07 | QA light after_each (cmake-libcurl) | PASS scoped — BUILD_EXIT=0; ctest N/A; other tracks FAIL/static | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `02-link-symbols.txt`; `03-cli-ftp-help.txt` (no --ftp-*); `05-no-password-argv.txt` (stub only); `07-ftp-api-consumers.txt`; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP; ship Done criteria NOT met |
| 2026-08-07 | ftp-native-core | done — libcurl get/put/batch/list/cancel in ftp_native_*; public ftp_* unchanged | Release build OK; `02-link-symbols.txt`, `05-no-password-argv.txt` PASS native; `07-ftp-api-consumers.txt` |
| 2026-08-07 | QA light after_each (ftp-native-core) | PASS scoped — BUILD_EXIT=0; CTEST_SKIP=1; cmake-libcurl+ftp-native-core ok; dispatch/security/advanced/cli/linux/docs still FAIL | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `02-link-symbols.txt` OBJ_STRING_OK; `03-cli-ftp-help.txt` MAIN_MISS --ftp-*; `05-no-password-argv.txt` RESULT_NATIVE_PATH PASS; `07-ftp-api-consumers.txt` ABI PASS/dispatch FAIL; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP=1; ship Done criteria NOT met |
| 2026-08-07 | ftp-backend-dispatch | done — core ftp_* prefer native; WinSCP Windows-only optional fallback; dep soft-skip non-Win | Release BUILD_EXIT=0; `07-ftp-api-consumers.txt` PASS dispatch; `track-notes.md` P7 order |
| 2026-08-07 | QA light after_each (ftp-backend-dispatch) | PASS scoped — BUILD_EXIT=0; CTEST_SKIP=1; cmake-libcurl+ftp-native-core+ftp-backend-dispatch+linux-no-winscp(structural) ok; security/advanced/cli/docs FAIL | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `02-link-symbols.txt`; `05-no-password-argv.txt` RESULT_P4_NATIVE PASS / HOST_KEY FAIL; `07-ftp-api-consumers.txt` DISPATCH PASS; `03-cli-ftp-help.txt` MAIN_MISS --ftp-*; `06-linux-or-unix-configure.log` structural; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP=1; ship Done criteria NOT met |
| 2026-08-07 | transfer-security | done — known_hosts/HOST_KEY_PIN/policy; no native argv/TEMP passwords; WinSCP legacy docs | Release BUILD_EXIT=0; `05-no-password-argv.txt` P4+HOST_KEY PASS; `config/ftp.ini.example` keys; `ftp_native_reload_security_config` additive |
| 2026-08-07 | QA light after_each (transfer-security) | PASS scoped — host-key + P4 green; advanced/cli/docs still FAIL | `05-no-password-argv.txt` RESULT_TRANSFER_SECURITY_TRACK PASS; `08-batch-acceptance.md`; ship Done criteria NOT met |
| 2026-08-07 | QA light after_each re-run (transfer-security) | PASS scoped — BUILD_EXIT=0; CTEST_SKIP=1; P4+HOST_KEY PASS; advanced/cli/docs FAIL | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `05-no-password-argv.txt` RESULT_TRANSFER_SECURITY_TRACK PASS; `07-ftp-api-consumers.txt` ADV FAIL; `03-cli-ftp-help.txt` MAIN_MISS --ftp-*; `08-batch-acceptance.md`; ship Done criteria NOT met |
| 2026-08-07 | ftp-native-advanced | done — recursive/core/upload_dir/cleanup/verify/restore via native; no WinSCP scrape | Release BUILD_EXIT=0; `07-ftp-api-consumers.txt` ADV PASS; track-notes semantic size-mask note |
| 2026-08-07 | QA light after_each (ftp-native-advanced) | PASS scoped — BUILD_EXIT=0; CTEST_SKIP=1; advanced native+dispatch green; cli/docs FAIL | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `02-link-symbols.txt` ADV OBJ_STRING_OK; `07-ftp-api-consumers.txt` RESULT_ADVANCED_NATIVE PASS; `05-no-password-argv.txt` PASS; `03-cli-ftp-help.txt` MAIN_MISS --ftp-*; `10-docs-scan.txt` docs FAIL; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP=1; ship Done criteria NOT met |
| 2026-08-07 | cli-standalone-ftp | done — --ftp-list/download/upload + --remote/--local/--dry-run; config-driven; headless | Release BUILD_EXIT=0; `03-cli-ftp-help.txt` PASS; `04-cli-ftp-smoke.txt` dry-run EXIT0; `05` CLI P4 PASS; heap RemoteFileBrowser |
| 2026-08-07 | QA light after_each (cli-standalone-ftp) | PASS scoped — CLI flags + dry-run smoke green; docs-readme-ship still FAIL | `03-cli-ftp-help.txt` PASS; `04-cli-ftp-smoke.txt` PASS; `05-no-password-argv.txt` RESULT_CLI_P4 PASS; ship Done criteria NOT met (docs track pending) |
| 2026-08-07 | QA light after_each re-run (cli-standalone-ftp) | PASS scoped — BUILD_EXIT=0; CTEST_SKIP=1; P3 dry-run 3/3 EXIT=0 + native backend log; P4/CLI PASS; docs FAIL | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `02-link-symbols.txt` OBJ_CORE/ADV PASS; `03-cli-ftp-help.txt` RESULT_P3=PASS; `04-cli-ftp-smoke.txt` RESULT_SMOKE=PASS RESULT_LOG_EVIDENCE=PASS; `05-no-password-argv.txt` RESULT_TRANSFER_SECURITY_TRACK PASS; `06-linux-or-unix-configure.log` structural PASS live DEFERRED; `07-ftp-api-consumers.txt` DISPATCH+ADV PASS; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP=1; `10-docs-scan.txt` docs-readme-ship FAIL; ship Done criteria NOT met |
| 2026-08-07 | linux-no-winscp | done — Linux live configure no WinSCP; native required off Windows; dep soft-skip; Windows Release OK | `qa/native-ftp/06-linux-or-unix-configure.log` P5 LIVE PASS; `01-windows-release-build.log` BUILD_EXIT=0; BUILD.md Linux transfer table |

| 2026-08-07 | QA light after_each (linux-no-winscp) | PASS scoped — BUILD_EXIT=0; CTEST_SKIP=1; P5 LIVE CMAKE_CONFIGURE_EXIT=0; linux-no-winscp PASS; docs FAIL | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `02-link-symbols.txt` OBJ_CORE/ADV PASS; `03-cli-ftp-help.txt` RESULT_P3=PASS; `04-cli-ftp-smoke.txt` 3/3 EXIT=0 backend=native; `05-no-password-argv.txt` PASS; `06-linux-or-unix-configure.log` RESULT_LINUX_NO_WINSCP_TRACK=PASS; `07-ftp-api-consumers.txt` DISPATCH+ADV PASS; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP=1; `10-docs-scan.txt` docs-readme-ship FAIL; ship Done criteria NOT met |
| 2026-08-07 | docs-readme-ship | done — README CLI/security/Linux/roadmap #9/#5/#4/#7; brief checked; P8 batch | `README.md`; `docs/NATIVE_FTP_SHIP_BRIEF.md`; `docs/BUILD.md`; `qa/native-ftp/08-batch-acceptance.md`; `10-docs-scan.txt` |
| 2026-08-07 | QA light after_each (docs-readme-ship) | **PASS** — BUILD_EXIT=0; CTEST_SKIP=1; P3 dry-run EXITS=0,0,0 backend=native; P4+HOST_KEY PASS; P5 LIVE prior CMAKE_CONFIGURE_EXIT=0; P8 RESULT_DOCS_README_SHIP=PASS; all 8 tracks ok; ship Done criteria **met** | `qa/native-ftp/01-windows-release-build.log` BUILD_EXIT=0; `02-link-symbols.txt` RESULT_LINK=PASS; `03-cli-ftp-help.txt` RESULT_P3=PASS; `04-cli-ftp-smoke.txt` RESULT_SMOKE=PASS RESULT_LOG_EVIDENCE=PASS; `05-no-password-argv.txt` RESULT_TRANSFER_SECURITY_TRACK=PASS; `06-linux-or-unix-configure.log` RESULT_LINUX_NO_WINSCP_TRACK=PASS; `07-ftp-api-consumers.txt` ABI+DISPATCH+ADV PASS; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP=1; `10-docs-scan.txt` RESULT_DOCS_README_SHIP=PASS; `11-protocol-cache.txt` RESULT_PROTOCOL=PASS |

| 2026-08-07 16:29:07 | QA FULL batch (all tracks) | **PASS** — BUILD_EXIT=0; CTEST_SKIP=1; P3 dry-run EXITS=0,0,0 backend=native libcurl; P4 CreateProcess_code=0 + HOST_KEY PASS; P5 LIVE prior CMAKE_CONFIGURE_EXIT=0; P8 RESULT_DOCS_README_SHIP=PASS; all 8 tracks ok; ship Done criteria **met** | `qa/native-ftp/pipeline_report.md`; `pipeline_report.json`; `01-windows-release-build.log` BUILD_EXIT=0; `04-cli-ftp-smoke.txt` RESULT_SMOKE=PASS RESULT_LOG_EVIDENCE=PASS; `05-no-password-argv.txt` PASS; `06-linux-or-unix-configure.log` PASS; `08-batch-acceptance.md`; `09-ctest.txt` CTEST_SKIP=1; `10-docs-scan.txt` PASS |

### Batch acceptance checklist (copy into `qa/native-ftp/08-batch-acceptance.md` at batch)

- [x] Build Release on Windows with native backend compiled in
- [x] `ftp_*` API still used by existing UI code paths
- [x] CLI `--ftp-list` or `--ftp-download` (and flags) documented in README
- [x] Linux build does not hard-require WinSCP/`resources.rc`
- [x] No password in CreateProcess command line on native path
- [x] Plan file written under `docs/` (this file)
- [x] Host-key / known_hosts on native path
- [x] No plaintext password temp scripts on native path
- [x] WinSCP optional Windows fallback only (not required on Linux)

---

## Track order (machine-readable)

```
cmake-libcurl
ftp-native-core
ftp-backend-dispatch
transfer-security
ftp-native-advanced
cli-standalone-ftp
linux-no-winscp
docs-readme-ship
```

**pipeline_mode:** `both`  
**qa_dir:** `qa/native-ftp`  
**plan_path:** `docs/process-native-ftp-ship.md`
