
#include "auditor.h"
#include "cli_commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
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

/* -------------------------------------------------------------------------
 * Login-field diagnostic export
 * Uses the SAME load path as onboard FTP (util_read_ini_value / load_ftp_credentials)
 * and writes a local cross-reference file so nothing is truncated/mangled invisibly.
 * Output is gitignored — never commit it.
 * ------------------------------------------------------------------------- */
#define FTP_LOGIN_EXPORT_PATH "config/ftp.login_export.txt"

static void write_hex_field(FILE *out, const char *label, const char *value)
{
    size_t i, n;
    if (!out) return;
    n = value ? strlen(value) : 0;
    fprintf(out, "%s_len=%zu\n", label, n);
    fprintf(out, "%s_utf8=", label);
    if (value) fputs(value, out);
    fputc('\n', out);
    fprintf(out, "%s_hex=", label);
    for (i = 0; i < n; i++)
        fprintf(out, "%02X%s", (unsigned char)value[i], (i + 1 < n) ? " " : "");
    fputc('\n', out);
    fprintf(out, "%s_codes=", label);
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c >= 32 && c < 127)
            fprintf(out, "'%c'%s", (char)c, (i + 1 < n) ? " " : "");
        else
            fprintf(out, "<%u>%s", (unsigned)c, (i + 1 < n) ? " " : "");
    }
    fputc('\n', out);
    fprintf(out, "%s_has_trailing_cr=%d\n", label, (n > 0 && value[n - 1] == '\r') ? 1 : 0);
    fprintf(out, "%s_has_trailing_lf=%d\n", label, (n > 0 && value[n - 1] == '\n') ? 1 : 0);
    fprintf(out, "%s_has_space=%d\n", label, (value && strchr(value, ' ')) ? 1 : 0);
    fprintf(out, "%s_has_tab=%d\n", label, (value && strchr(value, '\t')) ? 1 : 0);
    fputc('\n', out);
}

/** Dump raw KEY= line from disk (binary open) for side-by-side with parsed value. */
static int dump_raw_ini_line(FILE *out, const char *path, const char *key)
{
    FILE *f;
    char line[1024];
    size_t key_len;
    if (!out || !path || !key) return 0;
    f = fopen(path, "rb");
    if (!f) {
        fprintf(out, "raw_%s: FILE_OPEN_FAILED\n\n", key);
        return 0;
    }
    key_len = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        /* Match same key= prefix as util_read_ini_value */
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            size_t i;
            fprintf(out, "raw_%s_line_len_including_newlines=%zu\n", key, len);
            fprintf(out, "raw_%s_line_hex=", key);
            for (i = 0; i < len; i++)
                fprintf(out, "%02X%s", (unsigned char)line[i], (i + 1 < len) ? " " : "");
            fputc('\n', out);
            fprintf(out, "raw_%s_line_visible=", key);
            for (i = 0; i < len; i++) {
                unsigned char c = (unsigned char)line[i];
                if (c == '\r') fputs("<CR>", out);
                else if (c == '\n') fputs("<LF>", out);
                else if (c == '\t') fputs("<TAB>", out);
                else if (c >= 32 && c < 127) fputc((char)c, out);
                else fprintf(out, "<%02X>", c);
            }
            fputc('\n', out);
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    fprintf(out, "raw_%s: KEY_NOT_FOUND_IN_FILE\n\n", key);
    return 0;
}

/**
 * Export login fields exactly as the onboard FTP stack loads them.
 * Returns 0 on success. Writes config/ftp.login_export.txt (gitignored).
 */
