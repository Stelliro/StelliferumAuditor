
#include "auditor.h"
#include "resource_ids.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/**
 * Extract an embedded RCDATA resource to a file on disk.
 * Returns true on success.
 */
static bool extract_resource_to_file(int resource_id, const char *dest_path) {
#ifdef _WIN32
    HRSRC hRes = FindResource(NULL, MAKEINTRESOURCE(resource_id), RT_RCDATA);
    if (!hRes) {
        util_log(SEVERITY_ERROR, "Resource %d not found in executable.", resource_id);
        return false;
    }

    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) {
        util_log(SEVERITY_ERROR, "Failed to load resource %d.", resource_id);
        return false;
    }

    DWORD size = SizeofResource(NULL, hRes);
    void *data = LockResource(hData);
    if (!data || size == 0) {
        util_log(SEVERITY_ERROR, "Failed to lock resource %d (size=%lu).", resource_id, (unsigned long)size);
        return false;
    }

    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        util_log(SEVERITY_ERROR, "Cannot write to '%s'.", dest_path);
        return false;
    }

    size_t written = fwrite(data, 1, size, f);
    fclose(f);

    if (written != size) {
        util_log(SEVERITY_ERROR, "Incomplete write: %zu / %lu bytes.", written, (unsigned long)size);
        remove(dest_path);
        return false;
    }

    return true;
#else
    (void)resource_id;
    (void)dest_path;
    return false;
#endif
}

/**
 * Create WinSCP.com (console version) from WinSCP.exe (GUI version).
 *
 * Windows-only optional fallback path — never used on Linux/macOS (no PE patch).
 *
 * WinSCP.com and WinSCP.exe are identical binaries — the only difference is
 * the PE subsystem field: GUI (2) vs Console (3). The console version is
 * required for scripting because system() needs a console-subsystem process
 * to properly capture output and block until completion.
 *
 * This copies the file and patches exactly ONE word in the PE header.
 * The original WinSCP.exe is never modified.
 */
#ifdef _WIN32
static bool create_console_variant(const char *exe_path, const char *com_path) {
    FILE *src = fopen(exe_path, "rb");
    if (!src) return false;

    // Get file size
    fseek(src, 0, SEEK_END);
    long file_size = ftell(src);
    fseek(src, 0, SEEK_SET);

    // Read entire file into memory
    unsigned char *buf = (unsigned char *)malloc(file_size);
    if (!buf) { fclose(src); return false; }
    if ((long)fread(buf, 1, file_size, src) != file_size) {
        free(buf); fclose(src); return false;
    }
    fclose(src);

    // Validate DOS header magic ("MZ")
    if (file_size < 64 || buf[0] != 'M' || buf[1] != 'Z') {
        util_log(SEVERITY_ERROR, "Not a valid PE: missing MZ header.");
        free(buf); return false;
    }

    // Read e_lfanew (offset to PE header) at file offset 0x3C
    long e_lfanew = *(long *)(buf + 0x3C);
    if (e_lfanew <= 0 || e_lfanew + 0x5E > file_size) {
        util_log(SEVERITY_ERROR, "Not a valid PE: bad e_lfanew (%ld).", e_lfanew);
        free(buf); return false;
    }

    // Validate PE signature ("PE\0\0")
    if (buf[e_lfanew] != 'P' || buf[e_lfanew + 1] != 'E' ||
        buf[e_lfanew + 2] != 0  || buf[e_lfanew + 3] != 0) {
        util_log(SEVERITY_ERROR, "Not a valid PE: missing PE signature.");
        free(buf); return false;
    }

    // Subsystem field is at: e_lfanew + 24 (optional header start) + 68 = e_lfanew + 92 (0x5C)
    long subsystem_offset = e_lfanew + 0x5C;
    unsigned short subsystem = *(unsigned short *)(buf + subsystem_offset);

    if (subsystem != 2 /* IMAGE_SUBSYSTEM_WINDOWS_GUI */) {
        util_log(SEVERITY_WARNING, "PE subsystem is %d (expected 2/GUI). Copying as-is.", subsystem);
    }

    // Patch: GUI (2) -> Console (3)
    *(unsigned short *)(buf + subsystem_offset) = 3; /* IMAGE_SUBSYSTEM_WINDOWS_CUI */

    // Write patched copy
    FILE *dst = fopen(com_path, "wb");
    if (!dst) {
        util_log(SEVERITY_ERROR, "Cannot write to '%s'.", com_path);
        free(buf); return false;
    }

    size_t written = fwrite(buf, 1, file_size, dst);
    fclose(dst);
    free(buf);

    if ((long)written != file_size) {
        util_log(SEVERITY_ERROR, "Incomplete write for console variant.");
        remove(com_path);
        return false;
    }

    return true;
}
#endif /* _WIN32 */

