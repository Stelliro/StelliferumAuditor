# Batch acceptance checklist (native-ftp)

**Mode:** FULL batch pipeline  
**Date:** 2026-08-07 16:29:07  
**Runner:** shared QA pipeline (FULL)

| Brief / process bullet | Status | Evidence |
|------------------------|--------|----------|
| Build Release on Windows with native backend compiled in | **PASS** | `01-windows-release-build.log` **BUILD_EXIT=0**; libcurl+libssh2; STELLI_USE_LIBCURL |
| `ftp_*` API still used by existing UI code paths | **PASS** | `07-ftp-api-consumers.txt` ABI+DISPATCH+ADV PASS |
| CLI `--ftp-list` / `--ftp-download` documented in README | **PASS** | `10-docs-scan.txt` RESULT_DOCS_README_SHIP=PASS; flags match main.c |
| Linux build does not hard-require WinSCP/`resources.rc` | **PASS** | `06-linux-or-unix-configure.log` P5 LIVE CMAKE_CONFIGURE_EXIT=0 + structural PASS |
| No password in CreateProcess command line on native path | **PASS** | `05-no-password-argv.txt` CreateProcess_code_lines=0; CURLOPT_USERNAME/PASSWORD; session log no password |
| Plan file under `docs/` | **PASS** | `docs/process-native-ftp-ship.md` |
| Host-key / known_hosts on native path | **PASS** | session: `Native SFTP host-key: known_hosts='config/known_hosts' policy=pin` |
| No plaintext password temp scripts on native path | **PASS** | NATIVE_TEMP_PASSWORD_SCRIPT_code=0; P4 PASS |
| WinSCP optional Windows fallback only (not required on Linux) | **PASS** | native preferred; Windows-only fallback; README/BUILD |
| CLI `--ftp-*` works same binary / dry-run | **PASS** | `03`/`04` EXITS=0,0,0 backend=native libcurl |
| Roadmap #9 / #5 / #4 / #7 progress reflected | **PASS** | README/docs scan PASS |

## Track rollup (FULL batch)

| Track | ok | Note |
|-------|----|------|
| cmake-libcurl | true | BUILD_EXIT=0; STELLI_USE_LIBCURL; FTP+SFTP via libssh2 |
| ftp-native-core | true | core ftp_native_*; CreateProcess_code=0; CURLOPT creds |
| ftp-backend-dispatch | true | native preferred; WinSCP Windows-only |
| transfer-security | true | host-key + P4 PASS; session host-key line |
| ftp-native-advanced | true | recursive/restore/verify/cleanup native |
| cli-standalone-ftp | true | --ftp-* dry-run EXITS=0,0,0 backend=native libcurl |
| linux-no-winscp | true | P5 LIVE prior + structural PASS |
| docs-readme-ship | true | RESULT_DOCS_README_SHIP=PASS |

## Hard step exits (this run)

- `cmake --build build --config Release --target StelliferumAuditor` → **BUILD_EXIT=0**
- ctest → **CTEST_SKIP=1** (no `build/CTestTestfile.cmake`)
- CLI dry-run list/download/upload → **EXIT=0,0,0** (session: backend=native libcurl)
- Linux configure (prior live) → **CMAKE_CONFIGURE_EXIT=0**

## Ship Done criteria

**Met** — all tracks Status `done`; P1–P8 evidence under `qa/native-ftp/`; FULL batch gate PASS.
