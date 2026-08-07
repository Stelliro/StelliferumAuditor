
#include "auditor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

static void strict_url_encode(const char *src, char *dest, size_t dest_len) {
    size_t n = 0;
    const char *hex = "0123456789ABCDEF";
    while (*src && n < dest_len - 4) {
        if (isalnum((unsigned char)*src) || *src == '-' || *src == '_' || *src == '.' || *src == '~') {
            dest[n++] = *src;
        } else {
            dest[n++] = '%';
            dest[n++] = hex[((unsigned char)*src) >> 4];
            dest[n++] = hex[((unsigned char)*src) & 15];
        }
        src++;
    }
    dest[n] = '\0';
}

static void get_winscp_absolute_path(char *buffer, size_t size) {
    #ifdef _WIN32
    // Priority: WinSCP.com (console) > WinSCP.exe (GUI)
    const char *names[] = { "WinSCP.com", "WinSCP.exe" };
    const int name_count = 2;

    // 1. Next to our own executable (primary location — embedded extraction)
    {
        char exe_path[MAX_PATH_LEN];
        GetModuleFileNameA(NULL, exe_path, MAX_PATH_LEN);
        char *last_slash = strrchr(exe_path, '\\');
        if (last_slash) *last_slash = '\0';

        for (int i = 0; i < name_count; i++) {
            char test[MAX_PATH_LEN];
            snprintf(test, sizeof(test), "%s\\%s", exe_path, names[i]);
            FILE *f = fopen(test, "rb");
            if (f) { fclose(f); strncpy(buffer, test, size); return; }
        }
    }

    // 2. Working directory
    {
        for (int i = 0; i < name_count; i++) {
            FILE *f = fopen(names[i], "rb");
            if (f) {
                fclose(f);
                char cwd[MAX_PATH_LEN];
                GetCurrentDirectoryA(MAX_PATH_LEN, cwd);
                snprintf(buffer, size, "%s\\%s", cwd, names[i]);
                return;
            }
        }
    }

    // 3. Legacy .TEMP/ location
    {
        for (int i = 0; i < name_count; i++) {
            char test[MAX_PATH_LEN];
            snprintf(test, sizeof(test), ".TEMP\\%s", names[i]);
            FILE *f = fopen(test, "rb");
            if (f) {
                fclose(f);
                char cwd[MAX_PATH_LEN];
                GetCurrentDirectoryA(MAX_PATH_LEN, cwd);
                snprintf(buffer, size, "%s\\.TEMP\\%s", cwd, names[i]);
                return;
            }
        }
    }

    strncpy(buffer, "WinSCP.com", size);
    #else
    strncpy(buffer, "WinSCP.com", size);
    #endif
}

// ============================================================================
// BACKEND DISPATCH — prefer native libcurl; WinSCP optional Windows fallback
// ============================================================================
// Selection rules (ftp-backend-dispatch + ftp-native-advanced + linux-no-winscp):
//   1. When STELLI_USE_LIBCURL and ftp_native_backend_available() => use native.
//   2. Windows only: fall back to existing WinSCP path if native unavailable.
//   3. Linux/macOS: native is the REQUIRED transfer path — never require WinSCP
//      extraction, PE patch, or resources.rc; run_winscp_* refuses off Windows.
//   4. Advanced ops (recursive/core/upload_dir/cleanup/verify/restore) also
//      prefer native when available — no WinSCP log scraping on native path.
//
// SECURITY NOTE — WinSCP path is LEGACY / WEAKER than native:
//   - Passwords are passed as WinSCP /parameter argv tokens (visible on the
//     process command line briefly; not used on the native libcurl path).
//   - Session scripts under .TEMP/ use open ... -hostkey=* (trust-any host key).
//   - Prefer native (STELLI_USE_LIBCURL) for credential hygiene + host-key pin.

static volatile bool *s_winscp_cancel_flag = NULL;
static volatile int  *s_winscp_upload_counter = NULL;

/** True when public ftp_* should route core ops to libcurl (native preferred). */
static int ftp_use_native_backend(void) {
#ifdef STELLI_USE_LIBCURL
    return ftp_native_backend_available() ? 1 : 0;
#else
    return 0;
#endif
}

/** WinSCP path is allowed only on Windows as optional fallback. */
static int ftp_winscp_fallback_allowed(void) {
#ifdef _WIN32
    return 1;
#else
    return 0;
#endif
}

/**
 * Off Windows, transfers must use native libcurl. Call after
 * ftp_use_native_backend() is false to avoid walking into WinSCP scripts.
 * Returns true when the caller should abort (no usable backend).
 */
static int ftp_require_native_or_fail(const char *op_name) {
    if (ftp_use_native_backend())
        return 0;
    if (ftp_winscp_fallback_allowed())
        return 0;
    util_log(SEVERITY_ERROR,
             "%s: no transfer backend on this platform. "
             "Build with STELLI_USE_LIBCURL (native libcurl is required; "
             "WinSCP is Windows-only).",
             op_name ? op_name : "FTP");
    return 1;
}

void ftp_set_cancel_flag(volatile bool *flag) {
    s_winscp_cancel_flag = flag;
    /* Dual-wire: native path reads its own flag; always keep both in sync. */
    ftp_native_set_cancel_flag(flag);
}

void ftp_set_upload_counter(volatile int *counter) {
    s_winscp_upload_counter = counter;
    ftp_native_set_upload_counter(counter);
}

// ============================================================================
// WINSCP PROCESS EXECUTION (piped output for real-time logging)
// ============================================================================

/**
 * Run WinSCP via CreateProcess (hidden window), capturing stdout/stderr and
 * tailing the WinSCP session log (.TEMP/winscp.log) for real-time transfer
 * visibility in the Log tab.
 *
 * If capture_file is non-NULL, all raw stdout is also written to that file.
 * Checks s_winscp_cancel_flag each iteration; terminates child on cancel.
 * Returns: process exit code, or -1 on failure to launch.
 */
