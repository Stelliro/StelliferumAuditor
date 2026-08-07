/**
 * STELLIFERUM AUDITOR — Dear ImGui UI
 * ====================================
 * Clean, modern interface built on Dear ImGui + rlImGui (raylib backend).
 * Replaces the old raygui-based UI with proper tables, tabs, diff view,
 * and a sew/stitch workflow.
 */

extern "C" {
#include "auditor.h"
#include "loot_policy.h"
#include "loot_manager.h"
}

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <algorithm>

// Platform: Win32 symbol collision guards
// Suppress C4005 — we intentionally redefine Win32 macros to avoid collisions with raylib
#ifdef _WIN32
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
    #include <commdlg.h>
    #include <shlobj.h>
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
    // Windows winerror.h defines SEVERITY_ERROR=1 which clashes with our enum
    #undef SEVERITY_ERROR
    #undef SEVERITY_SUCCESS
    // Windows fileapi.h defines FILE_TYPE_UNKNOWN=0 which clashes with our FileType enum
    #undef FILE_TYPE_UNKNOWN
#endif

#include "raylib.h"
#include "imgui.h"
#include "rlImGui.h"

// ============================================================================
// CONSTANTS
// ============================================================================
#define WINDOW_WIDTH   1440
#define WINDOW_HEIGHT  900
#define SIDEBAR_WIDTH  290.0f
#define STATUS_BAR_H   32.0f
#define MAX_LOADED_FILES 4096

// Notification system
#define MAX_NOTIFICATIONS     8
#define NOTIFICATION_WIDTH    420.0f
#define NOTIFICATION_PADDING  10.0f
#define NOTIFICATION_LIFETIME 15.0f  // seconds before auto-dismiss

// ============================================================================
// WINSCP ERROR NOTIFICATIONS
// ============================================================================
struct WinSCPNotification {
    char title[128];          // e.g. "Connection Failed", "Upload Error"
    char message[2048];       // Full error detail (copyable)
    float birth_time;         // GetTime() when created
    bool  active;             // Slot in use
    bool  copied;             // Flash feedback after copy
    float copied_timer;       // Time remaining for "Copied!" feedback
};

static WinSCPNotification g_notifications[MAX_NOTIFICATIONS] = {};
static int g_notification_count = 0;

// ============================================================================
// UI STATE
// ============================================================================
struct UIState {
    // File tracking
    char loaded_files[MAX_LOADED_FILES][MAX_PATH_LEN];
    int  loaded_file_count = 0;

    // FTP credentials
    char ftp_host[128] = {};
    char ftp_user[128] = {};
    char ftp_pass[128] = {};
    char ftp_port[8]   = "21";
    char remote_path[256] = "/";

    // Search
    char search_buffer[128] = {};

    // Download state
    bool download_in_progress = false;
    bool download_ready       = false;
    bool download_success     = false;
    char download_status[256] = {};

    // Editing
    int  editing_item         = -1;
    int  edit_nominal         = 0;
    int  edit_lifetime        = 0;
    int  edit_restock         = 0;
    int  edit_min             = 0;

    // Tab state
    int  active_tab           = 0;  // 0=Items, 1=Issues, 2=Diff, 3=Swarm, 4=Connection

    // Scroll tracking for diff
    bool show_only_modified   = true;

    // File type filter for items tab
    // -1=All, 0=Unknown, 1=Economy(types), 2=Spawnable, 3=Territory, 4=Globals, 5=Config, 6=Events, 7=Trader, 8=RandomPresets
    int  file_type_filter     = -1;

    // Mod filter for items tab
    char mod_filter[128]      = {};

    // Hide deleted items after audit
    bool hide_deleted         = true;

    // Shop mod selection (persisted in server_paths.ini)
    int  shop_mod_index       = 0;  // 0=TraderPlus, 1=Dr.Jones, 2=Expansion

    // Pipeline state (background thread)
    bool pipeline_in_progress = false;
    bool pipeline_ready       = false;
    bool pipeline_success     = false;
    char pipeline_phase[256]  = {};   // Current phase description for status bar

    // Loot Policy tab (tier list + blacklist editors)
    char lp_tiers_buf[2048]      = {};
    char lp_blacklist_buf[16384] = {};
    bool lp_ui_loaded            = false;  // false => repopulate buffers from g_loot_policy
    char lp_status[128]          = {};
};

static UIState ui;

static const char* file_type_name(FileType ft) {
    switch (ft) {
        case FILE_TYPE_ECONOMY:       return "Types";
        case FILE_TYPE_SPAWNABLE:     return "Spawnable";
        case FILE_TYPE_TERRITORY:     return "Territory";
        case FILE_TYPE_GLOBALS:       return "Globals";
        case FILE_TYPE_CONFIG:        return "Config";
        case FILE_TYPE_EVENTS:        return "Events";
        case FILE_TYPE_TRADER:        return "Trader";
        case FILE_TYPE_RANDOMPRESETS: return "Presets";
        default:                      return "Unknown";
    }
}

static const ImVec4 file_type_color(FileType ft) {
    switch (ft) {
        case FILE_TYPE_ECONOMY:       return ImVec4(0.40f, 0.90f, 0.40f, 1.00f); // green
        case FILE_TYPE_SPAWNABLE:     return ImVec4(0.40f, 0.75f, 0.95f, 1.00f); // cyan
        case FILE_TYPE_TERRITORY:     return ImVec4(0.95f, 0.85f, 0.25f, 1.00f); // yellow
        case FILE_TYPE_TRADER:        return ImVec4(1.00f, 0.78f, 0.00f, 1.00f); // amber
        case FILE_TYPE_EVENTS:        return ImVec4(0.70f, 0.40f, 0.90f, 1.00f); // purple
        case FILE_TYPE_GLOBALS:       return ImVec4(0.95f, 0.55f, 0.30f, 1.00f); // orange
        case FILE_TYPE_RANDOMPRESETS: return ImVec4(0.60f, 0.80f, 0.60f, 1.00f); // soft green
        default:                      return ImVec4(0.50f, 0.50f, 0.53f, 1.00f); // dim
    }
}

// ============================================================================
// SERVER PATHS (same structure as before)
// ============================================================================
struct ServerPaths {
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
};

// ============================================================================
// DOWNLOAD THREAD (Windows)
// ============================================================================
struct DownloadJob {
    char host[128];
    char user[128];
    char pass[128];
    int  port;
    ServerPaths paths;
};

static DownloadJob download_job;
#ifdef _WIN32
static HANDLE download_thread = NULL;
#endif

// ============================================================================
// PIPELINE THREAD (runs audit + swarm sequentially in background)
// ============================================================================
struct PipelineJob {
    AuditorContext *ctx;
};

static PipelineJob pipeline_job;
#ifdef _WIN32
static HANDLE pipeline_thread = NULL;
#endif

static void ui_ensure_dir(const char *path) {
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
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
    if (!base || !*base) { out[0] = '\0'; return; }
    snprintf(out, out_len, "%s/%s", local_root, base);
}

static void ui_load_server_paths(ServerPaths *paths) {
    memset(paths, 0, sizeof(ServerPaths));
    char remote_root[256] = "/";
    strncpy(paths->local_root, "downloaded_mods", sizeof(paths->local_root) - 1);

    bool has_types = util_read_ini_value("config/server_paths.ini", "REMOTE_TYPES", paths->remote_types, sizeof(paths->remote_types));
    bool has_spawnable = util_read_ini_value("config/server_paths.ini", "REMOTE_SPAWNABLE", paths->remote_spawnable, sizeof(paths->remote_spawnable));
    bool has_events = util_read_ini_value("config/server_paths.ini", "REMOTE_EVENTS", paths->remote_events, sizeof(paths->remote_events));
    bool has_globals = util_read_ini_value("config/server_paths.ini", "REMOTE_GLOBALS", paths->remote_globals, sizeof(paths->remote_globals));
    util_read_ini_value("config/server_paths.ini", "REMOTE_ROOT", remote_root, sizeof(remote_root));
    util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", paths->local_root, sizeof(paths->local_root));
    if (!util_read_ini_value("config/server_paths.ini", "REMOTE_TRADER", paths->remote_trader, sizeof(paths->remote_trader))) {
        strncpy(paths->remote_trader, "profiles/TraderPlus/TraderPlusTrading.json", sizeof(paths->remote_trader) - 1);
    }

    // Load shop mod selection
    char shop_mod_buf[32] = {};
    if (util_read_ini_value("config/server_paths.ini", "SHOP_MOD", shop_mod_buf, sizeof(shop_mod_buf))) {
        if (util_strcasecmp(shop_mod_buf, "drjones") == 0 || util_strcasecmp(shop_mod_buf, "dr_jones") == 0)
            ui.shop_mod_index = 1;
        else if (util_strcasecmp(shop_mod_buf, "expansion") == 0)
            ui.shop_mod_index = 2;
        else
            ui.shop_mod_index = 0; // traderplus
    }

    strncpy(paths->remote_root, remote_root, sizeof(paths->remote_root) - 1);

    if (!has_types)     build_remote(paths->remote_types,     sizeof(paths->remote_types),     remote_root, "db/types.xml");
    if (!has_spawnable) build_remote(paths->remote_spawnable, sizeof(paths->remote_spawnable), remote_root, "cfgspawnabletypes.xml");
    if (!has_events)    build_remote(paths->remote_events,    sizeof(paths->remote_events),    remote_root, "db/events.xml");
    if (!has_globals)   build_remote(paths->remote_globals,   sizeof(paths->remote_globals),   remote_root, "db/globals.xml");

    build_local(paths->local_types,     sizeof(paths->local_types),     paths->local_root, paths->remote_types);
    build_local(paths->local_spawnable, sizeof(paths->local_spawnable), paths->local_root, paths->remote_spawnable);
    build_local(paths->local_events,    sizeof(paths->local_events),    paths->local_root, paths->remote_events);
    build_local(paths->local_globals,   sizeof(paths->local_globals),   paths->local_root, paths->remote_globals);
}

static void load_ftp_config() {
    strncpy(ui.remote_path, "/", 255);
    FILE *f = fopen("config/ftp.ini", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        if (len > 0 && line[len-1] == '\n') line[len-1] = '\0';
        if (strncmp(line, "HOST=", 5) == 0) strncpy(ui.ftp_host, line + 5, 127);
        else if (strncmp(line, "PORT=", 5) == 0) strncpy(ui.ftp_port, line + 5, 7);
        else if (strncmp(line, "USER=", 5) == 0) strncpy(ui.ftp_user, line + 5, 127);
        else if (strncmp(line, "PASS=", 5) == 0) strncpy(ui.ftp_pass, line + 5, 127);
    }
    fclose(f);
}

static void save_ftp_config() {
#ifdef _WIN32
    CreateDirectoryA("config", NULL);
#endif
    FILE *f = fopen("config/ftp.ini", "w");
    if (!f) return;
    fprintf(f, "HOST=%s\n", ui.ftp_host);
    fprintf(f, "PORT=%s\n", ui.ftp_port);
    fprintf(f, "USER=%s\n", ui.ftp_user);
    fprintf(f, "PASS=%s\n", ui.ftp_pass);
    fclose(f);
}

// ============================================================================
// FILE LOADING (recursive directory scan)
// ============================================================================
static bool scan_config_loaded = false;
static int  scan_rescan_days   = 0;

static int get_rescan_days() {
    if (!scan_config_loaded) {
        char buf[16] = {};
        if (util_read_ini_value("config/server_paths.ini", "RESCAN_DAYS", buf, sizeof(buf)))
            scan_rescan_days = atoi(buf);
        scan_config_loaded = true;
    }
    return scan_rescan_days;
}

// Active map mpmission directory (e.g. "dayzoffline.chernarusplus").
// Set by do_full_load() before scanning. Used to skip inactive map mpmissions.
static char active_mpmission[128] = {};

static void recursive_load_directory(AuditorContext *ctx, const char *path, bool force = false) {
#ifdef _WIN32
    if (!ctx || !path) return;
    if (ctx->item_count >= MAX_ITEMS - 1) return;
    if (strlen(path) >= MAX_PATH_LEN - 4) return;
    if (ui.loaded_file_count >= MAX_LOADED_FILES - 1) return;

    char path_lower[MAX_PATH_LEN];
    strncpy(path_lower, path, MAX_PATH_LEN - 1);
    path_lower[MAX_PATH_LEN - 1] = '\0';
    for (int i = 0; path_lower[i]; i++) path_lower[i] = (char)tolower((unsigned char)path_lower[i]);

    // Block non-data directories:
    // - sorted/          : duplicate copies created by file_sorter (loaded again during swarm)
    // - addons/          : only .pbo files — no XML data
    // - keys/            : only .bikey files
    // - docs/            : documentation only
    // - hosthavoc        : hosting provider runtime files
    // - commonredist     : Steam redistributables
    // - steamapps        : Steam metadata
    // - server_manager   : HostHavoc manager
    // - battleye         : anti-cheat
    // - backup           : old backups
    // - dta              : DayZ engine data
    // - bliss/users/logs : runtime data
    if (strstr(path_lower, "sorted")         || strstr(path_lower, "commonredist")  ||
        strstr(path_lower, "steamapps")      || strstr(path_lower, "server_manager") ||
        strstr(path_lower, "battleye")       || strstr(path_lower, "backup") ||
        strstr(path_lower, "dta")            || strstr(path_lower, "bliss") ||
        strstr(path_lower, "users")          || strstr(path_lower, "logs") ||
        strstr(path_lower, "addons")         || strstr(path_lower, "keys") ||
        strstr(path_lower, "docs")           || strstr(path_lower, "hosthavoc")) {
        return;
    }

    // Skip inactive map mpmission directories.
    // Only load from the server's active map (e.g. chernarusplus) — not enoch/sakhal.
    if (strstr(path_lower, "dayzoffline.") && active_mpmission[0]) {
        if (!strstr(path_lower, active_mpmission)) return;
    }

    char search_path[MAX_PATH_LEN];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s\\%s", path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            recursive_load_directory(ctx, full_path, force);
        } else {
            size_t len = strlen(fd.cFileName);
            bool is_xml  = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".xml") == 0);
            bool is_cfg  = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".cfg") == 0);
            bool is_ini  = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".ini") == 0);
            bool is_txt  = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".txt") == 0);
            bool is_json = (len > 5 && util_strcasecmp(fd.cFileName + len - 5, ".json") == 0);

            if (is_xml || is_cfg || is_ini || is_txt || is_json) {
                if (!force) {
                    int rescan_days = get_rescan_days();
                    if (rescan_days > 0 && !util_index_is_stale(full_path, rescan_days)) continue;
                }

                if (ui.loaded_file_count < MAX_LOADED_FILES) {
                    strncpy(ui.loaded_files[ui.loaded_file_count], full_path, MAX_PATH_LEN - 1);
                    ui.loaded_file_count++;
                }

                if (is_xml) {
                    FileType type = parser_detect_file_type(full_path);
                    if (type == FILE_TYPE_ECONOMY) {
                        // parser_load_types_xml already logs "Loaded: file (N items)"
                        parser_load_types_xml(ctx, full_path);
                    } else if (type != FILE_TYPE_UNKNOWN) {
                        util_index_touch(full_path, type);
                    }
                } else {
                    util_index_touch(full_path, FILE_TYPE_CONFIG);
                }
            }
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#endif
}