static int export_ftp_login_fields(void)
{
    /* Same buffer sizes as run_ftp_cli / load_ftp_credentials call sites */
    char host[128] = {0};
    char user[128] = {0};
    char pass[128] = {0};
    char port_buf[16] = {0};
    int port = 0;
    bool ok_host, ok_port, ok_user, ok_pass;
    FILE *out;
    long file_size = -1;
    FILE *rf;

    util_ensure_directory("config");

    /* --- Path A: identical to load_ftp_credentials --- */
    ok_host = util_read_ini_value("config/ftp.ini", "HOST", host, sizeof(host));
    ok_port = util_read_ini_value("config/ftp.ini", "PORT", port_buf, sizeof(port_buf));
    ok_user = util_read_ini_value("config/ftp.ini", "USER", user, sizeof(user));
    ok_pass = util_read_ini_value("config/ftp.ini", "PASS", pass, sizeof(pass));
    if (ok_port) port = atoi(port_buf);

    out = fopen(FTP_LOGIN_EXPORT_PATH, "w");
    if (!out) {
        util_log(SEVERITY_ERROR, "Cannot write %s", FTP_LOGIN_EXPORT_PATH);
        return 1;
    }

    fprintf(out,
        "# =============================================================================\n"
        "# FTP LOGIN FIELD EXPORT (diagnostic)\n"
        "# Generated by: StelliferumAuditor --ftp-export-login\n"
        "# Loader: util_read_ini_value() — SAME path as onboard FTP/GUI/CLI\n"
        "# Source file: config/ftp.ini\n"
        "# This file is LOCAL ONLY (gitignored). Delete after cross-checking.\n"
        "# =============================================================================\n\n");

    rf = fopen("config/ftp.ini", "rb");
    if (rf) {
        if (fseek(rf, 0, SEEK_END) == 0) file_size = ftell(rf);
        fclose(rf);
    }
    fprintf(out, "source_path=config/ftp.ini\n");
    fprintf(out, "source_size_bytes=%ld\n", file_size);
    fprintf(out, "parser=util_read_ini_value (fopen text mode, first KEY= match, strip all trailing CR/LF)\n");
    fprintf(out, "host_buf_capacity=%zu\n", sizeof(host));
    fprintf(out, "user_buf_capacity=%zu\n", sizeof(user));
    fprintf(out, "pass_buf_capacity=%zu\n", sizeof(pass));
    fprintf(out, "port_buf_capacity=%zu\n\n", sizeof(port_buf));

    fprintf(out, "=== PARSED VIA util_read_ini_value (app login path) ===\n\n");
    fprintf(out, "ok_HOST=%d\n", ok_host ? 1 : 0);
    fprintf(out, "ok_PORT=%d\n", ok_port ? 1 : 0);
    fprintf(out, "ok_USER=%d\n", ok_user ? 1 : 0);
    fprintf(out, "ok_PASS=%d\n\n", ok_pass ? 1 : 0);

    write_hex_field(out, "HOST", host);
    write_hex_field(out, "PORT_STRING", port_buf);
    fprintf(out, "PORT_atoi=%d\n\n", port);
    write_hex_field(out, "USER", user);
    write_hex_field(out, "PASS", pass);

    /* Capacity near-miss warnings */
    fprintf(out, "=== CAPACITY CHECKS ===\n");
    fprintf(out, "HOST_fits=%d (len %zu / cap %zu)\n",
            (strlen(host) + 1 < sizeof(host)) ? 1 : 0, strlen(host), sizeof(host));
    fprintf(out, "USER_fits=%d (len %zu / cap %zu)\n",
            (strlen(user) + 1 < sizeof(user)) ? 1 : 0, strlen(user), sizeof(user));
    fprintf(out, "PASS_fits=%d (len %zu / cap %zu)\n",
            (strlen(pass) + 1 < sizeof(pass)) ? 1 : 0, strlen(pass), sizeof(pass));
    fprintf(out, "PASS_near_full=%d (warn if 1 — within 8 bytes of buffer end)\n\n",
            (strlen(pass) + 8 >= sizeof(pass)) ? 1 : 0);

    fprintf(out, "=== RAW FILE LINES (binary read, includes CR/LF as hex) ===\n\n");
    dump_raw_ini_line(out, "config/ftp.ini", "HOST");
    dump_raw_ini_line(out, "config/ftp.ini", "PORT");
    dump_raw_ini_line(out, "config/ftp.ini", "USER");
    dump_raw_ini_line(out, "config/ftp.ini", "PASS");

    fprintf(out, "=== CROSS-CHECK NOTES ===\n");
    fprintf(out, "# Compare HOST_hex vs raw_HOST_line_hex (raw includes 'HOST=' prefix and newlines).\n");
    fprintf(out, "# Compare PASS_len to the password length shown in your host panel.\n");
    fprintf(out, "# If raw ends with <CR> and parsed has_trailing_cr=1, the INI stripper left a CR in the value.\n");
    fprintf(out, "# If PASS_fits=0, the app truncated the password (buffer 128).\n");

    fclose(out);
    util_log(SEVERITY_INFO, "Wrote login field export to %s (gitignored — review then delete)",
             FTP_LOGIN_EXPORT_PATH);
    return 0;
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

// ============================================================================
// Standalone transfer CLI (--ftp-download / --ftp-upload / --ftp-list)
// Same binary, same config/ftp.ini + config/server_paths.ini as the GUI.
// Headless: quiet console by default (no focus-stealing windows). Credentials never on argv.
// ============================================================================

typedef enum {
    FTP_CLI_NONE = 0,
    FTP_CLI_DOWNLOAD,
    FTP_CLI_UPLOAD,
    FTP_CLI_LIST,
    FTP_CLI_PUSH_ECONOMY
} FtpCliMode;

/* True if argv requests quiet CLI (default for --ftp-*; opt out with --verbose). */
static int cli_wants_quiet(int argc, char **argv) {
    int i;
    int verbose = 0;
    int force_quiet = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
            verbose = 1;
        if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0)
            force_quiet = 1;
    }
    if (force_quiet) return 1;
    if (verbose) return 0;
    /* Default quiet for transfer CLI to avoid popping consoles per file. */
    return 1;
}

