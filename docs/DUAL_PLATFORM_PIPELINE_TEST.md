# Dual-platform headless pipeline test + adaptive fix

**Ship name:** dual-platform-pipeline-test  
**Workflow:** `implement-from-plan` (universal)  
**QA dir:** `qa/dual-platform/`  
**Baseline snapshot (pre-test remote restore pack):** `qa/dual-platform/baseline/`  
**Auto-approve plan:** yes  

## Goal

Run a **full headless pipeline test on both Alpine Linux (AlpineWSL) and Windows**, **restore server files from the baseline snapshot between platforms and after the last platform**, and **fix anything that breaks**. Be adaptive: if a step fails, diagnose, fix code or harness, re-run that platform’s cycle, then continue.

This is a **debug + verify** job, not a feature redesign. Prefer smallest correct fixes. Keep the public CLI surface (`sfa` verbs) intact.

## Constraints (hard)

1. **CLI only — never open the GUI.**  
   - Use `sfa.cmd` / `sfa` / simple verbs (`help`, `list`, `pull`, `pipeline`, `push`, `restore`, `run`) or long flags (`--ftp-*`, `--regen`, `--headless`).  
   - **Do not** launch bare `StelliferumAuditor.exe` / binary with **no args** (that opens raylib/ImGui GUI).  
   - Prefer quiet CLI (`util_setup_console_quiet` path). Kill any stray `StelliferumAuditor` GUI processes before tests.

2. **Between each platform test and after the suite:** restore remote economy files from `qa/dual-platform/baseline/` via native upload (`--ftp-upload` or equivalent), **not** by assuming `sfa restore` has a useful restore point unless you create one first.

3. **Path quoting:** project lives under `D:\STELLIROS_WORKSHOP\DayZ Server\...` (space in path).  
   - Prefer **relative paths** from repo root for `--local`.  
   - On Windows PowerShell, use `ProcessStartInfo.ArgumentList` (or carefully quoted args) — do **not** pass unquoted paths through stringy `Start-Process -ArgumentList` that splits on spaces.

4. **WSL distro name is `AlpineWSL`**, not `Alpine`.  
   - Binary: `build-alpine/bin/StelliferumAuditor` (ELF musl).  
   - Run with `wsl -d AlpineWSL` from repo root (or mount path `/mnt/d/STELLIROS_WORKSHOP/DayZ Server/StelliferumAuditor`).  
   - Credentials: `config/ftp.ini` (gitignored) must be readable; do not log passwords.

5. **Do not rewrite** the native FTP public ABI or rename `sfa` verbs. Extend/fix only.

6. **Credentials never on argv.** Use `config/ftp.ini` only.

## Baseline (already taken — re-verify sizes before first push)

| Local file under `qa/dual-platform/baseline/` | Expected |
|---|---|
| `types.xml` | ~2.3MB non-zero |
| `cfgspawnabletypes.xml` | ~2.2MB non-zero |
| `cfgeconomycore.xml` | non-zero |
| `cfglimitsdefinitionuser.xml` | non-zero |
| `cfgrandompresets.xml` | ~34KB non-zero |
| `TraderConfig.txt` | ~150KB non-zero |

Remote roots (from `config/server_paths.ini`):

- `REMOTE_ROOT=/104.192.226.196_2322/`
- Types: `mpmissions/dayzOffline.chernarusplus/db/types.xml`
- Spawnable: `mpmissions/dayzOffline.chernarusplus/cfgspawnabletypes.xml`
- Economy core / limits / random presets: under mission root
- Trader: `profiles/Trader/TraderConfig.txt`

If any baseline file is 0-byte or missing, **re-snap** with properly quoted `--ftp-download` before changing the server.

## Restore helper (required)

Implement a small durable restore step (script or documented command sequence under `qa/dual-platform/`) that:

1. Uploads each baseline file to its remote path with `--ftp-upload --local <rel> --remote <full>`.  
2. Logs exit codes and sizes to `qa/dual-platform/restore-*.log`.  
3. Optionally re-downloads one key file (`types.xml`) and checks size matches baseline.

Call this restore:

- Before Alpine test (if server may have drifted)  
- After Alpine push/verify, **before** Windows test  
- After Windows push/verify (leave server on baseline)

## Platform cycle (each of Alpine then Windows)

For **platform P**:

1. **Build/refresh binary if needed**  
   - Alpine: rebuild in AlpineWSL if sources newer than `build-alpine/bin/StelliferumAuditor`.  
   - Windows: ensure `build/bin/Release/StelliferumAuditor.exe` builds (`cmake --build` Release).

2. **pipeline** (local regen only)  
   - Windows: `.\sfa.cmd pipeline` or `StelliferumAuditor.exe pipeline` from repo root.  
   - Alpine: run binary with `pipeline` cwd = repo root so `config/` and `downloaded_mods/` resolve.  
   - Evidence: session log + `output/types.xml` size; write `qa/dual-platform/<platform>-pipeline.log`.

3. **push**  
   - `sfa push` / binary `push` (native batch upload of output economy pack).  
   - Evidence: exit 0, “push complete: N ok, 0 fail”.

4. **pull-verify**  
   - Download remote `types.xml` (and optionally spawnable) to `qa/dual-platform/<platform>-verify/`.  
   - Compare **size and SHA256** to local `output/types.xml`.  
   - Acceptance: size match + SHA match + push exit 0 + pull exit 0.

5. **restore baseline** to server (always, even on failure after a push).

6. If any step fails: **diagnose → fix → re-run cycle for that platform** (still restore between attempts that pushed). Cap reasonable fix rounds but do not stop after first flake if the fix is clear.

## Order

1. Confirm baseline integrity  
2. AlpineWSL full cycle + restore  
3. Windows full cycle + restore  
4. Write `qa/dual-platform/batch-acceptance.md` with pass/fail table  
5. Local visual/perf for this workflow: **CLI smoke only** is acceptable (help/list dry; **do not** require opening GUI). Note in report that GUI was intentionally skipped for headless ship verification. Optional: one quiet `--ftp-list` smoke.

## Done criteria

- [x] Alpine: pipeline exit 0, push exit 0, pull size+SHA match for `types.xml`  
- [x] Server restored to baseline after Alpine  
- [x] Windows: pipeline exit 0, push exit 0, pull size+SHA match for `types.xml`  
- [x] Server restored to baseline after Windows  
- [x] No GUI launched during automated tests  
- [x] Any breakage found is fixed or documented with a clear remaining blocker  
- [x] Evidence under `qa/dual-platform/` (logs, verify copies, batch-acceptance.md)

**Status:** COMPLETE — see `docs/process-dual-platform-pipeline-test.md` and `qa/dual-platform/batch-acceptance.md`.

## Out of scope

- Full UI overhaul  
- Expansion Market export  
- Re-shipping multi-track loot features (already completed in multi-track-ship-2)  
- Changing public FTP ABI or sfa verb names  

## Context from prior work

- Multi-track-ship-2 (roadmap-loot) COMPLETE: P1–P8 green on Windows; push/pull verified once.  
- Prior pull harness bug: unquoted local path with space → `FTP CLI: unknown argument: Server\...` — fixed by ArgumentList/relative paths.  
- Alpine distro: `AlpineWSL`. Old binary may be stale morning build — rebuild if needed.  
- `sfa` with no args → interactive shell (OK); bare exe no args → GUI (BAD for CI).

## Test commands (hints)

```text
# Windows
.\sfa.cmd help
.\sfa.cmd pipeline
.\sfa.cmd push
# download with relative local:
build\bin\Release\StelliferumAuditor.exe --ftp-download --remote /104.192.226.196_2322/mpmissions/dayzOffline.chernarusplus/db/types.xml --local qa/dual-platform/windows-verify/types.xml

# AlpineWSL
wsl -d AlpineWSL -- sh -lc 'cd "/mnt/d/STELLIROS_WORKSHOP/DayZ Server/StelliferumAuditor" && ./build-alpine/bin/StelliferumAuditor pipeline'
wsl -d AlpineWSL -- sh -lc 'cd "/mnt/d/STELLIROS_WORKSHOP/DayZ Server/StelliferumAuditor" && ./build-alpine/bin/StelliferumAuditor push'
```

## Risks

- Live FTP against production DayZ host — always restore baseline; do not leave partial broken economy if avoidable.  
- Pipeline cleans `output/` — never store baseline only under `output/`.  
- Concurrent `StelliferumAuditor` processes fight for console/FTP — kill strays first.  
- Alpine musl build may lag Windows code after multi-track-ship — rebuild Alpine before Alpine cycle.