// ============================================================================
// DOWNLOAD THREAD
// ============================================================================
#ifdef _WIN32
static DWORD WINAPI download_thread_proc(LPVOID param) {
    DownloadJob *job = (DownloadJob *)param;

    // Build full remote paths by joining remote_root + relative path.
    // Without this, WinSCP `get` uses bare relative paths and the FTP
    // server returns 550 (file not found) because it needs the root prefix.
    char full_types[512], full_spawn[512], full_events[512], full_globals[512];
    path_join(full_types,   sizeof(full_types),   job->paths.remote_root, job->paths.remote_types);
    path_join(full_spawn,   sizeof(full_spawn),   job->paths.remote_root, job->paths.remote_spawnable);
    path_join(full_events,  sizeof(full_events),  job->paths.remote_root, job->paths.remote_events);
    path_join(full_globals, sizeof(full_globals),  job->paths.remote_root, job->paths.remote_globals);
    const char *remote_paths[] = { full_types, full_spawn, full_events, full_globals };
    const char *local_paths[] = {
        job->paths.local_types, job->paths.local_spawnable,
        job->paths.local_events, job->paths.local_globals
    };

    bool downloaded = false;
    char flag[8] = {};
    if (util_read_ini_value("config/server_paths.ini", "DOWNLOAD_ALL_XML", flag, sizeof(flag)) && atoi(flag) == 1) {
        downloaded = ftp_download_recursive(job->host, job->port, job->user, job->pass, job->paths.remote_root, job->paths.local_root);
    }
    // Fallback: if recursive download was disabled or failed, grab key files individually
    if (!downloaded || !util_file_exists(job->paths.local_types)) {
        ftp_download_batch(job->host, job->port, job->user, job->pass, remote_paths, local_paths, 4);
    }
    if (!util_file_exists(job->paths.local_types))
        util_find_file_by_name(job->paths.local_root, "types.xml", job->paths.local_types, sizeof(job->paths.local_types));
    if (!util_file_exists(job->paths.local_spawnable))
        util_find_file_by_name(job->paths.local_root, "cfgspawnabletypes.xml", job->paths.local_spawnable, sizeof(job->paths.local_spawnable));
    if (!util_file_exists(job->paths.local_events))
        util_find_file_by_name(job->paths.local_root, "events.xml", job->paths.local_events, sizeof(job->paths.local_events));
    if (!util_file_exists(job->paths.local_globals))
        util_find_file_by_name(job->paths.local_root, "globals.xml", job->paths.local_globals, sizeof(job->paths.local_globals));

    ui.download_success     = util_file_exists(job->paths.local_types);
    ui.download_ready       = true;
    ui.download_in_progress = false;
    return 0;
}
#endif

static void start_download_job() {
    if (ui.download_in_progress) return;
    memset(&download_job, 0, sizeof(download_job));
    strncpy(download_job.host, ui.ftp_host, sizeof(download_job.host) - 1);
    strncpy(download_job.user, ui.ftp_user, sizeof(download_job.user) - 1);
    strncpy(download_job.pass, ui.ftp_pass, sizeof(download_job.pass) - 1);
    download_job.port = atoi(ui.ftp_port);
    ui_load_server_paths(&download_job.paths);
    ui_ensure_dir(download_job.paths.local_root);

    ui.download_in_progress = true;
    ui.download_ready       = false;
    ui.download_success     = false;
    snprintf(ui.download_status, sizeof(ui.download_status), "Downloading...");
#ifdef _WIN32
    download_thread = CreateThread(NULL, 0, download_thread_proc, &download_job, 0, NULL);
#endif
}

// ============================================================================
// PIPELINE THREAD — runs audit + swarm pipelines sequentially in background
// ============================================================================
#ifdef _WIN32
static DWORD WINAPI pipeline_thread_proc(LPVOID param) {
    PipelineJob *job = (PipelineJob *)param;
    AuditorContext *ctx = job->ctx;

    // Phase 0: Backup & clean output directory (prevents stale files from prior runs)
    snprintf(ui.pipeline_phase, sizeof(ui.pipeline_phase), "Backing up & cleaning output...");
    snprintf(ctx->status_message, 255, "Pipeline: Cleaning output directory...");
    util_log(SEVERITY_INFO, "Pipeline Thread: Backing up and cleaning output directory...");
    util_backup_and_clean_output();

    // NOTE: auditor_run_audit_pipeline() is intentionally NOT called here.
    // The swarm pipeline (below) performs every step the monolithic pipeline
    // does — sort, dedup, rename, gaps, fill, audit, pricing, stitch, export —
    // as individual tracked tasks.  Running both causes double-processing on
    // the same ctx->items[] which can break sort order and leave items in an
    // inconsistent state for the swarm's export pass.

    // Phase 2: Swarm Pipeline
    snprintf(ui.pipeline_phase, sizeof(ui.pipeline_phase), "Running Swarm Pipeline...");
    snprintf(ctx->status_message, 255, "Pipeline: Running Swarm Pipeline...");
    util_log(SEVERITY_INFO, "Pipeline Thread: Starting Swarm Pipeline...");
    util_ensure_directory("output");
    swarm_plan(ctx);
    swarm_execute(ctx);
    util_log(SEVERITY_INFO, "Pipeline Thread: Swarm Pipeline complete.");

    // Phase 3: Quarantine any stale files from prior runs
    snprintf(ui.pipeline_phase, sizeof(ui.pipeline_phase), "Quarantining stale files...");
    {
        int quarantined = util_quarantine_stale_output(ctx->shop_mod);
        if (quarantined > 0)
            util_log(SEVERITY_WARNING, "Pipeline Thread: Quarantined %d stale file(s) from output/", quarantined);
    }

    // Phase 3b: Pre-upload validation with auto-remediation loop.
    // If remediable issues are found (duplicates, Tier5+), re-exports affected
    // files and re-validates.  Only blocks on non-recoverable corruption.
    snprintf(ui.pipeline_phase, sizeof(ui.pipeline_phase), "Validating output files...");
    snprintf(ctx->status_message, 255, "Pipeline: Validating output files...");
    {
        int passed = 0, failed = 0;
        bool valid = auditor_remediate_and_validate(ctx, &passed, &failed);
        if (!valid) {
            util_log(SEVERITY_ERROR, "Pipeline Thread: Validation FAILED after remediation — %d file(s) still corrupt. Upload blocked.", failed);
            snprintf(ui.pipeline_phase, sizeof(ui.pipeline_phase), "Validation FAILED — %d file(s) corrupt", failed);
            snprintf(ctx->status_message, 255, "BLOCKED: %d output file(s) failed validation after remediation. Check logs.", failed);
            ui.pipeline_success     = false;
            ui.pipeline_ready       = true;
            ui.pipeline_in_progress = false;
            return 1;
        }
        util_log(SEVERITY_INFO, "Pipeline Thread: Pre-upload validation passed — %d file(s) OK.", passed);
    }

    // Signal completion
    snprintf(ui.pipeline_phase, sizeof(ui.pipeline_phase), "Pipeline complete.");
    ui.pipeline_success     = true;
    ui.pipeline_ready       = true;
    ui.pipeline_in_progress = false;
    return 0;
}
#endif

static void start_pipeline_job(AuditorContext *ctx) {
    if (ui.pipeline_in_progress || ui.download_in_progress) return;
    memset(&pipeline_job, 0, sizeof(pipeline_job));
    pipeline_job.ctx = ctx;

    ui.pipeline_in_progress = true;
    ui.pipeline_ready       = false;
    ui.pipeline_success     = false;
    snprintf(ui.pipeline_phase, sizeof(ui.pipeline_phase), "Initializing pipeline...");
    snprintf(ctx->status_message, 255, "Pipeline: Starting...");
#ifdef _WIN32
    pipeline_thread = CreateThread(NULL, 0, pipeline_thread_proc, &pipeline_job, 0, NULL);
#endif
}

// ============================================================================
// FILE BROWSER THREAD (async directory listing)
// ============================================================================
struct BrowseJob {
    char host[128];
    char user[128];
    char pass[128];
    int  port;
    char remote_path[MAX_PATH_LEN];
    AuditorContext *ctx;
};

static BrowseJob browse_job;
#ifdef _WIN32
static HANDLE browse_thread = NULL;

static DWORD WINAPI browse_thread_proc(LPVOID param) {
    BrowseJob *job = (BrowseJob *)param;
    RemoteFileBrowser *browser = &job->ctx->browser;
    ftp_list_directory(job->host, job->port, job->user, job->pass,
                       job->remote_path, browser);
    browser->loading = false;
    browser->ready   = true;
    return 0;
}
#endif

static void start_browse_job(AuditorContext *ctx, const char *remote_path) {
    if (ctx->browser.loading) return;

    // Clean up previous thread
#ifdef _WIN32
    if (browse_thread) {
        WaitForSingleObject(browse_thread, 0);
        CloseHandle(browse_thread);
        browse_thread = NULL;
    }
#endif

    memset(&browse_job, 0, sizeof(browse_job));
    strncpy(browse_job.host, ui.ftp_host, sizeof(browse_job.host) - 1);
    strncpy(browse_job.user, ui.ftp_user, sizeof(browse_job.user) - 1);
    strncpy(browse_job.pass, ui.ftp_pass, sizeof(browse_job.pass) - 1);
    browse_job.port = atoi(ui.ftp_port);
    strncpy(browse_job.remote_path, remote_path, sizeof(browse_job.remote_path) - 1);
    browse_job.ctx = ctx;

    ctx->browser.loading  = true;
    ctx->browser.ready    = false;
    ctx->browser.selected = -1;

#ifdef _WIN32
    browse_thread = CreateThread(NULL, 0, browse_thread_proc, &browse_job, 0, NULL);
#endif
}

// ============================================================================
// FILE DIALOG
// ============================================================================
#ifdef _WIN32
static int open_file_dialog_multi(AuditorContext *ctx) {
    char fileBuffer[8192] = {};
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile   = fileBuffer;
    ofn.nMaxFile    = sizeof(fileBuffer);
    ofn.lpstrFilter = "XML Files (*.xml)\0*.xml\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) {
        int count = 0;
        char *p = fileBuffer;
        if (fileBuffer[strlen(fileBuffer) + 1] == '\0') {
            if (ui.loaded_file_count < MAX_LOADED_FILES) {
                strncpy(ui.loaded_files[ui.loaded_file_count], fileBuffer, MAX_PATH_LEN - 1);
                ui.loaded_file_count++;
                parser_load_types_xml(ctx, fileBuffer);
                count = 1;
            }
        } else {
            char directory[MAX_PATH_LEN];
            strncpy(directory, p, MAX_PATH_LEN - 1);
            p += strlen(p) + 1;
            while (*p && ui.loaded_file_count < MAX_LOADED_FILES) {
                char fullPath[MAX_PATH_LEN];
                snprintf(fullPath, sizeof(fullPath), "%s\\%s", directory, p);
                strncpy(ui.loaded_files[ui.loaded_file_count], fullPath, MAX_PATH_LEN - 1);
                ui.loaded_file_count++;
                parser_load_types_xml(ctx, fullPath);
                count++;
                p += strlen(p) + 1;
            }
        }
        snprintf(ctx->status_message, 255, "Loaded %d files.", count);
        return count;
    }
    return 0;
}
#endif

// ============================================================================
// LOADING HELPERS (reset + load + snapshot)
// ============================================================================
static void do_full_load(AuditorContext *ctx, const char *root) {
    ctx->item_count       = 0;
    ctx->spawn_block_count = 0;
    ctx->data_loaded      = false;
    ctx->dedup_skipped    = 0;
    ui.loaded_file_count  = 0;

    // Initialize inline dedup hash map — pre-allocates ~512KB, stays for the
    // lifetime of the context.  Cleared (not freed) on each full load so the
    // allocation is reused.
    classname_map_init(ctx);

    // Clear file fingerprint cache for this full load — since we reset item_count
    // to 0, cached "skip" decisions are invalid.  The cache will rebuild as each
    // file is parsed.  (Pruning still removes deleted files before the rebuild.)
    if (!ctx->file_cache.loaded) {
        file_cache_load(&ctx->file_cache);
    }
    file_cache_prune(&ctx->file_cache);
    // Don't skip any files on a forced full load — clear the cache entries.
    // They will be repopulated as the parser processes each file.
    ctx->file_cache.count = 0;

    // Detect active map from REMOTE_TYPES to skip inactive mpmission directories.
    // This prevents loading vanilla types.xml 3x (chernarus + enoch + sakhal).
    active_mpmission[0] = '\0';
    char remote_types_path[256] = {};
    if (util_read_ini_value("config/server_paths.ini", "REMOTE_TYPES", remote_types_path, sizeof(remote_types_path))) {
        if (strstr(remote_types_path, "chernarusplus")) strncpy(active_mpmission, "dayzoffline.chernarusplus", sizeof(active_mpmission) - 1);
        else if (strstr(remote_types_path, "enoch"))    strncpy(active_mpmission, "dayzoffline.enoch", sizeof(active_mpmission) - 1);
        else if (strstr(remote_types_path, "sakhal"))   strncpy(active_mpmission, "dayzoffline.sakhal", sizeof(active_mpmission) - 1);
    }
    if (active_mpmission[0])
        util_log(SEVERITY_INFO, "Active map: %s (skipping inactive map data)", active_mpmission);

    util_ensure_directory("output");
    recursive_load_directory(ctx, root, true);  // force=true: bypass rescan check on full load

    // Save updated file cache after loading
    file_cache_save(&ctx->file_cache);

    // Log dedup stats
    if (ctx->dedup_skipped > 0)
        util_log(SEVERITY_INFO, "Inline dedup: %d duplicate item definitions merged during parse (saved %d array slots)",
                 ctx->dedup_skipped, ctx->dedup_skipped);

    // Build file index for stitching/swarm
    file_index_scan(&ctx->file_index, root);
    file_index_classify(&ctx->file_index);

    // Snapshot originals for diff
    auditor_snapshot_originals(ctx);

    util_log(SEVERITY_INFO, "UI: Loaded %d unique items, %d files indexed. Snapshot saved.",
             ctx->item_count, ctx->file_index.count);
    snprintf(ctx->status_message, 255, "Loaded %d unique items from %d files. Ready.", ctx->item_count, ui.loaded_file_count);
}

