# Pipeline report — native-ftp FULL batch

**Date:** 2026-08-07 16:29:07  
**QA dir:** `qa/native-ftp`  
**Mode:** FULL batch  
**passed:** true

## Commands

1. `cmake --build build --config Release --target StelliferumAuditor` → BUILD_EXIT=0
2. ctest → CTEST_SKIP=1 (no CTestTestfile.cmake)
3. `StelliferumAuditor.exe --ftp-list|--ftp-download|--ftp-upload --dry-run` → EXITS=0,0,0
4. Session log evidence: backend=native libcurl; host-key known_hosts policy=pin
5. Static greps: link symbols, P4 credential hygiene, ABI consumers, docs scan, protocol heuristic
6. P5 Linux: reaffirm prior live CMAKE_CONFIGURE_EXIT=0 + structural gates

## Evidence (quoted exits)

BUILD_EXIT=0; CTEST_SKIP=1; CLI_DRY_RUN_EXITS=0,0,0; SESSION backend=native libcurl (libcurl/8.11.1-DEV Schannel libssh2/1.11.1_DEV); CreateProcess_code_lines=0; RESULT_LINK=PASS; RESULT_P3=PASS; RESULT_SMOKE=PASS; RESULT_LOG_EVIDENCE=PASS; RESULT_TRANSFER_SECURITY_TRACK=PASS; RESULT_LINUX_NO_WINSCP_TRACK=PASS; RESULT_DOCS_README_SHIP=PASS; P5_CMAKE_CONFIGURE_EXIT=0

## Track results

| track_id | ok | note |
|----------|----|------|
| cmake-libcurl | true | Windows Release links libcurl+libssh2; STELLI_USE_LIBCURL; BUILD_EXIT=0 |
| ftp-native-core | true | ftp_native_* core ops; no argv passwords; protocol sftp heuristic |
| ftp-backend-dispatch | true | Prefer native; WinSCP Windows-only optional fallback |
| transfer-security | true | known_hosts/HOST_KEY_*; CreateProcess_code=0; no TEMP password scripts |
| ftp-native-advanced | true | recursive/restore/verify/cleanup via native without WinSCP scrape |
| cli-standalone-ftp | true | --ftp-list/download/upload dry-run EXITS=0,0,0; backend=native |
| linux-no-winscp | true | No hard WinSCP/resources.rc; prior Linux configure EXIT=0 |
| docs-readme-ship | true | README/BUILD/brief CLI+security+Linux; docs scan PASS |

## Artifacts

All under `qa/native-ftp/` including `pipeline_report.json`.