static void cli_setup_console(int quiet) {
    if (quiet)
        util_setup_console_quiet();
    else
        util_setup_console();
}

static int path_ends_with_slash(const char *path) {
    size_t len;
    if (!path || !*path) return 0;
    len = strlen(path);
    return (path[len - 1] == '/' || path[len - 1] == '\\') ? 1 : 0;
}

/* File-like remote/local path: no trailing slash and basename has an extension. */
static int path_looks_like_file(const char *path) {
    const char *base;
    const char *dot;
    if (!path || !*path) return 0;
    if (path_ends_with_slash(path)) return 0;
    base = util_basename(path);
    if (!base || !*base) return 0;
    dot = strrchr(base, '.');
    return (dot && dot != base && dot[1] != '\0') ? 1 : 0;
}

static int local_path_is_directory(const char *path) {
    if (!path || !*path) return 0;
    if (path_ends_with_slash(path)) return 1;
#ifdef _WIN32
    {
        DWORD attr = GetFileAttributesA(path);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
            return 1;
    }
#else
    {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 1;
    }
#endif
    return 0;
}

static void print_ftp_cli_usage(void) {
    printf(
        "StelliferumAuditor - standalone FTP/SFTP transfer (same binary)\n"
        "\n"
        "Usage:\n"
        "  StelliferumAuditor --ftp-list [path] [--remote <path>] [--dry-run]\n"
        "  StelliferumAuditor --ftp-download [--remote <path>] [--local <path>] [--dry-run]\n"
        "  StelliferumAuditor --ftp-upload   [--remote <path>] [--local <path>] [--dry-run]\n"
        "  StelliferumAuditor --ftp-push-economy [--dry-run]\n"
        "\n"
        "Options:\n"
        "  --ftp-list [path]   List remote directory (path optional; default REMOTE_ROOT)\n"
        "  --ftp-download      Download remote -> local (file or recursive directory)\n"
        "  --ftp-upload        Upload local -> remote (file or directory)\n"
        "  --ftp-push-economy  Upload standard output/ economy pack in ONE process\n"
        "                      (types, spawnables, trader, shops, env, SFL, ...)\n"
        "  --remote <path>     Remote path (default: REMOTE_ROOT from server_paths.ini)\n"
        "  --local <path>      Local path  (default: LOCAL_ROOT from server_paths.ini)\n"
        "  --dry-run           Print planned action; no network transfer\n"
        "  --quiet, -q         No new console window (default for --ftp-*)\n"
        "  --verbose, -v       Allow console attach/banner (still prefers parent TTY)\n"
        "  --ftp-export-login  Dump parsed HOST/PORT/USER/PASS (same loader as FTP) to\n"
        "                      config/ftp.login_export.txt for cross-check (gitignored)\n"
        "  --help, -h          Show this help\n"
        "\n"
        "Config (same as GUI):\n"
        "  config/ftp.ini           HOST, PORT, USER, PASS (+ host-key keys for native SFTP)\n"
        "  config/server_paths.ini  REMOTE_ROOT, LOCAL_ROOT, ...\n"
        "\n"
        "Notes:\n"
        "  * Credentials are read only from config/ftp.ini - never pass passwords on argv.\n"
        "  * Prefer native libcurl (FTP/SFTP). WinSCP is optional Windows fallback only.\n"
        "  * Linux does not require WinSCP.\n"
        "  * Port 22/2222/8827 => SFTP; other ports => FTP.\n"
        "  * CLI defaults to quiet consoles so multi-file scripts do not steal focus.\n"
        "\n"
        "Other CLI modes (unchanged):\n"
        "  --headless              Full download -> pipeline -> upload\n"
        "  --regen | --local       Local pipeline only (no FTP); --local alone is NOT --local <path>\n"
        "  --restore-last-upload   Restore newest pre-upload snapshot\n"
    );
}