// ============================================================================
// IMGUI THEME (Dark + Amber/Gold accents)
// ============================================================================
static void setup_theme() {
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding    = 4.0f;
    style.FrameRounding     = 3.0f;
    style.GrabRounding      = 2.0f;
    style.TabRounding       = 3.0f;
    style.ScrollbarRounding = 3.0f;
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 0.0f;
    style.PopupBorderSize   = 1.0f;
    style.WindowPadding     = ImVec2(10, 10);
    style.FramePadding      = ImVec2(8, 5);
    style.ItemSpacing       = ImVec2(8, 6);
    style.ItemInnerSpacing  = ImVec2(6, 4);
    style.IndentSpacing     = 20.0f;
    style.ScrollbarSize     = 14.0f;

    ImVec4 *c = style.Colors;
    // Backgrounds
    c[ImGuiCol_WindowBg]           = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_ChildBg]            = ImVec4(0.11f, 0.11f, 0.13f, 1.00f);
    c[ImGuiCol_PopupBg]            = ImVec4(0.11f, 0.11f, 0.14f, 0.96f);
    c[ImGuiCol_Border]             = ImVec4(0.28f, 0.28f, 0.32f, 0.50f);
    // Frames
    c[ImGuiCol_FrameBg]            = ImVec4(0.15f, 0.15f, 0.19f, 1.00f);
    c[ImGuiCol_FrameBgHovered]     = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_FrameBgActive]      = ImVec4(0.28f, 0.28f, 0.36f, 1.00f);
    // Title
    c[ImGuiCol_TitleBg]            = ImVec4(0.07f, 0.07f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]      = ImVec4(0.12f, 0.12f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.07f, 0.07f, 0.09f, 0.75f);
    c[ImGuiCol_MenuBarBg]          = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    // Headers
    c[ImGuiCol_Header]             = ImVec4(0.20f, 0.20f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderHovered]      = ImVec4(1.00f, 0.78f, 0.00f, 0.25f);
    c[ImGuiCol_HeaderActive]       = ImVec4(1.00f, 0.78f, 0.00f, 0.40f);
    // Tabs
    c[ImGuiCol_Tab]                = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);
    c[ImGuiCol_TabHovered]         = ImVec4(1.00f, 0.78f, 0.00f, 0.35f);
    c[ImGuiCol_TabActive]          = ImVec4(0.22f, 0.20f, 0.14f, 1.00f);
    c[ImGuiCol_TabUnfocused]       = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.18f, 0.17f, 0.14f, 1.00f);
    // Buttons
    c[ImGuiCol_Button]             = ImVec4(0.18f, 0.18f, 0.24f, 1.00f);
    c[ImGuiCol_ButtonHovered]      = ImVec4(1.00f, 0.78f, 0.00f, 0.35f);
    c[ImGuiCol_ButtonActive]       = ImVec4(1.00f, 0.78f, 0.00f, 0.55f);
    // Text
    c[ImGuiCol_Text]               = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]       = ImVec4(0.45f, 0.45f, 0.48f, 1.00f);
    // Separators
    c[ImGuiCol_Separator]          = ImVec4(0.28f, 0.28f, 0.32f, 0.50f);
    c[ImGuiCol_SeparatorHovered]   = ImVec4(1.00f, 0.78f, 0.00f, 0.50f);
    c[ImGuiCol_SeparatorActive]    = ImVec4(1.00f, 0.78f, 0.00f, 0.80f);
    // Misc
    c[ImGuiCol_CheckMark]          = ImVec4(1.00f, 0.78f, 0.00f, 1.00f);
    c[ImGuiCol_SliderGrab]         = ImVec4(1.00f, 0.78f, 0.00f, 0.50f);
    c[ImGuiCol_SliderGrabActive]   = ImVec4(1.00f, 0.78f, 0.00f, 0.80f);
    // Scrollbar
    c[ImGuiCol_ScrollbarBg]        = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
    c[ImGuiCol_ScrollbarGrab]      = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.38f, 0.42f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(1.00f, 0.78f, 0.00f, 0.50f);
    // Tables
    c[ImGuiCol_TableHeaderBg]      = ImVec4(0.13f, 0.13f, 0.17f, 1.00f);
    c[ImGuiCol_TableBorderStrong]  = ImVec4(0.28f, 0.28f, 0.32f, 1.00f);
    c[ImGuiCol_TableBorderLight]   = ImVec4(0.20f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_TableRowBg]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]      = ImVec4(1.00f, 1.00f, 1.00f, 0.025f);
    // Resize grip
    c[ImGuiCol_ResizeGrip]         = ImVec4(1.00f, 0.78f, 0.00f, 0.15f);
    c[ImGuiCol_ResizeGripHovered]  = ImVec4(1.00f, 0.78f, 0.00f, 0.40f);
    c[ImGuiCol_ResizeGripActive]   = ImVec4(1.00f, 0.78f, 0.00f, 0.70f);
}

// ============================================================================
// COLOR HELPERS
// ============================================================================
static const ImVec4 COL_AMBER  = ImVec4(1.00f, 0.78f, 0.00f, 1.00f);
static const ImVec4 COL_GREEN  = ImVec4(0.40f, 0.90f, 0.40f, 1.00f);
static const ImVec4 COL_RED    = ImVec4(0.95f, 0.35f, 0.35f, 1.00f);
static const ImVec4 COL_YELLOW = ImVec4(0.95f, 0.85f, 0.25f, 1.00f);
static const ImVec4 COL_PURPLE = ImVec4(0.70f, 0.40f, 0.90f, 1.00f);
static const ImVec4 COL_DIM    = ImVec4(0.50f, 0.50f, 0.53f, 1.00f);
static const ImVec4 COL_WHITE  = ImVec4(0.92f, 0.92f, 0.94f, 1.00f);

static ImVec4 tier_color(int tier) {
    switch (tier) {
        case 1: return COL_GREEN;
        case 2: return COL_YELLOW;
        case 3: return COL_RED;
        case 4: return COL_PURPLE;
        default: return COL_DIM;
    }
}

// ============================================================================
// WINSCP ERROR NOTIFICATION HELPERS
// ============================================================================

// Push a new error notification to the ring buffer.
static void push_winscp_notification(const char *title, const char *message) {
    // Find an inactive slot, or evict the oldest
    int slot = -1;
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!g_notifications[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        // Evict oldest (lowest birth_time)
        float oldest = 1e30f;
        for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
            if (g_notifications[i].birth_time < oldest) {
                oldest = g_notifications[i].birth_time;
                slot = i;
            }
        }
    }
    if (slot < 0) slot = 0;

    WinSCPNotification *n = &g_notifications[slot];
    memset(n, 0, sizeof(WinSCPNotification));
    strncpy(n->title, title, sizeof(n->title) - 1);
    strncpy(n->message, message, sizeof(n->message) - 1);
    n->birth_time = (float)GetTime();
    n->active = true;
    n->copied = false;
    n->copied_timer = 0.0f;
    g_notification_count++;

    util_log(SEVERITY_ERROR, "WinSCP Notification: [%s] %s", title, message);
}

// Parse .TEMP/winscp.log (or winscp_ls.log) for connection / authentication errors.
// Returns a human-readable error extracted from the log, or a generic fallback.
static void parse_winscp_log_errors(const char *log_path, char *out, size_t out_len) {
    out[0] = '\0';
    FILE *f = fopen(log_path, "r");
    if (!f) {
        snprintf(out, out_len, "WinSCP operation failed. No log file found at %s", log_path);
        return;
    }

    // Known WinSCP error patterns to extract
    // Lines containing these indicate specific error conditions
    static const char *error_patterns[] = {
        "Connection refused",
        "Connection timed out",
        "Connection has been unexpectedly closed",
        "Network error",
        "Host not found",
        "No connection",
        "Authentication failed",
        "Access denied",
        "Permission denied",
        "Login failed",
        "Wrong password",
        "Host key wasn't verified",
        "Server refused our key",
        "Timeout detected",
        "Could not retrieve directory listing",
        "No such file or directory",
        "Error transferring",
        "Cannot create remote file",
        "General failure",
        "Server sent disconnect message",
        "Received too large",
        "Cannot initialize SFTP protocol",
        NULL
    };

    char line[2048];
    char collected[2048] = {};
    int collected_len = 0;
    int error_count = 0;

    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        // Check each error pattern
        for (int i = 0; error_patterns[i]; i++) {
            if (strstr(line, error_patterns[i])) {
                // Extract the meaningful portion — skip WinSCP log timestamp prefix
                const char *msg = line;
                // WinSCP log lines start with ". YYYY-MM-DD..." or "> YYYY-MM-DD..."
                // Skip the timestamp to get the actual message
                const char *bracket = strchr(line, ']');
                if (bracket) msg = bracket + 1;
                while (*msg == ' ') msg++;

                if (error_count > 0 && collected_len < (int)sizeof(collected) - 2) {
                    collected[collected_len++] = '\n';
                }
                int remaining = (int)sizeof(collected) - collected_len - 1;
                if (remaining > 0) {
                    int written = snprintf(collected + collected_len, remaining, "%s", msg);
                    if (written > 0) collected_len += (written < remaining) ? written : remaining;
                }
                error_count++;
                if (error_count >= 5) break;  // Limit to 5 error lines
            }
        }
        if (error_count >= 5) break;
    }

    fclose(f);

    if (collected[0]) {
        snprintf(out, out_len, "%s", collected);
    } else {
        snprintf(out, out_len, "WinSCP operation failed. Check %s for details.", log_path);
    }
}

// Push a notification by parsing the WinSCP log file for error details.
static void push_winscp_error_from_log(const char *title, const char *log_path) {
    char error_msg[2048];
    parse_winscp_log_errors(log_path, error_msg, sizeof(error_msg));
    push_winscp_notification(title, error_msg);
}

// Draw all active notifications as floating toast windows in the top-right corner.
static void draw_notifications() {
    float current_time = (float)GetTime();
    float screen_w = (float)GetScreenWidth();
    float y_offset = NOTIFICATION_PADDING;

    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        WinSCPNotification *n = &g_notifications[i];
        if (!n->active) continue;

        // Auto-dismiss after lifetime
        float age = current_time - n->birth_time;
        if (age > NOTIFICATION_LIFETIME) {
            n->active = false;
            g_notification_count--;
            continue;
        }

        // Fade out in the last 2 seconds
        float alpha = 1.0f;
        if (age > NOTIFICATION_LIFETIME - 2.0f)
            alpha = (NOTIFICATION_LIFETIME - age) / 2.0f;
        if (alpha < 0.0f) alpha = 0.0f;

        // Position: top-right, stacked vertically
        float x_pos = screen_w - NOTIFICATION_WIDTH - NOTIFICATION_PADDING;
        ImGui::SetNextWindowPos(ImVec2(x_pos, y_offset), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(NOTIFICATION_WIDTH, 0), ImGuiCond_Always);  // auto-height

        // Style: dark red-tinted background for errors
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.18f, 0.06f, 0.06f, 0.95f * alpha));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.20f, 0.20f, 0.60f * alpha));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.90f, 0.90f, alpha));

        char win_id[64];
        snprintf(win_id, sizeof(win_id), "##WinSCPNotif_%d", i);

        ImGui::Begin(win_id, nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav);

        // Header row: icon + title + Close button
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.30f, 0.30f, alpha));
        ImGui::Text("!! WinSCP Error");
        ImGui::PopStyleColor();

        ImGui::SameLine(NOTIFICATION_WIDTH - 56);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 0.3f * alpha));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.8f, 0.2f, 0.2f, 0.5f * alpha));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.30f, 0.30f, alpha));
        char close_id[32];
        snprintf(close_id, sizeof(close_id), "X##close_%d", i);
        if (ImGui::SmallButton(close_id)) {
            n->active = false;
            g_notification_count--;
        }
        ImGui::PopStyleColor(4);

        // Title
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.30f, alpha));
        ImGui::Text("%s", n->title);
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Error message body (selectable = copyable)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, alpha));
        ImGui::TextWrapped("%s", n->message);
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Copy button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.12f, 0.12f, 0.8f * alpha));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.15f, 0.15f, 0.9f * alpha));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.50f, 0.20f, 0.20f, 1.0f * alpha));
        char copy_id[32];
        snprintf(copy_id, sizeof(copy_id), "Copy##copy_%d", i);

        // Update copied feedback timer
        if (n->copied && n->copied_timer > 0.0f) {
            n->copied_timer -= GetFrameTime();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.90f, 0.40f, alpha));
            ImGui::SmallButton("Copied!");
            ImGui::PopStyleColor();
        } else {
            n->copied = false;
            if (ImGui::SmallButton(copy_id)) {
                // Build clipboard text: title + message
                char clipboard[2200];
                snprintf(clipboard, sizeof(clipboard), "[%s]\n%s", n->title, n->message);
                ImGui::SetClipboardText(clipboard);
                n->copied = true;
                n->copied_timer = 1.5f;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy error details to clipboard");
        ImGui::PopStyleColor(3);

        // Track this window's height for stacking
        float win_h = ImGui::GetWindowSize().y;
        ImGui::End();

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);

        y_offset += win_h + NOTIFICATION_PADDING;
    }
}

