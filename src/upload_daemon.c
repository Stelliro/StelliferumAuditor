/**
 * STELLIFERUM AUDITOR — Upload Daemon Service
 * ==============================================
 * Background thread service for Save & Upload operations.
 * Generates all output files and uploads them to the server
 * without blocking the UI thread.
 *
 * Architecture:
 *   - UI submits a job via upload_daemon_start()
 *   - Daemon thread runs file generation → restore point → sequential uploads
 *   - UI polls upload_daemon_state() for progress/status
 *   - On completion, UI reads final results from the state struct
 *
 * Thread safety:
 *   - UploadDaemonState fields are written only by the daemon thread
 *     (except `cancel_requested` written by UI)
 *   - UI reads state via volatile-qualified accessors
 *   - No mutex needed: single-writer, single-reader, aligned fields
 */

#include "auditor.h"

#ifdef _WIN32
// Suppress C4005 — intentional Win32 macro redefines (same as ui.cpp)
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4005)
#endif
#define WIN32_LEAN_AND_MEAN
#define Rectangle   WinRectangle
#define CloseWindow WinCloseWindow
#define ShowCursor  WinShowCursor
#define DrawText    WinDrawText
#define DrawTextEx  WinDrawTextEx
#define DrawLine    WinDrawLine
#define LoadImage   WinLoadImage
#define PlaySound   WinPlaySound
#include <windows.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#undef Rectangle
#undef CloseWindow
#undef ShowCursor
#undef DrawText
#undef DrawTextEx
#undef DrawLine
#undef LoadImage
#undef PlaySound
#undef SEVERITY_ERROR
#undef SEVERITY_SUCCESS
#undef FILE_TYPE_UNKNOWN
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ============================================================================
// INTERNAL STATE
// ============================================================================

static UploadDaemonState s_daemon = {0};

#ifdef _WIN32
static HANDLE s_daemon_thread = NULL;
#endif

// ============================================================================
// HELPER: path_join (mirrors ui.cpp version, kept local to avoid cross-module dep)
// ============================================================================
static void daemon_normalize_root(char *root, size_t max_len) {
    size_t len = strlen(root);
    if (len == 0 || len >= max_len - 2) return;
    if (root[len - 1] != '/') {
        root[len] = '/';
        root[len + 1] = '\0';
    }
}

static void daemon_path_join(char *out, size_t out_len, const char *base, const char *rel) {
    char tmp[256];
    strncpy(tmp, base, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    daemon_normalize_root(tmp, sizeof(tmp));
    while (*rel == '/') rel++;
    snprintf(out, out_len, "%s%s", tmp, rel);
}

// ============================================================================
// PHASE 0: BACKUP & CLEAN OUTPUT DIRECTORY
// ============================================================================
static void daemon_phase_clean_output(void) {
    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Backing up & cleaning output...");
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "output/");
    util_log(SEVERITY_INFO, "[UploadDaemon] Phase 0: Backing up and cleaning output directory...");
    util_backup_and_clean_output();
}