static int run_winscp_piped(const char *cmd_line, const char *capture_file) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        util_log(SEVERITY_ERROR, "WinSCP: Failed to create output pipe.");
        return -1;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    // NUL device for stdin — WIN32 subsystem has no console, so
    // GetStdHandle(STD_INPUT_HANDLE) is invalid and can hang the child.
    HANDLE hNulIn = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ,
                                &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags  = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWritePipe;
    si.hStdError  = hWritePipe;
    si.hStdInput  = hNulIn;
    si.wShowWindow = SW_HIDE;
    ZeroMemory(&pi, sizeof(pi));

    char cmd_buf[4096];
    strncpy(cmd_buf, cmd_line, sizeof(cmd_buf) - 1);
    cmd_buf[sizeof(cmd_buf) - 1] = '\0';

    if (!CreateProcessA(NULL, cmd_buf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        util_log(SEVERITY_ERROR, "WinSCP: CreateProcess failed (error %lu).", GetLastError());
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        if (hNulIn != INVALID_HANDLE_VALUE) CloseHandle(hNulIn);
        return -1;
    }

    util_assign_child_process(pi.hProcess);
    CloseHandle(hWritePipe);

    FILE *capture = NULL;
    if (capture_file) {
        capture = fopen(capture_file, "w");
    }

    // ── Open the WinSCP session log for real-time tailing ──────────────
    // WinSCP writes this incrementally; we parse it every 200ms for
    // file-transfer events so the Log tab shows what's happening.
    const char *log_path = ".TEMP/winscp.log";
    HANDLE hLog = CreateFileA(log_path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    LARGE_INTEGER log_pos;
    log_pos.QuadPart = 0;
    // If file doesn't exist yet, we'll retry later
    char log_tail_buf[4096];
    char log_line[1024];
    int  log_line_pos = 0;

    // ── Non-blocking pump: pipe stdout + tail log ──────────────────────
    char read_buf[1024];
    DWORD bytes_read;
    char  line_buf[2048];
    int   line_pos = 0;
    bool  running  = true;

    while (running) {
        // Check external cancel flag — terminate child if requested
        if (s_winscp_cancel_flag && *s_winscp_cancel_flag) {
            util_log(SEVERITY_WARNING, "WinSCP: Cancel requested — terminating child process.");
            TerminateProcess(pi.hProcess, 1);
            running = false;
            break;
        }

        DWORD wait = WaitForSingleObject(pi.hProcess, 200);
        if (wait == WAIT_OBJECT_0) running = false;

        // Drain any stdout/stderr from the pipe (non-blocking)
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) || avail == 0)
                break;
            DWORD to_read = (avail < sizeof(read_buf) - 1) ? avail : (DWORD)(sizeof(read_buf) - 1);
            if (!ReadFile(hReadPipe, read_buf, to_read, &bytes_read, NULL) || bytes_read == 0)
                break;
            read_buf[bytes_read] = '\0';

            if (capture) { fwrite(read_buf, 1, bytes_read, capture); fflush(capture); }

            for (DWORD i = 0; i < bytes_read; i++) {
                if (read_buf[i] == '\n' || read_buf[i] == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        bool blank = true;
                        for (int j = 0; j < line_pos; j++) {
                            if (line_buf[j] != ' ' && line_buf[j] != '\t') { blank = false; break; }
                        }
                        if (!blank)
                            util_log(SEVERITY_INFO, "  | %s", line_buf);
                        line_pos = 0;
                    }
                } else if (line_pos < (int)sizeof(line_buf) - 1) {
                    line_buf[line_pos++] = read_buf[i];
                }
            }
        }

        // ── Tail WinSCP session log for transfer events ────────────────
        if (hLog == INVALID_HANDLE_VALUE) {
            // Retry opening — WinSCP may not have created it yet
            hLog = CreateFileA(log_path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hLog != INVALID_HANDLE_VALUE) {
                log_pos.QuadPart = 0;
                log_line_pos = 0;
            }
        }
        if (hLog != INVALID_HANDLE_VALUE) {
            SetFilePointerEx(hLog, log_pos, NULL, FILE_BEGIN);
            DWORD log_read = 0;
            while (ReadFile(hLog, log_tail_buf, sizeof(log_tail_buf) - 1, &log_read, NULL) && log_read > 0) {
                log_pos.QuadPart += log_read;
                log_tail_buf[log_read] = '\0';

                for (DWORD i = 0; i < log_read; i++) {
                    if (log_tail_buf[i] == '\n' || log_tail_buf[i] == '\r') {
                        if (log_line_pos > 0) {
                            log_line[log_line_pos] = '\0';

                            // Filter for interesting events in the WinSCP log.
                            // Lines start with ". YYYY-MM-DD HH:MM:SS.mmm <msg>"
                            //                or "< YYYY-MM-DD ..." (server)
                            //                or "> YYYY-MM-DD ..." (client)
                            const char *msg = log_line;
                            // Skip leading marker + timestamp (26 chars):
                            //   1 marker + 1 space + 10 date + 1 space + 12 time.ms + 1 space = 26
                            if (log_line_pos > 26 && (msg[0] == '.' || msg[0] == '<' || msg[0] == '>') && msg[1] == ' ')
                                msg += 26;

                            // Decide if this line is worth showing
                            bool show = false;
                            Severity sev = SEVERITY_INFO;

                            // Connection / session events
                            if (strstr(msg, "Connected") || strstr(msg, "Session started")
                                || strstr(msg, "Disconnected"))
                                show = true;

                            // Synchronization progress (one line per directory scan)
                            if (strstr(msg, "Collecting synchronization list for")) {
                                show = true;
                                // Shorten: extract just the remote dir name
                                const char *rd = strstr(msg, "remote directory '");
                                if (rd) {
                                    rd += 18; // skip "remote directory '"
                                    char short_msg[512];
                                    snprintf(short_msg, sizeof(short_msg), "Scanning: %.480s", rd);
                                    // Trim trailing "'..."
                                    char *trail = strstr(short_msg, "',");
                                    if (trail) *trail = '\0';
                                    util_log(SEVERITY_INFO, "  > %s", short_msg);
                                    log_line_pos = 0;
                                    continue;
                                }
                            }

                            // "Nothing to synchronize" — show so user knows it finished
                            if (strstr(msg, "Nothing to synchronize"))
                                show = true;

                            // Actual file transfers
                            if (strstr(msg, "Copying") || strstr(msg, "Transfer done")
                                || strstr(msg, "File:") || strstr(msg, "Downloading")
                                || strstr(msg, "download of") || strstr(msg, "Uploading")
                                || strstr(msg, "Copying finished"))
                                show = true;

                            // Increment upload counter on each completed transfer
                            if (s_winscp_upload_counter && strstr(msg, "Transfer done"))
                                (*s_winscp_upload_counter)++;

                            // Script commands echoed back
                            if (strstr(msg, "Script:"))
                                show = true;

                            // Errors / warnings — but suppress harmless mkdir-on-existing-dir failures
                            if ((strstr(msg, "Cannot") || strstr(msg, "Error")
                                || strstr(msg, "No such file"))
                                && !strstr(msg, "Error creating folder")) {
                                show = true;
                                sev = SEVERITY_WARNING;
                            }

                            if (show)
                                util_log(sev, "  > %s", msg);
                            log_line_pos = 0;
                        }
                    } else if (log_line_pos < (int)sizeof(log_line) - 1) {
                        log_line[log_line_pos++] = log_tail_buf[i];
                    }
                }
            }
        }
    }

    // Final drain of pipe after process exit
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hReadPipe, NULL, 0, NULL, &avail, NULL) || avail == 0)
            break;
        DWORD to_read = (avail < sizeof(read_buf) - 1) ? avail : (DWORD)(sizeof(read_buf) - 1);
        if (!ReadFile(hReadPipe, read_buf, to_read, &bytes_read, NULL) || bytes_read == 0)
            break;
        read_buf[bytes_read] = '\0';
        if (capture) { fwrite(read_buf, 1, bytes_read, capture); fflush(capture); }
        for (DWORD i = 0; i < bytes_read; i++) {
            if (read_buf[i] == '\n' || read_buf[i] == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    util_log(SEVERITY_INFO, "  | %s", line_buf);
                    line_pos = 0;
                }
            } else if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = read_buf[i];
            }
        }
    }
    if (line_pos > 0) {
        line_buf[line_pos] = '\0';
        util_log(SEVERITY_INFO, "  | %s", line_buf);
    }

    if (capture) fclose(capture);
    CloseHandle(hReadPipe);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);
    if (hNulIn != INVALID_HANDLE_VALUE) CloseHandle(hNulIn);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (int)exit_code;
#else
    /* linux-no-winscp: never spawn WinSCP (or shell out WinSCP cmdline) off Windows. */
    (void)cmd_line;
    (void)capture_file;
    util_log(SEVERITY_ERROR,
             "WinSCP process path is not available on this platform "
             "(native libcurl is the required transfer backend).");
    return -1;
#endif
}

/*
 * LEGACY Windows-only fallback. Weaker security than native libcurl:
 * credentials on CreateProcess /parameter argv; -hostkey=* / -certificate=*
 * trusts any key. Native path never uses this when STELLI_USE_LIBCURL prefers it.
 */