// ============================================================================
// SPAWNABLE BLOCK LOOKUP
// ============================================================================
static const SpawnableBlock* find_spawnable_block(AuditorContext *ctx, const char *classname) {
    if (!ctx || !classname || !classname[0]) return nullptr;
    for (int i = 0; i < ctx->spawn_block_count; i++) {
        if (util_strcasecmp(ctx->spawn_blocks[i].classname, classname) == 0)
            return &ctx->spawn_blocks[i];
    }
    return nullptr;
}

// ============================================================================
// SORT COMPARATOR FOR ITEMS TABLE
// ============================================================================
struct SortContext {
    AuditorContext *ctx;
    ImGuiSortDirection direction;
    int column_id;
};

static SortContext g_sort_ctx;

static int compare_items(const void *a, const void *b) {
    int idx_a = *(const int *)a;
    int idx_b = *(const int *)b;
    const LootItem *ia = &g_sort_ctx.ctx->items[idx_a];
    const LootItem *ib = &g_sort_ctx.ctx->items[idx_b];
    int result = 0;

    switch (g_sort_ctx.column_id) {
        case 0:  result = util_strcasecmp(ia->classname, ib->classname); break;
        case 1:  result = ia->nominal - ib->nominal; break;
        case 2:  result = ia->min - ib->min; break;
        case 3:  result = ia->lifetime - ib->lifetime; break;
        case 4:  result = ia->restock - ib->restock; break;
        case 5:  result = ia->assigned_tier - ib->assigned_tier; break;
        case 6:  result = ia->buy_price - ib->buy_price; break;
        case 7:  result = ia->sell_price - ib->sell_price; break;
        case 8:  result = util_strcasecmp(ia->mod_name, ib->mod_name); break;
        case 9:  result = util_strcasecmp(ia->mod_source, ib->mod_source); break;
        case 10: {
            // Sort order: Debug < Deleted < Renamed < Modified < OK
            auto status_rank = [](const LootItem *item) -> int {
                if (item->is_debug_item) return 0;
                if (item->deleted) return 1;
                if (item->renamed) return 2;
                if (item->modified) return 3;
                return 4;
            };
            result = status_rank(ia) - status_rank(ib);
            break;
        }
        default: result = 0; break;
    }

    if (g_sort_ctx.direction == ImGuiSortDirection_Descending)
        result = -result;
    return result;
}

// ============================================================================
// SECTION: SIDEBAR (Actions + Item Inspector)
// ============================================================================
static void draw_sidebar(AuditorContext *ctx) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
    float bw = ImGui::GetContentRegionAvail().x;  // full button width

    // ---- Server Connection (compact) ----
    if (ImGui::CollapsingHeader("Server Connection", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushItemWidth(bw - 60);
        ImGui::InputText("Host", ui.ftp_host, sizeof(ui.ftp_host));
        ImGui::InputText("Port", ui.ftp_port, sizeof(ui.ftp_port));
        ImGui::InputText("User", ui.ftp_user, sizeof(ui.ftp_user));
        ImGui::InputText("Pass", ui.ftp_pass, sizeof(ui.ftp_pass), ImGuiInputTextFlags_Password);
        ImGui::PopItemWidth();

        if (ui.download_in_progress) {
            ImGui::BeginDisabled();
            ImGui::Button("Downloading...", ImVec2(bw, 30));
            ImGui::EndDisabled();
        } else {
            if (ImGui::Button("Download Server Files", ImVec2(bw, 30))) {
                save_ftp_config();
                start_download_job();
            }
        }
    }

    ImGui::Spacing();

    // ---- Actions ----
    if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Load Files (Manual)...", ImVec2(bw, 28))) {
#ifdef _WIN32
            open_file_dialog_multi(ctx);
            if (ctx->item_count > 0 && !ctx->data_loaded) {
                auditor_snapshot_originals(ctx);
            }
#endif
        }

        if (ImGui::Button("Load Scanned Data", ImVec2(bw, 28))) {
            do_full_load(ctx, download_job.paths.local_root[0] ? download_job.paths.local_root : "downloaded_mods");
        }

        ImGui::Separator();

        bool has_items = ctx->item_count > 0;
        bool pipeline_busy = ui.pipeline_in_progress;
        if (!has_items) ImGui::BeginDisabled();

        if (ui.pipeline_in_progress) {
            // Show animated progress button while pipeline is running
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.20f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.20f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20f, 0.20f, 0.10f, 1.0f));
            ImGui::Button(ui.pipeline_phase, ImVec2(bw, 32));
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.50f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.45f, 0.34f, 0.08f, 1.0f));
            if (ImGui::Button("Run Pipeline", ImVec2(bw, 32))) {
                start_pipeline_job(ctx);
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("All-in-one: Sort -> Index -> Parse -> Audit ->\n"
                                  "Stitch -> Export (types.xml, spawnables, trader,\n"
                                  "economy, zombie/animal configs, reports) -> Verify.\n"
                                  "All output files ready for upload in /output.");
            }
        }

        ImGui::Spacing();

        // ---- Shop/Trader Mod Selection ----
        ImGui::TextColored(COL_AMBER, "Shop Mod");
        ImGui::Separator();
        {
            const char *shop_mod_items[] = { "TraderPlus (JSON)", "Dr. Jones Trader (TXT)", "Expansion Market (JSON)" };
            ImGui::PushItemWidth(bw);
            if (ImGui::Combo("##ShopMod", &ui.shop_mod_index, shop_mod_items, SHOP_MOD_COUNT)) {
                ctx->shop_mod = (ShopMod)ui.shop_mod_index;
                util_log(SEVERITY_INFO, "Shop mod changed to: %s", shop_mod_name(ctx->shop_mod));
            }
            ImGui::PopItemWidth();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Select which trader mod format to export.\n"
                              "TraderPlus: JSON format (TraderPlusTrading.json)\n"
                              "Dr. Jones: TXT format (TraderConfig.txt)\n"
                              "Expansion: JSON format (future)");
        }

        ImGui::Spacing();

        bool upload_busy = upload_daemon_is_running();
        if (pipeline_busy || upload_busy) ImGui::BeginDisabled();
        if (ImGui::Button("Save & Upload to Server", ImVec2(bw, 32))) {
            save_ftp_config();

            // Build the daemon job from current UI state
            UploadDaemonJob job;
            memset(&job, 0, sizeof(job));
            job.ctx = ctx;
            job.shop_mod = (ShopMod)ui.shop_mod_index;
            strncpy(job.host, ui.ftp_host, sizeof(job.host) - 1);
            strncpy(job.user, ui.ftp_user, sizeof(job.user) - 1);
            strncpy(job.pass, ui.ftp_pass, sizeof(job.pass) - 1);
            job.port = atoi(ui.ftp_port);

            ServerPaths sp;
            ui_load_server_paths(&sp);
            strncpy(job.remote_root, sp.remote_root, sizeof(job.remote_root) - 1);
            strncpy(job.remote_types, sp.remote_types, sizeof(job.remote_types) - 1);
            strncpy(job.remote_trader, sp.remote_trader, sizeof(job.remote_trader) - 1);

            if (upload_daemon_start(&job)) {
                snprintf(ctx->status_message, 255, "Save & Upload started (background)...");
            } else {
                snprintf(ctx->status_message, 255, "Failed to start upload daemon.");
            }
        }
        if (pipeline_busy || upload_busy) ImGui::EndDisabled();

        // ---- Upload Daemon Progress HUD ----
        if (upload_busy) {
            const UploadDaemonState *uds = upload_daemon_state();
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.10f, 0.80f));
            ImGui::BeginChild("##UploadProgress", ImVec2(bw, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);

            // Phase label
            ImGui::TextColored(COL_AMBER, "%s", uds->phase_label);

            // Current file
            if (uds->current_file[0])
                ImGui::TextColored(COL_DIM, "  %s", uds->current_file);

            // Progress bar during upload phase
            if (uds->phase == UPLOAD_PHASE_UPLOADING && uds->files_total > 0) {
                char overlay[64];
                snprintf(overlay, sizeof(overlay), "%d / %d", uds->files_uploaded, uds->files_total);
                float pct = (float)uds->files_uploaded / (float)uds->files_total;
                ImGui::ProgressBar(pct, ImVec2(bw - 16, 0), overlay);
                if (uds->files_failed > 0)
                    ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "%d failed", uds->files_failed);
            }

            // Cancel button
            if (ImGui::Button("Cancel Upload", ImVec2(bw - 16, 24)))
                upload_daemon_cancel();

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ---- Poll daemon completion (once per frame after it finishes) ----
        {
            static bool upload_result_consumed = false;
            const UploadDaemonState *uds = upload_daemon_state();
            if (uds->finished && uds->result_message[0] && !upload_busy) {
                // One-shot: copy result to status bar and fire error notifications
                if (!upload_result_consumed) {
                    upload_result_consumed = true;
                    snprintf(ctx->status_message, 255, "%s", uds->result_message);
                    if (uds->result == UPLOAD_RESULT_PARTIAL)
                        push_winscp_error_from_log("Upload Partially Failed", ".TEMP/winscp.log");
                    else if (uds->result == UPLOAD_RESULT_FAILED)
                        push_winscp_error_from_log("Upload Failed", ".TEMP/winscp.log");
                }
            } else if (uds->running) {
                // Daemon is active — reset so we fire the one-shot on next completion
                upload_result_consumed = false;
            }
        }

        if (!has_items) ImGui::EndDisabled();  // closes the outer has_items guard

        // ---- Restore Points (always available, even without loaded items) ----
        ImGui::Spacing();
        ImGui::TextColored(COL_AMBER, "Recovery");
        ImGui::Separator();

        // Refresh restore point list periodically or on first frame
        static RestorePointInfo restore_points[MAX_RESTORE_POINTS];
        static int restore_count = 0;
        static int selected_restore = 0;
        static bool restore_list_dirty = true;
        static float restore_refresh_timer = 0.0f;

        restore_refresh_timer += ImGui::GetIO().DeltaTime;
        if (restore_list_dirty || restore_refresh_timer > 5.0f) {
            restore_count = ftp_list_restore_points(restore_points, MAX_RESTORE_POINTS);
            if (selected_restore >= restore_count) selected_restore = 0;
            restore_list_dirty = false;
            restore_refresh_timer = 0.0f;
        }

        if (restore_count == 0) {
            ImGui::TextColored(COL_DIM, "No restore points available.");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Restore points are created automatically\nbefore each Save & Upload.");
        } else {
            ImGui::TextColored(COL_DIM, "%d restore point(s)", restore_count);

            // Combo box listing all restore points (newest first)
            if (ImGui::BeginCombo("##restore_combo",
                                  (selected_restore < restore_count) ? restore_points[selected_restore].label : "Select...",
                                  ImGuiComboFlags_None)) {
                for (int i = 0; i < restore_count; i++) {
                    bool is_selected = (selected_restore == i);
                    if (ImGui::Selectable(restore_points[i].label, is_selected))
                        selected_restore = i;
                    if (is_selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            if (ImGui::Button("Restore Selected", ImVec2(bw, 28))) {
                save_ftp_config();
                int port = atoi(ui.ftp_port);
                const char *manifest = restore_points[selected_restore].manifest;
                if (ftp_restore_from_manifest(ui.ftp_host, port, ui.ftp_user, ui.ftp_pass, manifest)) {
                    snprintf(ctx->status_message, 255, "Restore complete — server reverted to '%s'.", restore_points[selected_restore].label);
                } else {
                    snprintf(ctx->status_message, 255, "Restore failed. Check %s and log.", restore_points[selected_restore].dir);
                    push_winscp_error_from_log("Restore Failed", ".TEMP/winscp.log");
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Re-uploads the server files captured\nbefore the selected upload.\nManifest: %s", restore_points[selected_restore].manifest);
        }
    }

    ImGui::Spacing();

    // ---- File Index (organized by file type) ----
    if (ctx->file_index.count > 0) {
        if (ImGui::CollapsingHeader("File Index")) {
            // Count files by type
            int type_counts[9] = {};
            for (int i = 0; i < ctx->file_index.count; i++) {
                int ft = (int)ctx->file_index.entries[i].file_type;
                if (ft >= 0 && ft < 9) type_counts[ft]++;
            }

            ImGui::TextColored(COL_DIM, "%d files indexed", ctx->file_index.count);
            ImGui::Separator();

            // Show each file type as a collapsible tree
            FileType type_order[] = {
                FILE_TYPE_ECONOMY, FILE_TYPE_SPAWNABLE, FILE_TYPE_TERRITORY,
                FILE_TYPE_TRADER, FILE_TYPE_EVENTS, FILE_TYPE_GLOBALS,
                FILE_TYPE_RANDOMPRESETS, FILE_TYPE_CONFIG, FILE_TYPE_UNKNOWN
            };

            for (int t = 0; t < 9; t++) {
                FileType ft = type_order[t];
                int count = type_counts[static_cast<int>(ft)];
                if (count == 0) continue;

                char label[128];
                snprintf(label, sizeof(label), "%s (%d)###ftype_%d", file_type_name(ft), count, (int)ft);

                ImGui::PushStyleColor(ImGuiCol_Text, file_type_color(ft));
                bool open = ImGui::TreeNode(label);
                ImGui::PopStyleColor();

                if (open) {
                    for (int i = 0; i < ctx->file_index.count; i++) {
                        FileIndexEntry *e = &ctx->file_index.entries[i];
                        if (e->file_type != ft) continue;

                        ImGui::BulletText("%s", e->filename);
                        if (ImGui::IsItemHovered() && e->mod_name[0]) {
                            ImGui::SetTooltip("Mod: %s\nPath: %s", e->mod_name, e->filepath);
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }
    }

    ImGui::Spacing();

    // ---- Item Inspector ----
    if (ctx->selected_item >= 0 && ctx->selected_item < ctx->item_count) {
        LootItem *item = &ctx->items[ctx->selected_item];
        if (ImGui::CollapsingHeader("Item Inspector", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextColored(COL_AMBER, "%s", item->classname);
            ImGui::TextColored(COL_DIM, "Source: %s", item->mod_source[0] ? item->mod_source : "(unknown)");
            if (item->deleted) {
                ImGui::TextColored(COL_RED, "[OVERRIDDEN]");
            } else {
                ImGui::Spacing();

                // Load values on first select
                if (ui.editing_item != ctx->selected_item) {
                    ui.editing_item  = ctx->selected_item;
                    ui.edit_nominal  = item->nominal;
                    ui.edit_lifetime = item->lifetime;
                    ui.edit_restock  = item->restock;
                    ui.edit_min      = item->min;
                }

                ImGui::PushItemWidth(bw * 0.5f);
                if (ImGui::InputInt("Nominal",  &ui.edit_nominal))  item->nominal  = ui.edit_nominal;
                if (ImGui::InputInt("Lifetime", &ui.edit_lifetime)) item->lifetime = ui.edit_lifetime;
                if (ImGui::InputInt("Restock",  &ui.edit_restock))  item->restock  = ui.edit_restock;
                if (ImGui::InputInt("Min",      &ui.edit_min))      item->min      = ui.edit_min;
                ImGui::PopItemWidth();

                ImGui::Text("Tier: %d", item->assigned_tier);
                ImGui::Text("Category: %s", item->category[0] ? item->category : "(none)");
                ImGui::Text("Mod: %s", item->mod_name[0] ? item->mod_name : "(unknown)");

                // Show file presence — items can exist in multiple XML files
                {
                    const SpawnableBlock *insp_spawn = find_spawnable_block(ctx, item->classname);
                    if (insp_spawn) {
                        ImGui::TextColored(COL_GREEN, "File: %s", file_type_name(item->file_type));
                        ImGui::SameLine(0, 0);
                        ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), " + Spawnable");
                        if (ImGui::IsItemHovered()) {
                            ImGui::BeginTooltip();
                            ImGui::TextColored(COL_AMBER, "This item exists in TWO file systems:");
                            ImGui::Spacing();
                            ImGui::TextColored(COL_GREEN, "1. types.xml (%s)", item->mod_source);
                            ImGui::TextColored(COL_DIM, "   Controls: nominal, min, lifetime, restock,");
                            ImGui::TextColored(COL_DIM, "   category, usage zones, value tiers, flags");
                            ImGui::TextColored(COL_DIM, "   = HOW MANY spawn and WHERE on the map");
                            ImGui::Spacing();
                            ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "2. cfgspawnabletypes.xml (%s)", insp_spawn->source_file);
                            ImGui::TextColored(COL_DIM, "   Controls: cargo presets, attachment configs,");
                            ImGui::TextColored(COL_DIM, "   what items/attachments spawn ON this item");
                            ImGui::TextColored(COL_DIM, "   = WHAT IT SPAWNS WITH (mags, ammo, gear)");
                            ImGui::Spacing();
                            ImGui::TextColored(COL_YELLOW, "These are COMPLEMENTARY, not competing.");
                            ImGui::TextColored(COL_DIM, "Both reference the same classname. If you delete");
                            ImGui::TextColored(COL_DIM, "an item from types.xml, its spawnable entry is");
                            ImGui::TextColored(COL_DIM, "orphaned. If you add a new item to types.xml,");
                            ImGui::TextColored(COL_DIM, "consider adding a spawnable entry too.");
                            ImGui::EndTooltip();
                        }
                    } else {
                        ImGui::Text("File Type: %s", file_type_name(item->file_type));
                    }
                }

                // ---- Trader Pricing ----
                if (item->buy_price > 0 || item->sell_price > 0 || item->trader_cat[0]) {
                    ImGui::Spacing();
                    ImGui::Separator();
                    ImGui::TextColored(COL_AMBER, "Trader");

                    if (item->trader_cat[0])
                        ImGui::Text("Category: %s", item->trader_cat);

                    if (item->buy_price > 0)
                        ImGui::TextColored(COL_GREEN, "Buy:  %d %s", item->buy_price, item->currency);
                    else if (item->buy_price == -1)
                        ImGui::TextColored(COL_RED, "Buy:  Not Buyable");

                    if (item->sell_price > 0)
                        ImGui::TextColored(COL_YELLOW, "Sell: %d %s", item->sell_price, item->currency);

                    if (item->black_market)
                        ImGui::TextColored(COL_PURPLE, "[Black Market]");
                    if (item->admin_only)
                        ImGui::TextColored(COL_RED, "[Admin Only]");

                    if (item->stock_override > 0)
                        ImGui::Text("Stock: %d  Restock: %ds", item->stock_override, item->restock_override);
                }

                ImGui::Spacing();
                ImGui::Separator();

                if (item->usage_count > 0) {
                    ImGui::Text("Usages:");
                    for (int i = 0; i < item->usage_count && i < 8; i++)
                        ImGui::BulletText("%s", item->usages[i]);
                }
                if (item->value_count > 0) {
                    ImGui::Text("Values:");
                    for (int i = 0; i < item->value_count && i < 8; i++)
                        ImGui::BulletText("%s", item->values[i]);
                }

                // ---- Spawnable Types XML (complementary to types.xml) ----
                {
                    const SpawnableBlock *spawn_insp = find_spawnable_block(ctx, item->classname);
                    if (spawn_insp) {
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "cfgspawnabletypes.xml");
                        ImGui::SameLine();
                        ImGui::TextColored(COL_DIM, "(spawn attachments/cargo)");
                        ImGui::TextColored(COL_DIM, "Source: %s", spawn_insp->source_file);
                        ImGui::Spacing();

                        // Context banner explaining the relationship
                        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.12f, 0.18f, 1.0f));
                        ImGui::BeginChild("SpawnContext", ImVec2(0, 36), true);
                        ImGui::TextColored(COL_YELLOW, "types.xml = quantity/location | spawnabletypes.xml = spawn loadout");
                        ImGui::TextColored(COL_DIM, "Both files use classname '%s' — they must stay in sync.", item->classname);
                        ImGui::EndChild();
                        ImGui::PopStyleColor();

                        ImGui::Spacing();
                        // Display the raw XML in a scrollable child
                        ImGui::BeginChild("SpawnableXML", ImVec2(0, 120), true, ImGuiWindowFlags_HorizontalScrollbar);
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.75f, 0.95f, 0.85f));
                        ImGui::TextUnformatted(spawn_insp->raw_xml);
                        ImGui::PopStyleColor();
                        ImGui::EndChild();
                    }
                }
            }

            if (ImGui::Button("Deselect", ImVec2(bw, 24))) {
                ctx->selected_item = -1;
                ui.editing_item = -1;
            }
        }
    }

    ImGui::PopStyleVar();
}

