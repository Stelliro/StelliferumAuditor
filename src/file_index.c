/**
 * STELLIFERUM FILE INDEX
 * ----------------------
 * Scans and indexes the downloaded server file structure.
 * Tracks every file's path, type, mod origin, and map association.
 * This index drives the stitcher and swarm systems.
 */

#include "auditor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// HELPERS
// ============================================================================

static void str_to_lower(const char *src, char *dst, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = tolower((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

// Extract mod name from path (e.g. "downloaded_mods/@SNAFU Weapons/..." -> "@SNAFU Weapons")
static void extract_mod_name(const char *filepath, char *mod_name, size_t max) {
    mod_name[0] = '\0';
    
    // Find @ModName pattern
    const char *at = strchr(filepath, '@');
    if (at) {
        const char *end = at;
        while (*end && *end != '\\' && *end != '/') end++;
        size_t len = (size_t)(end - at);
        if (len >= max) len = max - 1;
        memcpy(mod_name, at, len);
        mod_name[len] = '\0';
        return;
    }
    
    // Check if it's a vanilla mpmissions path
    char lower[MAX_PATH_LEN];
    str_to_lower(filepath, lower, MAX_PATH_LEN);
    
    if (strstr(lower, "mpmissions")) {
        strncpy(mod_name, "vanilla", max - 1);
        return;
    }
    
    // Server root files
    strncpy(mod_name, "server_root", max - 1);
}

// ============================================================================
// MAP DETECTION
// ============================================================================

MapId file_index_detect_map(const char *filepath) {
    if (!filepath) return MAP_UNKNOWN;
    
    char lower[MAX_PATH_LEN];
    str_to_lower(filepath, lower, MAX_PATH_LEN);
    
    if (strstr(lower, "chernarusplus") || strstr(lower, "chernarus")) return MAP_CHERNARUSPLUS;
    if (strstr(lower, "enoch") || strstr(lower, "livonia")) return MAP_ENOCH;
    if (strstr(lower, "sakhal")) return MAP_SAKHAL;
    
    return MAP_UNKNOWN;
}

static const char* map_name(MapId id) {
    switch (id) {
        case MAP_CHERNARUSPLUS: return "Chernarus";
        case MAP_ENOCH: return "Livonia";
        case MAP_SAKHAL: return "Sakhal";
        default: return "Unknown";
    }
}

// ============================================================================
// SHOULD SKIP — Directories that contain no useful config/XML
// ============================================================================

static bool should_skip_dir(const char *dirname) {
    char lower[256];
    str_to_lower(dirname, lower, sizeof(lower));
    
    // Skip binary/addon/key directories
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
    if (strcmp(lower, "sorted") == 0) return true;  // Skip sorted output from file_sorter
    if (strcmp(lower, "backups") == 0) return true;  // Skip restore-point backup files
    if (strcmp(lower, "storage_1") == 0) return true; // Skip CE persistence data
    if (strcmp(lower, ".temp") == 0) return true;     // Skip temporary WinSCP scripts
    if (strcmp(lower, "output") == 0) return true;    // Skip auditor output directory
    
    return false;
}

static bool is_useful_extension(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    
    return (util_strcasecmp(ext, ".xml") == 0 ||
            util_strcasecmp(ext, ".json") == 0 ||
            util_strcasecmp(ext, ".cfg") == 0 ||
            util_strcasecmp(ext, ".ini") == 0 ||
            util_strcasecmp(ext, ".txt") == 0);
}

// ============================================================================
// RECURSIVE SCAN
// ============================================================================

#ifdef _WIN32
static void scan_recurse(FileIndex *idx, const char *path) {
    if (!idx || !path) return;
    if (idx->count >= MAX_INDEX_ENTRIES) return;
    if (strlen(path) >= MAX_PATH_LEN - 4) return;
    
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", path);
    
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        
        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s\\%s", path, fd.cFileName);
        
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!should_skip_dir(fd.cFileName)) {
                scan_recurse(idx, full_path);
            }
        } else {
            if (is_useful_extension(fd.cFileName) && idx->count < MAX_INDEX_ENTRIES) {
                FileIndexEntry *entry = &idx->entries[idx->count];
                memset(entry, 0, sizeof(FileIndexEntry));
                
                strncpy(entry->filepath, full_path, MAX_PATH_LEN - 1);
                strncpy(entry->filename, fd.cFileName, 127);
                extract_mod_name(full_path, entry->mod_name, 128);
                entry->map_id = file_index_detect_map(full_path);
                entry->is_vanilla = (strcmp(entry->mod_name, "vanilla") == 0);
                entry->is_mod = (entry->mod_name[0] == '@');
                entry->file_type = FILE_TYPE_UNKNOWN; // Classified later
                entry->processed = false;
                
                idx->count++;
            }
        }
    } while (FindNextFileA(hFind, &fd) && idx->count < MAX_INDEX_ENTRIES);
    
    FindClose(hFind);
}
#endif

// ============================================================================
// PUBLIC API
// ============================================================================

void file_index_init(FileIndex *idx) {
    if (!idx) return;
    memset(idx, 0, sizeof(FileIndex));
}

void file_index_scan(FileIndex *idx, const char *root_dir) {
    if (!idx || !root_dir) return;
    
    idx->count = 0;
    util_log(SEVERITY_INFO, "FileIndex: Scanning '%s'...", root_dir);
    
#ifdef _WIN32
    scan_recurse(idx, root_dir);
#endif
    
    util_log(SEVERITY_INFO, "FileIndex: Found %d files.", idx->count);
}