static bool run_winscp_script(const char *host, int port, const char *user, const char *pass, const char *script_content) {
    /* Linux/macOS: never require WinSCP for transfer — native backend only. */
    if (!ftp_winscp_fallback_allowed()) {
        util_log(SEVERITY_ERROR,
                 "WinSCP fallback is not available on this platform. "
                 "Use the native libcurl backend (STELLI_USE_LIBCURL).");
        (void)host; (void)port; (void)user; (void)pass; (void)script_content;
        return false;
    }

    char winscp_path[512];
    get_winscp_absolute_path(winscp_path, sizeof(winscp_path));
    
    util_log(SEVERITY_WARNING,
             "WinSCP (legacy/weaker Windows fallback): Launching from '%s' "
             "(prefer native libcurl for host-key pin + no password on argv)...",
             winscp_path);
    
    FILE *f = fopen(winscp_path, "r");
    if (!f) {
        util_log(SEVERITY_ERROR, "CRITICAL: WinSCP not found at expected path.");
        util_log(SEVERITY_ERROR, "Target: %s", winscp_path);
        return false;
    }
    fclose(f);

    char safe_user[512];
    char safe_pass[512];
    strict_url_encode(user, safe_user, sizeof(safe_user));
    strict_url_encode(pass, safe_pass, sizeof(safe_pass));

    FILE *script = fopen(".TEMP/winscp_script.txt", "w");
    if (!script) return false;

    fprintf(script, "option batch on\n");
    fprintf(script, "option confirm off\n");
    fprintf(script, "option reconnecttime 120\n");
    
    /* -hostkey=* / -certificate=* = trust-any (weaker than native known_hosts). */
    if (port == 8827 || port == 22 || port == 2222) {
        fprintf(script, "open sftp://%%1%%:%%2%%@%s:%d/ -hostkey=* "
                        "-rawsettings PingType=1 PingInterval=10 Timeout=120\n", host, port);
    } else {
        fprintf(script, "open ftp://%%1%%:%%2%%@%s:%d/ -certificate=* "
                        "-rawsettings FtpPingType=1 FtpPingInterval=10 Timeout=120\n", host, port);
    }
    
    fprintf(script, "%s\n", script_content);
    fprintf(script, "exit\n");
    fclose(script);

    char cmd[4096];
    
    // CreateProcess is used instead of system() — no cmd.exe double-quote trick needed.
    // Output is piped through util_log for real-time console display.
    // Truncate the old log so the tail reader starts fresh.
    { FILE *trunc = fopen(".TEMP/winscp.log", "w"); if (trunc) fclose(trunc); }

    snprintf(cmd, sizeof(cmd), 
        "\"%s\" /ini=nul /script=.TEMP/winscp_script.txt /log=.TEMP/winscp.log /loglevel=1 /parameter \"%s\" \"%s\"", 
        winscp_path, safe_user, safe_pass);
        
    int result = run_winscp_piped(cmd, NULL);
    remove(".TEMP/winscp_script.txt");

    if (result == 0) {
        util_log(SEVERITY_INFO, "WinSCP: Operation Successful.");
        return true;
    } else {
        util_log(SEVERITY_ERROR, "WinSCP Failed (Code %d). Check .TEMP/winscp.log.", result);
        return false;
    }
}

// Resolve a potentially relative path to an absolute path.
// On Windows, uses GetFullPathNameA; on other platforms, uses realpath or returns as-is.
static void resolve_absolute_path(const char *relative, char *absolute, size_t abs_len) {
#ifdef _WIN32
    if (!GetFullPathNameA(relative, (DWORD)abs_len, absolute, NULL)) {
        strncpy(absolute, relative, abs_len - 1);
        absolute[abs_len - 1] = '\0';
    }
#else
    char *resolved = realpath(relative, NULL);
    if (resolved) {
        strncpy(absolute, resolved, abs_len - 1);
        absolute[abs_len - 1] = '\0';
        free(resolved);
    } else {
        strncpy(absolute, relative, abs_len - 1);
        absolute[abs_len - 1] = '\0';
    }
#endif
}

// Forward declaration — defined later in this file (restore point section)
static void ensure_directory_tree(const char *path);

// Extract the directory portion of a remote path.
// e.g. "/root/profiles/Trader/TraderConfig.txt" -> "/root/profiles/Trader"
static void extract_remote_dir(const char *remote_path, char *dir_out, size_t dir_len) {
    strncpy(dir_out, remote_path, dir_len - 1);
    dir_out[dir_len - 1] = '\0';
    char *last_slash = strrchr(dir_out, '/');
    if (last_slash && last_slash != dir_out) {
        *last_slash = '\0';
    }
}

bool ftp_download_file(const char *host, int port, const char *user, const char *pass, const char *remote_path, const char *local_path) {
    /* Prefer native libcurl when compiled in (no password on argv / no TEMP scripts). */
    if (ftp_use_native_backend()) {
        return ftp_native_download_file(host, port, user, pass, remote_path, local_path);
    }
    if (ftp_require_native_or_fail("ftp_download_file"))
        return false;

    // Resolve local path to absolute to avoid CWD/WinSCP path resolution mismatches
    char abs_local[MAX_PATH_LEN];
    resolve_absolute_path(local_path, abs_local, sizeof(abs_local));

    // Ensure local parent directory exists
    {
        char parent[MAX_PATH_LEN];
        strncpy(parent, abs_local, sizeof(parent) - 1);
        parent[sizeof(parent) - 1] = '\0';
        // Find last separator
        char *last_sep = strrchr(parent, '\\');
        if (!last_sep) last_sep = strrchr(parent, '/');
        if (last_sep) {
            *last_sep = '\0';
            ensure_directory_tree(parent);
        }
    }

    char script[2048];
    snprintf(script, sizeof(script), "get \"%s\" \"%s\"", remote_path, abs_local);
    return run_winscp_script(host, port, user, pass, script);
}

bool ftp_upload_file(const char *host, int port, const char *user, const char *pass, const char *local_path, const char *remote_path) {
    if (ftp_use_native_backend()) {
        return ftp_native_upload_file(host, port, user, pass, local_path, remote_path);
    }
    if (ftp_require_native_or_fail("ftp_upload_file"))
        return false;

    // Resolve local path to absolute to avoid CWD/WinSCP path resolution mismatches
    char abs_local[MAX_PATH_LEN];
    resolve_absolute_path(local_path, abs_local, sizeof(abs_local));

    // Verify local file actually exists before spawning WinSCP
    if (!util_file_exists(abs_local)) {
        util_log(SEVERITY_WARNING, "Upload skipped: Local file not found '%s'", abs_local);
        return false;
    }

    // Create the remote directory tree if needed, then upload.
    // mkdir uses batch=continue so existing directories don't abort the script.
    // put uses batch=on so actual transfer failures are reported.
    char remote_dir[MAX_PATH_LEN];
    extract_remote_dir(remote_path, remote_dir, sizeof(remote_dir));

    char script[2048];
    snprintf(script, sizeof(script),
             "option batch continue\n"
             "mkdir \"%s\"\n"
             "option batch on\n"
             "put \"%s\" \"%s\"",
             remote_dir, abs_local, remote_path);
    return run_winscp_script(host, port, user, pass, script);
}

