
#include "auditor.h"
#include "loot_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#ifdef SEVERITY_ERROR
#undef SEVERITY_ERROR
#endif
#ifdef SEVERITY_WARNING
#undef SEVERITY_WARNING
#endif
#endif

static FILE *log_file = NULL;

// ── Log ring buffer ────────────────────────────────────────────────────────
static LogEntry log_ring[LOG_RING_CAPACITY];
static int      log_ring_head  = 0;   // next write position
static int      log_ring_count = 0;   // entries stored (≤ capacity)

int util_log_get_count(void) {
    return log_ring_count;
}

const LogEntry* util_log_get_entry(int index) {
    if (index < 0 || index >= log_ring_count) return NULL;
    int real = (log_ring_head - log_ring_count + index + LOG_RING_CAPACITY) % LOG_RING_CAPACITY;
    return &log_ring[real];
}

static void log_ring_push(Severity sev, const char *ts, const char *msg) {
    LogEntry *e = &log_ring[log_ring_head];
    e->severity = sev;
    strncpy(e->timestamp, ts, sizeof(e->timestamp) - 1);
    e->timestamp[sizeof(e->timestamp) - 1] = '\0';
    strncpy(e->message, msg, sizeof(e->message) - 1);
    e->message[sizeof(e->message) - 1] = '\0';
    log_ring_head = (log_ring_head + 1) % LOG_RING_CAPACITY;
    if (log_ring_count < LOG_RING_CAPACITY) log_ring_count++;
}
// ────────────────────────────────────────────────────────────────────────────

void util_init_logger() {
    // Ensure .TEMP directory exists
    util_ensure_directory(".TEMP");
    log_file = fopen(".TEMP/auditor_crash_log.txt", "w");
    if (log_file) {
        fprintf(log_file, "=== STELLIFERUM AUDITOR SESSION START ===\n");
        fflush(log_file);
    }
}

void util_close_logger() {
    if (log_file) {
        fprintf(log_file, "=== SESSION END ===\n");
        fclose(log_file);
        log_file = NULL;
    }
}

static bool console_colors_enabled = false;

// ── Win32 Job Object (kill all child processes when auditor exits) ──────────
#ifdef _WIN32
static HANDLE g_child_job = NULL;
#endif

