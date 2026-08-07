# Goal: Cross-platform native FTP/SFTP + integrated standalone transfer mode

## Product outcome
Ship transfer that works on Linux (and Windows) without requiring WinSCP.
Integrate into StelliferumAuditor (same config, same ftp_* API used by UI/upload daemon).
Add a CLI "standalone" transfer mode so ops can pull/push without the full GUI economy pipeline.

## Priority alignment (README roadmap)
1. #9 transfer security (host keys, no password on argv, scrub logs)
2. #5 native SFTP/FTP (libcurl preferred)
4. #4 standalone FTP tool (as CLI mode of the same binary)
11. #7 cross-platform structure (no WinSCP required on non-Windows)

## Technical design (implement this)

### Backend
1. Add libcurl via CMake FetchContent (or find_package) with CURL_USE_LIBSSH2/SFTP enabled if practical; at minimum HTTP(S) not enough — need FTP and SFTP.
2. New module `src/ftp_native.c` (or refactor `ftp_manager.c`) implementing:
   - ftp_download_file, ftp_upload_file, ftp_upload_batch
   - ftp_download_batch, ftp_download_recursive (or directory walk + get)
   - ftp_list_directory
   - ftp_set_cancel_flag / progress hooks
3. Keep public signatures in `include/auditor.h` stable so ui.cpp / upload_daemon.c / main.c keep compiling.
4. Backend selection:
   - Prefer native/libcurl always when available
   - On Windows, optional WinSCP fallback only if curl backend fails to build or feature missing
   - On Linux/macOS, NEVER require WinSCP
5. Security:
   - Do not put password on process command line
   - Support known_hosts / host key verification (config or first-connect pin file under config/)
   - Avoid writing plaintext passwords into .TEMP scripts when using native path
6. Protocol: port 22/2222/8827 => sftp://, else ftp:// (match existing heuristic)

### Standalone CLI (integrated, not a second product)
Add CLI flags to StelliferumAuditor:
  --ftp-download
  --ftp-upload
  --ftp-list [path]
  --remote <path>
  --local <path>
  --dry-run
Reads config/ftp.ini + config/server_paths.ini (same as GUI). Headless, no raylib window when these flags present (or early exit before ui_run).

### Build
- CMakeLists.txt: FetchContent curl; link libcurl; define STELLI_USE_LIBCURL
- On Windows keep existing WinSCP resource optional for fallback
- Document: core transfer no longer requires WinSCP on Linux

### Acceptance
- [x] build Release on Windows with native backend compiled in
- [x] ftp_* API still used by existing UI code paths
- [x] CLI --ftp-list or --ftp-download --help documented in README
- [x] Linux build does not hard-require WinSCP/resources.rc
- [x] No password in CreateProcess command line on native path
- [x] Plan file written under docs/ or .grok/

**Ship status (2026-08-07):** Acceptance bullets above met by multi-track native-ftp ship. Execution record: `docs/process-native-ftp-ship.md`. QA: `qa/native-ftp/` (incl. `08-batch-acceptance.md`). Extra (beyond original brief list): host-key/known_hosts, no plaintext temp scripts on native path, WinSCP optional Windows-only, advanced native ops, CLI dry-run smoke.

## Key files
- src/ftp_manager.c (current WinSCP implementation)
- src/dependency_manager.c (PE patch)
- src/main.c (CLI entry)
- src/ui.cpp, src/upload_daemon.c (callers)
- include/auditor.h (API)
- CMakeLists.txt
- README.md roadmap