// ============================================================================
// PHASE 1: GENERATE OUTPUT FILES
// ============================================================================
static void daemon_phase_generate(UploadDaemonJob *job) {
    AuditorContext *ctx = job->ctx;

    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Generating output files...");
    s_daemon.phase = UPLOAD_PHASE_GENERATING;
    util_log(SEVERITY_INFO, "[UploadDaemon] Phase 1: Generating output files...");

    util_ensure_directory("output");
    auditor_sort_items(ctx);

    // types.xml
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "types.xml");
    writer_export_merged_xml(ctx, "output/types.xml");

    if (s_daemon.cancel_requested) return;

    // cfgspawnabletypes.xml — intentionally NOT uploaded.
    // The auditor's spawnable generator produces 5000+ entries (including auto-generated
    // cargo for clothing/misc) which causes DayZ CE to freeze for 15+ minutes at startup.
    // Mod-internal spawnables (compiled into PBOs) are sufficient; a mission-level
    // cfgspawnabletypes.xml is only needed for server-specific overrides.
    // Still export locally so the file is available for review.
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "cfgspawnabletypes.xml");
    writer_export_spawnable_types(ctx, "output/cfgspawnabletypes.xml");

    if (s_daemon.cancel_requested) return;

    // Store/trader data
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "Trader config");
    auditor_generate_store_data(ctx);
    ctx->shop_mod = job->shop_mod;
    writer_export_trader_by_shop_mod(ctx);

    if (s_daemon.cancel_requested) return;

    // Zombie/wildlife configs
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "Zombie & wildlife types");
    writer_export_zombie_config(ctx, "output");

    // cfgeconomycore.xml (MUST be after zombie config)
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "cfgeconomycore.xml");
    writer_export_cfgeconomycore(ctx, "output/cfgeconomycore.xml");

    if (s_daemon.cancel_requested) return;

    // cfglimitsdefinitionuser.xml (custom tier/usage combos)
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "cfglimitsdefinitionuser.xml");
    writer_export_cfglimitsdefinitionuser(ctx, "output/cfglimitsdefinitionuser.xml");

    if (s_daemon.cancel_requested) return;

    // SearchForLoot (opt-in)
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "SearchForLoot.json");
    sfl_generate_config(ctx, "config/SearchForLoot.json", "output/SearchForLoot.json");

    // Random presets
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "cfgrandompresets.xml");
    {
        char sorted_root[256];
        char local_root_buf[256] = "downloaded_mods";
        util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root_buf, sizeof(local_root_buf));
        snprintf(sorted_root, sizeof(sorted_root), "%s/sorted", local_root_buf);
        writer_merge_random_presets(ctx, sorted_root, "output/cfgrandompresets.xml");
    }

    util_log(SEVERITY_INFO, "[UploadDaemon] Phase 1 complete — all output files generated.");

    // Quarantine stale files from prior pipeline runs (wrong trader format, etc.)
    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Quarantining stale files...");
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "output/");
    int quarantined = util_quarantine_stale_output(job->shop_mod);
    if (quarantined > 0) {
        util_log(SEVERITY_WARNING, "[UploadDaemon] Quarantined %d stale file(s) from output/", quarantined);
    }
}

// ============================================================================
// PHASE 2: BUILD UPLOAD MANIFEST (collect restore targets + upload pairs)
// ============================================================================

// Single upload entry: local path → remote path
typedef struct {
    char local[MAX_PATH_LEN];
    char remote[MAX_PATH_LEN];
} UploadEntry;

#define MAX_UPLOAD_ENTRIES 128

static int daemon_build_upload_list(UploadDaemonJob *job, UploadEntry *entries) {
    int count = 0;
    const char *remote_root = job->remote_root;

    // Build remote mission root (strip /db/types.xml from remote_types)
    char remote_mission[512];
    {
        char mission_base[256];
        strncpy(mission_base, job->remote_types, sizeof(mission_base) - 1);
        mission_base[sizeof(mission_base) - 1] = '\0';
        char *db_ptr = strstr(mission_base, "/db/");
        if (db_ptr) *db_ptr = '\0';
        daemon_path_join(remote_mission, sizeof(remote_mission), remote_root, mission_base);
        size_t rm_len = strlen(remote_mission);
        if (rm_len > 1 && remote_mission[rm_len - 1] == '/')
            remote_mission[rm_len - 1] = '\0';
    }

    char remote_db[512];
    snprintf(remote_db, sizeof(remote_db), "%s/db", remote_mission);

    // Derive remote trader directory
    char remote_trader_dir[MAX_PATH_LEN];
    strncpy(remote_trader_dir, job->remote_trader, sizeof(remote_trader_dir) - 1);
    remote_trader_dir[sizeof(remote_trader_dir) - 1] = '\0';
    {
        char *last_slash = strrchr(remote_trader_dir, '/');
        if (last_slash) *(last_slash + 1) = '\0';
        else strncpy(remote_trader_dir, "profiles/Trader/", sizeof(remote_trader_dir) - 1);
    }

    // Helper macro — rejects artifact-named files
    // Artifact check: name starts with 2+ digits followed by underscore
    #define ADD_ENTRY(local_str, remote_str) do { \
        if (count < MAX_UPLOAD_ENTRIES) { \
            const char *_bn = strrchr(local_str, '\\'); \
            if (!_bn) _bn = strrchr(local_str, '/'); \
            if (_bn) _bn++; else _bn = local_str; \
            int _d = 0; \
            while (_bn[_d] >= '0' && _bn[_d] <= '9') _d++; \
            if (_d >= 2 && _bn[_d] == '_') { \
                util_log(SEVERITY_WARNING, "[UploadDaemon] Rejecting artifact file: %s", local_str); \
            } else { \
                strncpy(entries[count].local, local_str, MAX_PATH_LEN - 1); \
                entries[count].local[MAX_PATH_LEN - 1] = '\0'; \
                strncpy(entries[count].remote, remote_str, MAX_PATH_LEN - 1); \
                entries[count].remote[MAX_PATH_LEN - 1] = '\0'; \
                count++; \
            } \
        } \
    } while(0)

    // 1. types.xml → db/types.xml
    if (util_file_exists("output/types.xml")) {
        char full[512];
        daemon_path_join(full, sizeof(full), remote_root, job->remote_types);
        ADD_ENTRY("output/types.xml", full);
    }

    // 2. cfgspawnabletypes.xml — skipped (see generation comment above)

    // 3. Trader config (legacy combined)
    if (util_file_exists(shop_mod_output_filename(job->shop_mod))) {
        char full[512];
        daemon_path_join(full, sizeof(full), remote_root, job->remote_trader);
        ADD_ENTRY(shop_mod_output_filename(job->shop_mod), full);
    }

    // 3b. Per-shop trader files