void util_init_job_object(void) {
#ifdef _WIN32
    g_child_job = CreateJobObject(NULL, NULL);
    if (g_child_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        ZeroMemory(&jeli, sizeof(jeli));
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_child_job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
#endif
}

void util_assign_child_process(void *hProcess) {
#ifdef _WIN32
    if (g_child_job && hProcess) {
        AssignProcessToJobObject(g_child_job, (HANDLE)hProcess);
    }
#else
    (void)hProcess;
#endif
}

void util_close_job_object(void) {
#ifdef _WIN32
    if (g_child_job) {
        CloseHandle(g_child_job);
        g_child_job = NULL;
    }
#endif
}
// ────────────────────────────────────────────────────────────────────────────

void util_setup_console(void) {
#ifdef _WIN32
    // Allocate a console for headless / CLI mode (WIN32 subsystem has none)
    AllocConsole();
    freopen("CONOUT$", "w", stdout);
    freopen("CONOUT$", "w", stderr);
    freopen("CONIN$", "r", stdin);

    // Set console title
    SetConsoleTitleA("Stelliferum Auditor \xe2\x80\x94 Log");

    // Disable Quick-Edit mode (prevents accidental freeze when user clicks console)
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD in_mode = 0;
    if (GetConsoleMode(hIn, &in_mode)) {
        in_mode &= ~0x0040; // ENABLE_QUICK_EDIT_MODE
        in_mode |= 0x0080;  // ENABLE_EXTENDED_FLAGS
        SetConsoleMode(hIn, in_mode);
    }

    // Enable ANSI / Virtual Terminal Processing for colored output
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode = 0;
    if (GetConsoleMode(hOut, &out_mode)) {
        out_mode |= 0x0004; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
        if (SetConsoleMode(hOut, out_mode)) {
            console_colors_enabled = true;
        }
    }

    // Banner (CLI / headless — native libcurl preferred; WinSCP optional Windows fallback)
    if (console_colors_enabled) {
        printf("\033[33m");
        printf("==========================================================\n");
        printf("  STELLIFERUM AUDITOR  \xe2\x80\x94  Active Log\n");
        printf("  This window shows real-time operation logs.\n");
        printf("  Transfer: native FTP/SFTP (libcurl); WinSCP optional.\n");
        printf("==========================================================\n");
        printf("\033[0m\n");
    } else {
        printf("==========================================================\n");
        printf("  STELLIFERUM AUDITOR  - Active Log\n");
        printf("  This window shows real-time operation logs.\n");
        printf("  Transfer: native FTP/SFTP (libcurl); WinSCP optional.\n");
        printf("==========================================================\n\n");
    }
    fflush(stdout);
#else
    /* Linux/macOS: process already has a TTY for CLI/headless — no WinSCP,
     * no AllocConsole. Native libcurl is the required transfer path. */
    console_colors_enabled = true;
    printf("==========================================================\n");
    printf("  STELLIFERUM AUDITOR  - Active Log\n");
    printf("  Transfer: native FTP/SFTP (libcurl); WinSCP not used.\n");
    printf("==========================================================\n\n");
    fflush(stdout);
#endif
}

void util_log(Severity severity, const char *fmt, ...) {
    va_list args;
    const char *prefix = "[INFO]";
    if (severity == SEVERITY_WARNING)  prefix = "[WARN]";
    if (severity == SEVERITY_ERROR)    prefix = "[ERROR]";
    if (severity == SEVERITY_CRITICAL) prefix = "[CRIT]";

    // Compute timestamp once
    time_t now;
    time(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", localtime(&now));

    // Console output: timestamp + colored severity
    if (console_colors_enabled) {
        const char *color = "\033[37m";  // white
        if (severity == SEVERITY_WARNING)  color = "\033[33m";    // yellow
        if (severity == SEVERITY_ERROR)    color = "\033[31m";    // red
        if (severity == SEVERITY_CRITICAL) color = "\033[35;1m";  // bold magenta
        printf("\033[90m%s\033[0m %s%-7s\033[0m ", time_buf, color, prefix);
    } else {
        printf("%s %-7s ", time_buf, prefix);
    }
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);

    // File log (plain text, no colors)
    if (log_file) {
        fprintf(log_file, "%s %s ", time_buf, prefix);
        va_start(args, fmt);
        vfprintf(log_file, fmt, args);
        va_end(args);
        fprintf(log_file, "\n");
        fflush(log_file);
    }

    // Push to ring buffer so the UI can read it
    {
        char msg_buf[LOG_ENTRY_MAX];
        va_start(args, fmt);
        vsnprintf(msg_buf, sizeof(msg_buf), fmt, args);
        va_end(args);
        log_ring_push(severity, time_buf, msg_buf);
    }
}

// STRING TABLES
static const char *ZONE_NAMES[] = { "CIVILIAN", "MILITARY", "END-GAME", "UNKNOWN" };
static const char *ISSUE_TYPE_NAMES[] = { "NONE", "ORPHAN", "WRONG ZONE", "NOMINAL HIGH", "NOMINAL LOW", "FORBIDDEN CAT", "MISSING USAGE", "DUPLICATE", "SHORT LIFE", "LONG LIFE" };
static const char *SEVERITY_NAMES[] = { "INFO", "WARNING", "ERROR", "CRITICAL" };

void util_init_context(AuditorContext *ctx) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(AuditorContext));
    
    // [FIX] Force selection to -1 to show Global Actions by default
    ctx->selected_item = -1;
    ctx->original_items = NULL;
    ctx->original_item_count = 0;
    ctx->data_loaded = false;
    ctx->audit_complete = false;
    ctx->web = NULL;
    
    strncpy(ctx->output_dir, "./output", MAX_PATH_LEN);
    strncpy(ctx->config_dir, "./config", MAX_PATH_LEN);
    strncpy(ctx->backup_dir, "./backups", MAX_PATH_LEN);
    snprintf(ctx->status_message, 256, "Ready.");

    // Load shop mod preference from config
    char shop_mod_buf[32] = {0};
    if (util_read_ini_value("config/server_paths.ini", "SHOP_MOD", shop_mod_buf, sizeof(shop_mod_buf))) {
        if (util_strcasecmp(shop_mod_buf, "drjones") == 0 || util_strcasecmp(shop_mod_buf, "dr_jones") == 0)
            ctx->shop_mod = SHOP_MOD_DRJONES;
        else if (util_strcasecmp(shop_mod_buf, "expansion") == 0)
            ctx->shop_mod = SHOP_MOD_EXPANSION;
        else
            ctx->shop_mod = SHOP_MOD_TRADERPLUS;
    }

    // Load the user-configurable tier system + blacklist (config/loot_policy.ini).
    // Falls back to the built-in 12-tier default map if the file is absent.
    loot_policy_load(LP_CONFIG_PATH);
}

