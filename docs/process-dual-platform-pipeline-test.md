# Implementation Process

**Ship:** dual-platform-pipeline-test  
**Plan path:** `docs/process-dual-platform-pipeline-test.md`  
**Brief / source of truth:** `docs/DUAL_PLATFORM_PIPELINE_TEST.md`  
**QA dir:** `qa/dual-platform/`  
**Baseline pack:** `qa/dual-platform/baseline/` (6 economy/trader files)  
**Created:** 2026-08-07  
**Auto-approve plan:** yes  

Agents execute steps in order. Update each step **Status** as work progresses (`pending` → `in_progress` → `done` / `blocked`). Prefer smallest correct fixes. Keep public CLI surface intact. **CLI only** — never launch bare `StelliferumAuditor` with no args (opens GUI).

---

## Goal

Ship dual-platform adaptive headless debug+verify of **pipeline → push → pull size+SHA** on **AlpineWSL first**, then **Windows**, with durable baseline restore of `qa/dual-platform/baseline/*` to live FTP before Alpine, between platforms, and after Windows. Implement restore harness + evidence under `qa/dual-platform/`; fix breakage; leave server on the 6-file baseline pack. CLI-only via `sfa` / `sfa.cmd` or flagged binary.

---

## Sources

- `docs/DUAL_PLATFORM_PIPELINE_TEST.md`
- `docs/process-roadmap-loot-ship.md` (prior green Windows context; do not re-ship loot features)
- `docs/BUILD.md`
- `qa/dual-platform/00-baseline-resnap.txt`
- `qa/dual-platform/workflow-watch.txt`
- `qa/roadmap-loot/09-pull-verify.txt` (prior pull size/SHA pattern)
- `sfa.cmd` / `sfa`
- `config/server_paths.ini`
- `config/ftp.ini` (gitignored credentials only — never log/argv)
- `src/cli_commands.c`, `src/main.c`, `include/cli_commands.h`
- Binaries: `build-alpine/bin/StelliferumAuditor`, `build/bin/Release/StelliferumAuditor.exe`

---

## Context

Prior multi-track-ship-2 already green on Windows (`types.xml` size **2331636** + SHA verified). Baseline resnap intact under `qa/dual-platform/baseline/` (see `00-baseline-resnap.txt`). Binaries present on disk; Alpine musl may lag sources and need rebuild before cycle. Pipeline cleans/regenerates `output/` — baseline must stay under `qa/dual-platform/baseline/`. `sfa push` may upload more files (e.g. SFL/zombie) than the 6-file restore pack; restore **only** the documented 6-file baseline (hybrid remote leave-behind for non-baseline files is accepted). `sfa restore` uses `backups/*/restore_manifest.txt` and is **not** the primary baseline restore unless a dedicated restore point is created first.

Remote roots (from `config/server_paths.ini`):

| Local baseline file | Remote (under `REMOTE_ROOT=/104.192.226.196_2322/`) |
|---|---|
| `types.xml` | `mpmissions/dayzOffline.chernarusplus/db/types.xml` |
| `cfgspawnabletypes.xml` | `mpmissions/dayzOffline.chernarusplus/cfgspawnabletypes.xml` |
| `cfgeconomycore.xml` | `mpmissions/dayzOffline.chernarusplus/cfgeconomycore.xml` |
| `cfglimitsdefinitionuser.xml` | `mpmissions/dayzOffline.chernarusplus/cfglimitsdefinitionuser.xml` |
| `cfgrandompresets.xml` | `mpmissions/dayzOffline.chernarusplus/cfgrandompresets.xml` |
| `TraderConfig.txt` | `profiles/Trader/TraderConfig.txt` |

Expected baseline sizes (from resnap): types **2331636**, spawnable **2198232**, economy core **2049**, limits **2902**, random presets **34615**, TraderConfig **153737**.

---

## Constraints