#ifdef _WIN32
    {
        char shops_dir[MAX_PATH_LEN];
        snprintf(shops_dir, sizeof(shops_dir), "output\\shops");
        bool is_drjones = (job->shop_mod == SHOP_MOD_DRJONES);
        char search_pat[MAX_PATH_LEN];
        snprintf(search_pat, sizeof(search_pat), "%s\\*.%s", shops_dir, is_drjones ? "txt" : "json");
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA(search_pat, &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                char local_shop[MAX_PATH_LEN];
                snprintf(local_shop, sizeof(local_shop), "%s\\%s", shops_dir, fd.cFileName);
                char remote_shop[512];
                daemon_path_join(remote_shop, sizeof(remote_shop), remote_root, remote_trader_dir);
                size_t rlen = strlen(remote_shop);
                if (rlen > 0 && remote_shop[rlen - 1] != '/') {
                    remote_shop[rlen] = '/'; remote_shop[rlen + 1] = '\0';
                }
                strncat(remote_shop, fd.cFileName, sizeof(remote_shop) - strlen(remote_shop) - 1);
                ADD_ENTRY(local_shop, remote_shop);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
#endif

    // 4. cfgeconomycore.xml → mission root
    if (util_file_exists("output/cfgeconomycore.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/cfgeconomycore.xml", remote_mission);
        ADD_ENTRY("output/cfgeconomycore.xml", full);
    }

    // 4b. cfglimitsdefinitionuser.xml → mission root (custom tier/usage combos)
    if (util_file_exists("output/cfglimitsdefinitionuser.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/cfglimitsdefinitionuser.xml", remote_mission);
        ADD_ENTRY("output/cfglimitsdefinitionuser.xml", full);
    }

    // 5. cfgrandompresets.xml → mission root
    if (util_file_exists("output/cfgrandompresets.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/cfgrandompresets.xml", remote_mission);
        ADD_ENTRY("output/cfgrandompresets.xml", full);
    }

    // 6. Zombie types → db/types_infected.xml
    if (util_file_exists("output/zombie_tiers/types_infected.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/types_infected.xml", remote_db);
        ADD_ENTRY("output/zombie_tiers/types_infected.xml", full);
    }

    // 7. Wildlife types → db/types_wildlife.xml
    if (util_file_exists("output/zombie_tiers/types_wildlife.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/types_wildlife.xml", remote_db);
        ADD_ENTRY("output/zombie_tiers/types_wildlife.xml", full);
    }

    // 8. Territory env files — only upload the ACTIVE map's territory files.
    //    Detect active map from remote_mission path (e.g., "dayzOffline.chernarusplus").
    //    Previously uploaded ALL three maps to the same remote env/ dir, causing
    //    enoch/sakhal files to overwrite the correct map's territories (broke animals/zombies).
    {
        const char *active_map = NULL;
        if (strstr(remote_mission, "chernarusplus")) active_map = "chernarusplus";
        else if (strstr(remote_mission, "enoch"))     active_map = "enoch";
        else if (strstr(remote_mission, "sakhal"))    active_map = "sakhal";

        if (active_map) {
            util_log(SEVERITY_INFO, "[UploadDaemon] Active map detected: %s", active_map);
            char local_env[MAX_PATH_LEN];
            snprintf(local_env, sizeof(local_env), "output/%s/env", active_map);
#ifdef _WIN32
            char search_env[MAX_PATH_LEN];
            snprintf(search_env, sizeof(search_env), "%s\\*.xml", local_env);
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(search_env, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    char local_file[MAX_PATH_LEN];
                    snprintf(local_file, sizeof(local_file), "%s\\%s", local_env, fd.cFileName);
                    char remote_file[512];
                    snprintf(remote_file, sizeof(remote_file), "%s/env/%s", remote_mission, fd.cFileName);
                    ADD_ENTRY(local_file, remote_file);
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
#endif
        } else {
            util_log(SEVERITY_WARNING, "[UploadDaemon] Cannot detect active map from mission path: %s — skipping territory upload.", remote_mission);
        }
    }

    // 9. SearchForLoot.json (opt-in)
    if (util_file_exists("output/SearchForLoot.json")) {
        char rt_sfl[512] = "SearchForLoot/SearchForLoot.json";
        util_read_ini_value("config/server_paths.ini", "REMOTE_SFL", rt_sfl, sizeof(rt_sfl));
        char full_sfl[512];
        snprintf(full_sfl, sizeof(full_sfl), "%s%s", remote_root, rt_sfl);
        ADD_ENTRY("output/SearchForLoot.json", full_sfl);
    }

    #undef ADD_ENTRY
    return count;
}

// ============================================================================
// PHASE 3: CREATE RESTORE POINT
// ============================================================================
static void daemon_phase_restore_point(UploadDaemonJob *job, UploadEntry *entries, int count) {
    s_daemon.phase = UPLOAD_PHASE_RESTORE_POINT;
    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Creating restore point...");
    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "Backup (%d files)", count);
    util_log(SEVERITY_INFO, "[UploadDaemon] Phase 2: Creating restore point (%d files)...", count);

    if (count <= 0) return;

    // Build array of remote paths for restore point
    const char *remote_paths[MAX_UPLOAD_ENTRIES];
    for (int i = 0; i < count && i < MAX_UPLOAD_ENTRIES; i++) {
        remote_paths[i] = entries[i].remote;
    }

    // Generate timestamped backup directory (e.g., "backups/20250115_143022")
    char backup_dir[MAX_PATH_LEN];
    char manifest_path[MAX_PATH_LEN];
    ftp_generate_restore_dir(backup_dir, sizeof(backup_dir), manifest_path, sizeof(manifest_path));

    ftp_create_restore_point(job->host, job->port, job->user, job->pass,
                             remote_paths, count,
                             backup_dir, manifest_path);

    // Purge old restore points, keeping the 10 most recent
    ftp_purge_old_restore_points(10);
}

// ============================================================================
// PHASE 4: UPLOAD FILES (single-session batch — one WinSCP connection)
// ============================================================================
static void daemon_phase_upload(UploadDaemonJob *job, UploadEntry *entries, int count) {
    s_daemon.phase = UPLOAD_PHASE_UPLOADING;
    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Uploading %d file(s)...", count);
    s_daemon.files_total = count;
    s_daemon.files_uploaded = 0;
    s_daemon.files_failed = 0;
    s_daemon.progress_pct = 0.0f;

    if (s_daemon.cancel_requested) return;

    util_log(SEVERITY_INFO, "[UploadDaemon] Phase 3: Uploading %d file(s) in single session...", count);

    // Log each file that will be uploaded
    for (int i = 0; i < count; i++) {
        const char *basename = strrchr(entries[i].local, '\\');
        if (!basename) basename = strrchr(entries[i].local, '/');
        if (basename) basename++;
        else basename = entries[i].local;
        util_log(SEVERITY_INFO, "[UploadDaemon]   [%d/%d] %s -> %s", i + 1, count, basename, entries[i].remote);
    }

    // Build parallel pointer arrays for ftp_upload_batch
    const char **locals  = (const char **)malloc(sizeof(const char *) * (size_t)count);
    const char **remotes = (const char **)malloc(sizeof(const char *) * (size_t)count);
    if (!locals || !remotes) {
        util_log(SEVERITY_ERROR, "[UploadDaemon] Memory allocation failed for upload arrays.");
        s_daemon.files_failed = count;
        free(locals); free(remotes);
        s_daemon.progress_pct = 1.0f;
        return;
    }
    for (int i = 0; i < count; i++) {
        locals[i]  = entries[i].local;
        remotes[i] = entries[i].remote;
    }

    snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "Batch: %d files", count);
    s_daemon.progress_pct = 0.0f;

    // Wire up cancel flag so WinSCP child is killed on user cancel
    ftp_set_cancel_flag(&s_daemon.cancel_requested);
    // Wire up upload counter so log-tailing increments files_uploaded in real-time
    ftp_set_upload_counter(&s_daemon.files_uploaded);

    int succeeded = 0, failed = 0;
    bool ok = ftp_upload_batch(job->host, job->port, job->user, job->pass,
                               locals, remotes, count, &succeeded, &failed);

    ftp_set_cancel_flag(NULL);  // Disconnect cancel flag
    ftp_set_upload_counter(NULL);  // Disconnect upload counter

    free(locals);
    free(remotes);

    // On success, files_uploaded was already incremented in real-time by the
    // upload counter; on failure ftp_upload_batch reports 0 succeeded so use that.
    if (!ok) {
        s_daemon.files_uploaded = succeeded;
        s_daemon.files_failed   = failed;
    } else {
        // Live counter tracked completions; set failed from skipped files
        s_daemon.files_failed = failed;
    }
    s_daemon.progress_pct = 1.0f;

    if (ok) {
        util_log(SEVERITY_INFO, "[UploadDaemon] Batch upload complete: %d succeeded.", succeeded);
    } else if (s_daemon.cancel_requested) {
        util_log(SEVERITY_WARNING, "[UploadDaemon] Upload cancelled by user.");
    } else {
        util_log(SEVERITY_ERROR, "[UploadDaemon] Batch upload had failures: %d failed.", failed);
    }
}