void util_cleanup_context(AuditorContext *ctx) {
    if (!ctx) return;
    if (ctx->original_items) {
        free(ctx->original_items);
        ctx->original_items = NULL;
    }
    ctx->original_item_count = 0;
    // Free classname dedup hash map
    classname_map_free(ctx);
    // Shutdown web lookup if active
    if (ctx->web) {
        // Include web_lookup.h indirectly via the declared prototype
        extern void web_lookup_shutdown(struct WebLookupState *state);
        web_lookup_shutdown(ctx->web);
        free(ctx->web);
        ctx->web = NULL;
    }
}

const char* util_zone_name(Zone zone) {
    if (zone < 0 || zone > ZONE_UNKNOWN) return "INVALID";
    return ZONE_NAMES[zone];
}

const char* util_issue_type_name(IssueType type) {
    if (type < 0 || type > ISSUE_LIFETIME_LONG) return "UNKNOWN";
    return ISSUE_TYPE_NAMES[type];
}

const char* util_severity_name(Severity sev) {
    if (sev < 0 || sev > SEVERITY_CRITICAL) return "UNKNOWN";
    return SEVERITY_NAMES[sev];
}

int util_strcasecmp(const char *a, const char *b) {
    return _stricmp(a, b);
}

bool util_str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return false;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return true;
    }
    return false;
}

unsigned int util_hash_string(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) hash = ((hash << 5) + hash) + (unsigned int)c;
    return hash;
}

bool util_file_exists(const char *path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path);
    return (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY));
#else
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return true; }
    return false;
#endif
}

void util_trim(char *str) {
    if (!str || !*str) return;
    char *start = str;
    while (*start && isspace((unsigned char)*start)) start++;
    if (!*start) { *str = '\0'; return; }
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    if (start != str) memmove(str, start, strlen(start) + 1);
}

void util_timestamp(char *buf, size_t len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, len, "%Y%m%d_%H%M%S", tm_info);
}

const char* util_basename(const char *path) {
    if (!path) return NULL;
    const char *last_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *name = path;
    if (last_slash && last_slash > name) name = last_slash + 1;
    if (last_backslash && last_backslash > name) name = last_backslash + 1;
    return name;
}

/* util_file_exists moved above after util_str_contains_ci */

bool util_find_file_by_name(const char *root, const char *filename, char *out, size_t out_len) {
#ifdef _WIN32
    if (!root || !filename || !out || out_len == 0) return false;
    char search_path[MAX_PATH_LEN];
    snprintf(search_path, sizeof(search_path), "%s\\*", root);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s\\%s", root, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (util_find_file_by_name(full_path, filename, out, out_len)) {
                found = true;
                break;
            }
        } else {
            if (util_strcasecmp(fd.cFileName, filename) == 0) {
                strncpy(out, full_path, out_len - 1);
                out[out_len - 1] = '\0';
                found = true;
                break;
            }
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return found;
#else
    (void)root; (void)filename; (void)out; (void)out_len;
    return false;
#endif
}

void util_index_touch(const char *filepath, FileType type) {
    if (!filepath || !*filepath) return;
    const char *index_path = "config/audit_index.csv";
    bool exists = util_file_exists(index_path);
    FILE *f = fopen(index_path, exists ? "a" : "w");
    if (!f) return;
    if (!exists) {
        fprintf(f, "path,file_type,last_touched\n");
    }
    fprintf(f, "%s,%d,%ld\n", filepath, (int)type, (long)time(NULL));
    fclose(f);
}

static bool parse_index_line(char *line, const char *filepath, long *ts_out) {
    char *c1 = strchr(line, ',');
    if (!c1) return false;
    char *c2 = strchr(c1 + 1, ',');
    if (!c2) return false;
    *c1 = '\0';
    *c2 = '\0';
    char *path = line;
    char *ts = c2 + 1;
    size_t len = strlen(ts);
    if (len > 0 && (ts[len - 1] == '\n' || ts[len - 1] == '\r')) ts[len - 1] = '\0';
    if (util_strcasecmp(path, filepath) != 0) return false;
    *ts_out = atol(ts);
    return true;
}