- **CLI only:** use `sfa.cmd` / `sfa` / simple verbs (`help|list|pull|pipeline|push|restore|run`) or long flags (`--ftp-*`, `--regen`, `--headless`); never launch bare `StelliferumAuditor` with no args (GUI). Prefer quiet CLI path; kill stray GUI processes before tests.
- **Restore:** between platforms and after suite, restore remote economy from `qa/dual-platform/baseline/` via native `--ftp-upload` (or harness wrapping it). Do not assume `sfa restore` is useful unless a restore point was created first.
- **Path with space** (`DayZ Server`): prefer relative `--local` paths from repo root; on PowerShell use `ProcessStartInfo.ArgumentList` / careful quoting — never unquoted stringy `Start-Process` that splits on spaces.
- **WSL distro name is `AlpineWSL`** (not `Alpine`); binary `build-alpine/bin/StelliferumAuditor`; run with `wsl -d AlpineWSL` and cwd = repo root (`/mnt/d/STELLIROS_WORKSHOP/DayZ Server/StelliferumAuditor`).
- **Credentials** only from `config/ftp.ini`; never put passwords on argv or in logs.
- **Do not** rewrite native FTP public ABI or rename `sfa` verbs; extend/fix only; keep public CLI surface intact.
- **Debug+verify:** prefer smallest correct fixes; re-run failed platform cycle after fix (cap ~2–3 diagnose→fix→re-run rounds per platform unless a clear fix remains); **restore after any push even on failure**.
- **Order:** confirm baseline integrity → Alpine full cycle+restore → Windows full cycle+restore → `qa/dual-platform/batch-acceptance.md`. CLI smoke only (GUI intentionally skipped).
- **Acceptance per platform:** pipeline exit 0; push exit 0 (`push complete: N ok, 0 fail`); pull `types.xml` size+SHA256 match vs local `output/types.xml`; server restored to baseline after each platform and at end.
- **Evidence** under `qa/dual-platform/` only for this ship (logs, verify dirs, `restore-*.log`, `batch-acceptance.md`); do not store sole baseline under `output/`.
- **Out of scope:** full UI overhaul, Expansion Market export, re-shipping multi-track loot features, changing public FTP ABI / sfa verb names.
- **Restore pack scope:** only the 6 economy/trader baseline files — do **not** also baseline-restore SFL/zombie files that push may have changed.

---

## Steps

### Step 1: Preflight — kill strays, CLI smoke, tool readiness
- Status: done
- Notes: NO_STRAYS; Win exe + Alpine musl present; ftp.ini present; sfa help/list exit 0; AlpineWSL ok with cd to repo. Evidence: `qa/dual-platform/01-preflight-cli.txt`.
- Actions:
  - From repo root, kill any stray `StelliferumAuditor` GUI/console processes contending for console/FTP.
  - Confirm Windows binary path: `build/bin/Release/StelliferumAuditor.exe` (or rebuild later if missing).
  - Confirm Alpine binary path on disk: `build-alpine/bin/StelliferumAuditor` (rebuild later if stale).
  - Confirm `config/ftp.ini` exists (do not print secrets).
  - Confirm `config/server_paths.ini` `REMOTE_ROOT` and remote relative paths match the baseline map above.
  - CLI smoke (never bare exe): `.\sfa.cmd help` then `.\sfa.cmd list` (or quiet `--ftp-list` if `list` needs network). Capture brief evidence to `qa/dual-platform/01-preflight-cli.txt`.
  - Verify AlpineWSL: `wsl -d AlpineWSL -- sh -lc 'uname -a; which cmake ninja cc 2>/dev/null; ls -la build-alpine/bin/StelliferumAuditor'`. If distro/toolchain missing, document BLOCKED with evidence — do not invent PASS.
- Acceptance: No stray auditor processes; CLI help works; ftp.ini present; AlpineWSL reachable or documented blocker; preflight log written.
- Verify: Inspect `qa/dual-platform/01-preflight-cli.txt`; exit codes of help/list; `wsl -d AlpineWSL` status.

### Step 2: Confirm baseline integrity (and re-snap only if broken)
- Status: done
- Notes: All 6 files match resnap sizes exactly (types 2331636 etc.). Evidence: `qa/dual-platform/02-baseline-check.txt`.
- Actions:
  - Verify all six files exist under `qa/dual-platform/baseline/` with non-zero sizes matching `qa/dual-platform/00-baseline-resnap.txt` (±0 bytes preferred):
    - `types.xml` ≈ 2331636
    - `cfgspawnabletypes.xml` ≈ 2198232
    - `cfgeconomycore.xml` ≈ 2049
    - `cfglimitsdefinitionuser.xml` ≈ 2902
    - `cfgrandompresets.xml` ≈ 34615
    - `TraderConfig.txt` ≈ 153737
  - If any file is missing or 0-byte: re-snap with properly quoted/relative `--ftp-download` into `qa/dual-platform/baseline/` and update `00-baseline-resnap.txt`. Never store sole baseline under `output/`.
  - Write size checklist to `qa/dual-platform/02-baseline-check.txt`.
- Acceptance: All six baseline files non-zero and size-aligned with resnap; checklist written.
- Verify: File sizes on disk vs `00-baseline-resnap.txt`; read `02-baseline-check.txt`.

