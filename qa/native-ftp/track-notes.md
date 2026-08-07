## cmake-libcurl
- Status: done (verified light after_each 2026-08-07)
- FetchContent libssh2 1.11.1 (WinCNG) + curl 8.11.1 (Schannel, CURL_USE_LIBSSH2).
- Pre-seed LIBSSH2_INCLUDE_DIR/LIBSSH2_LIBRARY so curl enables scp/sftp.
- STELLI_USE_LIBCURL=1 compile def when linked; src/ftp_native.c in SOURCES.
- WinSCP resources.rc remains optional when libs/winscp present.
- Windows Release rebuild BUILD_EXIT=0; libcurl.lib + libssh2.lib + ftp_native.obj present.
- Protocols: CURL_DISABLE_FTP=OFF, CURL_USE_LIBSSH2=ON.

## ftp-native-core
- Status: done (verified light after_each 2026-08-07)
- Implemented in src/ftp_native.c (ftp_native_* mirrors, public ftp_* unchanged):
  download_file, upload_file, upload_batch, download_batch, list_directory,
  set_cancel_flag, set_upload_counter; progress via CURLOPT_XFERINFOFUNCTION.
- Protocol: ports 22/2222/8827 => sftp:// else ftp:// (native_use_sftp).
- Credentials: CURLOPT_USERNAME/PASSWORD only; no argv; no TEMP scripts on native path.
- Host-key: full known_hosts / pin / policy (see transfer-security).
- Windows Release build SUCCESS; artifacts: 02-link-symbols.txt, 05-no-password-argv.txt, 07-ftp-api-consumers.txt.
- OBJ_STRING_OK for core ftp_native_* symbols after Release build.

## ftp-backend-dispatch
- Status: done (verified light after_each 2026-08-07 QA)
- Dispatch order (P7):
  1. ftp_use_native_backend() => true when STELLI_USE_LIBCURL && ftp_native_backend_available()
  2. Core public ftp_* early-return to ftp_native_*: download_file, upload_file, upload_batch, download_batch, list_directory
  3. ftp_set_cancel_flag / ftp_set_upload_counter dual-wire native + WinSCP state
  4. run_winscp_script: refused when !ftp_winscp_fallback_allowed() (non-Windows)
  5. Advanced ops (recursive/restore/verify/cleanup/upload_directory/download_core) still WinSCP on Windows only until ftp-native-advanced
- dependency_manager: non-Windows early return (no extract/PE patch); Windows WinSCP install optional soft-fail when STELLI_USE_LIBCURL
- create_console_variant wrapped #ifdef _WIN32 (no PE patch path on Linux/macOS)
- CMakeLists: no change needed (STELLI_USE_LIBCURL already defines + links)
- Windows Release BUILD_EXIT=0 after dispatch; manager.obj links ftp_native_download_file / ftp_native_list_directory
- Artifacts: 07-ftp-api-consumers.txt DISPATCH_RESULT PASS; 05-no-password-argv native PASS

## transfer-security
- Status: done (re-verified light after_each 2026-08-07)
- Native SFTP host-key verification in ftp_native.c:
  - KNOWN_HOSTS (default config/known_hosts) via CURLOPT_SSH_KNOWNHOSTS
  - HOST_KEY_PIN optional SHA256 pin via CURLOPT_SSH_HOST_PUBLIC_KEY_SHA256
  - HOST_KEY_POLICY=pin|fail|trust (default pin = first-connect FINE_ADD_TO_FILE under config/)
  - Mismatch always REJECT (unless trust); fail rejects missing keys