bool util_index_is_stale(const char *filepath, int days) {
    if (!filepath || days <= 0) return true;
    const char *index_path = "config/audit_index.csv";
    FILE *f = fopen(index_path, "r");
    if (!f) return true;
    char line[1024];
    long last_ts = 0;
    bool found = false;
    bool first = true;
    while (fgets(line, sizeof(line), f)) {
        if (first) { first = false; continue; }
        long ts = 0;
        if (parse_index_line(line, filepath, &ts)) {
            if (ts > last_ts) last_ts = ts;
            found = true;
        }
    }
    fclose(f);
    if (!found) return true;
    long now = (long)time(NULL);
    long age = now - last_ts;
    long max_age = (long)days * 86400L;
    return age > max_age;
}

// ============================================================================
// BASE SNAPSHOT: Copy server files to base/ on first download (preserves originals)
// ============================================================================
static void snapshot_recurse(const char *src, const char *dst) {
#ifdef _WIN32
    CreateDirectoryA(dst, NULL);
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", src);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char s_full[MAX_PATH_LEN], d_full[MAX_PATH_LEN];
        snprintf(s_full, sizeof(s_full), "%s\\%s", src, fd.cFileName);
        snprintf(d_full, sizeof(d_full), "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            snapshot_recurse(s_full, d_full);
        } else {
            // Only copy if base version doesn't exist yet
            if (!util_file_exists(d_full)) {
                CopyFileA(s_full, d_full, TRUE); // TRUE = don't overwrite
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
}

void util_snapshot_base_files(const char *source_root, const char *base_root) {
    if (!source_root || !base_root) return;
    util_log(SEVERITY_INFO, "Snapshot: Preserving base files to '%s'...", base_root);
#ifdef _WIN32
    CreateDirectoryA(base_root, NULL);
#endif
    snapshot_recurse(source_root, base_root);
    util_log(SEVERITY_INFO, "Snapshot: Base files preserved.");
}

// ============================================================================
// MOD MANIFEST: Generate structured inventory of all mods and their XML files
// This file is designed to be read by an LLM to understand the server layout.
// ============================================================================
static const char* filetype_label(FileType t) {
    switch(t) {
        case FILE_TYPE_ECONOMY:   return "Economy (types.xml)";
        case FILE_TYPE_SPAWNABLE: return "Spawnable (cfgspawnabletypes)";
        case FILE_TYPE_TERRITORY: return "Territory (mapgrouppos/territories)";
        case FILE_TYPE_GLOBALS:   return "Globals (globals.xml)";
        case FILE_TYPE_CONFIG:    return "Config (json/cfg/ini/txt)";
        default:                  return "Unknown";
    }
}

static void manifest_scan_dir(FILE *out, const char *path, const char *mod_name, int depth) {
#ifdef _WIN32
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s\\%s", path, fd.cFileName);
        
        // Convert to lowercase for skip checks
        char lower[MAX_PATH_LEN];
        strncpy(lower, fd.cFileName, MAX_PATH_LEN-1);
        lower[MAX_PATH_LEN-1] = '\0';
        for(int i=0; lower[i]; i++) lower[i] = tolower((unsigned char)lower[i]);
        
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Skip junk dirs
            if (strstr(lower, "addons") || strstr(lower, "keys") || strstr(lower, "key") ||
                strstr(lower, "commonredist") || strstr(lower, "steamapps") || 
                strstr(lower, "battleye") || strstr(lower, "dta")) {
                continue;
            }
            // New mod-level directory?
            if (depth == 0 && lower[0] == '@') {
                fprintf(out, "\n### Mod: `%s`\n", fd.cFileName);
                manifest_scan_dir(out, full, fd.cFileName, depth + 1);
            } else if (depth == 0 && strcmp(lower, "mpmissions") == 0) {
                fprintf(out, "\n### Vanilla Server (mpmissions)\n");
                manifest_scan_dir(out, full, "vanilla", depth + 1);
            } else {
                manifest_scan_dir(out, full, mod_name, depth + 1);
            }
        } else {
            size_t len = strlen(fd.cFileName);
            bool is_xml = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".xml") == 0);
            bool is_json = (len > 5 && util_strcasecmp(fd.cFileName + len - 5, ".json") == 0);
            bool is_cfg = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".cfg") == 0);
            bool is_txt = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".txt") == 0);
            bool is_ini = (len > 4 && util_strcasecmp(fd.cFileName + len - 4, ".ini") == 0);
            
            if (is_xml || is_json || is_cfg || is_txt || is_ini) {
                FileType type = FILE_TYPE_UNKNOWN;
                if (is_xml) type = parser_detect_file_type(full);
                else if (is_json || is_cfg || is_ini || is_txt) type = FILE_TYPE_CONFIG;
                
                fprintf(out, "- **%s** → %s | `%s`\n", fd.cFileName, filetype_label(type), full);
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#endif
}