bool ftp_upload_batch(const char *host, int port, const char *user, const char *pass,
                     const char **local_paths, const char **remote_paths, int count,
                     int *out_succeeded, int *out_failed) {
    if (!local_paths || !remote_paths || count <= 0) return false;

    if (ftp_use_native_backend()) {
        return ftp_native_upload_batch(host, port, user, pass,
                                       local_paths, remote_paths, count,
                                       out_succeeded, out_failed);
    }
    if (ftp_require_native_or_fail("ftp_upload_batch"))
        return false;

    // Pre-validate local files and resolve to absolute paths.
    // Build parallel arrays of resolved locals and their remote dirs.
    char (*abs_locals)[MAX_PATH_LEN] = (char (*)[MAX_PATH_LEN])calloc((size_t)count, MAX_PATH_LEN);
    bool *valid = (bool *)calloc((size_t)count, sizeof(bool));
    if (!abs_locals || !valid) {
        util_log(SEVERITY_ERROR, "ftp_upload_batch: Memory allocation failed.");
        free(abs_locals); free(valid);
        return false;
    }

    int valid_count = 0;
    int skipped = 0;
    for (int i = 0; i < count; i++) {
        if (!local_paths[i] || !remote_paths[i]) { valid[i] = false; skipped++; continue; }
        resolve_absolute_path(local_paths[i], abs_locals[i], MAX_PATH_LEN);
        if (!util_file_exists(abs_locals[i])) {
            util_log(SEVERITY_WARNING, "Upload skipped: Local file not found '%s'", abs_locals[i]);
            valid[i] = false;
            skipped++;
        } else {
            valid[i] = true;
            valid_count++;
        }
    }

    if (valid_count == 0) {
        util_log(SEVERITY_WARNING, "ftp_upload_batch: No valid files to upload.");
        if (out_succeeded) *out_succeeded = 0;
        if (out_failed)    *out_failed = skipped;
        free(abs_locals); free(valid);
        return false;
    }

    // Build a single WinSCP script that creates all remote dirs then uploads all files.
    // Using a temp file since the script can be large (many put commands).
    util_ensure_directory(".TEMP");
    FILE *sf = fopen(".TEMP/winscp_batch_upload.txt", "w");
    if (!sf) {
        util_log(SEVERITY_ERROR, "ftp_upload_batch: Cannot create batch script file.");
        free(abs_locals); free(valid);
        return false;
    }

    // Collect unique remote directories for mkdir
    char (*unique_dirs)[MAX_PATH_LEN] = (char (*)[MAX_PATH_LEN])calloc((size_t)count, MAX_PATH_LEN);
    int dir_count = 0;
    for (int i = 0; i < count; i++) {
        if (!valid[i]) continue;
        char remote_dir_buf[MAX_PATH_LEN];
        extract_remote_dir(remote_paths[i], remote_dir_buf, sizeof(remote_dir_buf));
        // Deduplicate
        bool found = false;
        for (int d = 0; d < dir_count; d++) {
            if (strcmp(unique_dirs[d], remote_dir_buf) == 0) { found = true; break; }
        }
        if (!found && dir_count < count) {
            strncpy(unique_dirs[dir_count], remote_dir_buf, MAX_PATH_LEN - 1);
            dir_count++;
        }
    }

    // Phase 1: mkdir with batch continue (existing dirs won't abort)
    fprintf(sf, "option batch continue\n");
    for (int d = 0; d < dir_count; d++) {
        fprintf(sf, "mkdir \"%s\"\n", unique_dirs[d]);
    }

    // Phase 2: Upload files — DELETE first, then PUT to replace cleanly.
    // WinSCP's `put "local" "remote"` explicitly names the destination file,
    // which is essential when local filenames don't match the desired remote
    // names (e.g. restore point backups like "001__sanitized_path.xml").
    // Multi-file `put "l1" "l2" "/dir/"` uses LOCAL filename on the remote,
    // which caused numbered backup artifacts to pollute the server's db/ folder.
    // Individual puts in the same session have no reconnect overhead.
    // DELETE-THEN-PUT strategy: remove existing file before uploading the new
    // version.  This avoids SFTP/FTP servers that fail to fully overwrite files
    // (partial STOR, stale inode, or cached data).  Safe because:
    //   1. We already have local backups (output_backups/) and a restore point
    //      (backups/YYYYMMDD_HHMMSS/) before this function runs.
    //   2. A failed put after rm is caught by retry logic (3 attempts).
    //   3. Post-upload verification (ftp_verify_uploads) catches any gaps.
    for (int d = 0; d < dir_count; d++) {
        int files_in_dir = 0;
        for (int i = 0; i < count; i++) {
            if (!valid[i]) continue;
            char remote_dir_buf[MAX_PATH_LEN];
            extract_remote_dir(remote_paths[i], remote_dir_buf, sizeof(remote_dir_buf));
            if (strcmp(unique_dirs[d], remote_dir_buf) == 0) {
                // Delete existing file first, then upload fresh copy.
                fprintf(sf, "rm \"%s\"\n", remote_paths[i]);
                fprintf(sf, "put \"%s\" \"%s\"\n", abs_locals[i], remote_paths[i]);
                files_in_dir++;
            }
        }
        if (files_in_dir > 1) {
            util_log(SEVERITY_INFO, "  Upload: %d files -> %s/", files_in_dir, unique_dirs[d]);
        }
    }

    fclose(sf);
    free(unique_dirs);

    util_log(SEVERITY_INFO, "ftp_upload_batch: Uploading %d file(s) in single session (%d dirs)...",
             valid_count, dir_count);

    // Read the script content back and run through the standard WinSCP runner
    // (which handles connection, logging, piped output, and process management)
    FILE *rf = fopen(".TEMP/winscp_batch_upload.txt", "r");
    if (!rf) {
        util_log(SEVERITY_ERROR, "ftp_upload_batch: Cannot read batch script.");
        free(abs_locals); free(valid);
        return false;
    }
    fseek(rf, 0, SEEK_END);
    long fsize = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    char *script_content = (char *)malloc((size_t)fsize + 1);
    if (!script_content) {
        fclose(rf);
        free(abs_locals); free(valid);
        return false;
    }
    size_t read_n = fread(script_content, 1, (size_t)fsize, rf);
    script_content[read_n] = '\0';
    fclose(rf);

    bool result = run_winscp_script(host, port, user, pass, script_content);

    // Retry loop: TCAdmin FTP servers intermittently fail passive data channel
    // transfers with 451 timeout. Since put (STOR) is idempotent (overwrites),
    // retrying the entire script is safe — already-uploaded files simply get
    // overwritten with the same content, and failed files get a fresh attempt
    // with a new PASV port allocation.
    static const int MAX_RETRIES = 3;
    for (int attempt = 1; !result && attempt < MAX_RETRIES; attempt++) {
        util_log(SEVERITY_WARNING, "ftp_upload_batch: Retry %d/%d — re-running upload script...",
                 attempt, MAX_RETRIES - 1);
        result = run_winscp_script(host, port, user, pass, script_content);
    }

    free(script_content);
    remove(".TEMP/winscp_batch_upload.txt");

    // With batch continue, WinSCP returns 0 only if ALL commands succeeded.
    // On partial failure, exit code is 1. We can't determine per-file status
    // from exit code alone, but the winscp.log shows individual transfers.
    if (result) {
        if (out_succeeded) *out_succeeded = valid_count;
        if (out_failed)    *out_failed = skipped;
        util_log(SEVERITY_INFO, "ftp_upload_batch: All %d file(s) uploaded successfully.", valid_count);
    } else {
        // Some or all failed — we report all valid as "attempted" and let caller
        // check the log for details. Conservative: mark all as failed.
        if (out_succeeded) *out_succeeded = 0;
        if (out_failed)    *out_failed = valid_count + skipped;
        util_log(SEVERITY_ERROR, "ftp_upload_batch: Upload session failed. Check .TEMP/winscp.log for details.");
    }

    free(abs_locals);
    free(valid);
    return result;
}

bool ftp_upload_directory(const char *host, int port, const char *user, const char *pass, const char *local_dir, const char *remote_dir) {
    if (ftp_use_native_backend()) {
        return ftp_native_upload_directory(host, port, user, pass, local_dir, remote_dir);
    }
    if (ftp_require_native_or_fail("ftp_upload_directory"))
        return false;
    char script[4096];
    // synchronize remote: upload all files from local_dir to remote_dir, creating dirs as needed
    snprintf(script, sizeof(script),
        "synchronize remote -filemask=\"*.xml;*.json;*.txt;*.md\" \"%s\" \"%s\"",
        remote_dir, local_dir);
    return run_winscp_script(host, port, user, pass, script);
}

