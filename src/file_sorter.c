/**
 * STELLIFERUM FILE SORTER
 * -----------------------
 * Runs IMMEDIATELY after FTP download, before any parsing.
 *
 * Problem:  Mod authors name their files inconsistently:
 *   "SNAFU_types.xml", "mmg_types_alpine.xml", "Morty_types.xml",
 *   "snafuspawnabletypes25percent.xml", "TacticalFlava_spawnabletypes.xml",
 *   "SnafuTraderPlus.json", "snafu_trader_config.xml", "types(NOT A REPLACER).xml"
 *
 * Solution: Read the FIRST ~2KB of every file, detect the root XML tag or JSON
 *           structure, and copy the file into an organized directory:
 *     sorted/types/          — All economy types files  (FILE_TYPE_ECONOMY)
 *     sorted/spawnabletypes/ — All spawnable type files  (FILE_TYPE_SPAWNABLE)
 *     sorted/trader/         — All trader configs         (FILE_TYPE_TRADER)
 *     sorted/territory/      — All territory files        (FILE_TYPE_TERRITORY)
 *     sorted/events/         — All event files            (FILE_TYPE_EVENTS)
 *     sorted/globals/        — All globals files          (FILE_TYPE_GLOBALS)
 *     sorted/randompresets/  — All random preset files    (FILE_TYPE_RANDOMPRESETS)
 *     sorted/config/         — Other config files         (FILE_TYPE_CONFIG)
 *     sorted/unknown/        — Unclassified files
 *
 * Files are copied (not moved) so the originals remain intact. Each copy is
 * prefixed with the mod name for traceability:
 *   sorted/types/@SNAFU_Weapons__SNAFU_types.xml
 *   sorted/types/@MMG__mmg_types_alpine.xml
 *
 * After sorting, the pipeline loads files from sorted/ directories instead of
 * hunting through the raw download tree.
 */

#include "auditor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// ============================================================================
// HELPERS
// ============================================================================