void util_generate_mod_manifest(const char *download_root, const char *output_path) {
    if (!download_root || !output_path) return;
    FILE *out = fopen(output_path, "w");
    if (!out) return;
    
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    fprintf(out, "# Server Mod Manifest\n");
    fprintf(out, "Generated: %s\n\n", ts);
    fprintf(out, "## Purpose\n");
    fprintf(out, "This manifest lists every mod installed on the server and every config/XML file\n");
    fprintf(out, "that the mod provides. Use this to understand what is available for editing.\n\n");
    
    fprintf(out, "## XML File Types (DayZ Economy)\n");
    fprintf(out, "| Type | Root Tag | Purpose |\n");
    fprintf(out, "|------|----------|---------|\n");
    fprintf(out, "| **Economy (types.xml)** | `<types>` | Item spawn rates: nominal, min, lifetime, restock, tier, usage, flags |\n");
    fprintf(out, "| **Spawnable (cfgspawnabletypes.xml)** | `<spawnabletypes>` | Attachment/cargo presets for items when they spawn |\n");
    fprintf(out, "| **Territory (mapgrouppos.xml)** | `<territory-type>` | Map spawn point groups - where items can appear |\n");
    fprintf(out, "| **Globals (globals.xml)** | `<globals>` | Server-wide economy settings (cleanup time, spawn rates) |\n");
    fprintf(out, "| **Events (events.xml)** | `<events>` | Timed/triggered spawn events (airdrops, helicrashes) |\n");
    fprintf(out, "| **Config** | varies | Trader files (.json), server config (.cfg/.ini/.txt) |\n\n");
    
    fprintf(out, "## Important Notes for AI\n");
    fprintf(out, "- Each mod may provide its OWN types.xml with items that need to be MERGED into the server types.xml\n");
    fprintf(out, "- Mods may also provide cfgspawnabletypes.xml entries that define what spawns WITH their items\n");
    fprintf(out, "- Territory files define WHERE on the map items appear - some mods need custom territory entries\n");
    fprintf(out, "- If a mod item exists in types.xml but NOT in cfgspawnabletypes.xml, it spawns bare (no attachments)\n");
    fprintf(out, "- If a mod item exists in types.xml but has no usage/value tags, it will NEVER spawn\n");
    fprintf(out, "- The `base/` directory contains unmodified server originals - never edit those\n");
    fprintf(out, "- The `output/` directory contains merged/audited results ready for upload\n\n");
    
    fprintf(out, "## Installed Mods and Files\n");
    
    manifest_scan_dir(out, download_root, "server", 0);
    
    fprintf(out, "\n---\n");
    fprintf(out, "End of manifest. Total scan root: `%s`\n", download_root);
    fclose(out);
    
    util_log(SEVERITY_INFO, "Manifest: Generated mod inventory at '%s'", output_path);
}

bool util_read_ini_value(const char *path, const char *key, char *out, size_t out_len) {
    if (!path || !key || !out || out_len == 0) return false;
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[512];
    size_t key_len = strlen(key);
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[len - 1] = '\0';
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            strncpy(out, line + key_len + 1, out_len - 1);
            out[out_len - 1] = '\0';
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

void util_ensure_directory(const char *path) {
    if (!path || !*path) return;
#ifdef _WIN32
    // Create all intermediate directories in the path
    char buf[MAX_PATH_LEN];
    strncpy(buf, path, MAX_PATH_LEN - 1);
    buf[MAX_PATH_LEN - 1] = '\0';
    for (char *p = buf; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            if (buf[0]) CreateDirectoryA(buf, NULL);
            *p = saved;
        }
    }
    CreateDirectoryA(buf, NULL);
#endif
}

Zone util_tier_to_zone(int tier) {
    if (tier <= 0) return ZONE_UNKNOWN;
    if (tier <= 4) return ZONE_CIVILIAN;
    if (tier <= 7) return ZONE_MILITARY;
    return ZONE_ENDGAME;
}

float util_calculate_score(AuditorContext *ctx) {
    if (!ctx || ctx->item_count == 0) return 0.0f;
    float score = 100.0f;
    for (int i = 0; i < ctx->issue_count; i++) {
        AuditIssue *issue = &ctx->issues[i];
        if (issue->resolved) continue;
        switch (issue->severity) {
            case SEVERITY_INFO: score -= 0.5f; break;
            case SEVERITY_WARNING: score -= 2.0f; break;
            case SEVERITY_ERROR: score -= 5.0f; break;
            case SEVERITY_CRITICAL: score -= 10.0f; break;
        }
    }
    if (score < 0.0f) score = 0.0f;
    return score;
}