bool ftp_download_recursive(const char *host, int port, const char *user, const char *pass, const char *remote_dir, const char *local_dir) {
    if (ftp_use_native_backend()) {
        return ftp_native_download_recursive(host, port, user, pass, remote_dir, local_dir);
    }
    if (ftp_require_native_or_fail("ftp_download_recursive"))
        return false;
    char script[4096];
    util_log(SEVERITY_INFO, "Hunter: Incremental download from '%s' — only changed files will transfer.", remote_dir);

    char include_mask[128] = "*.xml";
    char flag[8] = {0};
    if (util_read_ini_value("config/server_paths.ini", "DOWNLOAD_CONFIG_FILES", flag, sizeof(flag)) && atoi(flag) == 1) {
        strncpy(include_mask, "*.xml;*.json;*.cfg;*.ini;*.txt", sizeof(include_mask) - 1);
    }

    // Exclude engine dirs, cache, logs, hosting provider, runtime data, binaries,
    // persistence, server profiles, and mod utility sub-dirs.
    // WinSCP filemask: trailing slash = directory, NO leading slash (leading / = anchored).
    char mask[768];
    snprintf(mask, sizeof(mask),
        "%s | _CommonRedist/; steamapps/; battleye/; docs/; keys/; addons/;"
        " appcache/; logs/; dta/; HostHavocDayZServer/; server_manager/;"
        " backups/; config/;"
        " storage_1/; profiles/;"
        " Key/; key/; info/; extra/; extras/; Extras/; Readme_Terms/;"
        " *.pbo; *.bin; *.exe; *.dll; *.vdf; *.mdmp; *.RPT; *.log; *.ADM",
        include_mask);

    // Resolve local path to absolute — prevents CWD mismatch between
    // the auditor exe and the WinSCP child process.
    char abs_local[MAX_PATH_LEN];
    resolve_absolute_path(local_dir, abs_local, sizeof(abs_local));
    ensure_directory_tree(abs_local);

    // `synchronize local -criteria=size`:
    //   Uses FILE SIZE comparison instead of timestamps.
    //   FTP servers often have timezone offsets or minute-only precision,
    //   making timestamp comparison unreliable (`-neweronly` skips wrong files
    //   or re-downloads unchanged ones). Size comparison is immune to this:
    //     - Same size   → skip (file content unchanged)
    //     - Diff size   → download (mod updated, items added/removed)
    //     - New remote   → download
    //   Syntax: synchronize local <local_dir> <remote_dir>
    //   Note: `synchronize local` = download direction (remote → local).
    snprintf(script, sizeof(script),
        "synchronize local -criteria=size -filemask=\"%s\" \"%s\\\" \"%s\"",
        mask, abs_local, remote_dir);

    return run_winscp_script(host, port, user, pass, script);
}

bool ftp_download_batch(const char *host, int port, const char *user, const char *pass, const char **remote_paths, const char **local_paths, int count) {
    if (!remote_paths || !local_paths || count <= 0) return false;

    if (ftp_use_native_backend()) {
        return ftp_native_download_batch(host, port, user, pass,
                                         remote_paths, local_paths, count);
    }
    if (ftp_require_native_or_fail("ftp_download_batch"))
        return false;

    char script[4096];
    script[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (!remote_paths[i] || !local_paths[i]) continue;
        char abs_local[MAX_PATH_LEN];
        resolve_absolute_path(local_paths[i], abs_local, sizeof(abs_local));
        char line[512];
        // -neweronly + -preservetime: FTP timestamp comparison is unreliable,
        // but for single named files it's acceptable as a best-effort skip.
        snprintf(line, sizeof(line), "get -neweronly -preservetime \"%s\" \"%s\"\n", remote_paths[i], abs_local);
        if (strlen(script) + strlen(line) + 1 < sizeof(script)) {
            strcat(script, line);
        }
    }

    if (script[0] == '\0') return false;
    return run_winscp_script(host, port, user, pass, script);
}

bool ftp_download_core_files(const char *host, int port, const char *user, const char *pass, const char *remote_root, const char *local_root) {
    if (!remote_root || !local_root) return false;
    if (ftp_use_native_backend()) {
        return ftp_native_download_core_files(host, port, user, pass, remote_root, local_root);
    }
    if (ftp_require_native_or_fail("ftp_download_core_files"))
        return false;
    // Fallback: synchronize ALL XML+JSON files by size comparison.
    // Same approach as ftp_download_recursive but XML+JSON only (no cfg/ini/txt).
    char abs_local[MAX_PATH_LEN];
    resolve_absolute_path(local_root, abs_local, sizeof(abs_local));
    ensure_directory_tree(abs_local);

    char script[1536];
    snprintf(script, sizeof(script),
        "synchronize local -criteria=size -filemask=\"*.xml;*.json"
        " | _CommonRedist/; steamapps/; battleye/; addons/; keys/;"
        " appcache/; logs/; dta/; HostHavocDayZServer/; server_manager/;"
        " backups/; config/;"
        " storage_1/; profiles/;"
        " Key/; key/; info/; extra/; extras/; Extras/; Readme_Terms/;"
        " *.pbo; *.bin; *.exe; *.dll; *.mdmp; *.RPT; *.log; *.ADM\""
        " \"%s\\\" \"%s\"",
        abs_local, remote_root);
    return run_winscp_script(host, port, user, pass, script);
}

// ============================================================================
// DIRECTORY LISTING (for file browser)
// ============================================================================

/**
 * Parse one line of WinSCP `ls` console output.
 * WinSCP outputs lines like:
 *   D          02-01-26  04:13PM            Addons
 *   -  6796    02-01-26  04:21PM            typesDONOTREPLACE.xml
 */
static bool parse_ls_line(const char *line, RemoteFileEntry *entry) {
    if (!line || !entry) return false;

    // Skip blank lines and header lines
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '\n') return false;

    // WinSCP console `ls` format:
    // <type> <size/blank> <date> <time> <name>
    // D            02-01-26  04:13PM            Addons
    // -     6796   02-01-26  04:21PM            types.xml
    // ..           02-01-26  04:13PM            ..

    memset(entry, 0, sizeof(RemoteFileEntry));

    // First char: D=directory, -=file, .=parent
    char type = line[0];
    if (type == 'D') entry->is_directory = true;
    else if (type == '-') entry->is_directory = false;
    else if (type == '.') return false;  // skip "." and ".."
    else return false;  // unknown line

    // Skip type char and spaces
    const char *p = line + 1;
    while (*p == ' ') p++;

    // If file: parse size (digits), if dir: no size
    if (!entry->is_directory) {
        entry->size = 0;
        while (*p >= '0' && *p <= '9') {
            entry->size = entry->size * 10 + (*p - '0');
            p++;
        }
        while (*p == ' ') p++;
    }

    // Parse date: MM-DD-YY
    const char *date_start = p;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    // Parse time: HH:MMAM/PM
    while (*p && *p != ' ') p++;
    // Copy date+time range
    {
        int len = (int)(p - date_start);
        if (len > 0 && len < (int)sizeof(entry->date_str)) {
            memcpy(entry->date_str, date_start, len);
            entry->date_str[len] = '\0';
        }
    }

    while (*p == ' ') p++;

    // Rest is the filename
    if (*p) {
        // Trim trailing newline/whitespace
        char name_buf[256];
        strncpy(name_buf, p, sizeof(name_buf) - 1);
        name_buf[sizeof(name_buf) - 1] = '\0';
        int len = (int)strlen(name_buf);
        while (len > 0 && (name_buf[len-1] == '\n' || name_buf[len-1] == '\r' || name_buf[len-1] == ' '))
            name_buf[--len] = '\0';
        if (len == 0) return false;
        if (strcmp(name_buf, ".") == 0 || strcmp(name_buf, "..") == 0) return false;
        strncpy(entry->name, name_buf, sizeof(entry->name) - 1);
    } else {
        return false;
    }

    return true;
}

// Comparator for RemoteFileEntry: directories first, then alphabetical by name.
static int cmp_remote_entries(const void *a, const void *b) {
    const RemoteFileEntry *ea = (const RemoteFileEntry *)a;
    const RemoteFileEntry *eb = (const RemoteFileEntry *)b;
    if (ea->is_directory != eb->is_directory)
        return eb->is_directory - ea->is_directory; // dirs first
    return strcmp(ea->name, eb->name);
}