void util_check_and_install_dependencies() {
    /*
     * linux-no-winscp / roadmap #7:
     * WinSCP extract + PE subsystem patch is Windows-only optional fallback.
     * Linux/macOS: soft-skip / no-op — never extract, PE-patch, or require
     * WinSCP.exe for transfers. Native libcurl is the required path.
     */
#ifndef _WIN32
    util_log(SEVERITY_INFO,
             "Dependency Check: soft-skip on non-Windows — WinSCP not used "
             "(transfer path: native FTP/SFTP via libcurl only).");
#ifdef STELLI_USE_LIBCURL
    util_log(SEVERITY_INFO,
             "Dependency Check: STELLI_USE_LIBCURL enabled (native backend ready).");
#else
    util_log(SEVERITY_WARNING,
             "Dependency Check: STELLI_USE_LIBCURL not built — FTP/SFTP "
             "unavailable on this platform (no WinSCP fallback).");
#endif
    return;
#else
    // Build paths next to the running executable
    char exe_dir[MAX_PATH_LEN] = ".";
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH_LEN);
    char *slash = strrchr(exe_dir, '\\');
    if (slash) *slash = '\0';

    char winscp_exe[MAX_PATH_LEN];
    char winscp_com[MAX_PATH_LEN];
    snprintf(winscp_exe, sizeof(winscp_exe), "%s\\WinSCP.exe", exe_dir);
    snprintf(winscp_com, sizeof(winscp_com), "%s\\WinSCP.com", exe_dir);

    // If console variant already exists next to our exe, we're good
    FILE *f = fopen(winscp_com, "rb");
    if (f) {
        fclose(f);
        util_log(SEVERITY_INFO, "Dependency Check: WinSCP ready (optional Windows fallback).");
        return;
    }

#ifdef STELLI_USE_LIBCURL
    /* Native preferred: still try to install WinSCP for advanced/fallback ops,
     * but soft-fail if embed missing — core transfer does not require it. */
    util_log(SEVERITY_INFO,
             "Dependency Check: native libcurl enabled; WinSCP install is optional fallback.");
#endif

    // Step 1: Extract embedded WinSCP.exe from our resources
    f = fopen(winscp_exe, "rb");
    if (!f) {
        util_log(SEVERITY_INFO, "Extracting embedded WinSCP (optional fallback) ...");
        if (!extract_resource_to_file(IDR_WINSCP_EXE, winscp_exe)) {
#ifdef STELLI_USE_LIBCURL
            util_log(SEVERITY_INFO,
                     "WinSCP not embedded/found — OK: core transfer uses native libcurl. "
                     "Advanced WinSCP-only ops need WinSCP.exe beside the executable.");
#else
            util_log(SEVERITY_WARNING, "Could not extract embedded WinSCP. FTP features unavailable.");
            util_log(SEVERITY_INFO, "Manual fallback: place WinSCP.exe next to StelliferumAuditor.exe");
#endif
            return;
        }
    } else {
        fclose(f);
    }

    // Step 2: Create WinSCP.com (console variant) by patching the PE subsystem header
    util_log(SEVERITY_INFO, "Creating WinSCP console interface (optional fallback) ...");
    if (create_console_variant(winscp_exe, winscp_com)) {
        f = fopen(winscp_com, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            util_log(SEVERITY_INFO, "WinSCP ready: WinSCP.com (%.1f MB, console mode, optional fallback).",
                     (double)sz / (1024.0 * 1024.0));
        }
    } else {
#ifdef STELLI_USE_LIBCURL
        util_log(SEVERITY_WARNING,
                 "Failed to create WinSCP console variant. Core transfer still uses native libcurl.");
#else
        util_log(SEVERITY_WARNING, "Failed to create console variant. FTP may hang.");
#endif
    }
#endif /* _WIN32 */
}