// ============================================================================
// DAEMON THREAD ENTRY POINT
// ============================================================================
#ifdef _WIN32
static DWORD WINAPI upload_daemon_thread_proc(LPVOID param) {
    UploadDaemonJob *job = (UploadDaemonJob *)param;

    s_daemon.running = true;
    s_daemon.finished = false;
    s_daemon.cancel_requested = false;
    s_daemon.files_uploaded = 0;
    s_daemon.files_failed = 0;
    s_daemon.files_total = 0;
    s_daemon.progress_pct = 0.0f;

    // ---- Phase 0: Backup & clean output directory ----
    daemon_phase_clean_output();
    if (s_daemon.cancel_requested) goto done;

    // ---- Phase 1: Generate output files ----
    daemon_phase_generate(job);
    if (s_daemon.cancel_requested) goto done;

    // ---- Phase 2: Build upload manifest ----
    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Building upload manifest...");
    UploadEntry entries[MAX_UPLOAD_ENTRIES];
    int entry_count = daemon_build_upload_list(job, entries);

    if (entry_count == 0) {
        util_log(SEVERITY_WARNING, "[UploadDaemon] No files to upload.");
        snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
                 "No output files found to upload.");
        s_daemon.result = UPLOAD_RESULT_FAILED;
        goto done;
    }

    // ---- Phase 3: Create restore point ----
    daemon_phase_restore_point(job, entries, entry_count);
    if (s_daemon.cancel_requested) goto done;

    // ---- Phase 3a: Pre-upload output validation with auto-remediation ----
    //  Validates all output XML files.  Remediable issues (duplicate type
    //  entries, Tier5+ values) are auto-fixed by re-exporting the affected
    //  files.  Only blocks upload on non-recoverable structural corruption.
    {
        snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Validating output files...");
        snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "Pre-upload checks");
        int v_passed = 0, v_failed = 0;
        bool valid = auditor_remediate_and_validate(job->ctx, &v_passed, &v_failed);
        if (!valid) {
            util_log(SEVERITY_ERROR, "[UploadDaemon] Validation FAILED after remediation: %d file(s) still corrupt. Upload blocked.", v_failed);
            snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
                     "Upload blocked: %d output file(s) failed validation after remediation. Check logs.", v_failed);
            s_daemon.result = UPLOAD_RESULT_FAILED;
            goto done;
        }
        util_log(SEVERITY_INFO, "[UploadDaemon] Pre-upload validation PASSED: %d file(s) verified.", v_passed);
    }
    if (s_daemon.cancel_requested) goto done;

    // ---- Phase 3b: Cleanup stale artifacts from remote db/ ----
    //  Previous uploads (via restore or the old directory-grouped put) may have
    //  left numbered files (001__*, 017__*, etc.) in the server's db/ folder.
    //  The DayZ CE engine loads ALL files in db/ — extras crash it with
    //  ACCESS_VIOLATION during "Loading core data".
    {
        char remote_mission[512];
        char mission_base[256];
        strncpy(mission_base, job->remote_types, sizeof(mission_base) - 1);
        mission_base[sizeof(mission_base) - 1] = '\0';
        char *db_ptr = strstr(mission_base, "/db/");
        if (db_ptr) *db_ptr = '\0';
        daemon_path_join(remote_mission, sizeof(remote_mission), job->remote_root, mission_base);
        size_t rm_len = strlen(remote_mission);
        if (rm_len > 1 && remote_mission[rm_len - 1] == '/')
            remote_mission[rm_len - 1] = '\0';

        char remote_db[512];
        snprintf(remote_db, sizeof(remote_db), "%s/db", remote_mission);

        snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Cleaning stale artifacts...");
        snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "%s/", remote_db);
        util_log(SEVERITY_INFO, "[UploadDaemon] Phase 3b: Cleaning numbered artifacts from %s/", remote_db);
        ftp_cleanup_remote_dir(job->host, job->port, job->user, job->pass, remote_db);

        char remote_env[512];
        snprintf(remote_env, sizeof(remote_env), "%s/env", remote_mission);
        snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "%s/", remote_env);
        util_log(SEVERITY_INFO, "[UploadDaemon] Phase 3b: Cleaning numbered artifacts from %s/", remote_env);
        ftp_cleanup_remote_dir(job->host, job->port, job->user, job->pass, remote_env);
    }
    if (s_daemon.cancel_requested) goto done;

    // ---- Phase 4: Upload ----
    daemon_phase_upload(job, entries, entry_count);
    if (s_daemon.cancel_requested) goto done;

    // ---- Phase 5: Post-upload integrity verification ----
    if (s_daemon.files_uploaded > 0 && s_daemon.files_failed == 0) {
        snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Verifying uploads...");
        snprintf(s_daemon.current_file, sizeof(s_daemon.current_file), "Integrity check");
        util_log(SEVERITY_INFO, "[UploadDaemon] Phase 5: Post-upload integrity verification...");

        const char **verify_locals  = (const char **)malloc(sizeof(const char *) * (size_t)entry_count);
        const char **verify_remotes = (const char **)malloc(sizeof(const char *) * (size_t)entry_count);
        if (verify_locals && verify_remotes) {
            for (int i = 0; i < entry_count; i++) {
                verify_locals[i]  = entries[i].local;
                verify_remotes[i] = entries[i].remote;
            }
            bool integrity_ok = ftp_verify_uploads(job->host, job->port, job->user, job->pass,
                                                    verify_locals, verify_remotes, entry_count);
            if (!integrity_ok) {
                util_log(SEVERITY_ERROR, "[UploadDaemon] Post-upload integrity check FAILED!");
                s_daemon.result = UPLOAD_RESULT_PARTIAL;
            }
        }
        free(verify_locals);
        free(verify_remotes);
    }

    // ---- Build result message ----
    if (s_daemon.cancel_requested) {
        snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
                 "Upload cancelled. %d uploaded, %d failed, %d remaining.",
                 s_daemon.files_uploaded, s_daemon.files_failed,
                 s_daemon.files_total - s_daemon.files_uploaded - s_daemon.files_failed);
        s_daemon.result = UPLOAD_RESULT_CANCELLED;
    } else if (s_daemon.files_failed == 0 && s_daemon.files_uploaded > 0) {
        snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
                 "Uploaded %d file(s) to server. Restore point saved.",
                 s_daemon.files_uploaded);
        s_daemon.result = UPLOAD_RESULT_SUCCESS;
    } else if (s_daemon.files_uploaded > 0) {
        snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
                 "Partial upload: %d OK, %d failed.",
                 s_daemon.files_uploaded, s_daemon.files_failed);
        s_daemon.result = UPLOAD_RESULT_PARTIAL;
    } else {
        snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
                 "Upload failed. Check FTP credentials/log.");
        s_daemon.result = UPLOAD_RESULT_FAILED;
    }