### Step 3: Implement durable baseline restore harness
- Status: done
- Notes: `restore-baseline.ps1` + `restore-baseline.sh`; PS5.1 uses quoted Arguments fallback when ArgumentList null.
- Actions:
  - Add a small durable restore helper under `qa/dual-platform/` (preferred: `restore-baseline.ps1` and/or `restore-baseline.sh`, or a single documented command sequence file that is executable by agents).
  - Helper must, from repo root:
    1. Upload each of the 6 baseline files via native CLI:  
       `StelliferumAuditor.exe --ftp-upload --local <rel> --remote <full>`  
       (or Alpine binary equivalent). Use **relative** `--local` paths (e.g. `qa/dual-platform/baseline/types.xml`). Full remote = `REMOTE_ROOT` + path from table (e.g. `/104.192.226.196_2322/mpmissions/dayzOffline.chernarusplus/db/types.xml`).
    2. Log each file’s exit code + local size to a caller-chosen log path (convention: `qa/dual-platform/restore-<tag>.log`).
    3. Optionally re-download remote `types.xml` to a temp/verify path and check size matches baseline `types.xml`.
  - PowerShell: use `ProcessStartInfo.ArgumentList` (or equivalent non-splitting argv) — never unquoted paths through stringy Start-Process.
  - Do not rename sfa verbs or change public FTP ABI; harness only wraps existing `--ftp-upload` / binary flags.
  - Smoke the helper once dry if possible; otherwise a single real restore is Step 4.
- Acceptance: Restore script/sequence exists under `qa/dual-platform/`; uploads all 6 files with relative paths; logs exit codes+sizes; path-space safe.
- Verify: Read harness file; dry-run or parse that argv construction uses ArgumentList/relative paths; no passwords in harness source.

### Step 4: Pre-Alpine restore server to baseline
- Status: done
- Notes: All 6 uploads ec=0; verify types size match 2331636. Log: `restore-pre-alpine.log`.
- Actions:
  - Run restore harness with tag `pre-alpine` → log `qa/dual-platform/restore-pre-alpine.log`.
  - Optional: size-check re-download of remote `types.xml` vs baseline.
  - If network/auth/host-key fails: mark BLOCKED with log evidence; do not invent PASS; do not proceed to push on either platform.
- Acceptance: All 6 uploads exit 0 (or documented BLOCKED); restore log present; server economy pack matches baseline before Alpine cycle.
- Verify: Read `restore-pre-alpine.log` for `ec=0` per file and non-zero sizes.

### Step 5: AlpineWSL platform cycle (rebuild → pipeline → push → pull-verify → restore)
- Status: done
- Notes: Rebuilt musl; pipeline exit 0 types=2267091; push 9 ok 0 fail; pull size+SHA match (FFFE3825…); restore-post-alpine ALL_OK. First WSL pull quoting flake fixed by relative --local + Windows download for evidence.
- Actions:
  - **Rebuild if needed:** If sources newer than `build-alpine/bin/StelliferumAuditor`, rebuild inside AlpineWSL (Ninja Release preferred per prior Alpine layout; follow `docs/BUILD.md` / existing `build-alpine` tree). Example pattern:  
    `wsl -d AlpineWSL -- sh -lc 'cd "/mnt/d/STELLIROS_WORKSHOP/DayZ Server/StelliferumAuditor" && cmake --build build-alpine --config Release -j$(nproc)'`  
    (adjust generator/dir if tree differs; do not invent APIs — inspect tree first).
  - **pipeline:**  
    `wsl -d AlpineWSL -- sh -lc 'cd "/mnt/d/STELLIROS_WORKSHOP/DayZ Server/StelliferumAuditor" && ./build-alpine/bin/StelliferumAuditor pipeline'`  
    Capture → `qa/dual-platform/alpine-pipeline.log`. Confirm exit 0 and `output/types.xml` non-zero.
  - **push:** same cwd, `./build-alpine/bin/StelliferumAuditor push`. Capture → `qa/dual-platform/alpine-push.log`. Require exit 0 and log line `push complete: N ok, 0 fail`.
  - **pull-verify:** download remote types to `qa/dual-platform/alpine-verify/types.xml` with relative `--local` and full `--remote`. Compare **size and SHA256** to local `output/types.xml`. Write `qa/dual-platform/alpine-pull-verify.txt` with sizes + hashes + match result.
  - **restore always** after any push (success or fail): harness tag `post-alpine` → `qa/dual-platform/restore-post-alpine.log`.
  - On failure: diagnose → smallest fix (code/harness/build) → re-run Alpine cycle (still restore if push occurred). Cap ~2–3 fix rounds unless a clear remaining fix is in progress.