// ============================================================================
// OUTPUT DIRECTORY BACKUP & CLEANUP
// ============================================================================

// Recursively delete all FILES in a directory tree, preserving the directory structure.
static void clean_files_recurse(const char *dir_path) {
#ifdef _WIN32
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", dir_path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s\\%s", dir_path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            clean_files_recurse(full);
        } else {
            DeleteFileA(full);
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#endif
}

// Count files in a directory tree (non-recursive into subdirectories is fine, we want total file count).
static int count_files_recurse(const char *dir_path) {
    int count = 0;
#ifdef _WIN32
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", dir_path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return 0;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s\\%s", dir_path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            count += count_files_recurse(full);
        } else {
            count++;
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
#endif
    return count;
}

// Run a command line via CreateProcess with a hard timeout, WITHOUT inheriting
// handles and WITHOUT a console window. This avoids the intermittent deadlock
// that system() can hit in a WIN32 (GUI) subsystem process whose stdio handles
// were reconfigured by AllocConsole — the child would occasionally never signal
// completion, freezing the whole pipeline. Returns:
//    0  = completed with exit code 0
//   >0  = completed with a non-zero exit code
//   -1  = failed to launch
//   -2  = timed out (child was terminated)
static int util_run_cmd_timeout(const char *cmdline, DWORD timeout_ms) {
#ifdef _WIN32
    char mutable_cmd[2048];
    strncpy(mutable_cmd, cmdline, sizeof(mutable_cmd) - 1);
    mutable_cmd[sizeof(mutable_cmd) - 1] = '\0';

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    BOOL ok = CreateProcessA(
        NULL,             // parse application name from the command line
        mutable_cmd,      // command line (must be writable)
        NULL, NULL,       // default process/thread security
        FALSE,            // do NOT inherit handles — avoids console-handle deadlock
        CREATE_NO_WINDOW, // headless: no console window
        NULL, NULL,       // inherit environment and current directory
        &si, &pi);

    if (!ok) return -1;

    DWORD wait = WaitForSingleObject(pi.hProcess, timeout_ms);
    int rc;
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 2000);
        rc = -2;
    } else {
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        rc = (int)code;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return rc;
#else
    (void)timeout_ms;
    return system(cmdline);
#endif
}

bool util_backup_and_clean_output(void) {
    // Check if output/ has any files worth backing up
    int file_count = count_files_recurse("output");
    if (file_count == 0) {
        util_log(SEVERITY_INFO, "Output directory is empty — no backup needed.");
        return true;
    }

    util_log(SEVERITY_INFO, "Output directory has %d file(s) — creating backup before cleanup...", file_count);

    // Ensure output_backups/ exists
    util_ensure_directory("output_backups");

    // Generate timestamped zip filename
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char zip_name[128];
    snprintf(zip_name, sizeof(zip_name), "output_backups\\%04d%02d%02d_%02d%02d%02d.zip",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    // Use PowerShell Compress-Archive to create the zip. Launched via a
    // timeout-bounded CreateProcess (not system()) so a hung child can never
    // freeze the pipeline — worst case we skip the backup after the timeout.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
             "\"Compress-Archive -Path 'output\\*' -DestinationPath '%s' -Force\"",
             zip_name);

    util_log(SEVERITY_INFO, "Compressing output/ -> %s ...", zip_name);
    int result = util_run_cmd_timeout(cmd, 30000);  // 30s ceiling — never hang forever

    if (result == -2) {
        util_log(SEVERITY_WARNING, "Backup compression timed out (>30s) — skipped. Cleaning output/ anyway.");
    } else if (result != 0) {
        util_log(SEVERITY_WARNING, "Backup compression failed (code %d). Cleaning output/ anyway.", result);
    } else {
        util_log(SEVERITY_INFO, "Backup saved: %s", zip_name);
    }

    // Purge old output backups: keep only the 10 most recent zips
#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA("output_backups\\*.zip", &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            // Collect all zip filenames (timestamped names sort lexicographically)
            char zips[64][MAX_PATH_LEN];
            int zip_count = 0;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && zip_count < 64) {
                    snprintf(zips[zip_count], MAX_PATH_LEN, "output_backups\\%s", fd.cFileName);
                    zip_count++;
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);

            // Sort ascending (oldest first — timestamp filenames sort naturally)
            for (int i = 0; i < zip_count - 1; i++) {
                for (int j = i + 1; j < zip_count; j++) {
                    if (strcmp(zips[i], zips[j]) > 0) {
                        char tmp[MAX_PATH_LEN];
                        strncpy(tmp, zips[i], MAX_PATH_LEN);
                        strncpy(zips[i], zips[j], MAX_PATH_LEN);
                        strncpy(zips[j], tmp, MAX_PATH_LEN);
                    }
                }
            }

            // Delete oldest, keep newest 10
            int to_delete = zip_count - 10;
            for (int i = 0; i < to_delete; i++) {
                util_log(SEVERITY_INFO, "Purging old output backup: %s", zips[i]);
                DeleteFileA(zips[i]);
            }
        }
    }
