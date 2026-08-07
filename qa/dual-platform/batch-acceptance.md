# Dual-platform pipeline test — batch acceptance

**Ship:** dual-platform-pipeline-test  
**When:** 2026-08-07  
**Mode:** CLI-only headless (GUI intentionally skipped)  
**Server end state:** 6-file baseline restored (`restore-post-windows.log` ALL_OK)

## Pass/fail table

| Platform | pipeline | push | pull size | pull SHA | restore | overall |
|---|---|---|---|---|---|---|
| AlpineWSL | PASS exit 0 | PASS `9 ok, 0 fail` | PASS 2267091==2267091 | PASS FFFE3825… | PASS all ec=0 | **PASS** |
| Windows | PASS exit 0 | PASS `9 ok, 0 fail` | PASS 2331636==2331636 | PASS FF0F5368… | PASS all ec=0 | **PASS** |

## Binary paths / builds

| Platform | Binary | Notes |
|---|---|---|
| AlpineWSL | `build-alpine/bin/StelliferumAuditor` | Rebuilt (sources newer): Ninja Release, size ~3407984, mtime 2026-08-07 ~21:13 |
| Windows | `build/bin/Release/StelliferumAuditor.exe` | Fresh enough (mtime ≥ sources); size 28905984; no rebuild this run |

## Evidence paths (`qa/dual-platform/`)

| Artifact | Purpose |
|---|---|
| `01-preflight-cli.txt` | Kill strays, CLI smoke, AlpineWSL readiness |
| `02-baseline-check.txt` | Six baseline files size-aligned with resnap |
| `00-baseline-resnap.txt` | Original resnap sizes |
| `baseline/*` | Durable 6-file restore pack (never sole under `output/`) |
| `restore-baseline.ps1` / `restore-baseline.sh` | Restore harness |
| `restore-pre-alpine.log` | Pre-Alpine restore |
| `alpine-pipeline.log` | Alpine pipeline |
| `alpine-push.log` | Alpine push (+ re-push for verify) |
| `alpine-pull-verify.txt` | Alpine types size+SHA |
| `alpine-verify/types.xml` | Pulled remote types after Alpine push |
| `restore-post-alpine.log` | Post-Alpine restore |
| `windows-pipeline.log` | Windows pipeline |
| `windows-push.log` | Windows push |
| `windows-pull-verify.txt` | Windows types size+SHA |
| `windows-verify/types.xml` | Pulled remote types after Windows push |
| `restore-post-windows.log` | Post-Windows restore (server left here) |
| `final-cli-visual.txt` | `sfa help` smoke |
| `final-cli-perf.txt` | `sfa list` smoke + wall time |
| `batch-acceptance.md` | This report |

## Fix log (smallest diffs)

1. **Restore harness (new):** `qa/dual-platform/restore-baseline.ps1` + `restore-baseline.sh`  
   - Uploads 6 baseline files via `--ftp-upload` with **relative** `--local` and full `--remote`.  
   - Logs exit codes + sizes; optional types re-download size check.  
   - PowerShell 5.1: `ProcessStartInfo.ArgumentList` is null on .NET Framework — fallback to carefully quoted `Arguments` string (relative paths remain space-free).

2. **Alpine pull-verify quoting flake:** Nested PowerShell → `wsl -d AlpineWSL -- sh -lc "cd \"...\DayZ Server...\""` broke on path space.  
   - Fix: re-push Alpine once, then download with **Windows** binary + relative `--local` for evidence (process allows relative paths).  
   - No product code change; harness/verify operational fix only.

3. **Alpine rebuild:** Sources newer than musl binary → `cmake --build build-alpine` succeeded.

No public CLI surface / FTP ABI renames. No sfa verb renames.

## Hybrid leave-behind note

`sfa push` uploads **9** core files (includes SFL + zombie tier XMLs). Restore pack covers **only** the documented 6 economy/trader files. Remote SFL/zombie may remain at last push state (hybrid leave-behind accepted for this ship).

## Cross-platform observation (not a fail)

| | Alpine `output/types.xml` | Windows `output/types.xml` |
|---|---|---|
| Size | 2267091 | 2331636 |
| SHA256 | FFFE3825C52A8E7B7E6B6E237ACA22988A12249A1C8168C28FE5E39D82312E2A | FF0F53680792DD03CE0AC072164093A0FD3D745D25288B587403E10D63DF1342 |

Acceptance is **per-platform** size+SHA of remote vs that platform’s local `output/types.xml` after its own pipeline+push — both matched. Cross-platform output parity is out of scope for this ship.

## BLOCKED items

None.

## CLI-only / GUI

GUI intentionally skipped for this headless ship. All automation used `sfa.cmd` / simple verbs / long flags / Alpine binary with args — never bare `StelliferumAuditor` with no args.