// ============================================================================
// TAB: ITEMS TABLE  
// ============================================================================
static void draw_items_tab(AuditorContext *ctx) {
    // Search bar
    ImGui::PushItemWidth(300);
    ImGui::InputTextWithHint("##Search", "Search items...", ui.search_buffer, sizeof(ui.search_buffer));
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) ui.search_buffer[0] = '\0';
    ImGui::SameLine();

    // Mod filter
    ImGui::SameLine();
    ImGui::PushItemWidth(180);
    ImGui::InputTextWithHint("##ModFilter", "Filter by mod...", ui.mod_filter, sizeof(ui.mod_filter));
    ImGui::PopItemWidth();

    // Hide deleted checkbox
    ImGui::SameLine();
    ImGui::Checkbox("Hide Deleted", &ui.hide_deleted);

    // Count visible items for display
    int visible_count = 0;
    for (int i = 0; i < ctx->item_count; i++) {
        if (!ctx->items[i].deleted) visible_count++;
    }
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, "  %d items (%d visible)", ctx->item_count, visible_count);

    ImGui::Separator();

    const ImGuiTableFlags table_flags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersV | ImGuiTableFlags_Hideable |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Reorderable;

    if (ImGui::BeginTable("ItemsTable", 12, table_flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Classname",    ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
        ImGui::TableSetupColumn("Nominal",      ImGuiTableColumnFlags_WidthFixed, 65.0f, 1);
        ImGui::TableSetupColumn("Min",          ImGuiTableColumnFlags_WidthFixed, 50.0f, 2);
        ImGui::TableSetupColumn("Lifetime",     ImGuiTableColumnFlags_WidthFixed, 75.0f, 3);
        ImGui::TableSetupColumn("Restock",      ImGuiTableColumnFlags_WidthFixed, 65.0f, 4);
        ImGui::TableSetupColumn("Tier",         ImGuiTableColumnFlags_WidthFixed, 45.0f, 5);
        ImGui::TableSetupColumn("Buy",          ImGuiTableColumnFlags_WidthFixed, 70.0f, 6);
        ImGui::TableSetupColumn("Sell",         ImGuiTableColumnFlags_WidthFixed, 70.0f, 7);
        ImGui::TableSetupColumn("Mod",          ImGuiTableColumnFlags_WidthFixed, 140.0f, 8);
        ImGui::TableSetupColumn("Source",       ImGuiTableColumnFlags_WidthFixed, 180.0f, 9);
        ImGui::TableSetupColumn("Spawn",        ImGuiTableColumnFlags_WidthFixed, 45.0f, 11);
        ImGui::TableSetupColumn("Status",       ImGuiTableColumnFlags_WidthFixed, 80.0f, 10);
        ImGui::TableHeadersRow();

        // Build filtered index list
        int search_len = (int)strlen(ui.search_buffer);
        int mod_filter_len = (int)strlen(ui.mod_filter);

        static int *sorted_indices = nullptr;
        static int sorted_count = 0;
        static int sorted_capacity = 0;

        // Ensure capacity
        if (sorted_capacity < ctx->item_count) {
            sorted_indices = (int *)realloc(sorted_indices, sizeof(int) * (ctx->item_count + 1));
            sorted_capacity = ctx->item_count;
        }

        sorted_count = 0;
        for (int i = 0; i < ctx->item_count; i++) {
            LootItem *item = &ctx->items[i];

            // Hide deleted items if checkbox is active
            if (ui.hide_deleted && item->deleted) continue;

            // Search filter (classname)
            if (search_len > 0) {
                bool match = false;
                for (const char *p = item->classname; *p; p++) {
                    int j = 0;
                    while (j < search_len && p[j] &&
                           tolower((unsigned char)p[j]) == tolower((unsigned char)ui.search_buffer[j])) j++;
                    if (j == search_len) { match = true; break; }
                }
                if (!match) continue;
            }

            // Mod filter (mod_name)
            if (mod_filter_len > 0) {
                bool match = false;
                for (const char *p = item->mod_name; *p; p++) {
                    int j = 0;
                    while (j < mod_filter_len && p[j] &&
                           tolower((unsigned char)p[j]) == tolower((unsigned char)ui.mod_filter[j])) j++;
                    if (j == mod_filter_len) { match = true; break; }
                }
                if (!match) continue;
            }

            sorted_indices[sorted_count++] = i;
        }

        // Apply sorting based on ImGui sort specs
        ImGuiTableSortSpecs *sort_specs = ImGui::TableGetSortSpecs();
        if (sort_specs && sort_specs->SpecsCount > 0 && sorted_count > 1) {
            g_sort_ctx.ctx = ctx;
            g_sort_ctx.column_id = (int)sort_specs->Specs[0].ColumnUserID;
            g_sort_ctx.direction = sort_specs->Specs[0].SortDirection;
            qsort(sorted_indices, (size_t)sorted_count, sizeof(int), compare_items);
            sort_specs->SpecsDirty = false;
        }

        // Render sorted/filtered items
        for (int si = 0; si < sorted_count; si++) {
            int i = sorted_indices[si];
            LootItem *item = &ctx->items[i];

            ImGui::PushID(i);
            ImGui::TableNextRow();

            // Classname
            ImGui::TableSetColumnIndex(0);
            bool selected = (ctx->selected_item == i);
            ImGuiSelectableFlags sel_flags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap;
            if (ImGui::Selectable(item->classname, selected, sel_flags, ImVec2(0, 0))) {
                ctx->selected_item = i;
                ui.editing_item    = -1;  // force reload in inspector
            }
            // Tooltip: show spawnable XML if available
            if (ImGui::IsItemHovered()) {
                const SpawnableBlock *spawn = find_spawnable_block(ctx, item->classname);
                if (spawn) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "Spawnable Types XML (%s):", spawn->source_file);
                    ImGui::Separator();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.75f, 0.95f, 0.85f));
                    ImGui::TextUnformatted(spawn->raw_xml);
                    ImGui::PopStyleColor();
                    ImGui::EndTooltip();
                }
            }

            // Nominal
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", item->nominal);

            // Min
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", item->min);

            // Lifetime
            ImGui::TableSetColumnIndex(3);
            int hours = item->lifetime / 3600;
            int mins  = (item->lifetime % 3600) / 60;
            if (hours > 0) ImGui::Text("%dh %dm", hours, mins);
            else ImGui::Text("%ds", item->lifetime);

            // Restock
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%d", item->restock);

            // Tier
            ImGui::TableSetColumnIndex(5);
            ImGui::TextColored(tier_color(item->assigned_tier), "T%d", item->assigned_tier);

            // Buy Price
            ImGui::TableSetColumnIndex(6);
            if (item->buy_price > 0)
                ImGui::TextColored(COL_GREEN, "%d", item->buy_price);
            else if (item->buy_price == -1)
                ImGui::TextColored(COL_RED, "N/A");
            else
                ImGui::TextColored(COL_DIM, "-");

            // Sell Price
            ImGui::TableSetColumnIndex(7);
            if (item->sell_price > 0)
                ImGui::TextColored(COL_YELLOW, "%d", item->sell_price);
            else
                ImGui::TextColored(COL_DIM, "-");

            // Mod (mod_name from filepath)
            ImGui::TableSetColumnIndex(8);
            if (item->mod_name[0]) {
                bool is_vanilla = (strcmp(item->mod_name, "vanilla") == 0 || strcmp(item->mod_name, "server_root") == 0);
                ImGui::TextColored(is_vanilla ? COL_DIM : COL_GREEN, "%s", item->mod_name);
            } else {
                ImGui::TextColored(COL_DIM, "-");
            }

            // Source (filename — shows both types.xml and spawnable source when applicable)
            ImGui::TableSetColumnIndex(9);
            {
                const SpawnableBlock *spawn = find_spawnable_block(ctx, item->classname);
                if (spawn && item->mod_source[0]) {
                    // Item exists in BOTH types.xml and cfgspawnabletypes.xml
                    ImGui::TextColored(COL_GREEN, "%s", item->mod_source);
                    ImGui::SameLine(0, 0);
                    ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), " + %s", spawn->source_file);
                } else if (item->mod_source[0]) {
                    ImGui::TextColored(COL_DIM, "%s", item->mod_source);
                } else {
                    ImGui::TextColored(COL_DIM, "-");
                }
                if (ImGui::IsItemHovered() && spawn) {
                    ImGui::BeginTooltip();
                    ImGui::TextColored(COL_AMBER, "Linked Files:");
                    ImGui::TextColored(COL_GREEN, "  types.xml: %s", item->mod_source);
                    ImGui::TextColored(COL_DIM, "    Defines: nominal, min, lifetime, restock, category, usages, values");
                    ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "  spawnabletypes.xml: %s", spawn->source_file);
                    ImGui::TextColored(COL_DIM, "    Defines: cargo presets, attachment configs for this item at spawn");
                    ImGui::Spacing();
                    ImGui::TextColored(COL_YELLOW, "These files work TOGETHER:");
                    ImGui::TextColored(COL_DIM, "  types.xml controls HOW MANY spawn and WHERE");
                    ImGui::TextColored(COL_DIM, "  spawnabletypes.xml controls WHAT THEY SPAWN WITH");
                    ImGui::TextColored(COL_DIM, "  Both must agree on the classname. Edits to one");
                    ImGui::TextColored(COL_DIM, "  may need matching changes in the other.");
                    ImGui::EndTooltip();
                }
            }

            // Spawn (spawnable types indicator)
            ImGui::TableSetColumnIndex(10);
            {
                const SpawnableBlock *spawn = find_spawnable_block(ctx, item->classname);
                if (spawn) {
                    ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "Yes");
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        ImGui::TextColored(ImVec4(0.40f, 0.75f, 0.95f, 1.00f), "Has spawn attachments/cargo defined in:");
                        ImGui::Text("  %s", spawn->source_file);
                        ImGui::EndTooltip();
                    }
                } else {
                    ImGui::TextColored(COL_DIM, "-");
                }
            }

            // Status
            ImGui::TableSetColumnIndex(11);
            if (item->is_debug_item) {
                ImGui::TextColored(COL_PURPLE, "Debug");
            } else if (item->deleted) {
                ImGui::TextColored(COL_RED, "Deleted");
            } else if (item->renamed) {
                ImGui::TextColored(COL_AMBER, "Renamed");
            } else if (item->modified) {
                ImGui::TextColored(COL_YELLOW, "Modified");
            } else {
                ImGui::TextColored(COL_DIM, "OK");
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

// ============================================================================
// TAB: ISSUES
// ============================================================================
static void draw_issues_tab(AuditorContext *ctx) {
    ImGui::Text("Audit Issues: %d", ctx->issue_count);
    ImGui::Separator();

    if (ctx->issue_count == 0) {
        ImGui::TextColored(COL_DIM, "No issues. Run an audit first.");
        return;
    }

    if (ImGui::BeginTable("IssuesTable", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Severity", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Message",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < ctx->issue_count; i++) {
            AuditIssue *issue = &ctx->issues[i];
            if (issue->resolved) continue;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            switch (issue->severity) {
                case SEVERITY_INFO:     ImGui::TextColored(COL_DIM,    "INFO");     break;
                case SEVERITY_WARNING:  ImGui::TextColored(COL_YELLOW, "WARNING");  break;
                case SEVERITY_ERROR:    ImGui::TextColored(COL_RED,    "ERROR");    break;
                case SEVERITY_CRITICAL: ImGui::TextColored(COL_PURPLE, "CRITICAL"); break;
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextWrapped("%s", issue->message);
        }

        ImGui::EndTable();
    }
}

// ============================================================================
// TAB: DIFF VIEW
// ============================================================================
static LootItem* find_original(AuditorContext *ctx, const char *classname) {
    if (!ctx->original_items) return nullptr;
    for (int i = 0; i < ctx->original_item_count; i++) {
        if (util_strcasecmp(ctx->original_items[i].classname, classname) == 0)
            return &ctx->original_items[i];
    }
    return nullptr;
}

static void draw_diff_tab(AuditorContext *ctx) {
    if (!ctx->original_items || ctx->original_item_count == 0) {
        ImGui::TextColored(COL_YELLOW, "No original snapshot available.");
        ImGui::TextColored(COL_DIM, "Load data first — a snapshot is taken automatically on load.");
        return;
    }

    int modified_count = 0;
    for (int i = 0; i < ctx->item_count; i++)
        if (ctx->items[i].modified && !ctx->items[i].deleted) modified_count++;

    ImGui::Text("Modified: %d / %d items", modified_count, ctx->item_count);
    ImGui::SameLine();
    ImGui::Checkbox("Show only modified", &ui.show_only_modified);
    ImGui::Separator();

    if (ImGui::BeginTable("DiffTable", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Item",     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Field",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Original", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("New",      ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Source",   ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < ctx->item_count; i++) {
            LootItem *item = &ctx->items[i];
            if (item->deleted) continue;
            if (ui.show_only_modified && !item->modified) continue;

            LootItem *orig = find_original(ctx, item->classname);
            if (!orig) {
                // New item (not in original)
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(COL_GREEN, "%s", item->classname);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(COL_GREEN, "NEW");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(COL_DIM, "-");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(COL_GREEN, "Added");
                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", item->mod_source);
                continue;
            }

            // Compare fields
            struct DiffEntry { const char *field; int old_val; int new_val; };
            DiffEntry diffs[6];
            int diff_count = 0;

            if (orig->nominal  != item->nominal)  diffs[diff_count++] = {"Nominal",  orig->nominal,  item->nominal};
            if (orig->min      != item->min)      diffs[diff_count++] = {"Min",      orig->min,      item->min};
            if (orig->lifetime != item->lifetime)  diffs[diff_count++] = {"Lifetime", orig->lifetime, item->lifetime};
            if (orig->restock  != item->restock)  diffs[diff_count++] = {"Restock",  orig->restock,  item->restock};
            if (orig->cost     != item->cost)     diffs[diff_count++] = {"Cost",     orig->cost,     item->cost};
            if (orig->assigned_tier != item->assigned_tier) diffs[diff_count++] = {"Tier", orig->assigned_tier, item->assigned_tier};

            // Check usage/value changes (simplified: just check counts)
            bool usage_changed = (orig->usage_count != item->usage_count);
            bool value_changed = (orig->value_count != item->value_count);

            if (diff_count == 0 && !usage_changed && !value_changed) continue;

            for (int d = 0; d < diff_count; d++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (d == 0) ImGui::Text("%s", item->classname);
                else ImGui::TextColored(COL_DIM, "");

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", diffs[d].field);

                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(COL_RED, "%d", diffs[d].old_val);

                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(COL_GREEN, "%d", diffs[d].new_val);

                ImGui::TableSetColumnIndex(4);
                if (d == 0) ImGui::Text("%s", item->mod_source);
            }

            if (usage_changed) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (diff_count == 0) ImGui::Text("%s", item->classname);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Usages");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(COL_RED, "%d zones", orig->usage_count);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(COL_GREEN, "%d zones", item->usage_count);
            }

            if (value_changed) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (diff_count == 0 && !usage_changed) ImGui::Text("%s", item->classname);
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("Values");
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(COL_RED, "%d tiers", orig->value_count);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(COL_GREEN, "%d tiers", item->value_count);
            }
        }

        ImGui::EndTable();
    }
}