#endif

    // Now clean all files from output/ but keep directory structure
    util_log(SEVERITY_INFO, "Cleaning %d file(s) from output/ ...", file_count);
    clean_files_recurse("output");
    util_log(SEVERITY_INFO, "Output directory cleaned. Ready for fresh pipeline output.");
    return true;
}

// ============================================================================
// STALE FILE QUARANTINE
// ============================================================================
// Scans output/ for files that don't match the expected pipeline outputs for the
// current SHOP_MOD configuration. Moves them to output_quarantine/YYYYMMDD_HHMMSS/
// so they aren't accidentally uploaded but remain available for debugging.
// The quarantine directory is never uploaded.

/**
 * Check if a file is an expected pipeline output.
 * Returns true if the file belongs to the current pipeline generation.
 */
static bool is_expected_output_file(const char *rel_path, ShopMod shop_mod) {
    if (!rel_path) return false;

    // Always expected: core economy files
    if (util_strcasecmp(rel_path, "merged_types.xml") == 0) return true;
    if (util_strcasecmp(rel_path, "types.xml") == 0) return true;
    if (util_strcasecmp(rel_path, "cfgspawnabletypes.xml") == 0) return true;
    if (util_strcasecmp(rel_path, "cfgeconomycore.xml") == 0) return true;
    if (util_strcasecmp(rel_path, "cfglimitsdefinitionuser.xml") == 0) return true;
    if (util_strcasecmp(rel_path, "cfgrandompresets.xml") == 0) return true;
    if (util_strcasecmp(rel_path, "SearchForLoot.json") == 0) return true;
    if (util_strcasecmp(rel_path, "items.csv") == 0) return true;
    if (util_strcasecmp(rel_path, "mod_manifest.md") == 0) return true;

    // Audit reports
    if (util_strcasecmp(rel_path, "audit_report.txt") == 0) return true;
    if (util_strcasecmp(rel_path, "audit_report_clean.txt") == 0) return true;
    if (util_strcasecmp(rel_path, "audit_report_raw.txt") == 0) return true;

    // Zombie/wildlife tiers
    if (util_str_contains_ci(rel_path, "zombie_tiers")) return true;

    // Territory files (any map)
    if (util_str_contains_ci(rel_path, "chernarusplus") ||
        util_str_contains_ci(rel_path, "enoch") ||
        util_str_contains_ci(rel_path, "sakhal")) return true;

    // Spawn templates
    if (util_str_contains_ci(rel_path, "spawn_templates")) return true;

    // Trader file — must match current SHOP_MOD
    switch (shop_mod) {
        case SHOP_MOD_TRADERPLUS:
            if (util_strcasecmp(rel_path, "TraderPlusTrading.json") == 0) return true;
            break;
        case SHOP_MOD_DRJONES:
            if (util_strcasecmp(rel_path, "TraderConfig.txt") == 0) return true;
            break;
        case SHOP_MOD_EXPANSION:
            if (util_strcasecmp(rel_path, "ExpansionMarket.json") == 0) return true;
            break;
        default:
            break;
    }

    // Per-shop files
    if (util_str_contains_ci(rel_path, "shops")) return true;

    return false;
}

/**
 * Known stale files: trader configs that DON'T match the current SHOP_MOD.
 * These are dangerous because they could be uploaded to the wrong trader mod.
 */
static bool is_wrong_trader_format(const char *filename, ShopMod shop_mod) {
    if (!filename) return false;

    // Match against all trader output filenames that are NOT the current format
    if (shop_mod != SHOP_MOD_TRADERPLUS) {
        if (util_strcasecmp(filename, "TraderPlusTrading.json") == 0) return true;
    }
    if (shop_mod != SHOP_MOD_DRJONES) {
        if (util_strcasecmp(filename, "TraderConfig.txt") == 0) return true;
    }
    if (shop_mod != SHOP_MOD_EXPANSION) {
        if (util_strcasecmp(filename, "ExpansionMarket.json") == 0) return true;
    }
    // Legacy/generic trader filenames from older versions
    if (util_strcasecmp(filename, "trader_config.json") == 0) return true;
    if (util_strcasecmp(filename, "trader_config.txt") == 0) return true;

    return false;
}