- config/ftp.ini.example documents KNOWN_HOSTS / HOST_KEY_PIN / HOST_KEY_POLICY
- Additive ABI: ftp_native_reload_security_config() in auditor.h
- P4: native path still no CreateProcess/argv passwords; no .TEMP password scripts
  - CreateProcess( call sites in ftp_native.c = 0 (word hits only in security comments)
- WinSCP fallback documented LEGACY/WEAKER in ftp_manager.c (comments + runtime warning log)
- Note: UI Save Config still rewrites only HOST/PORT/USER/PASS (pre-existing); host-key keys are manual ftp.ini edits
- Artifacts: 05-no-password-argv.txt RESULT_TRANSFER_SECURITY_TRACK PASS; 01 BUILD_EXIT=0

## ftp-native-advanced
- Status: done (2026-08-07)
- Implemented in src/ftp_native.c (ftp_native_* advanced mirrors):
  - download_recursive / download_core_files: remote LIST walk + get; size-match skip
  - upload_directory: local walk upload xml/json/txt/md
  - cleanup_remote_dir: LIST + delete names matching NNN__* (3+ digits + __)
  - verify_uploads: CURLOPT_NOBODY remote size vs local size (no WinSCP log scrape)
  - create_restore_point: best-effort native get + manifest write
  - restore_from_manifest: parse manifest + ftp_native_upload_batch
- Dispatch: ftp_manager public ftp_* early-return when ftp_use_native_backend()
- Consumers: upload_daemon.c / main.c --headless / --restore-last-upload unchanged API; Release build OK
- Semantic delta vs WinSCP:
  - WinSCP: `synchronize local -criteria=size -filemask=...` single-session engine
  - Native: recursive LIST (full listing, not NLST), skip when local size == LIST size
  - Include masks match foundation (recursive: xml [+json/cfg/ini/txt if DOWNLOAD_CONFIG_FILES]; core: xml+json; upload_dir: xml/json/txt/md)
  - Exclude dir/ext lists match foundation WinSCP masks (addons/keys/pbo/etc.)
  - Verify is stronger on native (size compare) vs WinSCP path (stat log scrape for missing only)
- Artifacts: 07-ftp-api-consumers.txt RESULT_ADVANCED_NATIVE PASS; 01 BUILD_EXIT=0; 02-link-symbols advanced section

## cli-standalone-ftp
- Status: done
- Flags: --ftp-download, --ftp-upload, --ftp-list [path], --remote, --local, --dry-run, --help/--ftp-help
- main.c: run_ftp_cli + cli_wants_ftp_mode; util_setup_console; no ui_run; reads config/ftp.ini + server_paths.ini
- Coexist: --headless / bare --local|--regen / --restore-last-upload unchanged; --local <path> only in --ftp-* mode
- RemoteFileBrowser heap-allocated (stack overflow fix; ~1.6MB)
- util.c: console banner notes native FTP/SFTP
- config/*.example: CLI usage notes
- P3: 03-cli-ftp-help.txt PASS; 04-cli-ftp-smoke.txt dry-run EXIT 0 (3/3) + help EXIT 0
- P4: CLI path no password on argv (05 append PASS)
- P6: uses stable public ftp_* API
- Linux: no WinSCP required (native path via existing dispatch); no second binary
## linux-no-winscp
- Status: done (2026-08-07)
- CMake:
  - resources.rc only when WIN32 && libs/winscp/WinSCP.exe exists; else empty
  - non-Windows: STATUS "skipping WinSCP resources.rc"; transfer backend "native REQUIRED"
  - FATAL if STELLI_USE_LIBCURL=ON but libcurl not linked on non-Windows
  - Unix links Threads + m/dl when available
- dependency_manager: soft-skip #ifndef _WIN32 (no extract/PE patch); logs native readiness
- ftp_manager:
  - ftp_winscp_fallback_allowed() = 0 off Windows
  - ftp_require_native_or_fail() on all public ftp_* before WinSCP fallthrough
  - run_winscp_piped #else: refuse (no system() of WinSCP cmdline)
- util_setup_console: non-Windows banner "WinSCP not used"
- BUILD.md: Linux transfer path table + apt/dnf openssl/raylib notes
- P5 live: AlpineWSL cmake configure SUCCESS — libcurl FTP+SFTP, no WinSCP
  Artifact: qa/native-ftp/06-linux-or-unix-configure.log RESULT_P5_LIVE_CONFIGURE=PASS
- Windows Release after track: BUILD_EXIT=0

## docs-readme-ship
- Status: done (2026-08-07)
- README.md:
  - Features: native libcurl preferred; WinSCP optional Windows fallback; Linux no WinSCP
  - Standalone transfer CLI section: --ftp-list / --ftp-download / --ftp-upload + --remote / --local / --dry-run / help
  - Flags cross-checked against main.c print_ftp_cli_usage
  - Host-key security: KNOWN_HOSTS / HOST_KEY_PIN / HOST_KEY_POLICY; no password on argv
  - Roadmap: #9 Done (native path), #5 Done, #4 Done (CLI mode), #7 In progress
  - Dependencies: libcurl+libssh2 primary; WinSCP optional
- docs/BUILD.md: CLI + security cross-links under native transfer notes
- docs/NATIVE_FTP_SHIP_BRIEF.md: all acceptance checkboxes ticked + ship status note
- docs/process-native-ftp-ship.md: track Status done; batch checklist checked
- config/ftp.ini.example: already had CLI + host-key keys (no change required)
- P8: qa/native-ftp/08-batch-acceptance.md PASS; 10-docs-scan.txt RESULT_DOCS_README_SHIP=PASS

- ftp.ini.example host-key keys present (transfer-security).

## QA light after_each (transfer-security) 2026-08-07
- BUILD_EXIT=0; CTEST_SKIP=1; evidence under qa/native-ftp/*
- ok tracks: cmake-libcurl, ftp-native-core, ftp-backend-dispatch, transfer-security, linux-no-winscp (structural)
- fail tracks: ftp-native-advanced, cli-standalone-ftp, docs-readme-ship
- Ship Done criteria NOT met

## QA light after_each (ftp-native-advanced) 2026-08-07
- BUILD_EXIT=0 (cmake --build build --config Release --target StelliferumAuditor); ELAPSED_SEC~1.01; EXE size 28852736
- CTEST_SKIP=1 (no build/CTestTestfile.cmake)
- P1/P2: STELLI_USE_LIBCURL in vcxproj; libcurl.lib + libssh2.lib; OBJ_STRING_OK all advanced ftp_native_* symbols
- P4/host-key: reaffirmed PASS (05-no-password-argv.txt)
- P5: structural PASS; live Linux configure DEFERRED
- P6/P7: RESULT_ADVANCED_NATIVE=PASS; all advanced public ftp_* early-return to ftp_native_*; main/upload_daemon/--headless/--restore-last-upload consumers OK
- P3: MAIN_MISS --ftp-*; SMOKE_SKIP=1
- P8 docs: README FAIL (no CLI flags; roadmap still Todo); BUILD.md partial PASS
- Semantic note (advanced): native size-match skip via LIST size vs local stat (not WinSCP synchronize -criteria=size); verify uses CURLOPT_NOBODY size compare
- ok tracks: cmake-libcurl, ftp-native-core, ftp-backend-dispatch, transfer-security, ftp-native-advanced, linux-no-winscp (structural)
- fail tracks: cli-standalone-ftp, docs-readme-ship
- Ship Done criteria NOT met

## QA light after_each (cli-standalone-ftp) 2026-08-07
- BUILD_EXIT=0 after main.c/util.c CLI work
- P3: 03-cli-ftp-help.txt RESULT PASS (all --ftp-* flags); 04-cli-ftp-smoke.txt dry-run 3/3 EXIT0 + help EXIT0
- P4: RESULT_CLI_P4 PASS (credentials from config/ftp.ini only)
- P6: main uses public ftp_* for list/download/upload; no signature changes
- Note: first smoke hit 0xC00000FD stack overflow from stack RemoteFileBrowser; fixed via malloc
- ok tracks: cmake-libcurl, ftp-native-core, ftp-backend-dispatch, transfer-security, ftp-native-advanced, cli-standalone-ftp, linux-no-winscp (structural)
- fail tracks: docs-readme-ship (README still needs CLI flag docs — separate track)
- Ship Done criteria NOT met (docs track pending)


## QA light after_each (cli-standalone-ftp) 2026-08-07
- BUILD_EXIT=0 (cmake --build build --config Release --target StelliferumAuditor); EXE size 28864000
- CTEST_SKIP=1
- P3: RESULT_P3=PASS (static flags + EXE strings + --ftp-help EXIT=0); dry-run 3/3 EXIT=0
- Session log: FTP CLI mode=list dry_run=1 backend=native libcurl/8.11.1-DEV host-key policy=pin
- P4/CLI: RESULT_CLI_P4=PASS; RESULT_TRANSFER_SECURITY_TRACK=PASS
- P5: structural PASS; live Linux DEFERRED
- P6/P7: ABI+dispatch+advanced PASS
- P8: docs-readme-ship FAIL (README no --ftp-*; roadmap Todo)
- ok tracks: cmake-libcurl, ftp-native-core, ftp-backend-dispatch, transfer-security, ftp-native-advanced, cli-standalone-ftp, linux-no-winscp(structural)
- fail tracks: docs-readme-ship
- Ship Done criteria NOT met

## QA light after_each (linux-no-winscp) 2026-08-07
- BUILD_EXIT=0 (cmake --build build --config Release --target StelliferumAuditor); ELAPSED_SEC~1.06; EXE size 28864000
- CTEST_SKIP=1 (no build/CTestTestfile.cmake)
- P1/P2: STELLI_USE_LIBCURL in vcxproj; libcurl.lib + libssh2.lib; OBJ_CORE 6/6; OBJ_ADV 7/7; WinSCP resources optional
- P3: RESULT_P3=PASS; HELP_EXIT=0; dry-run 3/3 EXIT=0
- Session log: FTP CLI backend=native libcurl/8.11.1-DEV Schannel libssh2/1.11.1_DEV; host-key policy=pin
- P4: RESULT_TRANSFER_SECURITY_TRACK=PASS; CreateProcess in ftp_native.c=0
- P5: LIVE AlpineWSL CMAKE_CONFIGURE_EXIT=0; RESULT_LINUX_NO_WINSCP_TRACK=PASS; reaffirm structural PASS
- P6/P7: ABI+DISPATCH+ADV PASS; native required off Windows
- P8: docs-readme-ship FAIL (README no --ftp-*; roadmap Todo; BUILD.md Linux table PASS)
- ok tracks: cmake-libcurl, ftp-native-core, ftp-backend-dispatch, transfer-security, ftp-native-advanced, cli-standalone-ftp, linux-no-winscp
- fail tracks: docs-readme-ship
- Ship Done criteria NOT met (docs track pending)

## QA light after_each (docs-readme-ship) 2026-08-07
- BUILD_EXIT=0 (`cmake --build build --config Release --target StelliferumAuditor`); EXE ~28.8MB
- CTEST_SKIP=1 (no build/CTestTestfile.cmake)
- P1/P2: RESULT_LINK=PASS; STELLI_USE_LIBCURL; libcurl+libssh2; OBJ_CORE/ADV STRING_OK; WinSCP resources optional
- P3: RESULT_P3=PASS; dry-run list/download/upload EXITS=0,0,0
- Session log (`.TEMP/auditor_crash_log.txt`): backend=native libcurl/8.11.1-DEV Schannel libssh2/1.11.1_DEV; host-key policy=pin
- P4: RESULT_P4_NATIVE=PASS; CreateProcess_code_lines=0; CURLOPT_USERNAME/PASSWORD; RESULT_TRANSFER_SECURITY_TRACK=PASS
- P5: prior live AlpineWSL CMAKE_CONFIGURE_EXIT=0 retained; RESULT_LINUX_NO_WINSCP_TRACK=PASS
- P6/P7: RESULT_ABI=PASS; RESULT_DISPATCH=PASS; RESULT_ADVANCED_NATIVE=PASS
- P8: RESULT_DOCS_README_SHIP=PASS; roadmap #9/#5/#4 Done, #7 In_progress; brief checkboxes; CLI flags cross-match main.c
- ok tracks: cmake-libcurl, ftp-native-core, ftp-backend-dispatch, transfer-security, ftp-native-advanced, cli-standalone-ftp, linux-no-winscp, docs-readme-ship
- fail tracks: none
- Ship Done criteria **met**

## FULL batch pipeline — 2026-08-07 16:27:18
- Mode: FULL (build + ctest-skip + P1–P8 + track_checks + visual/perf notes)
- BUILD_EXIT=0
- CTEST_SKIP=1
- CLI dry-run EXITS=0,0,0
- CreateProcess in ftp_native.c: code=0 comment_only=1 (PASS)
- RESULT_DOCS_README_SHIP=PASS
- Ship passed=true

## FULL batch pipeline finalize — 2026-08-07 16:29:07
- BUILD_EXIT=0; CTEST_SKIP=1; CLI EXITS=0,0,0
- Session: backend=native libcurl (libcurl/8.11.1-DEV Schannel libssh2/1.11.1_DEV)
- Session: host-key known_hosts policy=pin; password not logged
- CreateProcess_code_lines=0 (comment-only mention of CreateProcess)
- RESULT_DOCS_README_SHIP=PASS; all 8 tracks ok; ship passed=true
- Artifacts: pipeline_report.md / pipeline_report.json