/* True if any standalone --ftp-* action flag is present (not --local alone). */
static int cli_wants_ftp_mode(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ftp-download") == 0 ||
            strcmp(argv[i], "--ftp-upload") == 0 ||
            strcmp(argv[i], "--ftp-list") == 0 ||
            strcmp(argv[i], "--ftp-push-economy") == 0 ||
            strcmp(argv[i], "--ftp-help") == 0)
            return 1;
    }
    return 0;
}

/** Join remote root + relative path with single slash. */
static void ftp_join_remote(char *out, size_t out_len, const char *root, const char *rel) {
    size_t rlen;
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!root) root = "";
    if (!rel) rel = "";
    while (*rel == '/') rel++;
    rlen = strlen(root);
    if (rlen > 0 && root[rlen - 1] == '/')
        snprintf(out, out_len, "%s%s", root, rel);
    else if (rlen > 0)
        snprintf(out, out_len, "%s/%s", root, rel);
    else
        snprintf(out, out_len, "/%s", rel);
}

/**
 * One-process economy push: all standard output/ targets without spawning N consoles.
 * Uses same remote mapping as headless phase-3 upload.
 */
static int run_ftp_push_economy(int dry_run) {
    char host[128] = {0};
    char user[128] = {0};
    char pass[128] = {0};
    int port = 0;
    ServerPaths paths;
    char remote_mission[512];
    char mission_base[256];
    char *db_ptr;

#define PUSH_MAX 128
    char locals[PUSH_MAX][MAX_PATH_LEN];
    char remotes[PUSH_MAX][MAX_PATH_LEN];
    const char *local_ptrs[PUSH_MAX];
    const char *remote_ptrs[PUSH_MAX];
    int count = 0;
    int i;
    int upload_ok = 0, upload_fail = 0;

    if (!load_ftp_credentials(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "FTP push: missing config/ftp.ini credentials");
        return 1;
    }
    load_server_paths(&paths);

    /* Mission base from REMOTE_TYPES (strip /db/...) */
    strncpy(mission_base, paths.remote_types, sizeof(mission_base) - 1);
    mission_base[sizeof(mission_base) - 1] = '\0';
    db_ptr = strstr(mission_base, "/db/");
    if (db_ptr) *db_ptr = '\0';
    ftp_join_remote(remote_mission, sizeof(remote_mission), paths.remote_root, mission_base);
    {
        size_t rm = strlen(remote_mission);
        if (rm > 1 && remote_mission[rm - 1] == '/')
            remote_mission[rm - 1] = '\0';
    }

#define PUSH_ADD(local_path, remote_path) do { \
    if (count < PUSH_MAX && util_file_exists(local_path)) { \
        strncpy(locals[count], local_path, MAX_PATH_LEN - 1); \
        locals[count][MAX_PATH_LEN - 1] = '\0'; \
        strncpy(remotes[count], remote_path, MAX_PATH_LEN - 1); \
        remotes[count][MAX_PATH_LEN - 1] = '\0'; \
        count++; \
    } \
} while (0)

    {
        char full[512];
        ftp_join_remote(full, sizeof(full), paths.remote_root, paths.remote_types);
        PUSH_ADD("output/types.xml", full);
        snprintf(full, sizeof(full), "%s/cfgspawnabletypes.xml", remote_mission);
        PUSH_ADD("output/cfgspawnabletypes.xml", full);
        snprintf(full, sizeof(full), "%s/cfgeconomycore.xml", remote_mission);
        PUSH_ADD("output/cfgeconomycore.xml", full);
        snprintf(full, sizeof(full), "%s/cfglimitsdefinitionuser.xml", remote_mission);
        PUSH_ADD("output/cfglimitsdefinitionuser.xml", full);
        snprintf(full, sizeof(full), "%s/cfgrandompresets.xml", remote_mission);
        PUSH_ADD("output/cfgrandompresets.xml", full);
        if (paths.remote_trader[0]) {
            ftp_join_remote(full, sizeof(full), paths.remote_root, paths.remote_trader);
            PUSH_ADD("output/TraderConfig.txt", full);
        }
        {
            char rt_sfl[512] = "SearchForLoot/SearchForLoot.json";
            util_read_ini_value("config/server_paths.ini", "REMOTE_SFL", rt_sfl, sizeof(rt_sfl));
            ftp_join_remote(full, sizeof(full), paths.remote_root, rt_sfl);
            PUSH_ADD("output/SearchForLoot.json", full);
        }
        snprintf(full, sizeof(full), "%s/db/types_infected.xml", remote_mission);
        PUSH_ADD("output/zombie_tiers/types_infected.xml", full);
        snprintf(full, sizeof(full), "%s/db/types_wildlife.xml", remote_mission);
        PUSH_ADD("output/zombie_tiers/types_wildlife.xml", full);
    }

    /* Per-shop files */