- Acceptance: Alpine pipeline exit 0; push exit 0 with 0 fail; alpine-verify types size+SHA match `output/types.xml`; server restored (post-alpine restore log all ec=0); evidence files present. Or BLOCKED with evidence if infra/auth only.
- Verify: Logs under `qa/dual-platform/alpine-*.log`, `alpine-pull-verify.txt`, `restore-post-alpine.log`; SHA compare commands recorded.

### Step 6: Windows platform cycle (rebuild → pipeline → push → pull-verify → restore)
- Status: done
- Notes: pipeline exit 0 types=2331636; push 9 ok 0 fail; pull size+SHA match (FF0F5368…); restore-post-windows ALL_OK.
- Actions:
  - Confirm post-Alpine restore completed (Step 5) so server is baseline before Windows push.
  - **Rebuild if needed:** If sources newer than `build/bin/Release/StelliferumAuditor.exe`, run `cmake --build build --config Release` (or `build.bat` / project convention).
  - **pipeline:** from repo root `.\sfa.cmd pipeline` (or flagged Release exe with `pipeline`). Capture → `qa/dual-platform/windows-pipeline.log`. Exit 0; `output/types.xml` non-zero.
  - **push:** `.\sfa.cmd push`. Capture → `qa/dual-platform/windows-push.log`. Exit 0; `push complete: N ok, 0 fail`.
  - **pull-verify:**  
    `build\bin\Release\StelliferumAuditor.exe --ftp-download --remote /104.192.226.196_2322/mpmissions/dayzOffline.chernarusplus/db/types.xml --local qa/dual-platform/windows-verify/types.xml`  
    Compare size+SHA256 to `output/types.xml`. Write `qa/dual-platform/windows-pull-verify.txt`.
  - **restore always** after any push: harness tag `post-windows` → `qa/dual-platform/restore-post-windows.log`.
  - On failure: diagnose → smallest fix → re-run Windows cycle (restore if push occurred). Cap ~2–3 fix rounds.
- Acceptance: Windows pipeline/push exit 0; windows-verify types size+SHA match; server restored post-windows; evidence present. Or BLOCKED with evidence.
- Verify: `windows-pipeline.log`, `windows-push.log`, `windows-pull-verify.txt`, `restore-post-windows.log`.

### Step 7: Final server baseline confirmation + batch acceptance report
- Status: done
- Notes: Server left on 6-file baseline via post-windows restore; `qa/dual-platform/batch-acceptance.md` written.
- Actions:
  - Confirm last restore was post-Windows (or run harness tag `final` → `qa/dual-platform/restore-final.log` if uncertain). Optional types.xml re-download size check vs baseline.
  - Write `qa/dual-platform/batch-acceptance.md` with:
    - Pass/fail table per platform: pipeline, push, pull size, pull SHA, restore
    - Binary paths/build times used
    - Fix log (what broke, what changed — smallest diffs)
    - Note: GUI intentionally skipped (CLI-only headless ship)
    - Hybrid leave-behind note: SFL/zombie not in 6-file restore pack
    - Any BLOCKED items with evidence paths
  - Ensure all evidence lives under `qa/dual-platform/` only for this ship.
- Acceptance: Server left on 6-file baseline pack; `batch-acceptance.md` complete; no sole baseline under `output/`.
- Verify: Read batch-acceptance + final restore log; optional remote types size == baseline types size.

### Step 8: Local visual + performance final test (CLI smoke only)
- Status: done
- Notes: Local re-run 2026-08-07: sfa help exit 0 (~0.145s); sfa list exit 0 (~8.688s, 101 entries); no stray GUI.
- Actions:
  - **Visual (CLI):** `.\sfa.cmd help` — observe usage/verbs printed; no GUI window. Append output to `qa/dual-platform/final-cli-visual.txt`.
  - **Performance (CLI):** `.\sfa.cmd list` (or quiet `--ftp-list`) — measure wall time; expect clean exit and listing without hang; append timing + exit to `qa/dual-platform/final-cli-perf.txt`.
  - Do **not** require opening raylib/ImGui GUI for this ship.
- Acceptance: help and list (or ftp-list) exit successfully without launching GUI; artifacts written.
- Verify: Artifact files exist; process list shows no unexpected GUI auditor instance started by these commands.

---

## Done criteria