bool ftp_list_directory(const char *host, int port, const char *user, const char *pass,
                        const char *remote_path, RemoteFileBrowser *browser) {
    if (!browser) return false;
    browser->count = 0;
    browser->error[0] = '\0';
    if (remote_path)
        strncpy(browser->current_path, remote_path, sizeof(browser->current_path) - 1);
    else
        browser->current_path[0] = '\0';

    /* Prefer native list (no password argv / TEMP scripts). */
    if (ftp_use_native_backend()) {
        return ftp_native_list_directory(host, port, user, pass, remote_path, browser);
    }

    if (!ftp_winscp_fallback_allowed()) {
        snprintf(browser->error, sizeof(browser->error),
                 "No FTP backend: build with STELLI_USE_LIBCURL "
                 "(native required; WinSCP is Windows-only)");
        util_log(SEVERITY_ERROR, "ftp_list_directory: %s", browser->error);
        return false;
    }

    char winscp_path[512];
    get_winscp_absolute_path(winscp_path, sizeof(winscp_path));

    // Verify WinSCP exists
    FILE *f = fopen(winscp_path, "rb");
    if (!f) {
        snprintf(browser->error, sizeof(browser->error), "WinSCP not found: %s", winscp_path);
        return false;
    }
    fclose(f);

    char safe_user[512], safe_pass[512];
    strict_url_encode(user, safe_user, sizeof(safe_user));
    strict_url_encode(pass, safe_pass, sizeof(safe_pass));

    // Write script
    util_ensure_directory(".TEMP");
    FILE *script = fopen(".TEMP/winscp_ls.txt", "w");
    if (!script) {
        snprintf(browser->error, sizeof(browser->error), "Cannot create script file");
        return false;
    }
    fprintf(script, "option batch on\n");
    fprintf(script, "option confirm off\n");
    fprintf(script, "option reconnecttime 120\n");
    /* Legacy WinSCP: -hostkey=* is weaker than native known_hosts/HOST_KEY_PIN. */
    if (port == 8827 || port == 22 || port == 2222) {
        fprintf(script, "open sftp://%%1%%:%%2%%@%s:%d/ -hostkey=* "
                        "-rawsettings PingType=1 PingInterval=10 Timeout=120\n", host, port);
    } else {
        fprintf(script, "open ftp://%%1%%:%%2%%@%s:%d/ -certificate=* "
                        "-rawsettings FtpPingType=1 FtpPingInterval=10 Timeout=120\n", host, port);
    }
    fprintf(script, "ls \"%s\"\n", remote_path);
    fprintf(script, "exit\n");
    fclose(script);

    // Build command — piped via CreateProcess, stdout captured to file for parsing
    char cmd[4096];
    snprintf(cmd, sizeof(cmd),
        "\"%s\" /ini=nul /script=.TEMP/winscp_ls.txt /log=.TEMP/winscp_ls.log /loglevel=0 /parameter \"%s\" \"%s\"",
        winscp_path, safe_user, safe_pass);

    int result = run_winscp_piped(cmd, ".TEMP/winscp_ls_output.txt");
    remove(".TEMP/winscp_ls.txt");

    if (result != 0) {
        snprintf(browser->error, sizeof(browser->error), "WinSCP ls failed (code %d)", result);
        return false;
    }

    // Parse output
    FILE *out = fopen(".TEMP/winscp_ls_output.txt", "r");
    if (!out) {
        snprintf(browser->error, sizeof(browser->error), "Cannot read ls output");
        return false;
    }

    char line[1024];
    while (fgets(line, sizeof(line), out) && browser->count < MAX_BROWSER_ENTRIES) {
        RemoteFileEntry entry;
        if (parse_ls_line(line, &entry)) {
            // Build full path
            char path_buf[MAX_PATH_LEN];
            size_t rlen = strlen(remote_path);
            if (rlen > 0 && remote_path[rlen - 1] == '/') {
                snprintf(path_buf, sizeof(path_buf), "%s%s", remote_path, entry.name);
            } else {
                snprintf(path_buf, sizeof(path_buf), "%s/%s", remote_path, entry.name);
            }
            strncpy(entry.full_path, path_buf, sizeof(entry.full_path) - 1);
            browser->entries[browser->count++] = entry;
        }
    }
    fclose(out);

    // Sort: directories first, then alphabetical (qsort — O(n log n))
    qsort(browser->entries, (size_t)browser->count, sizeof(RemoteFileEntry),
          cmp_remote_entries);

    util_log(SEVERITY_INFO, "Browser: Listed %d entries in '%s'", browser->count, remote_path);
    return true;
}

// ============================================================================
// REMOTE CLEANUP: Remove numbered restore-point artifacts from server dirs
// ============================================================================
// Restore point backups are named "NNN__sanitized_remote_path" locally, and a
// bug in the old directory-grouped `put` uploaded them to the server with those
// names instead of the intended target filename.  The DayZ CE engine loads ALL
// files in db/ — extra files crash it.  This function removes stale artifacts.

bool ftp_cleanup_remote_dir(const char *host, int port, const char *user, const char *pass,
                            const char *remote_dir) {
    if (!host || !user || !pass || !remote_dir) return false;

    if (ftp_use_native_backend()) {
        return ftp_native_cleanup_remote_dir(host, port, user, pass, remote_dir);
    }
    if (ftp_require_native_or_fail("ftp_cleanup_remote_dir"))
        return false;

    // WinSCP rm supports filemask/wildcards.
    // Pattern: 3-digit prefix, double underscore, anything  — e.g. 001__*.
    // We run with batch continue so missing matches don't abort.
    char script[2048];
    snprintf(script, sizeof(script),
             "option batch continue\n"
             "rm \"%s/[0-9][0-9][0-9]__*\"\n",
             remote_dir);

    util_log(SEVERITY_INFO, "Cleanup: Removing numbered artifact files from %s/", remote_dir);
    bool ok = run_winscp_script(host, port, user, pass, script);
    if (!ok) {
        // Not fatal — if no artifacts exist, WinSCP returns error with batch continue
        util_log(SEVERITY_INFO, "Cleanup: No numbered artifacts found in %s/ (or already clean).", remote_dir);
    }
    return true;  // Always succeed — cleanup is best-effort
}

// ============================================================================
// POST-UPLOAD INTEGRITY VERIFICATION
// ============================================================================