done:
    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Done.");
    s_daemon.phase = UPLOAD_PHASE_DONE;
    s_daemon.running = false;
    s_daemon.finished = true;

    util_log(SEVERITY_INFO, "[UploadDaemon] %s", s_daemon.result_message);
    return 0;
}
#endif

// ============================================================================
// PUBLIC API
// ============================================================================

static UploadDaemonJob s_job;  // Persistent storage for the active job

bool upload_daemon_start(UploadDaemonJob *job) {
    if (s_daemon.running) {
        util_log(SEVERITY_WARNING, "[UploadDaemon] Already running — ignoring start request.");
        return false;
    }

    // Clean up previous thread handle
#ifdef _WIN32
    if (s_daemon_thread) {
        WaitForSingleObject(s_daemon_thread, 0);
        CloseHandle(s_daemon_thread);
        s_daemon_thread = NULL;
    }
#endif

    // Reset state
    memset(&s_daemon, 0, sizeof(s_daemon));
    s_daemon.phase = UPLOAD_PHASE_GENERATING;
    s_daemon.result = UPLOAD_RESULT_SUCCESS;  // will be set properly later
    snprintf(s_daemon.phase_label, sizeof(s_daemon.phase_label), "Starting...");

    // Copy job to persistent storage
    memcpy(&s_job, job, sizeof(UploadDaemonJob));

#ifdef _WIN32
    s_daemon_thread = CreateThread(NULL, 0, upload_daemon_thread_proc, &s_job, 0, NULL);
    if (!s_daemon_thread) {
        util_log(SEVERITY_ERROR, "[UploadDaemon] Failed to create thread.");
        s_daemon.finished = true;
        s_daemon.result = UPLOAD_RESULT_FAILED;
        snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
                 "Failed to create upload thread.");
        return false;
    }
    return true;
#else
    // Non-Windows: not supported (matches project convention)
    s_daemon.finished = true;
    s_daemon.result = UPLOAD_RESULT_FAILED;
    snprintf(s_daemon.result_message, sizeof(s_daemon.result_message),
             "Upload daemon requires Windows.");
    return false;
#endif
}

void upload_daemon_cancel(void) {
    if (s_daemon.running) {
        util_log(SEVERITY_WARNING, "[UploadDaemon] Cancel requested by user.");
        s_daemon.cancel_requested = true;
    }
}

const UploadDaemonState* upload_daemon_state(void) {
    return &s_daemon;
}

bool upload_daemon_is_running(void) {
    return s_daemon.running;
}

void upload_daemon_cleanup(void) {
#ifdef _WIN32
    if (s_daemon_thread) {
        if (s_daemon.running) {
            s_daemon.cancel_requested = true;
            WaitForSingleObject(s_daemon_thread, 5000);
        }
        CloseHandle(s_daemon_thread);
        s_daemon_thread = NULL;
    }
#endif
    memset(&s_daemon, 0, sizeof(s_daemon));
}