#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE hFind;
        char trader_dir[MAX_PATH_LEN];
        char *slash;
        strncpy(trader_dir, paths.remote_trader, sizeof(trader_dir) - 1);
        trader_dir[sizeof(trader_dir) - 1] = '\0';
        slash = strrchr(trader_dir, '/');
        if (slash) *(slash + 1) = '\0';
        else strncpy(trader_dir, "profiles/Trader/", sizeof(trader_dir) - 1);

        hFind = FindFirstFileA("output\\shops\\*", &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                char local_shop[MAX_PATH_LEN];
                char remote_shop[512];
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                snprintf(local_shop, sizeof(local_shop), "output/shops/%s", fd.cFileName);
                ftp_join_remote(remote_shop, sizeof(remote_shop), paths.remote_root, trader_dir);
                {
                    size_t rlen = strlen(remote_shop);
                    if (rlen > 0 && remote_shop[rlen - 1] != '/') {
                        remote_shop[rlen] = '/';
                        remote_shop[rlen + 1] = '\0';
                    }
                }
                strncat(remote_shop, fd.cFileName, sizeof(remote_shop) - strlen(remote_shop) - 1);
                PUSH_ADD(local_shop, remote_shop);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
    /* Chernarus env (active map from mission path) */
    if (strstr(remote_mission, "chernarusplus")) {
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA("output\\chernarusplus\\env\\*.xml", &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                char local_file[MAX_PATH_LEN];
                char remote_file[512];
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                snprintf(local_file, sizeof(local_file), "output/chernarusplus/env/%s", fd.cFileName);
                snprintf(remote_file, sizeof(remote_file), "%s/env/%s", remote_mission, fd.cFileName);
                PUSH_ADD(local_file, remote_file);
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);
        }
    }
#endif

#undef PUSH_ADD

    if (count == 0) {
        util_log(SEVERITY_ERROR, "FTP push: no output files found under output/");
        return 1;
    }

    util_log(SEVERITY_INFO, "FTP push-economy: %d file(s) planned (single process, quiet console)", count);
    for (i = 0; i < count; i++) {
        util_log(SEVERITY_INFO, "  [%d/%d] %s -> %s", i + 1, count, locals[i], remotes[i]);
        if (dry_run)
            printf("[dry-run] %s -> %s\n", locals[i], remotes[i]);
        local_ptrs[i] = locals[i];
        remote_ptrs[i] = remotes[i];
    }

    if (dry_run) {
        printf("[dry-run] Would upload %d files via native/batch FTP (no console spam).\n", count);
        return 0;
    }

#ifdef STELLI_USE_LIBCURL
    if (ftp_native_backend_available())
        ftp_native_reload_security_config();
#endif

    if (!ftp_upload_batch(host, port, user, pass, local_ptrs, remote_ptrs, count,
                          &upload_ok, &upload_fail)) {
        util_log(SEVERITY_ERROR, "FTP push-economy: batch upload failed (%d ok, %d fail)",
                 upload_ok, upload_fail);
        return 1;
    }
    util_log(SEVERITY_INFO, "FTP push-economy complete: %d ok, %d fail", upload_ok, upload_fail);
    return (upload_fail > 0) ? 1 : 0;
#undef PUSH_MAX
}