// ============================================================================
// TAB: INTEGRITY REPORTS
// ============================================================================
static void draw_integrity_report(const char *label, const IntegrityReport *report) {
    if (!report || report->phase[0] == '\0') {
        ImGui::TextColored(COL_DIM, "Not yet run.");
        return;
    }

    // Status badge + Copy button on same line
    if (report->passed) {
        ImGui::TextColored(COL_GREEN, "%s: PASSED", label);
    } else {
        ImGui::TextColored(COL_RED, "%s: FAILED", label);
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60);
    char copy_id[64];
    snprintf(copy_id, sizeof(copy_id), "Copy##%s", label);
    if (ImGui::SmallButton(copy_id)) {
        // Build clipboard text from report
        static char clip_buf[32768];
        int pos = 0;
        pos += snprintf(clip_buf + pos, sizeof(clip_buf) - pos,
            "=== %s Integrity Report ===\n", label);
        pos += snprintf(clip_buf + pos, sizeof(clip_buf) - pos,
            "Status: %s\n", report->passed ? "PASSED" : "FAILED");
        pos += snprintf(clip_buf + pos, sizeof(clip_buf) - pos,
            "Summary: %s\n", report->summary);
        pos += snprintf(clip_buf + pos, sizeof(clip_buf) - pos,
            "Items: %d total, %d active | Findings: %d | Duplicate groups: %d\n",
            report->total_items, report->active_items,
            report->finding_count, report->duplicate_groups);
        if (report->finding_count > 0) {
            pos += snprintf(clip_buf + pos, sizeof(clip_buf) - pos, "\nFindings:\n");
            for (int i = 0; i < report->finding_count && pos < (int)sizeof(clip_buf) - 512; i++) {
                const IntegrityFinding *f = &report->findings[i];
                const char *sev = "INFO";
                switch (f->severity) {
                    case SEVERITY_WARNING:  sev = "WARN";  break;
                    case SEVERITY_ERROR:    sev = "ERROR"; break;
                    case SEVERITY_CRITICAL: sev = "CRIT";  break;
                    default: break;
                }
                pos += snprintf(clip_buf + pos, sizeof(clip_buf) - pos,
                    "  [%s] %s — %s\n", sev,
                    f->classname[0] ? f->classname : "-", f->message);
            }
        }
        clip_buf[sizeof(clip_buf) - 1] = '\0';
        ImGui::SetClipboardText(clip_buf);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Copy %s integrity report to clipboard", label);
    }

    ImGui::TextWrapped("%s", report->summary);
    ImGui::Text("Items: %d total, %d active | Findings: %d | Duplicate groups: %d",
                report->total_items, report->active_items,
                report->finding_count, report->duplicate_groups);

    if (report->finding_count > 0) {
        ImGui::Spacing();
        if (ImGui::BeginTable(label, 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, 250))) {
            ImGui::TableSetupColumn("Sev", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Classname", ImGuiTableColumnFlags_WidthFixed, 200);
            ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < report->finding_count; i++) {
                const IntegrityFinding *f = &report->findings[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                ImVec4 sev_col = COL_DIM;
                const char *sev_str = "INFO";
                switch (f->severity) {
                    case SEVERITY_WARNING:  sev_col = COL_AMBER;  sev_str = "WARN";  break;
                    case SEVERITY_ERROR:    sev_col = COL_RED;    sev_str = "ERROR"; break;
                    case SEVERITY_CRITICAL: sev_col = COL_RED;    sev_str = "CRIT";  break;
                    default: break;
                }
                ImGui::TextColored(sev_col, "%s", sev_str);
                ImGui::TableNextColumn();
                ImGui::Text("%s", f->classname[0] ? f->classname : "-");
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", f->message);
            }
            ImGui::EndTable();
        }
    }
}

static void draw_integrity_tab(AuditorContext *ctx) {
    ImGui::Text("Integrity Engine — Pre-audit and Post-sew verification");
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Pre-Audit Integrity", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_integrity_report("Pre-Audit", &ctx->pre_audit_integrity);
    }

    ImGui::Spacing();

    if (ImGui::CollapsingHeader("Sew Integrity", ImGuiTreeNodeFlags_DefaultOpen)) {
        draw_integrity_report("Sew", &ctx->sew_integrity);
    }
}

// ============================================================================
// TAB: SWARM LOG
// ============================================================================
static void draw_swarm_tab(AuditorContext *ctx) {
    SwarmState *swarm = &ctx->swarm;

    ImGui::Text("Pipeline Tasks: %d", swarm->task_count);
    ImGui::SameLine();
    if (swarm->complete) ImGui::TextColored(COL_GREEN, " [COMPLETE]");
    else if (swarm->current_task >= 0) ImGui::TextColored(COL_YELLOW, " [RUNNING]");
    else ImGui::TextColored(COL_DIM, " [IDLE]");
    ImGui::Separator();

    if (swarm->task_count == 0) {
        ImGui::TextColored(COL_DIM, "No pipeline has been run yet.");
        return;
    }

    if (ImGui::BeginTable("SwarmTable", 5,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersV,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#",           ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableSetupColumn("Agent",       ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Task",        ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status",      ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Result",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < swarm->task_count; i++) {
            SwarmTask *task = &swarm->tasks[i];
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i + 1);

            ImGui::TableSetColumnIndex(1);
            const char *agent_names[] = {"INDEX", "SORT", "PARSE", "AUDIT", "STITCH", "EXPORT", "RELAY"};
            int agent_idx = (int)task->agent;
            if (agent_idx >= 0 && agent_idx < 7) ImGui::Text("%s", agent_names[agent_idx]);
            else ImGui::Text("?");

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%s", task->description);

            ImGui::TableSetColumnIndex(3);
            switch (task->status) {
                case TASK_PENDING:     ImGui::TextColored(COL_DIM,    "Pending");     break;
                case TASK_IN_PROGRESS: ImGui::TextColored(COL_YELLOW, "Running");     break;
                case TASK_COMPLETE:    ImGui::TextColored(COL_GREEN,  "Complete");     break;
                case TASK_FAILED:      ImGui::TextColored(COL_RED,    "Failed");       break;
            }

            ImGui::TableSetColumnIndex(4);
            if (task->result[0]) ImGui::TextWrapped("%s", task->result);
            else ImGui::TextColored(COL_DIM, "-");
        }

        ImGui::EndTable();
    }
}