bool ftp_verify_uploads(const char *host, int port, const char *user, const char *pass,
                        const char **local_paths, const char **remote_paths, int count) {
    if (!host || !user || !pass || !local_paths || !remote_paths || count <= 0) return false;

    if (ftp_use_native_backend()) {
        return ftp_native_verify_uploads(host, port, user, pass,
                                         local_paths, remote_paths, count);
    }
    if (ftp_require_native_or_fail("ftp_verify_uploads"))
        return false;

    util_log(SEVERITY_INFO, "Upload integrity check: verifying %d file(s) on server...", count);

    // Build a WinSCP script that uses 'stat' on each remote file.
    // WinSCP 'stat' outputs file info — if it fails, the file is missing.
    // We use batch continue so one failure doesn't abort the rest.
    // Output is captured via piped stdout/stderr.
    size_t script_size = (size_t)(count * 600 + 256);
    char *script = (char *)malloc(script_size);
    if (!script) {
        util_log(SEVERITY_ERROR, "Verify: Memory allocation failed.");
        return false;
    }

    int offset = 0;
    offset += snprintf(script + offset, script_size - (size_t)offset, "option batch continue\n");

    for (int i = 0; i < count; i++) {
        if (!remote_paths[i]) continue;
        offset += snprintf(script + offset, script_size - (size_t)offset,
                           "stat \"%s\"\n", remote_paths[i]);
    }

    bool ok = run_winscp_script(host, port, user, pass, script);
    free(script);

    // Parse WinSCP log to find upload verification results.
    // The stat command outputs file attributes if the file exists.
    // If any stat fails, the log will contain error messages.
    // Parse the log to check for failures.
    int verified = 0;
    int missing = 0;

    FILE *log = fopen(".TEMP/winscp.log", "r");
    if (log) {
        char line[1024];
        while (fgets(line, sizeof(line), log)) {
            // WinSCP log lines with "< 2" followed by file path indicate stat output
            // Error lines indicate missing files
            if (strstr(line, "Can't get attributes of file") ||
                strstr(line, "No such file or directory") ||
                strstr(line, "File or folder") ) {
                missing++;
            }
        }
        fclose(log);
    }

    verified = count - missing;

    if (missing > 0) {
        util_log(SEVERITY_ERROR, "Upload integrity FAILED: %d/%d files missing or inaccessible on server!",
                 missing, count);

        // Do a local size sanity check — flag any 0-byte or suspiciously named files
        for (int i = 0; i < count; i++) {
            if (!local_paths[i]) continue;
            char abs_path[MAX_PATH_LEN];
            resolve_absolute_path(local_paths[i], abs_path, MAX_PATH_LEN);
#ifdef _WIN32
            WIN32_FIND_DATAA fd;
            HANDLE hf = FindFirstFileA(abs_path, &fd);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD size = fd.nFileSizeLow;
                FindClose(hf);
                if (size == 0) {
                    util_log(SEVERITY_ERROR, "  CORRUPT: '%s' is 0 bytes!", local_paths[i]);
                }
            } else {
                util_log(SEVERITY_ERROR, "  MISSING: '%s' not found locally!", local_paths[i]);
            }
#endif
        }
        return false;
    }

    // Also validate local files aren't corrupt (0-byte or artifact-named)
    int corrupt_count = 0;
    for (int i = 0; i < count; i++) {
        if (!local_paths[i]) continue;
        const char *basename = strrchr(local_paths[i], '\\');
        if (!basename) basename = strrchr(local_paths[i], '/');
        if (basename) basename++;
        else basename = local_paths[i];

        // Check for numbered artifact pattern in uploaded filename
        int d = 0;
        while (basename[d] >= '0' && basename[d] <= '9') d++;
        if (d >= 2 && basename[d] == '_') {
            util_log(SEVERITY_ERROR, "  ARTIFACT: '%s' has numbered artifact filename!", basename);
            corrupt_count++;
        }
    }

    if (corrupt_count > 0) {
        util_log(SEVERITY_ERROR, "Upload integrity WARNING: %d file(s) have artifact filenames.", corrupt_count);
        return false;
    }

    util_log(SEVERITY_INFO, "Upload integrity OK: all %d files verified on server.", verified);
    return true;
}

static void sanitize_remote_path_filename(const char *remote_path, char *out, size_t out_len) {
    if (!remote_path || !out || out_len == 0) return;
    size_t n = 0;
    for (const char *p = remote_path; *p && n < out_len - 1; p++) {
        unsigned char ch = (unsigned char)*p;
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            out[n++] = (char)ch;
        } else {
            out[n++] = '_';
        }
    }
    if (n == 0 && out_len > 1) {
        out[n++] = 'f';
        out[n++] = 'i';
        out[n++] = 'l';
        out[n++] = 'e';
    }
    out[n] = '\0';
}

static void ensure_directory_tree(const char *path) {
    if (!path || !*path) return;
#ifdef _WIN32
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            if (tmp[0]) CreateDirectoryA(tmp, NULL);
            *p = save;
        }
    }
    if (tmp[0]) CreateDirectoryA(tmp, NULL);
#else
    util_ensure_directory(path);
#endif
}