static int run_ftp_cli(int argc, char **argv) {
    FtpCliMode mode = FTP_CLI_NONE;
    int dry_run = 0;
    int show_help = 0;
    int multi_mode = 0;
    char remote_arg[MAX_PATH_LEN];
    char local_arg[MAX_PATH_LEN];
    char host[128] = {0};
    char user[128] = {0};
    char pass[128] = {0};
    int port = 0;
    ServerPaths paths;
    char remote_path[MAX_PATH_LEN];
    char local_path[MAX_PATH_LEN];
    const char *proto;
    int i;

    remote_arg[0] = '\0';
    local_arg[0] = '\0';

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--ftp-download") == 0) {
            if (mode != FTP_CLI_NONE) multi_mode = 1;
            mode = FTP_CLI_DOWNLOAD;
        } else if (strcmp(argv[i], "--ftp-upload") == 0) {
            if (mode != FTP_CLI_NONE) multi_mode = 1;
            mode = FTP_CLI_UPLOAD;
        } else if (strcmp(argv[i], "--ftp-list") == 0) {
            if (mode != FTP_CLI_NONE) multi_mode = 1;
            mode = FTP_CLI_LIST;
            /* Optional path immediately after --ftp-list if not another flag */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                strncpy(remote_arg, argv[++i], sizeof(remote_arg) - 1);
                remote_arg[sizeof(remote_arg) - 1] = '\0';
            }
        } else if (strcmp(argv[i], "--ftp-push-economy") == 0) {
            if (mode != FTP_CLI_NONE) multi_mode = 1;
            mode = FTP_CLI_PUSH_ECONOMY;
        } else if (strcmp(argv[i], "--remote") == 0) {
            if (i + 1 >= argc) {
                util_log(SEVERITY_ERROR, "FTP CLI: --remote requires a path argument");
                print_ftp_cli_usage();
                return 1;
            }
            strncpy(remote_arg, argv[++i], sizeof(remote_arg) - 1);
            remote_arg[sizeof(remote_arg) - 1] = '\0';
        } else if (strcmp(argv[i], "--local") == 0) {
            if (i + 1 >= argc) {
                util_log(SEVERITY_ERROR, "FTP CLI: --local requires a path argument");
                print_ftp_cli_usage();
                return 1;
            }
            strncpy(local_arg, argv[++i], sizeof(local_arg) - 1);
            local_arg[sizeof(local_arg) - 1] = '\0';
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0 ||
                   strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            /* handled by cli_wants_quiet / cli_setup_console before run_ftp_cli */
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--ftp-help") == 0) {
            show_help = 1;
        } else if (strcmp(argv[i], "--headless") == 0 ||
                   strcmp(argv[i], "--regen") == 0 ||
                   strcmp(argv[i], "--restore-last-upload") == 0) {
            util_log(SEVERITY_ERROR,
                     "FTP CLI: cannot combine --ftp-* with %s (use one mode per invocation)",
                     argv[i]);
            return 1;
        } else {
            util_log(SEVERITY_ERROR, "FTP CLI: unknown argument: %s", argv[i]);
            print_ftp_cli_usage();
            return 1;
        }
    }

    if (show_help) {
        print_ftp_cli_usage();
        return 0;
    }
    if (mode == FTP_CLI_NONE) {
        print_ftp_cli_usage();
        return 1;
    }
    if (multi_mode) {
        util_log(SEVERITY_ERROR, "FTP CLI: specify only one of --ftp-download / --ftp-upload / --ftp-list / --ftp-push-economy");
        return 1;
    }

    if (mode == FTP_CLI_PUSH_ECONOMY)
        return run_ftp_push_economy(dry_run);

    load_server_paths(&paths);

    if (remote_arg[0]) {
        strncpy(remote_path, remote_arg, sizeof(remote_path) - 1);
        remote_path[sizeof(remote_path) - 1] = '\0';
    } else {
        strncpy(remote_path, paths.remote_root, sizeof(remote_path) - 1);
        remote_path[sizeof(remote_path) - 1] = '\0';
        if (!remote_path[0])
            strncpy(remote_path, "/", sizeof(remote_path) - 1);
    }

    if (local_arg[0]) {
        strncpy(local_path, local_arg, sizeof(local_path) - 1);
        local_path[sizeof(local_path) - 1] = '\0';
    } else {
        strncpy(local_path, paths.local_root, sizeof(local_path) - 1);
        local_path[sizeof(local_path) - 1] = '\0';
        if (!local_path[0])
            strncpy(local_path, "downloaded_mods", sizeof(local_path) - 1);
    }

    if (!load_ftp_credentials(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR,
                 "FTP CLI: Missing FTP config. Copy config/ftp.ini.example to config/ftp.ini "
                 "and set HOST/PORT/USER/PASS.");
        return 1;
    }

    /* Never log or print PASS — only host/port/user for diagnostics. */
    proto = (port == 22 || port == 2222 || port == 8827) ? "sftp" : "ftp";
    util_log(SEVERITY_INFO,
             "FTP CLI: mode=%s proto=%s host=%s port=%d user=%s dry_run=%d",
             mode == FTP_CLI_DOWNLOAD ? "download" :
             mode == FTP_CLI_UPLOAD   ? "upload"   : "list",
             proto, host, port, user, dry_run);
    util_log(SEVERITY_INFO, "FTP CLI: remote='%s' local='%s'", remote_path, local_path);