// ============================================================================
// TAB: SERVER FILES (remote file browser)
// ============================================================================
static void draw_server_files_tab(AuditorContext *ctx) {
    RemoteFileBrowser *browser = &ctx->browser;
    float avail_w = ImGui::GetContentRegionAvail().x;

    // ---- Toolbar ----
    ImGui::TextColored(COL_AMBER, "Remote Server Files");
    ImGui::SameLine(avail_w - 100);
    if (browser->loading) {
        ImGui::TextColored(COL_YELLOW, "Loading...");
    }

    ImGui::Separator();

    // Path bar + navigation
    ImGui::Text("Path:");
    ImGui::SameLine();

    // Editable path (for manual navigation)
    static char nav_path[MAX_PATH_LEN] = "/";
    if (browser->current_path[0] && browser->ready) {
        strncpy(nav_path, browser->current_path, sizeof(nav_path) - 1);
    }
    ImGui::PushItemWidth(avail_w - 240);
    bool path_entered = ImGui::InputText("##NavPath", nav_path, sizeof(nav_path),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopItemWidth();
    ImGui::SameLine();

    bool do_browse = false;
    if (ImGui::Button("Browse", ImVec2(70, 0)) || path_entered) {
        do_browse = true;
    }
    ImGui::SameLine();
    // Up button
    bool can_go_up = strlen(nav_path) > 1;
    if (!can_go_up) ImGui::BeginDisabled();
    if (ImGui::Button("Up", ImVec2(40, 0))) {
        // Go to parent directory
        char *last_slash = strrchr(nav_path, '/');
        if (last_slash && last_slash != nav_path) {
            *last_slash = '\0';
        } else {
            strcpy(nav_path, "/");
        }
        do_browse = true;
    }
    if (!can_go_up) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(60, 0))) {
        do_browse = true;
    }

    if (do_browse && !browser->loading && ui.ftp_host[0] && ui.ftp_user[0]) {
        start_browse_job(ctx, nav_path);
    }

    ImGui::Spacing();

    // ---- Error display ----
    if (browser->error[0]) {
        ImGui::TextColored(COL_RED, "Error: %s", browser->error);

        // Push a notification for the browse error (once per error)
        static char last_browse_error[256] = {};
        if (strcmp(last_browse_error, browser->error) != 0) {
            strncpy(last_browse_error, browser->error, sizeof(last_browse_error) - 1);
            push_winscp_error_from_log("Browse Failed", ".TEMP/winscp_ls.log");
        }
    }

    // ---- Connection hint ----
    if (!browser->ready && !browser->loading && browser->count == 0) {
        ImGui::Spacing();
        ImGui::TextColored(COL_DIM, "Enter your server FTP credentials in the sidebar,");
        ImGui::TextColored(COL_DIM, "set the remote path, and click Browse.");
        ImGui::Spacing();
        ImGui::TextColored(COL_DIM, "Tip: Your remote root is configured in config/server_paths.ini");

        // Auto-fill from server_paths.ini
        ServerPaths sp;
        ui_load_server_paths(&sp);
        if (sp.remote_root[0] && strcmp(nav_path, "/") == 0) {
            strncpy(nav_path, sp.remote_root, sizeof(nav_path) - 1);
        }
        return;
    }

    // ---- File table ----
    ImGui::TextColored(COL_DIM, "%d entries", browser->count);
    ImGui::Separator();

    if (ImGui::BeginTable("##FileTable", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp,
            ImVec2(0, ImGui::GetContentRegionAvail().y))) {

        ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_DefaultSort, 0.50f);
        ImGui::TableSetupColumn("Type",     ImGuiTableColumnFlags_NoSort,      0.10f);
        ImGui::TableSetupColumn("Size",     0,                                 0.15f);
        ImGui::TableSetupColumn("Modified", 0,                                 0.25f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < browser->count; i++) {
            RemoteFileEntry *e = &browser->entries[i];
            ImGui::TableNextRow();

            // Name column (clickable)
            ImGui::TableNextColumn();
            bool selected = (browser->selected == i);

            // Icon + color
            if (e->is_directory) {
                ImGui::TextColored(COL_AMBER, "[DIR]");
            } else {
                // Color code by extension
                const char *ext = strrchr(e->name, '.');
                if (ext && (strcmp(ext, ".xml") == 0 || strcmp(ext, ".XML") == 0)) {
                    ImGui::TextColored(COL_GREEN, " XML ");
                } else if (ext && (strcmp(ext, ".json") == 0 || strcmp(ext, ".cfg") == 0 || strcmp(ext, ".ini") == 0)) {
                    ImGui::TextColored(COL_YELLOW, " CFG ");
                } else if (ext && (strcmp(ext, ".pbo") == 0 || strcmp(ext, ".bin") == 0)) {
                    ImGui::TextColored(COL_PURPLE, " BIN ");
                } else {
                    ImGui::TextColored(COL_DIM, " --- ");
                }
            }
            ImGui::SameLine();

            char label[300];
            snprintf(label, sizeof(label), "%s##file_%d", e->name, i);
            if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
                browser->selected = i;

                // Double-click to navigate into directory
                if (ImGui::IsMouseDoubleClicked(0) && e->is_directory) {
                    strncpy(nav_path, e->full_path, sizeof(nav_path) - 1);
                    if (!browser->loading) {
                        start_browse_job(ctx, nav_path);
                    }
                }
            }

            // Type column
            ImGui::TableNextColumn();
            ImGui::TextColored(COL_DIM, "%s", e->is_directory ? "Folder" : "File");

            // Size column
            ImGui::TableNextColumn();
            if (!e->is_directory) {
                if (e->size >= 1024 * 1024) {
                    ImGui::Text("%.1f MB", (double)e->size / (1024.0 * 1024.0));
                } else if (e->size >= 1024) {
                    ImGui::Text("%.1f KB", (double)e->size / 1024.0);
                } else {
                    ImGui::Text("%lld B", e->size);
                }
            } else {
                ImGui::TextColored(COL_DIM, "--");
            }

            // Modified column
            ImGui::TableNextColumn();
            ImGui::TextColored(COL_DIM, "%s", e->date_str);
        }

        ImGui::EndTable();
    }
}

// ============================================================================
// TAB: LOOT POLICY (configurable tiers + blacklist)
// ============================================================================
static void draw_loot_policy_tab(AuditorContext *ctx) {
    (void)ctx;

    // Populate editor buffers from the live policy on first view / after reload.
    if (!ui.lp_ui_loaded) {
        ui.lp_tiers_buf[0] = '\0';
        for (int i = 0; i < g_loot_policy.tier_count; i++) {
            if (i) strncat(ui.lp_tiers_buf, ", ", sizeof(ui.lp_tiers_buf) - strlen(ui.lp_tiers_buf) - 1);
            strncat(ui.lp_tiers_buf, g_loot_policy.tiers[i].name, sizeof(ui.lp_tiers_buf) - strlen(ui.lp_tiers_buf) - 1);
        }
        ui.lp_blacklist_buf[0] = '\0';
        for (int i = 0; i < g_loot_policy.blacklist_count; i++) {
            if (i) strncat(ui.lp_blacklist_buf, ", ", sizeof(ui.lp_blacklist_buf) - strlen(ui.lp_blacklist_buf) - 1);
            strncat(ui.lp_blacklist_buf, g_loot_policy.blacklist[i], sizeof(ui.lp_blacklist_buf) - strlen(ui.lp_blacklist_buf) - 1);
        }
        ui.lp_ui_loaded = true;
    }

    ImGui::Text("Loot Tiers & Blacklist");
    ImGui::Separator();
    ImGui::TextColored(COL_DIM, "Config file: %s", LP_CONFIG_PATH);
    ImGui::Spacing();

    ImGui::Text("Tier names (comma-separated). The number of names = the number of tiers.");
    ImGui::InputTextMultiline("##lp_tiers", ui.lp_tiers_buf, sizeof(ui.lp_tiers_buf), ImVec2(-1, 70));
    ImGui::TextColored(COL_GREEN, "Active tiers: %d", g_loot_policy.tier_count);
    ImGui::TextColored(COL_DIM,
        "Per-tier behaviour (no-spawn / no-trade / black-market) and Bitcoin range live in %s.",
        LP_CONFIG_PATH);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Blacklist (comma-separated classnames). These never spawn -> forced to the Contraband tier.");
    ImGui::InputTextMultiline("##lp_blacklist", ui.lp_blacklist_buf, sizeof(ui.lp_blacklist_buf), ImVec2(-1, 160));
    ImGui::TextColored(COL_GREEN, "Blacklisted: %d", g_loot_policy.blacklist_count);

    ImGui::Spacing();
    if (ImGui::Button("Save Policy", ImVec2(150, 30))) {
        loot_policy_set_tiers_csv(ui.lp_tiers_buf);
        loot_policy_set_blacklist_csv(ui.lp_blacklist_buf);
        if (loot_policy_save(LP_CONFIG_PATH))
            snprintf(ui.lp_status, sizeof(ui.lp_status), "Saved %d tiers, %d blacklisted.",
                     g_loot_policy.tier_count, g_loot_policy.blacklist_count);
        else
            snprintf(ui.lp_status, sizeof(ui.lp_status), "ERROR: could not write %s", LP_CONFIG_PATH);
        ui.lp_ui_loaded = false; // repopulate from the normalized policy
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload", ImVec2(150, 30))) {
        loot_policy_load(LP_CONFIG_PATH);
        ui.lp_ui_loaded = false;
    }
    if (ui.lp_status[0]) {
        ImGui::SameLine();
        ImGui::TextColored(COL_GREEN, "%s", ui.lp_status);
    }

    ImGui::Spacing();
    ImGui::TextColored(COL_DIM, "Changes apply on the next pipeline run (Run Pipeline / Save & Upload).");
}

// ============================================================================
// TAB: CONNECTION (full settings)
// ============================================================================
static void draw_connection_tab(AuditorContext *ctx) {
    ImGui::Text("FTP / SFTP Connection Settings");
    ImGui::Separator();

    float w = 500.0f;
    ImGui::PushItemWidth(w);
    ImGui::InputText("Host##conn",     ui.ftp_host, sizeof(ui.ftp_host));
    ImGui::InputText("Port##conn",     ui.ftp_port, sizeof(ui.ftp_port));
    ImGui::InputText("Username##conn", ui.ftp_user, sizeof(ui.ftp_user));
    ImGui::InputText("Password##conn", ui.ftp_pass, sizeof(ui.ftp_pass), ImGuiInputTextFlags_Password);
    ImGui::PopItemWidth();

    ImGui::Spacing();

    if (ImGui::Button("Save Config", ImVec2(150, 30))) save_ftp_config();
    ImGui::SameLine();
    if (ImGui::Button("Load Config", ImVec2(150, 30))) load_ftp_config();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Server Paths (from config/server_paths.ini):");

    ServerPaths p;
    ui_load_server_paths(&p);

    ImGui::TextColored(COL_DIM, "Remote Root:    %s", p.remote_root);
    ImGui::TextColored(COL_DIM, "Remote Types:   %s", p.remote_types);
    ImGui::TextColored(COL_DIM, "Remote Spawn:   %s", p.remote_spawnable);
    ImGui::TextColored(COL_DIM, "Remote Events:  %s", p.remote_events);
    ImGui::TextColored(COL_DIM, "Remote Globals: %s", p.remote_globals);
    ImGui::TextColored(COL_DIM, "Remote Trader:  %s", p.remote_trader);
    ImGui::TextColored(COL_GREEN, "Shop Mod:       %s", shop_mod_name((ShopMod)ui.shop_mod_index));
    ImGui::Spacing();
    ImGui::TextColored(COL_DIM, "Local Root:     %s", p.local_root);
    ImGui::TextColored(COL_DIM, "Local Types:    %s", p.local_types);
    ImGui::TextColored(COL_DIM, "Local Spawn:    %s", p.local_spawnable);
    ImGui::TextColored(COL_DIM, "Local Events:   %s", p.local_events);
    ImGui::TextColored(COL_DIM, "Local Globals:  %s", p.local_globals);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("File Index: %d entries", ctx->file_index.count);

    int economy = 0, territory = 0, spawnable = 0, config = 0;
    for (int i = 0; i < ctx->file_index.count; i++) {
        switch (ctx->file_index.entries[i].file_type) {
            case FILE_TYPE_ECONOMY:   economy++;   break;
            case FILE_TYPE_TERRITORY: territory++; break;
            case FILE_TYPE_SPAWNABLE: spawnable++; break;
            case FILE_TYPE_CONFIG:    config++;    break;
            default: break;
        }
    }
    ImGui::TextColored(COL_GREEN, "Economy: %d  Territory: %d  Spawnable: %d  Config: %d",
                       economy, territory, spawnable, config);

    ImGui::Spacing();
    ImGui::Text("Loaded Files: %d", ui.loaded_file_count);
}