void file_index_classify(FileIndex *idx) {
    if (!idx) return;
    
    int economy = 0, spawnable = 0, territory = 0, events = 0, globals = 0, config = 0, trader = 0, randompresets = 0;
    
    for (int i = 0; i < idx->count; i++) {
        FileIndexEntry *e = &idx->entries[i];
        
        const char *ext = strrchr(e->filename, '.');
        if (ext && util_strcasecmp(ext, ".xml") == 0) {
            e->file_type = parser_detect_file_type(e->filepath);
        } else if (ext && (util_strcasecmp(ext, ".json") == 0 || util_strcasecmp(ext, ".txt") == 0)) {
            // Check if it's a trader file by name heuristic
            char lower[128];
            size_t slen = strlen(e->filename);
            if (slen >= sizeof(lower)) slen = sizeof(lower) - 1;
            for (size_t j = 0; j < slen; j++) lower[j] = tolower((unsigned char)e->filename[j]);
            lower[slen] = '\0';
            if (strstr(lower, "trader")) e->file_type = FILE_TYPE_TRADER;
            else e->file_type = FILE_TYPE_CONFIG;
        } else {
            e->file_type = FILE_TYPE_CONFIG;
        }
        
        switch (e->file_type) {
            case FILE_TYPE_ECONOMY:       economy++; break;
            case FILE_TYPE_SPAWNABLE:     spawnable++; break;
            case FILE_TYPE_TERRITORY:     territory++; break;
            case FILE_TYPE_EVENTS:        events++; break;
            case FILE_TYPE_GLOBALS:       globals++; break;
            case FILE_TYPE_CONFIG:        config++; break;
            case FILE_TYPE_TRADER:        trader++; break;
            case FILE_TYPE_RANDOMPRESETS: randompresets++; break;
            default: break;
        }
    }
    
    util_log(SEVERITY_INFO, "FileIndex: Classified — Economy:%d Spawnable:%d Territory:%d Events:%d Globals:%d Trader:%d Presets:%d Config:%d",
             economy, spawnable, territory, events, globals, trader, randompresets, config);
}

int file_index_count_by_type(FileIndex *idx, FileType type) {
    if (!idx) return 0;
    int count = 0;
    for (int i = 0; i < idx->count; i++) {
        if (idx->entries[i].file_type == type) count++;
    }
    return count;
}

int file_index_count_by_mod(FileIndex *idx, const char *mod_name) {
    if (!idx || !mod_name) return 0;
    int count = 0;
    for (int i = 0; i < idx->count; i++) {
        if (util_strcasecmp(idx->entries[i].mod_name, mod_name) == 0) count++;
    }
    return count;
}

FileIndexEntry* file_index_find(FileIndex *idx, const char *filename, FileType type) {
    if (!idx || !filename) return NULL;
    for (int i = 0; i < idx->count; i++) {
        FileIndexEntry *e = &idx->entries[i];
        if (e->file_type == type && util_strcasecmp(e->filename, filename) == 0) {
            return e;
        }
    }
    return NULL;
}

void file_index_print_summary(FileIndex *idx) {
    if (!idx) return;
    
    util_log(SEVERITY_INFO, "=== FILE INDEX SUMMARY ===");
    util_log(SEVERITY_INFO, "Total Files: %d", idx->count);
    util_log(SEVERITY_INFO, "Economy (types.xml):     %d", file_index_count_by_type(idx, FILE_TYPE_ECONOMY));
    util_log(SEVERITY_INFO, "Spawnable:               %d", file_index_count_by_type(idx, FILE_TYPE_SPAWNABLE));
    util_log(SEVERITY_INFO, "Territory:               %d", file_index_count_by_type(idx, FILE_TYPE_TERRITORY));
    util_log(SEVERITY_INFO, "Events:                  %d", file_index_count_by_type(idx, FILE_TYPE_EVENTS));
    util_log(SEVERITY_INFO, "Globals:                 %d", file_index_count_by_type(idx, FILE_TYPE_GLOBALS));
    util_log(SEVERITY_INFO, "Trader:                  %d", file_index_count_by_type(idx, FILE_TYPE_TRADER));
    util_log(SEVERITY_INFO, "Random Presets:           %d", file_index_count_by_type(idx, FILE_TYPE_RANDOMPRESETS));
    util_log(SEVERITY_INFO, "Config:                  %d", file_index_count_by_type(idx, FILE_TYPE_CONFIG));
    
    // Count unique mods
    char seen_mods[64][128];
    int mod_count = 0;
    for (int i = 0; i < idx->count && mod_count < 64; i++) {
        bool found = false;
        for (int j = 0; j < mod_count; j++) {
            if (util_strcasecmp(seen_mods[j], idx->entries[i].mod_name) == 0) { found = true; break; }
        }
        if (!found) {
            strncpy(seen_mods[mod_count++], idx->entries[i].mod_name, 127);
        }
    }
    
    util_log(SEVERITY_INFO, "Unique Sources: %d", mod_count);
    for (int i = 0; i < mod_count; i++) {
        util_log(SEVERITY_INFO, "  [%s] → %d files", seen_mods[i], file_index_count_by_mod(idx, seen_mods[i]));
    }
    util_log(SEVERITY_INFO, "==========================");
}
