
#include "auditor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
    char remote_root[256];
    char remote_types[256];
    char remote_spawnable[256];
    char remote_events[256];
    char remote_globals[256];
    char remote_trader[256];
    char local_root[256];
    char local_types[256];
    char local_spawnable[256];
    char local_events[256];
    char local_globals[256];
} ServerPaths;

static void ensure_dir(const char *path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    (void)path;
#endif
}

static void normalize_root(char *root, size_t max_len) {
    size_t len = strlen(root);
    if (len == 0 || len >= max_len - 2) return;
    if (root[len - 1] != '/') {
        root[len] = '/';
        root[len + 1] = '\0';
    }
}

static void build_remote(char *out, size_t out_len, const char *root, const char *suffix) {
    // Store relative path only (without root prefix).
    // Root is prepended at point-of-use via path_join() to avoid double-slash.
    (void)root;
    snprintf(out, out_len, "%s", suffix);
}

// Safely join a root prefix and a relative path, preventing double slashes.
// e.g. path_join("/root/", "db/types.xml") => "/root/db/types.xml"
//      path_join("/",      "db/types.xml") => "/db/types.xml"
static void path_join(char *out, size_t out_len, const char *base, const char *rel) {
    char tmp[256];
    strncpy(tmp, base, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    normalize_root(tmp, sizeof(tmp));  // ensure base ends with '/'
    while (*rel == '/') rel++;         // strip leading slash from relative
    snprintf(out, out_len, "%s%s", tmp, rel);
}

static void build_local(char *out, size_t out_len, const char *local_root, const char *remote_path) {
    const char *base = util_basename(remote_path);
    if (!base || !*base) {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%s/%s", local_root, base);
}

static void load_server_paths(ServerPaths *paths) {
    memset(paths, 0, sizeof(ServerPaths));

    char remote_root[256] = "/";
    strncpy(paths->local_root, "downloaded_mods", sizeof(paths->local_root) - 1);

    bool has_remote_root = util_read_ini_value("config/server_paths.ini", "REMOTE_ROOT", remote_root, sizeof(remote_root));
    bool has_types = util_read_ini_value("config/server_paths.ini", "REMOTE_TYPES", paths->remote_types, sizeof(paths->remote_types));
    bool has_spawnable = util_read_ini_value("config/server_paths.ini", "REMOTE_SPAWNABLE", paths->remote_spawnable, sizeof(paths->remote_spawnable));
    bool has_events = util_read_ini_value("config/server_paths.ini", "REMOTE_EVENTS", paths->remote_events, sizeof(paths->remote_events));
    bool has_globals = util_read_ini_value("config/server_paths.ini", "REMOTE_GLOBALS", paths->remote_globals, sizeof(paths->remote_globals));
    util_read_ini_value("config/server_paths.ini", "REMOTE_TRADER", paths->remote_trader, sizeof(paths->remote_trader));
    util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", paths->local_root, sizeof(paths->local_root));

    strncpy(paths->remote_root, remote_root, sizeof(paths->remote_root) - 1);

    if (!has_types) build_remote(paths->remote_types, sizeof(paths->remote_types), paths->remote_root, "db/types.xml");
    if (!has_spawnable) build_remote(paths->remote_spawnable, sizeof(paths->remote_spawnable), paths->remote_root, "cfgspawnabletypes.xml");
    if (!has_events) build_remote(paths->remote_events, sizeof(paths->remote_events), paths->remote_root, "db/events.xml");
    if (!has_globals) build_remote(paths->remote_globals, sizeof(paths->remote_globals), paths->remote_root, "db/globals.xml");
    if (!paths->remote_trader[0]) strncpy(paths->remote_trader, shop_mod_default_remote_path(SHOP_MOD_TRADERPLUS), sizeof(paths->remote_trader) - 1);

    build_local(paths->local_types, sizeof(paths->local_types), paths->local_root, paths->remote_types);
    build_local(paths->local_spawnable, sizeof(paths->local_spawnable), paths->local_root, paths->remote_spawnable);
    build_local(paths->local_events, sizeof(paths->local_events), paths->local_root, paths->remote_events);
    build_local(paths->local_globals, sizeof(paths->local_globals), paths->local_root, paths->remote_globals);

    if (has_remote_root) {
        util_log(SEVERITY_INFO, "Server paths loaded from config/server_paths.ini");
    }
}

static bool load_ftp_credentials(char *host, size_t host_len, int *port, char *user, size_t user_len, char *pass, size_t pass_len) {
    char port_buf[16] = {0};
    bool ok = true;
    ok &= util_read_ini_value("config/ftp.ini", "HOST", host, host_len);
    ok &= util_read_ini_value("config/ftp.ini", "PORT", port_buf, sizeof(port_buf));
    ok &= util_read_ini_value("config/ftp.ini", "USER", user, user_len);
    ok &= util_read_ini_value("config/ftp.ini", "PASS", pass, pass_len);
    if (!ok) return false;
    *port = atoi(port_buf);
    return true;
}

static void append_restore_target(char targets[][MAX_PATH_LEN], int *count, int max_count, const char *remote_path) {
    if (!targets || !count || !remote_path || !*remote_path) return;
    if (*count >= max_count) return;

    for (int i = 0; i < *count; i++) {
        if (strcmp(targets[i], remote_path) == 0) return;
    }

    strncpy(targets[*count], remote_path, MAX_PATH_LEN - 1);
    targets[*count][MAX_PATH_LEN - 1] = '\0';
    (*count)++;
}

static int run_restore_last_upload(void) {
    char host[128] = {0};
    char user[128] = {0};
    char pass[128] = {0};
    int port = 0;

    if (!load_ftp_credentials(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "Restore: Missing FTP config in config/ftp.ini");
        return 1;
    }

    util_log(SEVERITY_INFO, "Restore: Searching for most recent restore point...");

    // Find the newest restore point (first entry — list is sorted newest-first)
    RestorePointInfo restore_points[MAX_RESTORE_POINTS];
    int rp_count = ftp_list_restore_points(restore_points, MAX_RESTORE_POINTS);
    const char *manifest_to_use = NULL;

    if (rp_count > 0) {
        manifest_to_use = restore_points[0].manifest;
        util_log(SEVERITY_INFO, "Restore: Using newest restore point: %s", restore_points[0].label);
    } else {
        util_log(SEVERITY_ERROR, "Restore: No restore points found in backups/");
        return 1;
    }

    if (ftp_restore_from_manifest(host, port, user, pass, manifest_to_use)) {
        util_log(SEVERITY_INFO, "Restore: Restore point applied successfully.");
        return 0;
    }

    util_log(SEVERITY_ERROR, "Restore: Failed to apply restore point.");
    return 1;
}

static int run_headless(AuditorContext *ctx) {
    char host[128] = {0};
    char user[128] = {0};
    char pass[128] = {0};
    int port = 0;
    if (!load_ftp_credentials(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "Headless: Missing FTP config in config/ftp.ini");
        return 1;
    }

    ServerPaths paths;
    load_server_paths(&paths);
    ensure_dir(paths.local_root);
    ensure_dir("output");

    // Initialize inline dedup hash map and file cache for the pipeline
    classname_map_init(ctx);
    // File cache: load from disk, prune deleted files, then clear —
    // headless mode starts with item_count=0 so cached "skip" entries
    // from a prior run would suppress files whose items aren't in memory.
    // The cache rebuilds as files are parsed in this run.
    if (!ctx->file_cache.loaded) {
        file_cache_load(&ctx->file_cache);
    }
    file_cache_prune(&ctx->file_cache);
    ctx->file_cache.count = 0;

    // ========================================================================
    // PHASE 1: DOWNLOAD EVERYTHING
    // ========================================================================
    util_log(SEVERITY_INFO, "=== MAGIC BUTTON: PHASE 1 — DOWNLOAD ===");
    bool dl_ok = ftp_download_recursive(host, port, user, pass, paths.remote_root, paths.local_root);
    // Fallback: if recursive download failed, try individual key files
    if (!dl_ok || !util_file_exists(paths.local_types)) {
        util_log(SEVERITY_WARNING, "Headless: Recursive download %s, trying individual files...",
                 dl_ok ? "succeeded but types.xml missing" : "failed");
        char dl_types[512], dl_spawn[512], dl_events[512], dl_globals[512];
        path_join(dl_types,   sizeof(dl_types),   paths.remote_root, paths.remote_types);
        path_join(dl_spawn,   sizeof(dl_spawn),   paths.remote_root, paths.remote_spawnable);
        path_join(dl_events,  sizeof(dl_events),  paths.remote_root, paths.remote_events);
        path_join(dl_globals, sizeof(dl_globals),  paths.remote_root, paths.remote_globals);
        const char *remote_dl[] = { dl_types, dl_spawn, dl_events, dl_globals };
        const char *local_dl[]  = { paths.local_types,  paths.local_spawnable,  paths.local_events,  paths.local_globals };
        ftp_download_batch(host, port, user, pass, remote_dl, local_dl, 4);
    }

    // ========================================================================
    // PHASE 2: FULL SWARM PIPELINE (Sort → Index → Parse → Audit → Stitch → Export)
    // ========================================================================
    // Backup and clean output directory before generating fresh files
    util_log(SEVERITY_INFO, "=== MAGIC BUTTON: PHASE 1b — CLEAN OUTPUT ===");
    util_backup_and_clean_output();

    util_log(SEVERITY_INFO, "=== MAGIC BUTTON: PHASE 2 — SWARM PIPELINE ===");
    swarm_plan(ctx);
    swarm_execute(ctx);

    // Save file cache after pipeline completes
    file_cache_save(&ctx->file_cache);

    if (ctx->dedup_skipped > 0)
        util_log(SEVERITY_INFO, "Inline dedup: %d duplicate definitions merged during parse", ctx->dedup_skipped);

    // Store data generation now runs inside the swarm (AUDIT stage)

    // Quarantine stale files from prior pipeline runs before upload
    {
        int quarantined = util_quarantine_stale_output(ctx->shop_mod);
        if (quarantined > 0)
            util_log(SEVERITY_WARNING, "Quarantined %d stale file(s) from output/", quarantined);
    }

    // ========================================================================
    // PHASE 3: UPLOAD ALL GENERATED FILES BACK TO SERVER (single session)
    // ========================================================================
    util_log(SEVERITY_INFO, "=== MAGIC BUTTON: PHASE 3 — UPLOAD ===");

    // Build remote base path (e.g. /remote_root/mpmissions/dayzOffline.chernarusplus)
    char remote_mission[512];
    {
        char mission_base[256];
        strncpy(mission_base, paths.remote_types, sizeof(mission_base) - 1);
        mission_base[sizeof(mission_base) - 1] = '\0';
        char *db_ptr = strstr(mission_base, "/db/");
        if (db_ptr) *db_ptr = '\0';
        path_join(remote_mission, sizeof(remote_mission), paths.remote_root, mission_base);
        size_t rm_len = strlen(remote_mission);
        if (rm_len > 1 && remote_mission[rm_len - 1] == '/')
            remote_mission[rm_len - 1] = '\0';
    }

    // Collect all upload pairs (local → remote) and restore targets
    #define HEADLESS_MAX_UPLOADS 128
    char headless_locals[HEADLESS_MAX_UPLOADS][MAX_PATH_LEN];
    char headless_remotes[HEADLESS_MAX_UPLOADS][MAX_PATH_LEN];
    int headless_upload_count = 0;

    // Helper macro — rejects artifact-named files
    #define HEADLESS_ADD(local_str, remote_str) do { \
        if (headless_upload_count < HEADLESS_MAX_UPLOADS) { \
            const char *_bn = strrchr(local_str, '\\'); \
            if (!_bn) _bn = strrchr(local_str, '/'); \
            if (_bn) _bn++; else _bn = local_str; \
            int _d = 0; \
            while (_bn[_d] >= '0' && _bn[_d] <= '9') _d++; \
            if (_d >= 2 && _bn[_d] == '_') { \
                util_log(SEVERITY_WARNING, "[Headless] Rejecting artifact file: %s", local_str); \
            } else { \
                strncpy(headless_locals[headless_upload_count], local_str, MAX_PATH_LEN - 1); \
                headless_locals[headless_upload_count][MAX_PATH_LEN - 1] = '\0'; \
                strncpy(headless_remotes[headless_upload_count], remote_str, MAX_PATH_LEN - 1); \
                headless_remotes[headless_upload_count][MAX_PATH_LEN - 1] = '\0'; \
                headless_upload_count++; \
            } \
        } \
    } while(0)

    // 1. types.xml -> server db/types.xml
    if (util_file_exists("output/types.xml")) {
        char full[512];
        path_join(full, sizeof(full), paths.remote_root, paths.remote_types);
        HEADLESS_ADD("output/types.xml", full);
    }

    // 2. cfgspawnabletypes.xml -> mission root
    if (util_file_exists("output/cfgspawnabletypes.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/cfgspawnabletypes.xml", remote_mission);
        HEADLESS_ADD("output/cfgspawnabletypes.xml", full);
    }

    // 3. Trader config (legacy combined)
    {
        const char *trader_output = shop_mod_output_filename(ctx->shop_mod);
        if (util_file_exists(trader_output) && paths.remote_trader[0]) {
            char full[512];
            path_join(full, sizeof(full), paths.remote_root, paths.remote_trader);
            HEADLESS_ADD(trader_output, full);
        }
    }

    // 3b. Per-shop trader files -> server trader directory
    {
        char remote_trader_dir[MAX_PATH_LEN];
        strncpy(remote_trader_dir, paths.remote_trader, sizeof(remote_trader_dir) - 1);
        remote_trader_dir[sizeof(remote_trader_dir) - 1] = '\0';
        char *last_slash = strrchr(remote_trader_dir, '/');
        if (last_slash) *(last_slash + 1) = '\0';
        else strncpy(remote_trader_dir, "profiles/Trader/", sizeof(remote_trader_dir) - 1);

#ifdef _WIN32
        char shops_dir[MAX_PATH_LEN];
        snprintf(shops_dir, sizeof(shops_dir), "output\\shops");
        bool is_drjones = (ctx->shop_mod == SHOP_MOD_DRJONES);
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
                path_join(remote_shop, sizeof(remote_shop), paths.remote_root, remote_trader_dir);
                size_t rlen = strlen(remote_shop);
                if (rlen > 0 && remote_shop[rlen - 1] != '/') {
                    remote_shop[rlen] = '/';
                    remote_shop[rlen + 1] = '\0';
                }
                strncat(remote_shop, fd.cFileName, sizeof(remote_shop) - strlen(remote_shop) - 1);
                HEADLESS_ADD(local_shop, remote_shop);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
#endif
    }

    // 4. cfgeconomycore.xml -> mission root
    if (util_file_exists("output/cfgeconomycore.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/cfgeconomycore.xml", remote_mission);
        HEADLESS_ADD("output/cfgeconomycore.xml", full);
    }

    // 4b. cfglimitsdefinitionuser.xml -> mission root (custom tier/usage combos)
    if (util_file_exists("output/cfglimitsdefinitionuser.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/cfglimitsdefinitionuser.xml", remote_mission);
        HEADLESS_ADD("output/cfglimitsdefinitionuser.xml", full);
    }

    // 5. cfgrandompresets.xml -> mission root
    if (util_file_exists("output/cfgrandompresets.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/cfgrandompresets.xml", remote_mission);
        HEADLESS_ADD("output/cfgrandompresets.xml", full);
    }

    // 6. Territory env files — only upload the ACTIVE map's territory files.
    //    Detect active map from remote_mission path (e.g., "dayzOffline.chernarusplus").
    {
        const char *active_map = NULL;
        if (strstr(remote_mission, "chernarusplus")) active_map = "chernarusplus";
        else if (strstr(remote_mission, "enoch"))     active_map = "enoch";
        else if (strstr(remote_mission, "sakhal"))    active_map = "sakhal";

        if (active_map) {
            util_log(SEVERITY_INFO, "[Headless] Active map detected: %s", active_map);
            char local_env[MAX_PATH_LEN];
            snprintf(local_env, sizeof(local_env), "output/%s/env", active_map);
#ifdef _WIN32
            char search[MAX_PATH_LEN];
            snprintf(search, sizeof(search), "%s\\*.xml", local_env);
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(search, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    char local_file[MAX_PATH_LEN];
                    snprintf(local_file, sizeof(local_file), "%s\\%s", local_env, fd.cFileName);
                    char remote_file[512];
                    snprintf(remote_file, sizeof(remote_file), "%s/env/%s", remote_mission, fd.cFileName);
                    HEADLESS_ADD(local_file, remote_file);
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
#endif
        } else {
            util_log(SEVERITY_WARNING, "[Headless] Cannot detect active map from mission path: %s — skipping territory upload.", remote_mission);
        }
    }

    // 7. Zombie types -> db/types_infected.xml
    if (util_file_exists("output/zombie_tiers/types_infected.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/db/types_infected.xml", remote_mission);
        HEADLESS_ADD("output/zombie_tiers/types_infected.xml", full);
    }

    // 8. Wildlife types -> db/types_wildlife.xml
    if (util_file_exists("output/zombie_tiers/types_wildlife.xml")) {
        char full[512];
        snprintf(full, sizeof(full), "%s/db/types_wildlife.xml", remote_mission);
        HEADLESS_ADD("output/zombie_tiers/types_wildlife.xml", full);
    }

    // 9. SearchForLoot.json (opt-in)
    if (util_file_exists("output/SearchForLoot.json")) {
        char rt_sfl[512] = "SearchForLoot/SearchForLoot.json";
        util_read_ini_value("config/server_paths.ini", "REMOTE_SFL", rt_sfl, sizeof(rt_sfl));
        char full_sfl[512];
        snprintf(full_sfl, sizeof(full_sfl), "%s%s", paths.remote_root, rt_sfl);
        HEADLESS_ADD("output/SearchForLoot.json", full_sfl);
    }

    // Build restore target list from the collected uploads
    if (headless_upload_count > 0) {
        int upload_ok = 0, upload_fail = 0;

        // Pre-upload output validation with auto-remediation loop.
        // Remediable issues (duplicates, Tier5+) are fixed by re-exporting;
        // only non-recoverable corruption blocks the upload.
        {
            int v_passed = 0, v_failed = 0;
            util_log(SEVERITY_INFO, "=== PRE-UPLOAD OUTPUT VALIDATION ===");
            bool valid = auditor_remediate_and_validate(ctx, &v_passed, &v_failed);
            if (!valid) {
                util_log(SEVERITY_ERROR, "Validation FAILED after remediation: %d file(s) still corrupt. Upload blocked.", v_failed);
                util_log(SEVERITY_ERROR, "Non-recoverable issues remain. Restore point NOT created — server unchanged.");
                upload_fail = v_failed;
                goto headless_cleanup;
            }
            util_log(SEVERITY_INFO, "Pre-upload validation PASSED: %d file(s) verified.", v_passed);
        }

        const char *restore_ptrs[HEADLESS_MAX_UPLOADS];
        for (int i = 0; i < headless_upload_count; i++) {
            restore_ptrs[i] = headless_remotes[i];
        }

        util_log(SEVERITY_INFO,
                 "Creating pre-upload restore point (%d target files)...",
                 headless_upload_count);

        // Generate timestamped backup directory (e.g., "backups/20250115_143022")
        char backup_dir[MAX_PATH_LEN];
        char manifest_path[MAX_PATH_LEN];
        ftp_generate_restore_dir(backup_dir, sizeof(backup_dir), manifest_path, sizeof(manifest_path));

        if (!ftp_create_restore_point(host, port, user, pass,
                                      restore_ptrs, headless_upload_count,
                                      backup_dir, manifest_path)) {
            util_log(SEVERITY_WARNING,
                     "Restore point capture incomplete. Upload will continue, but restore may be partial.");
        }

        // Clean stale numbered artifacts from remote db/ and env/ before uploading.
        // Previous uploads left "NNN__*" files that crash the DayZ CE engine.
        {
            char remote_db[512];
            snprintf(remote_db, sizeof(remote_db), "%s/db", remote_mission);
            util_log(SEVERITY_INFO, "Cleaning numbered artifact files from %s/ ...", remote_db);
            ftp_cleanup_remote_dir(host, port, user, pass, remote_db);

            char remote_env[512];
            snprintf(remote_env, sizeof(remote_env), "%s/env", remote_mission);
            util_log(SEVERITY_INFO, "Cleaning numbered artifact files from %s/ ...", remote_env);
            ftp_cleanup_remote_dir(host, port, user, pass, remote_env);
        }

        // Upload all files in a single WinSCP session
        const char *local_ptrs[HEADLESS_MAX_UPLOADS];
        const char *remote_ptrs_up[HEADLESS_MAX_UPLOADS];
        for (int i = 0; i < headless_upload_count; i++) {
            local_ptrs[i]      = headless_locals[i];
            remote_ptrs_up[i]  = headless_remotes[i];
        }

        ftp_upload_batch(host, port, user, pass,
                         local_ptrs, remote_ptrs_up, headless_upload_count,
                         &upload_ok, &upload_fail);

        // Post-upload integrity verification
        if (upload_ok > 0 && upload_fail == 0) {
            util_log(SEVERITY_INFO, "=== POST-UPLOAD INTEGRITY CHECK ===");
            bool integrity_ok = ftp_verify_uploads(host, port, user, pass,
                                                    local_ptrs, remote_ptrs_up, headless_upload_count);
            if (!integrity_ok) {
                util_log(SEVERITY_ERROR, "Post-upload integrity check FAILED! Some files may be corrupt on server.");
            }
        }

        util_log(SEVERITY_INFO, "=== MAGIC BUTTON: COMPLETE ===");
        util_log(SEVERITY_INFO, "Upload results: %d succeeded, %d failed", upload_ok, upload_fail);
        util_log(SEVERITY_INFO, "Pipeline tasks: %d/%d complete, %d issues found.",
                 ctx->swarm.task_count, ctx->swarm.task_count, ctx->issue_count);

        // Purge old restore points, keeping the 10 most recent
        ftp_purge_old_restore_points(10);

headless_cleanup:
        #undef HEADLESS_ADD
        #undef HEADLESS_MAX_UPLOADS
        return (upload_fail > 0) ? 1 : 0;
    } else {
        util_log(SEVERITY_INFO, "No upload targets detected, skipping restore point capture.");
        util_log(SEVERITY_INFO, "=== MAGIC BUTTON: COMPLETE ===");
        util_log(SEVERITY_INFO, "Pipeline tasks: %d/%d complete, %d issues found.",
                 ctx->swarm.task_count, ctx->swarm.task_count, ctx->issue_count);

        #undef HEADLESS_ADD
        #undef HEADLESS_MAX_UPLOADS
        return 0;
    }
}

// Local, no-FTP regeneration: run the full swarm pipeline against the files
// already present in LOCAL_ROOT (downloaded_mods) and write fresh files to
// output/. No FTP download, no upload — used for local testing against a
// DayZ server without touching the production host.
static int run_regen(AuditorContext *ctx) {
    ServerPaths paths;
    load_server_paths(&paths);
    ensure_dir(paths.local_root);
    ensure_dir("output");

    util_log(SEVERITY_INFO, "=== LOCAL REGEN: pipeline only (no FTP) ===");
    util_log(SEVERITY_INFO, "Reading input from: %s", paths.local_root);

    // Initialize inline dedup hash map and file cache for the pipeline
    classname_map_init(ctx);
    if (!ctx->file_cache.loaded) {
        file_cache_load(&ctx->file_cache);
    }
    file_cache_prune(&ctx->file_cache);
    ctx->file_cache.count = 0;

    // Backup and clean output directory before generating fresh files
    util_log(SEVERITY_INFO, "=== REGEN: CLEAN OUTPUT ===");
    util_backup_and_clean_output();

    util_log(SEVERITY_INFO, "=== REGEN: SWARM PIPELINE ===");
    swarm_plan(ctx);
    swarm_execute(ctx);

    file_cache_save(&ctx->file_cache);

    if (ctx->dedup_skipped > 0)
        util_log(SEVERITY_INFO, "Inline dedup: %d duplicate definitions merged during parse", ctx->dedup_skipped);

    util_log(SEVERITY_INFO, "=== REGEN COMPLETE ===");
    util_log(SEVERITY_INFO, "Pipeline tasks: %d/%d complete, %d issues found.",
             ctx->swarm.task_count, ctx->swarm.task_count, ctx->issue_count);
    return 0;
}

int main(int argc, char **argv) {
    util_init_logger();
    util_log(SEVERITY_INFO, "System Starting...");
    
    // [FIX] HEAP ALLOCATION
    // We allocate the massive context on the heap to prevent Stack Overflow.
    // 65,000 items * ~3KB = ~195MB. Default stack is only 1MB.
    AuditorContext *ctx = (AuditorContext*)malloc(sizeof(AuditorContext));
    if (!ctx) {
        util_log(SEVERITY_CRITICAL, "Failed to allocate memory for AuditorContext!");
        return -1;
    }
    util_log(SEVERITY_INFO, "Memory allocated (Heap).");
    
    util_init_context(ctx);
    
    // Install/Check Tools
    extern void util_check_and_install_dependencies();
    util_check_and_install_dependencies();
    
    if (argc > 1 && strcmp(argv[1], "--headless") == 0) {
        util_setup_console();   // WIN32 subsystem has no console; allocate one for CLI
        int result = run_headless(ctx);
        free(ctx);
        util_close_job_object();
        util_close_logger();
        return result;
    }

    if (argc > 1 && (strcmp(argv[1], "--regen") == 0 || strcmp(argv[1], "--local") == 0)) {
        util_setup_console();   // WIN32 subsystem has no console; allocate one for CLI
        int result = run_regen(ctx);
        free(ctx);
        util_close_job_object();
        util_close_logger();
        return result;
    }

    if (argc > 1 && strcmp(argv[1], "--restore-last-upload") == 0) {
        util_setup_console();   // WIN32 subsystem has no console; allocate one for CLI
        int result = run_restore_last_upload();
        free(ctx);
        util_close_job_object();
        util_close_logger();
        return result;
    }

    ui_init();
    ui_run(ctx);
    ui_shutdown();
    
    if (ctx->original_items) free(ctx->original_items);
    free(ctx);
    util_close_job_object();
    util_close_logger();
    return 0;
}