static void str_lower(const char *src, char *dst, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = tolower((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

static const char* get_filename(const char *path) {
    const char *s = strrchr(path, '\\');
    const char *f = strrchr(path, '/');
    const char *last = (s > f) ? s : f;
    return last ? last + 1 : path;
}

// Extract @ModName from path, sanitized for use in filenames (spaces -> _)
static void extract_mod_prefix(const char *filepath, char *prefix, size_t max) {
    prefix[0] = '\0';
    const char *at = strchr(filepath, '@');
    if (at) {
        const char *end = at;
        while (*end && *end != '\\' && *end != '/') end++;
        size_t len = (size_t)(end - at);
        if (len >= max) len = max - 1;
        memcpy(prefix, at, len);
        prefix[len] = '\0';
        // Replace spaces with underscores for filename safety
        for (size_t i = 0; prefix[i]; i++) {
            if (prefix[i] == ' ') prefix[i] = '_';
            if (prefix[i] == '\'') prefix[i] = '_';
        }
        return;
    }
    // Check for vanilla path
    char lower[MAX_PATH_LEN];
    str_lower(filepath, lower, MAX_PATH_LEN);
    if (strstr(lower, "mpmissions")) {
        strncpy(prefix, "vanilla", max - 1);
    } else {
        strncpy(prefix, "server", max - 1);
    }
}

// ============================================================================
// CONTENT-BASED FILE CLASSIFICATION
// ============================================================================
// This is the heart of the sorter. It reads the start of each file and
// determines its type by examining:
//   1. Root XML tag (most reliable)
//   2. JSON structure keys
//   3. Filename heuristics (fallback)
//
// This catches files like "SNAFU_types.xml" that parser_detect_file_type()
// already handles via content, but we also add trader detection here.
// ============================================================================

static FileType classify_file_by_content(const char *filepath) {
    if (!filepath) return FILE_TYPE_UNKNOWN;

    FILE *f = fopen(filepath, "rb");
    if (!f) return FILE_TYPE_UNKNOWN;
    
    char data[4097];
    size_t bytes = fread(data, 1, 4096, f);
    data[bytes] = '\0';
    fclose(f);

    if (bytes == 0) return FILE_TYPE_UNKNOWN;

    // ---- XML content detection (scan for root element) ----
    for (size_t i = 0; i < bytes && i + 1 < 4096; i++) {
        if (data[i] == '<' && data[i + 1] != '?' && data[i + 1] != '!' && data[i + 1] != '/') {
            // Types / Economy
            if (strncmp(data + i, "<types>", 7) == 0 || strncmp(data + i, "<types ", 7) == 0)
                return FILE_TYPE_ECONOMY;
            // Spawnable types
            if (strncmp(data + i, "<spawnabletypes>", 16) == 0 || strncmp(data + i, "<spawnabletypes ", 16) == 0)
                return FILE_TYPE_SPAWNABLE;
            // Territory
            if (strncmp(data + i, "<territory-type>", 16) == 0 || strncmp(data + i, "<territory-type ", 16) == 0)
                return FILE_TYPE_TERRITORY;
            // Events
            if (strncmp(data + i, "<events>", 8) == 0 || strncmp(data + i, "<events ", 8) == 0)
                return FILE_TYPE_EVENTS;
            // Globals
            if (strncmp(data + i, "<globals>", 9) == 0 || strncmp(data + i, "<globals ", 9) == 0)
                return FILE_TYPE_GLOBALS;
            // Random presets
            if (strncmp(data + i, "<randompresets>", 15) == 0 || strncmp(data + i, "<randompresets ", 15) == 0)
                return FILE_TYPE_RANDOMPRESETS;
            // Economy core
            if (strncmp(data + i, "<economycore>", 13) == 0 || strncmp(data + i, "<economycore ", 13) == 0)
                return FILE_TYPE_CONFIG;
            // cfgenvironment, cfgeventgroups, cfgeventspawns, cfgplayerspawnpoints, etc.
            if (strncmp(data + i, "<env>", 5) == 0 || strncmp(data + i, "<env ", 5) == 0)
                return FILE_TYPE_CONFIG;
            // Trader XML format (Dr. Jones style — <Trader> root or contains TraderCategories)
            if (strncmp(data + i, "<Trader>", 8) == 0 || strncmp(data + i, "<Trader ", 8) == 0 ||
                strncmp(data + i, "<trader>", 8) == 0 || strncmp(data + i, "<trader ", 8) == 0)
                return FILE_TYPE_TRADER;
        }
    }

    // ---- JSON content detection ----
    const char *ext = strrchr(filepath, '.');
    if (ext && util_strcasecmp(ext, ".json") == 0) {
        // Lowercase the data for searching
        char lower_data[4097];
        str_lower(data, lower_data, sizeof(lower_data));

        // TraderPlus format: contains "traderplustrading", "products", "tradercategories"
        if (strstr(lower_data, "traderplustrading") || strstr(lower_data, "traderplus") ||
            strstr(lower_data, "\"products\"") || strstr(lower_data, "tradercategor"))
            return FILE_TYPE_TRADER;

        // Expansion trader format: contains "expansiontrader" or "items" with "ClassName" + "buyPrice"
        if (strstr(lower_data, "expansiontrader") ||
            (strstr(lower_data, "classname") && (strstr(lower_data, "buyprice") || strstr(lower_data, "sellprice"))))
            return FILE_TYPE_TRADER;

        // General config JSON
        return FILE_TYPE_CONFIG;
    }

    // ---- TXT trader detection (Dr. Jones / standard trader format) ----
    if (ext && util_strcasecmp(ext, ".txt") == 0) {
        char lower_data[4097];
        str_lower(data, lower_data, sizeof(lower_data));
        // Dr. Jones trader .txt files contain <Trader> or category headers with classname,qty,buy,sell
        if (strstr(lower_data, "<trader>") || strstr(lower_data, "<category>") ||
            (strstr(lower_data, "traderconfig") || strstr(lower_data, "trader")) && strstr(lower_data, ",")) {
            return FILE_TYPE_TRADER;
        }
        return FILE_TYPE_CONFIG;
    }

    // ---- Filename heuristic fallback (when content wasn't conclusive) ----
    const char *filename = get_filename(filepath);
    char lower_name[256];
    str_lower(filename, lower_name, sizeof(lower_name));

    // Trader keywords in filename
    if (strstr(lower_name, "trader")) return FILE_TYPE_TRADER;

    // Types keywords in filename (but NOT spawnable which also contains "types")
    if (strstr(lower_name, "types") && !strstr(lower_name, "spawnable"))
        return FILE_TYPE_ECONOMY;
    
    // Spawnable keywords
    if (strstr(lower_name, "spawnable") || strstr(lower_name, "cfgspawnable"))
        return FILE_TYPE_SPAWNABLE;

    // Territory keywords
    if (strstr(lower_name, "territor"))
        return FILE_TYPE_TERRITORY;

    // Event keywords
    if (strstr(lower_name, "events"))
        return FILE_TYPE_EVENTS;

    // Globals keywords
    if (strstr(lower_name, "globals"))
        return FILE_TYPE_GLOBALS;

    // Random presets
    if (strstr(lower_name, "randompreset"))
        return FILE_TYPE_RANDOMPRESETS;

    // Extension-based fallback for config
    if (ext) {
        if (util_strcasecmp(ext, ".xml") == 0 || util_strcasecmp(ext, ".cfg") == 0 || 
            util_strcasecmp(ext, ".ini") == 0)
            return FILE_TYPE_CONFIG;
    }

    return FILE_TYPE_UNKNOWN;
}

static const char* filetype_dirname(FileType type) {
    switch (type) {
        case FILE_TYPE_ECONOMY:        return "types";
        case FILE_TYPE_SPAWNABLE:      return "spawnabletypes";
        case FILE_TYPE_TRADER:         return "trader";
        case FILE_TYPE_TERRITORY:      return "territory";
        case FILE_TYPE_EVENTS:         return "events";
        case FILE_TYPE_GLOBALS:        return "globals";
        case FILE_TYPE_RANDOMPRESETS:  return "randompresets";
        case FILE_TYPE_CONFIG:         return "config";
        default:                       return "unknown";
    }
}

static const char* filetype_label(FileType type) {
    switch (type) {
        case FILE_TYPE_ECONOMY:        return "Types/Economy";
        case FILE_TYPE_SPAWNABLE:      return "Spawnable Types";
        case FILE_TYPE_TRADER:         return "Trader Config";
        case FILE_TYPE_TERRITORY:      return "Territory";
        case FILE_TYPE_EVENTS:         return "Events";
        case FILE_TYPE_GLOBALS:        return "Globals";
        case FILE_TYPE_RANDOMPRESETS:  return "Random Presets";
        case FILE_TYPE_CONFIG:         return "Config";
        default:                       return "Unknown";
    }
}

// ============================================================================
// COPY FILE HELPER
// ============================================================================

static bool copy_file(const char *src, const char *dst) {
#ifdef _WIN32
    return CopyFileA(src, dst, FALSE) != 0;
#else
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return true;
#endif
}

// ============================================================================
// SKIP PATTERNS — files/dirs we should never sort
// ============================================================================

static bool should_skip_dir(const char *dirname) {
    char lower[256];
    str_lower(dirname, lower, sizeof(lower));
    if (strcmp(lower, "addons") == 0) return true;
    if (strcmp(lower, "keys") == 0 || strcmp(lower, "key") == 0) return true;
    if (strcmp(lower, "_commonredist") == 0) return true;
    if (strcmp(lower, "steamapps") == 0) return true;
    if (strcmp(lower, "battleye") == 0) return true;
    if (strcmp(lower, "dta") == 0) return true;
    if (strcmp(lower, "logs") == 0) return true;
    if (strcmp(lower, "server_manager") == 0) return true;
    if (strcmp(lower, "datacache") == 0) return true;
    if (strcmp(lower, "users") == 0) return true;
    if (strcmp(lower, "sorted") == 0) return true;  // Don't re-sort our own output!
    return false;
}

static bool is_sortable_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    return (util_strcasecmp(ext, ".xml") == 0 ||
            util_strcasecmp(ext, ".json") == 0 ||
            util_strcasecmp(ext, ".cfg") == 0 ||
            util_strcasecmp(ext, ".ini") == 0 ||
            util_strcasecmp(ext, ".txt") == 0);
}

// ============================================================================
// RECURSIVE SCAN + SORT
// ============================================================================

typedef struct {
    int types;
    int spawnable;
    int trader;
    int territory;
    int events;
    int globals;
    int randompresets;
    int config;
    int unknown;
    int total;
} SortCounts;

static void sort_one_file(const char *full_path, const char *filename,
                          const char *sorted_root, SortCounts *counts) {
    FileType type;
    char mod_prefix[128];
    const char *category_dir;
    char dest_dir[MAX_PATH_LEN];
    char dest_file[MAX_PATH_LEN];

    if (!is_sortable_extension(filename)) return;

    type = classify_file_by_content(full_path);
    extract_mod_prefix(full_path, mod_prefix, sizeof(mod_prefix));
    category_dir = filetype_dirname(type);

    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", sorted_root, category_dir);
    util_ensure_directory(dest_dir);
    snprintf(dest_file, sizeof(dest_file), "%s/%s__%s", dest_dir, mod_prefix, filename);

    if (copy_file(full_path, dest_file)) {
        util_log(SEVERITY_INFO, "Sorted: [%s] %s -> %s/%s__%s",
                 filetype_label(type), filename, category_dir, mod_prefix, filename);
        counts->total++;
        switch (type) {
            case FILE_TYPE_ECONOMY:       counts->types++; break;
            case FILE_TYPE_SPAWNABLE:     counts->spawnable++; break;
            case FILE_TYPE_TRADER:        counts->trader++; break;
            case FILE_TYPE_TERRITORY:     counts->territory++; break;
            case FILE_TYPE_EVENTS:        counts->events++; break;
            case FILE_TYPE_GLOBALS:       counts->globals++; break;
            case FILE_TYPE_RANDOMPRESETS: counts->randompresets++; break;
            case FILE_TYPE_CONFIG:        counts->config++; break;
            default:                      counts->unknown++; break;
        }
    } else {
        util_log(SEVERITY_WARNING, "Sorter: Failed to copy '%s'", full_path);
    }
}

#ifdef _WIN32
static void sort_recurse(const char *path, const char *sorted_root, SortCounts *counts) {
    if (!path || !sorted_root || !counts) return;
    if (strlen(path) >= MAX_PATH_LEN - 4) return;

    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", path);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        char full_path[MAX_PATH_LEN];
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        snprintf(full_path, sizeof(full_path), "%s\\%s", path, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!should_skip_dir(fd.cFileName))
                sort_recurse(full_path, sorted_root, counts);
        } else {
            sort_one_file(full_path, fd.cFileName, sorted_root, counts);
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
}
#else
static void sort_recurse(const char *path, const char *sorted_root, SortCounts *counts) {
    DIR *dir;
    struct dirent *ent;
    if (!path || !sorted_root || !counts) return;
    if (strlen(path) >= MAX_PATH_LEN - 4) return;

    dir = opendir(path);
    if (!dir) return;

    while ((ent = readdir(dir)) != NULL) {
        char full_path[MAX_PATH_LEN];
        struct stat st;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
        if (stat(full_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (!should_skip_dir(ent->d_name))
                sort_recurse(full_path, sorted_root, counts);
        } else if (S_ISREG(st.st_mode)) {
            sort_one_file(full_path, ent->d_name, sorted_root, counts);
        }
    }
    closedir(dir);
}
#endif

// ============================================================================
// PUBLIC API
// ============================================================================

void file_sorter_run(AuditorContext *ctx, const char *download_root, const char *sorted_root) {
    if (!ctx || !download_root || !sorted_root) return;

    util_log(SEVERITY_INFO, "=== FILE SORTER: Classifying downloaded files ===");
    util_log(SEVERITY_INFO, "Source: %s", download_root);
    util_log(SEVERITY_INFO, "Target: %s", sorted_root);

    // Create all sorted subdirectories
    util_ensure_directory(sorted_root);
    
    const char *subdirs[] = {
        "types", "spawnabletypes", "trader", "territory",
        "events", "globals", "randompresets", "config", "unknown"
    };
    for (int i = 0; i < 9; i++) {
        char dir[MAX_PATH_LEN];
        snprintf(dir, sizeof(dir), "%s/%s", sorted_root, subdirs[i]);
        util_ensure_directory(dir);
    }

    SortCounts counts = {0};

    sort_recurse(download_root, sorted_root, &counts);

    // Store results in context
    ctx->sorted_types_count = counts.types;
    ctx->sorted_spawnable_count = counts.spawnable;
    ctx->sorted_trader_count = counts.trader;
    ctx->sorted_territory_count = counts.territory;
    ctx->sorted_events_count = counts.events;
    ctx->sorted_globals_count = counts.globals;
    ctx->sorted_config_count = counts.config;
    ctx->sorted_randompresets_count = counts.randompresets;
    ctx->sorted_unknown_count = counts.unknown;
    ctx->sorted_total_count = counts.total;
    ctx->files_sorted = true;

    util_log(SEVERITY_INFO, "=== FILE SORTER COMPLETE ===");
    util_log(SEVERITY_INFO, "  Types/Economy:    %d files", counts.types);
    util_log(SEVERITY_INFO, "  Spawnable Types:  %d files", counts.spawnable);
    util_log(SEVERITY_INFO, "  Trader Configs:   %d files", counts.trader);
    util_log(SEVERITY_INFO, "  Territory:        %d files", counts.territory);
    util_log(SEVERITY_INFO, "  Events:           %d files", counts.events);
    util_log(SEVERITY_INFO, "  Globals:          %d files", counts.globals);
    util_log(SEVERITY_INFO, "  Random Presets:   %d files", counts.randompresets);
    util_log(SEVERITY_INFO, "  Config/Other:     %d files", counts.config);
    util_log(SEVERITY_INFO, "  Unknown:          %d files", counts.unknown);
    util_log(SEVERITY_INFO, "  TOTAL SORTED:     %d files", counts.total);
}

void file_sorter_print_summary(AuditorContext *ctx, const char *sorted_root) {
    if (!ctx || !ctx->files_sorted) {
        util_log(SEVERITY_WARNING, "File Sorter: No sort results available.");
        return;
    }

    util_log(SEVERITY_INFO, "=== SORTED FILE SUMMARY ===");
    util_log(SEVERITY_INFO, "  Types/Economy:    %d", ctx->sorted_types_count);
    util_log(SEVERITY_INFO, "  Spawnable Types:  %d", ctx->sorted_spawnable_count);
    util_log(SEVERITY_INFO, "  Trader Configs:   %d", ctx->sorted_trader_count);
    util_log(SEVERITY_INFO, "  Territory:        %d", ctx->sorted_territory_count);
    util_log(SEVERITY_INFO, "  Events:           %d", ctx->sorted_events_count);
    util_log(SEVERITY_INFO, "  Globals:          %d", ctx->sorted_globals_count);
    util_log(SEVERITY_INFO, "  Random Presets:   %d", ctx->sorted_randompresets_count);
    util_log(SEVERITY_INFO, "  Config:           %d", ctx->sorted_config_count);
    util_log(SEVERITY_INFO, "  Unknown:          %d", ctx->sorted_unknown_count);
    util_log(SEVERITY_INFO, "  TOTAL:            %d", ctx->sorted_total_count);

    // Write a manifest file into the sorted root
    if (sorted_root) {
        char manifest_path[MAX_PATH_LEN];
        snprintf(manifest_path, sizeof(manifest_path), "%s\\SORT_MANIFEST.txt", sorted_root);
        FILE *f = fopen(manifest_path, "w");
        if (f) {
            fprintf(f, "STELLIFERUM FILE SORTER MANIFEST\n");
            fprintf(f, "================================\n\n");
            fprintf(f, "Types/Economy:    %d files\n", ctx->sorted_types_count);
            fprintf(f, "Spawnable Types:  %d files\n", ctx->sorted_spawnable_count);
            fprintf(f, "Trader Configs:   %d files\n", ctx->sorted_trader_count);
            fprintf(f, "Territory:        %d files\n", ctx->sorted_territory_count);
            fprintf(f, "Events:           %d files\n", ctx->sorted_events_count);
            fprintf(f, "Globals:          %d files\n", ctx->sorted_globals_count);
            fprintf(f, "Random Presets:   %d files\n", ctx->sorted_randompresets_count);
            fprintf(f, "Config:           %d files\n", ctx->sorted_config_count);
            fprintf(f, "Unknown:          %d files\n", ctx->sorted_unknown_count);
            fprintf(f, "TOTAL:            %d files\n", ctx->sorted_total_count);

            // List files in each sorted directory
            const char *subdirs[] = {
                "types", "spawnabletypes", "trader", "territory",
                "events", "globals", "randompresets", "config", "unknown"
            };
            for (int d = 0; d < 9; d++) {
                char dir_path[MAX_PATH_LEN];
                snprintf(dir_path, sizeof(dir_path), "%s\\%s", sorted_root, subdirs[d]);

                fprintf(f, "\n--- %s/ ---\n", subdirs[d]);

#ifdef _WIN32
                char search[MAX_PATH_LEN];
                snprintf(search, sizeof(search), "%s\\*", dir_path);
                WIN32_FIND_DATAA fd;
                HANDLE hFind = FindFirstFileA(search, &fd);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (fd.cFileName[0] == '.') continue;
                        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            fprintf(f, "  %s\n", fd.cFileName);
                        }
                    } while (FindNextFileA(hFind, &fd));
                    FindClose(hFind);
                }
#endif
            }

            fclose(f);
            util_log(SEVERITY_INFO, "Wrote sort manifest: %s", manifest_path);
        }
    }
}