// ============================================================================
// STATUS BAR
// ============================================================================
static void draw_status_bar(AuditorContext *ctx) {
    float y = (float)GetScreenHeight() - STATUS_BAR_H;
    ImGui::SetNextWindowPos(ImVec2(0, y));
    ImGui::SetNextWindowSize(ImVec2((float)GetScreenWidth(), STATUS_BAR_H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07f, 0.07f, 0.09f, 1.00f));

    ImGui::Begin("##StatusBar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(COL_AMBER, "STATUS:");
    ImGui::SameLine();
    ImGui::Text("%s", ctx->status_message);

    ImGui::SameLine(ImGui::GetWindowWidth() - 300);
    ImGui::TextColored(COL_DIM, "Items: %d | Issues: %d | Files: %d",
                       ctx->item_count, ctx->issue_count, ui.loaded_file_count);

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ============================================================================
// LOG TAB — mirrors the console log inside the UI
// ============================================================================
static bool log_auto_scroll = true;
static int  log_severity_filter = 0; // 0 = ALL, 1 = WARN+, 2 = ERR+, 3 = CRIT only

// Selection state for log lines (indexed by ring-buffer position)
static bool log_selected[LOG_RING_CAPACITY] = {};
static int  log_last_clicked = -1;  // last clicked index for shift-select range
static bool log_copy_flash = false;
static float log_copy_flash_timer = 0.0f;

// Build the filtered log text as a string. Caller must free() the result.
static char *log_build_text(Severity min_sev, bool selected_only) {
    int count = util_log_get_count();
    // Generous upper bound: each entry ≤ 16 (timestamp) + 8 (tag) + LOG_ENTRY_MAX + 4 (spacing/newline)
    size_t buf_cap = (size_t)count * (LOG_ENTRY_MAX + 32) + 1;
    char *buf = (char *)malloc(buf_cap);
    if (!buf) return nullptr;
    size_t pos = 0;

    for (int i = 0; i < count; i++) {
        const LogEntry *e = util_log_get_entry(i);
        if (!e) continue;
        if (e->severity < min_sev) continue;
        if (selected_only && !log_selected[i]) continue;

        const char *tag = "INFO ";
        switch (e->severity) {
            case SEVERITY_INFO:     tag = "INFO "; break;
            case SEVERITY_WARNING:  tag = "WARN "; break;
            case SEVERITY_ERROR:    tag = "ERROR"; break;
            case SEVERITY_CRITICAL: tag = "CRIT "; break;
        }
        int n = snprintf(buf + pos, buf_cap - pos, "%s [%s] %s\n", e->timestamp, tag, e->message);
        if (n > 0) pos += (size_t)n;
    }
    buf[pos] = '\0';
    return buf;
}

// Count how many lines are currently selected (visible under filter)
static int log_count_selected(Severity min_sev) {
    int sel = 0;
    int count = util_log_get_count();
    for (int i = 0; i < count; i++) {
        const LogEntry *e = util_log_get_entry(i);
        if (!e || e->severity < min_sev) continue;
        if (log_selected[i]) sel++;
    }
    return sel;
}

#ifdef _WIN32
static void log_export_to_file(Severity min_sev) {
    char path[MAX_PATH] = "auditor_log.txt";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = sizeof(path);
    ofn.lpstrFilter  = "Text Files (*.txt)\0*.txt\0Log Files (*.log)\0*.log\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt  = "txt";
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) {
        char *text = log_build_text(min_sev, false);
        if (text) {
            FILE *f = fopen(path, "w");
            if (f) {
                fputs(text, f);
                fclose(f);
                util_log(SEVERITY_INFO, "Log exported to: %s", path);
            }
            free(text);
        }
    }
}
#endif

static void draw_log_tab(AuditorContext * /*ctx*/) {
    // Toolbar row
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(COL_DIM, "Filter:");
    ImGui::SameLine();
    const char *filter_labels[] = { "All", "Warn+", "Error+", "Critical" };
    for (int i = 0; i < 4; i++) {
        if (i > 0) ImGui::SameLine();
        bool sel = (log_severity_filter == i);
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.42f, 0.1f, 1.0f));
        if (ImGui::SmallButton(filter_labels[i])) log_severity_filter = i;
        if (sel) ImGui::PopStyleColor();
    }

    // --- Copy / Export buttons ---
    ImGui::SameLine();
    ImGui::TextColored(COL_DIM, " | ");
    ImGui::SameLine();

    // Flash effect for copy confirmation
    if (log_copy_flash) {
        log_copy_flash_timer -= ImGui::GetIO().DeltaTime;
        if (log_copy_flash_timer <= 0.0f) log_copy_flash = false;
    }

    if (log_copy_flash) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 0.2f, 1.0f));
        ImGui::SmallButton("Copied!");
        ImGui::PopStyleColor();
    } else {
        // Map filter index to minimum severity (used across the tab)
        Severity min_sev = SEVERITY_INFO;
        if (log_severity_filter == 1) min_sev = SEVERITY_WARNING;
        if (log_severity_filter == 2) min_sev = SEVERITY_ERROR;
        if (log_severity_filter == 3) min_sev = SEVERITY_CRITICAL;

        int num_sel = log_count_selected(min_sev);
        char copy_label[32];
        if (num_sel > 0)
            snprintf(copy_label, sizeof(copy_label), "Copy (%d)", num_sel);
        else
            snprintf(copy_label, sizeof(copy_label), "Copy All");

        if (ImGui::SmallButton(copy_label)) {
            char *text = log_build_text(min_sev, num_sel > 0);
            if (text) {
                ImGui::SetClipboardText(text);
                free(text);
                log_copy_flash = true;
                log_copy_flash_timer = 1.5f;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(num_sel > 0 ? "Copy selected log lines to clipboard" : "Copy all visible log lines to clipboard (Ctrl+C)");
    }
    ImGui::SameLine();

#ifdef _WIN32
    if (ImGui::SmallButton("Export")) {
        Severity min_sev_exp = SEVERITY_INFO;
        if (log_severity_filter == 1) min_sev_exp = SEVERITY_WARNING;
        if (log_severity_filter == 2) min_sev_exp = SEVERITY_ERROR;
        if (log_severity_filter == 3) min_sev_exp = SEVERITY_CRITICAL;
        log_export_to_file(min_sev_exp);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Export visible log to a text file");
    ImGui::SameLine();
#endif

    // Auto-scroll toggle (pushed to right)
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
    ImGui::Checkbox("Auto-scroll", &log_auto_scroll);
    ImGui::Separator();

    // Map filter index to minimum severity
    Severity min_sev = SEVERITY_INFO;
    if (log_severity_filter == 1) min_sev = SEVERITY_WARNING;
    if (log_severity_filter == 2) min_sev = SEVERITY_ERROR;
    if (log_severity_filter == 3) min_sev = SEVERITY_CRITICAL;

    // Keyboard shortcut: Ctrl+C copies log (same as Copy button)
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !ImGui::IsAnyItemActive()) {
        int num_sel = log_count_selected(min_sev);
        char *text = log_build_text(min_sev, num_sel > 0);
        if (text) {
            ImGui::SetClipboardText(text);
            free(text);
            log_copy_flash = true;
            log_copy_flash_timer = 1.5f;
        }
    }

    // Keyboard shortcut: Ctrl+A selects/deselects all visible lines
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A) && !ImGui::IsAnyItemActive()) {
        int vis_count = 0, sel_count = 0;
        int count = util_log_get_count();
        for (int i = 0; i < count; i++) {
            const LogEntry *e = util_log_get_entry(i);
            if (!e || e->severity < min_sev) continue;
            vis_count++;
            if (log_selected[i]) sel_count++;
        }
        bool select_all = (sel_count < vis_count);
        for (int i = 0; i < count; i++) {
            const LogEntry *e = util_log_get_entry(i);
            if (!e || e->severity < min_sev) { log_selected[i] = false; continue; }
            log_selected[i] = select_all;
        }
    }

    // Scrollable log region
    ImGui::BeginChild("LogScroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    int count = util_log_get_count();
    for (int i = 0; i < count; i++) {
        const LogEntry *e = util_log_get_entry(i);
        if (!e) continue;
        if (e->severity < min_sev) continue;

        // Severity color
        ImVec4 color = COL_DIM;
        const char *tag = "INFO ";
        switch (e->severity) {
            case SEVERITY_INFO:     color = ImVec4(0.7f, 0.7f, 0.7f, 1.0f); tag = "INFO "; break;
            case SEVERITY_WARNING:  color = ImVec4(1.0f, 0.85f, 0.3f, 1.0f); tag = "WARN "; break;
            case SEVERITY_ERROR:    color = ImVec4(1.0f, 0.35f, 0.3f, 1.0f); tag = "ERROR"; break;
            case SEVERITY_CRITICAL: color = ImVec4(1.0f, 0.2f, 0.6f, 1.0f);  tag = "CRIT "; break;
        }

        // Build the full line text for the selectable
        char line_buf[LOG_ENTRY_MAX + 32];
        snprintf(line_buf, sizeof(line_buf), "%s [%s] %s", e->timestamp, tag, e->message);

        // Selection highlight: draw a colored Selectable behind the text
        ImGui::PushID(i);
        if (log_selected[i]) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.55f, 0.42f, 0.1f, 0.35f));
        }
        // Invisible selectable spanning the line for click handling
        if (ImGui::Selectable("##logline", log_selected[i], ImGuiSelectableFlags_AllowOverlap, ImVec2(0, ImGui::GetTextLineHeight()))) {
            bool shift = ImGui::GetIO().KeyShift;
            bool ctrl  = ImGui::GetIO().KeyCtrl;

            if (shift && log_last_clicked >= 0) {
                // Range select: select everything between last click and this click
                int lo = (log_last_clicked < i) ? log_last_clicked : i;
                int hi = (log_last_clicked > i) ? log_last_clicked : i;
                if (!ctrl) {
                    // Clear all first unless Ctrl held
                    for (int j = 0; j < count; j++) log_selected[j] = false;
                }
                for (int j = lo; j <= hi; j++) {
                    const LogEntry *ej = util_log_get_entry(j);
                    if (ej && ej->severity >= min_sev) log_selected[j] = true;
                }
            } else if (ctrl) {
                // Toggle single
                log_selected[i] = !log_selected[i];
            } else {
                // Single click — clear others, select this
                for (int j = 0; j < count; j++) log_selected[j] = false;
                log_selected[i] = true;
            }
            log_last_clicked = i;
        }
        if (log_selected[i]) {
            ImGui::PopStyleColor();
        }

        // Draw the colored text on top of the selectable (same line)
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(COL_DIM, "%s", e->timestamp);
        ImGui::SameLine();
        ImGui::TextColored(color, "[%s]", tag);
        ImGui::SameLine();
        ImGui::TextColored(color, "%s", e->message);

        ImGui::PopID();
    }

    if (log_auto_scroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
}

// ============================================================================
// MAIN FRAME
// ============================================================================
static void draw_main_frame(AuditorContext *ctx) {
    float win_w = (float)GetScreenWidth();
    float win_h = (float)GetScreenHeight();

    // Full-window frame (no title bar)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(win_w, win_h - STATUS_BAR_H));
    ImGui::Begin("##MainFrame", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Title bar area
    ImGui::TextColored(COL_AMBER, "STELLIFERUM AUDITOR v5.0");
    ImGui::SameLine(win_w - 180);
    ImGui::TextColored(COL_DIM, "FPS: %d", GetFPS());
    ImGui::Separator();

    float content_h = ImGui::GetContentRegionAvail().y;

    // LEFT SIDEBAR
    ImGui::BeginChild("Sidebar", ImVec2(SIDEBAR_WIDTH, content_h), true);
    draw_sidebar(ctx);
    ImGui::EndChild();

    ImGui::SameLine();

    // MAIN CONTENT (tabbed)
    ImGui::BeginChild("Content", ImVec2(0, content_h), true);

    if (ImGui::BeginTabBar("MainTabs", ImGuiTabBarFlags_Reorderable)) {
        if (ImGui::BeginTabItem("Items")) {
            draw_items_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Issues")) {
            draw_issues_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diff")) {
            draw_diff_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Swarm")) {
            draw_swarm_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Integrity")) {
            draw_integrity_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Server Files")) {
            draw_server_files_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Loot Policy")) {
            draw_loot_policy_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Connection")) {
            draw_connection_tab(ctx);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Log")) {
            draw_log_tab(ctx);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();
    ImGui::End();
}

// ============================================================================
// HANDLE DOWNLOAD COMPLETION (post-download auto-load)
// ============================================================================
static void handle_download_completion(AuditorContext *ctx) {
    if (!ui.download_ready) return;
    ui.download_ready = false;

#ifdef _WIN32
    if (download_thread) {
        CloseHandle(download_thread);
        download_thread = NULL;
    }
#endif

    if (ui.download_success) {
        util_snapshot_base_files(download_job.paths.local_root, "base");
        util_generate_mod_manifest(download_job.paths.local_root, "output/mod_manifest.md");
        snprintf(ctx->status_message, 255, "Download complete! Auto-loading...");
        do_full_load(ctx, download_job.paths.local_root);
    } else {
        snprintf(ctx->status_message, 255, "Download failed. Check server paths or FTP credentials.");
        push_winscp_error_from_log("Download Failed", ".TEMP/winscp.log");
    }
}

// ============================================================================
// HANDLE PIPELINE COMPLETION (post-pipeline status update)
// ============================================================================
static void handle_pipeline_completion(AuditorContext *ctx) {
    if (!ui.pipeline_ready) return;
    ui.pipeline_ready = false;

#ifdef _WIN32
    if (pipeline_thread) {
        CloseHandle(pipeline_thread);
        pipeline_thread = NULL;
    }
#endif

    if (ui.pipeline_success) {
        snprintf(ctx->status_message, 255,
            "Pipeline complete: %d items, %d issues. Review Swarm + Integrity tabs.",
            ctx->item_count, ctx->issue_count);
        util_log(SEVERITY_INFO, "Pipeline thread finished successfully.");
    } else {
        snprintf(ctx->status_message, 255, "Pipeline finished with errors. Check Log tab.");
        util_log(SEVERITY_WARNING, "Pipeline thread finished with errors.");
    }
}

// ============================================================================
// PUBLIC API — Called from main.c
// ============================================================================
extern "C" {

void ui_init(void) {
    util_log(SEVERITY_INFO, "UI: Initializing ImGui Window...");

    util_ensure_directory("output");
    util_ensure_directory("config");
    util_ensure_directory("base");

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(WINDOW_WIDTH, WINDOW_HEIGHT, "Stelliferum Auditor v5.0");
    SetTargetFPS(60);
    SetExitKey(0);  // Disable ESC = close

    rlImGuiSetup(true);  // dark theme base
    setup_theme();        // our custom amber theme

    memset(&ui, 0, sizeof(UIState));
    strncpy(ui.ftp_port, "21", 7);
    load_ftp_config();

    // Pre-load server paths
    ui_load_server_paths(&download_job.paths);

    util_log(SEVERITY_INFO, "UI: ImGui ready.");
}

void ui_shutdown(void) {
#ifdef _WIN32
    // Kill all WinSCP child processes first — this unblocks pipe-reading threads.
    util_close_job_object();

    // Clean up upload daemon thread
    upload_daemon_cleanup();

    if (browse_thread) {
        WaitForSingleObject(browse_thread, 5000);
        CloseHandle(browse_thread);
        browse_thread = NULL;
    }
    if (download_thread) {
        WaitForSingleObject(download_thread, 5000);
        CloseHandle(download_thread);
        download_thread = NULL;
    }
    if (pipeline_thread) {
        WaitForSingleObject(pipeline_thread, 10000);
        CloseHandle(pipeline_thread);
        pipeline_thread = NULL;
    }
#endif
    rlImGuiShutdown();
    CloseWindow();
}

void ui_run(AuditorContext *ctx) {
    util_log(SEVERITY_INFO, "UI: Entering Main Loop");

    while (!WindowShouldClose()) {
        // Handle async download completion
        handle_download_completion(ctx);

        // Handle async pipeline completion
        handle_pipeline_completion(ctx);

        BeginDrawing();
        ClearBackground({20, 20, 25, 255});

        rlImGuiBegin();
        draw_main_frame(ctx);
        draw_status_bar(ctx);
        draw_notifications();
        rlImGuiEnd();

        EndDrawing();
    }

    util_log(SEVERITY_INFO, "UI: Exiting Main Loop");
}

} // extern "C"
