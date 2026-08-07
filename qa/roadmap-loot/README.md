# QA artifacts — roadmap-loot ship

Shared QA directory for `docs/process-roadmap-loot-ship.md`.

**Last FULL batch:** 2026-08-07 20:38:15 — **PASS**

## Canonical artifacts

| # | Path | Purpose |
|---|------|---------|
| 01 | `01-release-build.log` | Release build |
| 01b | `01b-ctest.txt` | ctest (or none) |
| 02 | `02-tier-policy-evidence.txt` | Tier rename / BM policy greps |
| 03 | `03-sfa-pipeline.log` | sfa pipeline / --regen session |
| 04 | `04-distribution-sanity.txt` | Tier labels / density samples |
| 05 | `05-config-validation-smoke.txt` | Soft config validation |
| 06 | `06-container-policy.txt` | Container policy stub |
| 07 | `07-readme-scan.txt` | README / roadmap consistency |
| 08 | `08-sfa-push.log` | Native FTP push |
| 09 | `09-pull-verify.txt` | Pull types size/hash |
| 10 | `10-sfa-verbs-smoke.txt` | Public sfa verbs |
| — | `batch-acceptance.md` | Batch gate checklist |
| — | `track-notes.md` | Per-track notes |
| — | `pipeline_report.md` / `.json` | Full pipeline report |
| — | `pulled/types.xml` | Pull-back of remote types |

## Notes

WIN32 GUI subsystem often freopen's stdout to CONOUT — use `.TEMP/auditor_crash_log.txt` as CLI evidence source of truth. Isolate sessions (delete log before each invoke) to avoid concurrent-process races.