#ifdef _WIN32
static void quarantine_recurse(const char *dir_path, const char *output_root,
                                const char *quarantine_dir, ShopMod shop_mod,
                                int *moved_count) {
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", dir_path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char full[MAX_PATH_LEN];
        snprintf(full, sizeof(full), "%s\\%s", dir_path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            quarantine_recurse(full, output_root, quarantine_dir, shop_mod, moved_count);
            continue;
        }

        // Get path relative to output/ root
        const char *rel = full;
        size_t root_len = strlen(output_root);
        if (strncmp(full, output_root, root_len) == 0) {
            rel = full + root_len;
            if (*rel == '\\' || *rel == '/') rel++;
        }

        // Check: wrong trader format? (dangerous stale file)
        bool stale = is_wrong_trader_format(fd.cFileName, shop_mod);

        // Check: not an expected output file?
        if (!stale && !is_expected_output_file(rel, shop_mod)) {
            stale = true;
        }

        if (stale) {
            // Build quarantine destination path preserving subdirectory structure
            char dest[MAX_PATH_LEN];
            snprintf(dest, sizeof(dest), "%s\\%s", quarantine_dir, rel);

            // Ensure destination subdirectory exists
            char dest_dir[MAX_PATH_LEN];
            strncpy(dest_dir, dest, MAX_PATH_LEN - 1);
            dest_dir[MAX_PATH_LEN - 1] = '\0';
            char *last_sep = strrchr(dest_dir, '\\');
            if (!last_sep) last_sep = strrchr(dest_dir, '/');
            if (last_sep) {
                *last_sep = '\0';
                util_ensure_directory(dest_dir);
            }

            if (MoveFileA(full, dest)) {
                util_log(SEVERITY_WARNING, "Quarantined stale file: %s -> %s", rel, dest);
                (*moved_count)++;
            } else {
                util_log(SEVERITY_ERROR, "Failed to quarantine: %s (error %lu)", rel, GetLastError());
            }
        }
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
}
#endif

int util_quarantine_stale_output(ShopMod shop_mod) {
    int moved = 0;

#ifdef _WIN32
    // Check if output/ has any files
    int file_count = count_files_recurse("output");
    if (file_count == 0) return 0;

    // Generate timestamped quarantine directory
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char quarantine_dir[MAX_PATH_LEN];
    snprintf(quarantine_dir, sizeof(quarantine_dir),
             "output_quarantine\\%04d%02d%02d_%02d%02d%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    // Scan and quarantine stale files
    quarantine_recurse("output", "output", quarantine_dir, shop_mod, &moved);

    if (moved > 0) {
        util_log(SEVERITY_INFO, "Quarantined %d stale file(s) to %s", moved, quarantine_dir);

        // Purge old quarantine directories: keep only 10 most recent
        WIN32_FIND_DATAA fd;
        HANDLE hFind = FindFirstFileA("output_quarantine\\*", &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            char dirs[64][MAX_PATH_LEN];
            int dir_count = 0;
            do {
                if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && dir_count < 64) {
                    snprintf(dirs[dir_count], MAX_PATH_LEN, "output_quarantine\\%s", fd.cFileName);
                    dir_count++;
                }
            } while (FindNextFileA(hFind, &fd));
            FindClose(hFind);

            // Sort ascending (oldest first)
            for (int i = 0; i < dir_count - 1; i++) {
                for (int j = i + 1; j < dir_count; j++) {
                    if (strcmp(dirs[i], dirs[j]) > 0) {
                        char tmp[MAX_PATH_LEN];
                        strncpy(tmp, dirs[i], MAX_PATH_LEN);
                        strncpy(dirs[i], dirs[j], MAX_PATH_LEN);
                        strncpy(dirs[j], tmp, MAX_PATH_LEN);
                    }
                }
            }

            // Delete oldest dirs, keep newest 10
            int to_delete = dir_count - 10;
            for (int i = 0; i < to_delete; i++) {
                util_log(SEVERITY_INFO, "Purging old quarantine: %s", dirs[i]);
                // Remove files inside the quarantine dir, then the dir itself
                clean_files_recurse(dirs[i]);
                RemoveDirectoryA(dirs[i]);
            }
        }
    }
#endif

    return moved;
}