#ifdef STELLI_USE_LIBCURL
    if (ftp_native_backend_available()) {
        util_log(SEVERITY_INFO, "FTP CLI: backend=native libcurl (%s)",
                 ftp_native_curl_version_string());
        ftp_native_reload_security_config();
    } else {
        util_log(SEVERITY_INFO, "FTP CLI: backend=fallback (native unavailable)");
    }
#else
    util_log(SEVERITY_INFO, "FTP CLI: backend=WinSCP/legacy (STELLI_USE_LIBCURL not built)");
#endif

    if (mode == FTP_CLI_LIST) {
        if (dry_run) {
            printf("[dry-run] Would list remote directory: %s\n", remote_path);
            printf("[dry-run]   %s://%s@%s:%d%s\n", proto, user, host, port, remote_path);
            printf("[dry-run]   (password from config/ftp.ini only; not shown)\n");
            return 0;
        }

        {
            /* Heap: RemoteFileBrowser is large (~1.6MB); avoid stack overflow. */
            RemoteFileBrowser *browser = (RemoteFileBrowser *)malloc(sizeof(RemoteFileBrowser));
            int e;
            int rc = 1;
            if (!browser) {
                util_log(SEVERITY_ERROR, "FTP CLI: out of memory for directory listing");
                return 1;
            }
            memset(browser, 0, sizeof(*browser));
            util_log(SEVERITY_INFO, "FTP CLI: listing %s ...", remote_path);
            if (!ftp_list_directory(host, port, user, pass, remote_path, browser)) {
                util_log(SEVERITY_ERROR, "FTP CLI: list failed: %s",
                         browser->error[0] ? browser->error : "unknown error");
                free(browser);
                return 1;
            }
            printf("Remote listing: %s  (%d entries)\n",
                   browser->current_path[0] ? browser->current_path : remote_path,
                   browser->count);
            printf("%-6s  %12s  %-19s  %s\n", "TYPE", "SIZE", "DATE", "NAME");
            printf("------  ------------  -------------------  ----\n");
            for (e = 0; e < browser->count; e++) {
                const RemoteFileEntry *ent = &browser->entries[e];
                printf("%-6s  %12lld  %-19s  %s\n",
                       ent->is_directory ? "dir" : "file",
                       (long long)ent->size,
                       ent->date_str[0] ? ent->date_str : "-",
                       ent->name);
            }
            util_log(SEVERITY_INFO, "FTP CLI: list complete (%d entries)", browser->count);
            free(browser);
            rc = 0;
            return rc;
        }
    }

    if (mode == FTP_CLI_DOWNLOAD) {
        int as_file = path_looks_like_file(remote_path);
        if (dry_run) {
            if (as_file) {
                printf("[dry-run] Would download file:\n");
                printf("[dry-run]   remote: %s\n", remote_path);
                printf("[dry-run]   local:  %s\n", local_path);
            } else {
                printf("[dry-run] Would download directory (recursive):\n");
                printf("[dry-run]   remote: %s\n", remote_path);
                printf("[dry-run]   local:  %s\n", local_path);
            }
            printf("[dry-run]   %s://%s@%s:%d  (password from config only)\n",
                   proto, user, host, port);
            return 0;
        }

        if (!as_file)
            util_ensure_directory(local_path);
        else {
            /* Ensure parent directory of local file exists when possible */
            char parent[MAX_PATH_LEN];
            char *slash;
            strncpy(parent, local_path, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            slash = strrchr(parent, '/');
#ifdef _WIN32
            {
                char *bslash = strrchr(parent, '\\');
                if (bslash && (!slash || bslash > slash)) slash = bslash;
            }
#endif
            if (slash && slash != parent) {
                *slash = '\0';
                util_ensure_directory(parent);
            }
        }

        if (as_file) {
            util_log(SEVERITY_INFO, "FTP CLI: downloading file %s -> %s", remote_path, local_path);
            if (!ftp_download_file(host, port, user, pass, remote_path, local_path)) {
                util_log(SEVERITY_ERROR, "FTP CLI: download file failed");
                return 1;
            }
        } else {
            util_log(SEVERITY_INFO, "FTP CLI: downloading recursive %s -> %s",
                     remote_path, local_path);
            if (!ftp_download_recursive(host, port, user, pass, remote_path, local_path)) {
                util_log(SEVERITY_ERROR, "FTP CLI: recursive download failed");
                return 1;
            }
        }
        util_log(SEVERITY_INFO, "FTP CLI: download complete");
        return 0;
    }

    if (mode == FTP_CLI_UPLOAD) {
        int as_dir = local_path_is_directory(local_path) ||
                     (!util_file_exists(local_path) && !path_looks_like_file(local_path));
        /* Prefer file upload when local exists as a regular file */
        if (util_file_exists(local_path) && !local_path_is_directory(local_path))
            as_dir = 0;

        if (dry_run) {
            if (as_dir) {
                printf("[dry-run] Would upload directory:\n");
            } else {
                printf("[dry-run] Would upload file:\n");
            }
            printf("[dry-run]   local:  %s\n", local_path);
            printf("[dry-run]   remote: %s\n", remote_path);
            printf("[dry-run]   %s://%s@%s:%d  (password from config only)\n",
                   proto, user, host, port);
            return 0;
        }

        if (!util_file_exists(local_path) && !local_path_is_directory(local_path)) {
            util_log(SEVERITY_ERROR, "FTP CLI: local path does not exist: %s", local_path);
            return 1;
        }

        if (as_dir) {
            util_log(SEVERITY_INFO, "FTP CLI: uploading directory %s -> %s",
                     local_path, remote_path);
            if (!ftp_upload_directory(host, port, user, pass, local_path, remote_path)) {
                util_log(SEVERITY_ERROR, "FTP CLI: directory upload failed");
                return 1;
            }
        } else {
            util_log(SEVERITY_INFO, "FTP CLI: uploading file %s -> %s", local_path, remote_path);
            if (!ftp_upload_file(host, port, user, pass, local_path, remote_path)) {
                util_log(SEVERITY_ERROR, "FTP CLI: file upload failed");
                return 1;
            }
        }
        util_log(SEVERITY_INFO, "FTP CLI: upload complete");
        return 0;
    }

    print_ftp_cli_usage();
    return 1;
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

    /*
     * Simple verbs: help | list | pull | pipeline | push | restore | run <recipe>
     * Also bare recipe names from config/commands.ini.
     * Keep long flags (--ftp-*, --regen, --headless) for scripts that already use them.
     */
    if (cli_is_simple_command(argc, argv)) {
        int result;
        {
            extern void util_check_and_install_dependencies(void);
            util_check_and_install_dependencies();
        }
        result = cli_run_simple_command(argc, argv);
        util_close_job_object();
        util_close_logger();
        return result;
    }

    /* Global help — never open the GUI for --help / -h / --ftp-help alone. */
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0 ||
                     strcmp(argv[1], "--ftp-help") == 0 || strcmp(argv[1], "/?") == 0)) {
        cli_setup_console(0); /* attach parent if any; avoid hidden-only for help text */
        cli_print_simple_help();
        printf("\n--- Long flags (advanced) ---\n\n");
        print_ftp_cli_usage();
        util_close_logger();
        return 0;
    }

    /*
     * Export login fields exactly as onboard FTP loads them (no network).
     * Writes config/ftp.login_export.txt for local cross-check (gitignored).
     */
    if (argc > 1 && (strcmp(argv[1], "--ftp-export-login") == 0 ||
                     strcmp(argv[1], "--export-ftp-login") == 0)) {
        cli_setup_console(cli_wants_quiet(argc, argv));
        {
            int result = export_ftp_login_fields();
            util_close_logger();
            return result;
        }
    }

    /*
     * Standalone FTP/SFTP CLI (--ftp-download / --ftp-upload / --ftp-list /
     * --ftp-push-economy). Quiet console by default so multi-file scripts do
     * not open focus-stealing windows. Use --verbose for a visible banner.
     */
    if (cli_wants_ftp_mode(argc, argv)) {
        cli_setup_console(cli_wants_quiet(argc, argv));
        {
            extern void util_check_and_install_dependencies(void);
            util_check_and_install_dependencies();
        }
        {
            int result = run_ftp_cli(argc, argv);
            util_close_job_object();
            util_close_logger();
            return result;
        }
    }

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
        cli_setup_console(cli_wants_quiet(argc, argv));
        int result = run_headless(ctx);
        free(ctx);
        util_close_job_object();
        util_close_logger();
        return result;
    }

    /* --regen and bare --local (no --ftp-*) = local pipeline only, no FTP.
     * Note: --local <path> is handled only inside --ftp-* mode above. */
    if (argc > 1 && (strcmp(argv[1], "--regen") == 0 || strcmp(argv[1], "--local") == 0)) {
        cli_setup_console(cli_wants_quiet(argc, argv));
        int result = run_regen(ctx);
        free(ctx);
        util_close_job_object();
        util_close_logger();
        return result;
    }

    if (argc > 1 && strcmp(argv[1], "--restore-last-upload") == 0) {
        cli_setup_console(cli_wants_quiet(argc, argv));
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