bool ftp_create_restore_point(const char *host, int port, const char *user, const char *pass,
                              const char **remote_paths, int count,
                              const char *backup_dir, const char *manifest_path) {
    if (!host || !user || !pass || !remote_paths || count <= 0 || !backup_dir || !manifest_path) {
        return false;
    }

    if (ftp_use_native_backend()) {
        return ftp_native_create_restore_point(host, port, user, pass,
                                               remote_paths, count,
                                               backup_dir, manifest_path);
    }
    if (ftp_require_native_or_fail("ftp_create_restore_point"))
        return false;

    ensure_directory_tree(backup_dir);

    // Pre-build the manifest and local backup paths, then download all in one session.
    char (*local_backups)[MAX_PATH_LEN] = (char (*)[MAX_PATH_LEN])calloc((size_t)count, MAX_PATH_LEN);
    char (*abs_backups)[MAX_PATH_LEN]   = (char (*)[MAX_PATH_LEN])calloc((size_t)count, MAX_PATH_LEN);
    bool *valid = (bool *)calloc((size_t)count, sizeof(bool));
    if (!local_backups || !abs_backups || !valid) {
        util_log(SEVERITY_ERROR, "Restore point: Memory allocation failed.");
        free(local_backups); free(abs_backups); free(valid);
        return false;
    }

    int valid_count = 0;
    for (int i = 0; i < count; i++) {
        if (!remote_paths[i] || !*remote_paths[i]) { valid[i] = false; continue; }

        char safe_name[384];
        sanitize_remote_path_filename(remote_paths[i], safe_name, sizeof(safe_name));
        snprintf(local_backups[i], MAX_PATH_LEN, "%s/%03d_%s", backup_dir, i + 1, safe_name);
        resolve_absolute_path(local_backups[i], abs_backups[i], MAX_PATH_LEN);

        // Ensure parent directory exists for each local backup file
        {
            char parent[MAX_PATH_LEN];
            strncpy(parent, abs_backups[i], sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            char *last_sep = strrchr(parent, '\\');
            if (!last_sep) last_sep = strrchr(parent, '/');
            if (last_sep) {
                *last_sep = '\0';
                ensure_directory_tree(parent);
            }
        }

        valid[i] = true;
        valid_count++;
    }

    if (valid_count == 0) {
        util_log(SEVERITY_ERROR, "Restore point: No valid remote paths to backup.");
        free(local_backups); free(abs_backups); free(valid);
        return false;
    }

    // Build a single WinSCP script with all get commands (batch continue so
    // missing remote files don't abort the session — new files won't exist yet).
    util_ensure_directory(".TEMP");
    FILE *sf = fopen(".TEMP/winscp_restore_dl.txt", "w");
    if (!sf) {
        util_log(SEVERITY_ERROR, "Restore point: Cannot create batch script file.");
        free(local_backups); free(abs_backups); free(valid);
        return false;
    }

    fprintf(sf, "option batch continue\n");
    for (int i = 0; i < count; i++) {
        if (!valid[i]) continue;
        fprintf(sf, "get \"%s\" \"%s\"\n", remote_paths[i], abs_backups[i]);
    }
    fclose(sf);

    // Read back and execute
    FILE *rf = fopen(".TEMP/winscp_restore_dl.txt", "r");
    if (!rf) {
        free(local_backups); free(abs_backups); free(valid);
        return false;
    }
    fseek(rf, 0, SEEK_END);
    long fsize = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    char *script_content = (char *)malloc((size_t)fsize + 1);
    if (!script_content) { fclose(rf); free(local_backups); free(abs_backups); free(valid); return false; }
    size_t read_n = fread(script_content, 1, (size_t)fsize, rf);
    script_content[read_n] = '\0';
    fclose(rf);

    util_log(SEVERITY_INFO, "Restore point: Downloading %d file(s) in single session...", valid_count);
    run_winscp_script(host, port, user, pass, script_content);
    free(script_content);
    remove(".TEMP/winscp_restore_dl.txt");

    // Build manifest from files that actually downloaded successfully
    FILE *manifest = fopen(manifest_path, "w");
    if (!manifest) {
        util_log(SEVERITY_ERROR, "Restore point: Failed to create manifest '%s'", manifest_path);
        free(local_backups); free(abs_backups); free(valid);
        return false;
    }

    char ts[64] = {0};
    util_timestamp(ts, sizeof(ts));
    fprintf(manifest, "# Stelliferum restore point\n");
    fprintf(manifest, "# Created: %s\n", ts);
    fprintf(manifest, "# Format: remote_path|local_backup_file\n");

    int success_count = 0;
    int fail_count = 0;
    for (int i = 0; i < count; i++) {
        if (!valid[i]) continue;
        if (util_file_exists(local_backups[i])) {
            fprintf(manifest, "%s|%s\n", remote_paths[i], local_backups[i]);
            success_count++;
        } else {
            util_log(SEVERITY_WARNING, "Restore point: Could not snapshot '%s' (file not on server?)", remote_paths[i]);
            fail_count++;
        }
    }
    fclose(manifest);

    free(local_backups);
    free(abs_backups);
    free(valid);

    if (success_count == 0) {
        util_log(SEVERITY_ERROR, "Restore point: No files captured (%d failed)", fail_count);
        return false;
    }

    util_log(SEVERITY_INFO,
             "Restore point: Captured %d file(s), %d skipped/failed. Manifest: %s",
             success_count, fail_count, manifest_path);
    return true;
}

bool ftp_restore_from_manifest(const char *host, int port, const char *user, const char *pass,
                               const char *manifest_path) {
    if (!host || !user || !pass || !manifest_path) return false;

    if (ftp_use_native_backend()) {
        return ftp_native_restore_from_manifest(host, port, user, pass, manifest_path);
    }
    if (ftp_require_native_or_fail("ftp_restore_from_manifest"))
        return false;

    FILE *manifest = fopen(manifest_path, "r");
    if (!manifest) {
        util_log(SEVERITY_ERROR, "Restore: Manifest not found '%s'", manifest_path);
        return false;
    }

    // First pass: count entries to allocate arrays
    char line[2048];
    int entry_count = 0;
    while (fgets(line, sizeof(line), manifest)) {
        util_trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (strchr(line, '|')) entry_count++;
    }

    if (entry_count == 0) {
        fclose(manifest);
        util_log(SEVERITY_ERROR, "Restore: Manifest has no entries.");
        return false;
    }

    // Allocate arrays for local/remote paths
    char (*locals)[MAX_PATH_LEN]  = (char (*)[MAX_PATH_LEN])calloc((size_t)entry_count, MAX_PATH_LEN);
    char (*remotes)[MAX_PATH_LEN] = (char (*)[MAX_PATH_LEN])calloc((size_t)entry_count, MAX_PATH_LEN);
    if (!locals || !remotes) {
        fclose(manifest);
        free(locals); free(remotes);
        util_log(SEVERITY_ERROR, "Restore: Memory allocation failed.");
        return false;
    }

    // Second pass: collect valid entries
    rewind(manifest);
    int valid_count = 0;
    int skipped = 0;
    while (fgets(line, sizeof(line), manifest)) {
        util_trim(line);
        if (!line[0] || line[0] == '#') continue;

        char *sep = strchr(line, '|');
        if (!sep) continue;
        *sep = '\0';

        const char *remote_path = line;
        const char *local_path = sep + 1;
        if (!remote_path[0] || !local_path[0]) continue;

        if (!util_file_exists(local_path)) {
            util_log(SEVERITY_WARNING, "Restore: Missing local backup '%s'", local_path);
            skipped++;
            continue;
        }

        strncpy(locals[valid_count], local_path, MAX_PATH_LEN - 1);
        strncpy(remotes[valid_count], remote_path, MAX_PATH_LEN - 1);
        valid_count++;
    }
    fclose(manifest);

    if (valid_count == 0) {
        util_log(SEVERITY_ERROR, "Restore: No valid files to restore (%d skipped).", skipped);
        free(locals); free(remotes);
        return false;
    }

    // Build pointer arrays for ftp_upload_batch
    const char **local_ptrs  = (const char **)malloc(sizeof(const char *) * (size_t)valid_count);
    const char **remote_ptrs = (const char **)malloc(sizeof(const char *) * (size_t)valid_count);
    if (!local_ptrs || !remote_ptrs) {
        free(locals); free(remotes); free(local_ptrs); free(remote_ptrs);
        return false;
    }
    for (int i = 0; i < valid_count; i++) {
        local_ptrs[i]  = locals[i];
        remote_ptrs[i] = remotes[i];
    }

    util_log(SEVERITY_INFO, "Restore: Uploading %d file(s) in single session...", valid_count);

    int succeeded = 0, failed = 0;
    bool ok = ftp_upload_batch(host, port, user, pass, local_ptrs, remote_ptrs, valid_count, &succeeded, &failed);

    free(local_ptrs);
    free(remote_ptrs);
    free(locals);
    free(remotes);

    util_log(SEVERITY_INFO, "Restore summary: %d restored, %d failed", succeeded, failed + skipped);
    return ok;
}

// ============================================================================
// RESTORE POINT MANAGEMENT — Multiple Timestamped Restore Points
// ============================================================================

void ftp_generate_restore_dir(char *dir_buf, int dir_size, char *manifest_buf, int manifest_size) {
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(dir_buf, dir_size, "backups/%04d%02d%02d_%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
#else
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    snprintf(dir_buf, dir_size, "backups/%04d%02d%02d_%02d%02d%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
#endif
    snprintf(manifest_buf, manifest_size, "%s/restore_manifest.txt", dir_buf);
}

// Compare RestorePointInfo by label (descending — newest first)
static int rp_compare_desc(const void *a, const void *b) {
    const RestorePointInfo *ra = (const RestorePointInfo *)a;
    const RestorePointInfo *rb = (const RestorePointInfo *)b;
    return strcmp(rb->label, ra->label);  // descending
}

int ftp_list_restore_points(RestorePointInfo *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    int count = 0;

#ifdef _WIN32
    // Scan backups/ for directories containing restore_manifest.txt
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("backups\\*", &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;  // skip . and ..
        if (count >= max_count) break;

        // Check if this directory has a restore_manifest.txt
        char manifest[MAX_PATH_LEN];
        snprintf(manifest, sizeof(manifest), "backups/%s/restore_manifest.txt", fd.cFileName);
        if (!util_file_exists(manifest)) continue;

        RestorePointInfo *rp = &out[count];
        snprintf(rp->dir, sizeof(rp->dir), "backups/%s", fd.cFileName);
        strncpy(rp->manifest, manifest, sizeof(rp->manifest) - 1);
        rp->manifest[sizeof(rp->manifest) - 1] = '\0';

        // Generate human-readable label from directory name
        // Format: "20250115_143022" -> "2025-01-15 14:30:22"
        // Or "last_upload" -> "Last Upload (legacy)"
        if (strcmp(fd.cFileName, "last_upload") == 0) {
            strncpy(rp->label, "Last Upload (legacy)", sizeof(rp->label) - 1);
        } else if (strlen(fd.cFileName) == 15 && fd.cFileName[8] == '_') {
            // Parse YYYYMMDD_HHMMSS format
            snprintf(rp->label, sizeof(rp->label),
                     "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                     fd.cFileName, fd.cFileName + 4, fd.cFileName + 6,
                     fd.cFileName + 9, fd.cFileName + 11, fd.cFileName + 13);
        } else {
            strncpy(rp->label, fd.cFileName, sizeof(rp->label) - 1);
        }
        rp->label[sizeof(rp->label) - 1] = '\0';

        count++;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#endif

    // Sort newest first (descending by label)
    if (count > 1) {
        qsort(out, (size_t)count, sizeof(RestorePointInfo), rp_compare_desc);
    }

    return count;
}

// Recursively delete a directory and all its contents
static void remove_directory_recursive(const char *path) {
#ifdef _WIN32
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (fd.cFileName[0] == '.' &&
            (fd.cFileName[1] == '\0' || (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0')))
            continue;

        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s\\%s", path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            remove_directory_recursive(full);
        } else {
            DeleteFileA(full);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);

    RemoveDirectoryA(path);
#endif
}

void ftp_purge_old_restore_points(int max_keep) {
    if (max_keep < 1) max_keep = 1;

    RestorePointInfo all[MAX_RESTORE_POINTS];
    int count = ftp_list_restore_points(all, MAX_RESTORE_POINTS);

    if (count <= max_keep) return;

    // all[] is sorted newest-first; delete everything past max_keep
    for (int i = max_keep; i < count; i++) {
        util_log(SEVERITY_INFO, "Purging old restore point: %s", all[i].label);
        remove_directory_recursive(all[i].dir);
    }
}
