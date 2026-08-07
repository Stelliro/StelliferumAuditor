# QA: native-ftp (multi-track-ship)

Pipeline artifacts for native FTP/SFTP (libcurl) ship.

## Canonical artifacts

| File | Purpose |
|------|---------|
| `01-windows-release-build.log` | P1 Windows Release build summary (exit codes) |
| `01-windows-release-build-compile.log` | Raw MSBuild/cmake --build stream |
| `02-link-symbols.txt` | STELLI_USE_LIBCURL, libcurl/libssh2 link, WinSCP optional gate |
| `03-cli-ftp-help.txt` | CLI `--ftp-*` static/binary probe |
| `04-cli-ftp-smoke.txt` | Dry-run smoke + session log evidence |
| `04-cli-session-log-sample.txt` | Last dry-run session log sample |
| `05-no-password-argv.txt` | Credential hygiene + host-key review (native path) |
| `06-linux-or-unix-configure.log` | P5 Linux/no-WinSCP structural + live configure |
| `07-ftp-api-consumers.txt` | P6/P7 `ftp_*` ABI / consumers / dispatch |
| `08-batch-acceptance.md` | Batch gate checklist vs brief |
| `09-ctest.txt` | ctest (if any) |
| `10-docs-scan.txt` | Docs/README static scan |
| `11-protocol-cache.txt` | Port heuristic + advanced native coverage |
| `12-visual-perf-notes.txt` | Visual/perf notes (CLI ship: N/A formal) |
| `pipeline_report.md` | FULL batch human report |
| `pipeline_report.json` | FULL batch machine report |
| `track-notes.md` | Per-track notes / blockers |

## Latest FULL batch run

**Mode:** FULL batch (2026-08-07 16:29:07)

- Hard: `cmake --build build --config Release --target StelliferumAuditor` → **BUILD_EXIT=0**
- ctest: **CTEST_SKIP=1** (no CTestTestfile.cmake)
- CLI dry-run: EXITS=0,0,0; session **backend=native libcurl**
- P4/host-key: PASS (CreateProcess_code_lines=0; known_hosts policy=pin in session)
- P5 Linux: prior live CMAKE_CONFIGURE_EXIT=0 reaffirmed + structural PASS
- Docs P8: RESULT_DOCS_README_SHIP=PASS
- Tracks: all eight ok=true
- Ship Done criteria: **met**
- Report: `pipeline_report.md` / `pipeline_report.json`

See process Verification log in `docs/process-native-ftp-ship.md`.