- [x] Alpine: pipeline exit 0, push exit 0, pull `types.xml` size+SHA match vs `output/types.xml`
- [x] Server restored to baseline after Alpine (`restore-post-alpine.log`)
- [x] Windows: pipeline exit 0, push exit 0, pull `types.xml` size+SHA match vs `output/types.xml`
- [x] Server restored to baseline after Windows (`restore-post-windows.log`) and left on baseline after suite
- [x] Restore harness under `qa/dual-platform/` uploads all 6 baseline files with relative paths + logs
- [x] No GUI launched during automated tests (CLI-only)
- [x] Breakage fixed with smallest correct changes, or documented BLOCKED with evidence (no invented PASS)
- [x] Evidence under `qa/dual-platform/` (logs, verify dirs, restore-*.log, batch-acceptance.md)
- [x] Public CLI surface / FTP ABI unchanged (extend/fix only)
- [x] Local visual test run on this machine (`sfa help` / CLI smoke)
- [x] Local performance test run on this machine (`sfa list` / CLI smoke with timing)

---

## Test strategy

1. **Preflight:** process kill, `sfa help`/`list`, AlpineWSL + binary presence, `ftp.ini` existence.
2. **Baseline integrity:** size-check six files vs `00-baseline-resnap.txt`; re-snap only if broken.
3. **Harness unit-ish:** restore helper path construction (relative locals, full remotes, ArgumentList) without secrets.
4. **Live dual-platform gate:** Alpine then Windows each do rebuild-if-needed → pipeline → push → pull size+SHA → restore; fix+re-run on failure (≤~3 rounds/platform).
5. **Batch acceptance:** `qa/dual-platform/batch-acceptance.md` pass/fail table.
6. **Mandatory final local visual + performance:** CLI smoke only (`sfa help`, `sfa list`) — GUI intentionally skipped.

---

## Local visual + performance final test

- Status: done
- Visual: From repo root run `.\sfa.cmd help`. Observe verb list/help text in console; confirm no raylib/ImGui window. Artifact: `qa/dual-platform/final-cli-visual.txt`.
- Performance: From repo root run `.\sfa.cmd list` (network list) or documented quiet `--ftp-list`; record wall-clock seconds and exit code. Pass bar: completes without hang/crash; exit 0 preferred (document network BLOCKED if auth fails). Artifact: `qa/dual-platform/final-cli-perf.txt`.
- Artifacts: `qa/dual-platform/final-cli-visual.txt`, `qa/dual-platform/final-cli-perf.txt`, plus suite logs already under `qa/dual-platform/`.
- Note: Runs on the developer’s LOCAL machine as the last automated gate before complete. GUI is intentionally out of scope for this headless ship.

---

## Verification log

- 2026-08-07 implementer: Steps 1–8 done. Alpine + Windows pipeline/push/pull-SHA green; restore harness + pre/post restores ALL_OK; server left on 6-file baseline. Evidence under `qa/dual-platform/`. Harness fix: PS5.1 ArgumentList null → quoted Arguments. Alpine pull evidence via Windows `--ftp-download` after WSL nested-quote flake.
- 2026-08-07 verifier: **PASS**. Spot-checked steps 1–8 vs disk: all claimed evidence present; baseline 6 files size-exact; restore pre/post-alpine/post-windows ALL_OK fail_count=0; alpine/windows pull-verify PASS size+SHA (recomputed SHA on verify trees match logs); harness relative `--local` + no secrets; alpine pipeline COMPLETE 39 ok 0 failed + push `9 ok, 0 fail`; windows pipeline/push headers exit=0 + push `9 ok, 0 fail`. No ctest suite in repo; preferred CLI unit check `.\sfa.cmd help` exit=0 (~163ms). No hard BLOCKED. Note: alpine-pipeline/push log headers omit explicit `exit=` (content proves success). Final visual/perf suite not re-run here (separate phase).
- 2026-08-07 local-visual-perf: **PASS** on this host. Commands: `.\sfa.cmd help` exit=0 wall_sec=0.145 no GUI/strays → `qa/dual-platform/final-cli-visual.txt`; `.\sfa.cmd list` exit=0 wall_sec=8.688 hang=False Listed 101 entries (native SFTP) no strays → `qa/dual-platform/final-cli-perf.txt`. Session logs via `.TEMP/auditor_crash_log.txt`. visual_passed=true perf_passed=true local_machine_confirmed=true.

---

## Risks (operator reminder)

- Live FTP against production DayZ host — always restore after push; do not leave partial broken core economy.
- Push may leave SFL/zombie at post-push state while core 6 files return to baseline.
- Pipeline cleans `output/` — baseline only under `qa/dual-platform/baseline/`.
- Path-space regression on `DayZ Server` path — relative `--local` + ArgumentList only.
- Alpine musl binary staleness — rebuild when sources newer.
- Concurrent auditor processes — kill strays first.
- Network/auth/host-key failures → document BLOCKED with evidence, never invent PASS.
