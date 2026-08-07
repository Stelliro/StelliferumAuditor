/**
 * STELLIFERUM CLASSNAME MAP & FILE CACHE
 * ----------------------------------------
 * Two subsystems that eliminate redundant work during item loading:
 *
 * 1. ClassnameMap — open-addressing hash table (classname → item index).
 *    When parser_load_types_xml() encounters a classname that already exists,
 *    it merges into the existing LootItem instead of appending a duplicate.
 *    Reduces ~41,000 loaded items to ~8,000 by resolving duplication at
 *    parse time rather than in a separate O(n²) dedup pass.
 *
 * 2. FileCache — persistent file fingerprint index (path + file size).
 *    On subsequent loads, files whose size hasn't changed are skipped entirely.
 *    Entries for deleted files are pruned automatically.
 *    Persisted to config/file_cache.csv.
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
// CLASSNAME HASH MAP
// ============================================================================

// djb2 hash of a case-folded classname
static unsigned int hash_classname(const char *name) {
    unsigned int h = 5381;
    while (*name) {
        h = ((h << 5) + h) + (unsigned int)tolower((unsigned char)*name);
        name++;
    }
    return h;
}

void classname_map_init(AuditorContext *ctx) {
    if (!ctx) return;
    if (!ctx->classname_map) {
        ctx->classname_map = (ClassnameMap *)malloc(sizeof(ClassnameMap));
        if (!ctx->classname_map) {
            util_log(SEVERITY_ERROR, "ClassnameMap: Failed to allocate %zu bytes", sizeof(ClassnameMap));
            return;
        }
    }
    classname_map_clear(ctx);
    ctx->dedup_skipped = 0;
}

void classname_map_clear(AuditorContext *ctx) {
    if (!ctx || !ctx->classname_map) return;
    // Fill every bucket with the EMPTY sentinel
    for (int i = 0; i < CLASSNAME_MAP_BUCKETS; i++) {
        ctx->classname_map->buckets[i] = CLASSNAME_MAP_EMPTY;
    }
}

void classname_map_free(AuditorContext *ctx) {
    if (!ctx || !ctx->classname_map) return;
    free(ctx->classname_map);
    ctx->classname_map = NULL;
}

// Find item index for a classname. Returns CLASSNAME_MAP_EMPTY (-1) if not found.
int classname_map_find(AuditorContext *ctx, const char *classname) {
    if (!ctx || !ctx->classname_map || !classname || !classname[0]) return CLASSNAME_MAP_EMPTY;

    unsigned int h = hash_classname(classname);
    unsigned int mask = CLASSNAME_MAP_BUCKETS - 1;  // power-of-2 modulo
    unsigned int idx = h & mask;

    // Linear probing
    for (int probe = 0; probe < CLASSNAME_MAP_BUCKETS; probe++) {
        int item_idx = ctx->classname_map->buckets[idx];
        if (item_idx == CLASSNAME_MAP_EMPTY) {
            return CLASSNAME_MAP_EMPTY;  // Empty bucket — classname not in map
        }
        // Bucket occupied — compare classnames (case-insensitive)
        if (item_idx >= 0 && item_idx < ctx->item_count &&
            util_strcasecmp(ctx->items[item_idx].classname, classname) == 0) {
            return item_idx;
        }
        idx = (idx + 1) & mask;
    }
    return CLASSNAME_MAP_EMPTY;  // Table full (shouldn't happen with 2x sizing)
}

void classname_map_insert(AuditorContext *ctx, const char *classname, int item_index) {
    if (!ctx || !ctx->classname_map || !classname || !classname[0]) return;

    unsigned int h = hash_classname(classname);
    unsigned int mask = CLASSNAME_MAP_BUCKETS - 1;
    unsigned int idx = h & mask;

    for (int probe = 0; probe < CLASSNAME_MAP_BUCKETS; probe++) {
        int existing = ctx->classname_map->buckets[idx];
        if (existing == CLASSNAME_MAP_EMPTY) {
            ctx->classname_map->buckets[idx] = item_index;
            return;
        }
        // If same classname, update the index (shouldn't normally happen)
        if (existing >= 0 && existing < ctx->item_count &&
            util_strcasecmp(ctx->items[existing].classname, classname) == 0) {
            ctx->classname_map->buckets[idx] = item_index;
            return;
        }
        idx = (idx + 1) & mask;
    }
    // Table full — shouldn't happen
    util_log(SEVERITY_WARNING, "ClassnameMap: Hash table full, cannot insert '%s'", classname);
}


// ============================================================================
// FILE FINGERPRINT CACHE
// ============================================================================

void file_cache_load(FileCache *cache) {
    if (!cache) return;
    cache->count = 0;
    cache->loaded = true;

    FILE *f = fopen(FILE_CACHE_PATH, "r");
    if (!f) return;

    char line[MAX_PATH_LEN + 64];
    // Skip header
    if (!fgets(line, sizeof(line), f)) { fclose(f); return; }

    while (fgets(line, sizeof(line), f) && cache->count < FILE_CACHE_MAX_ENTRIES) {
        // Format: path,size,item_count
        char *comma1 = strrchr(line, ',');
        if (!comma1) continue;
        *comma1 = '\0';
        int item_count = atoi(comma1 + 1);

        char *comma2 = strrchr(line, ',');
        if (!comma2) continue;
        *comma2 = '\0';
        long size = atol(comma2 + 1);

        // Remaining is the path
        char *path = line;
        // Trim trailing whitespace from path
        size_t plen = strlen(path);
        while (plen > 0 && (path[plen - 1] == '\n' || path[plen - 1] == '\r' || path[plen - 1] == ' '))
            path[--plen] = '\0';

        if (plen == 0 || size <= 0) continue;

        FileCacheEntry *e = &cache->entries[cache->count];
        strncpy(e->path, path, MAX_PATH_LEN - 1);
        e->path[MAX_PATH_LEN - 1] = '\0';
        e->size = size;
        e->item_count = item_count;
        cache->count++;
    }

    fclose(f);
    util_log(SEVERITY_INFO, "FileCache: Loaded %d entries from %s", cache->count, FILE_CACHE_PATH);
}

void file_cache_save(const FileCache *cache) {
    if (!cache) return;

    FILE *f = fopen(FILE_CACHE_PATH, "w");
    if (!f) {
        util_log(SEVERITY_WARNING, "FileCache: Cannot write %s", FILE_CACHE_PATH);
        return;
    }

    fprintf(f, "path,size,item_count\n");
    for (int i = 0; i < cache->count; i++) {
        const FileCacheEntry *e = &cache->entries[i];
        if (e->path[0] && e->size > 0) {
            fprintf(f, "%s,%ld,%d\n", e->path, e->size, e->item_count);
        }
    }

    fclose(f);
}

void file_cache_prune(FileCache *cache) {
    if (!cache) return;
    int pruned = 0;
    int write_idx = 0;

    for (int i = 0; i < cache->count; i++) {
        if (util_file_exists(cache->entries[i].path)) {
            if (write_idx != i) {
                memcpy(&cache->entries[write_idx], &cache->entries[i], sizeof(FileCacheEntry));
            }
            write_idx++;
        } else {
            pruned++;
        }
    }

    cache->count = write_idx;
    if (pruned > 0) {
        util_log(SEVERITY_INFO, "FileCache: Pruned %d stale entries (files no longer on disk)", pruned);
    }
}

bool file_cache_is_unchanged(const FileCache *cache, const char *filepath, long file_size) {
    if (!cache || !filepath || file_size <= 0) return false;

    for (int i = 0; i < cache->count; i++) {
        const FileCacheEntry *e = &cache->entries[i];
        if (util_strcasecmp(e->path, filepath) == 0) {
            return (e->size == file_size);
        }
    }
    return false;  // Not in cache — must be loaded
}

void file_cache_update(FileCache *cache, const char *filepath, long file_size, int item_count) {
    if (!cache || !filepath) return;

    // Try to update existing entry
    for (int i = 0; i < cache->count; i++) {
        if (util_strcasecmp(cache->entries[i].path, filepath) == 0) {
            cache->entries[i].size = file_size;
            cache->entries[i].item_count = item_count;
            return;
        }
    }

    // New entry
    if (cache->count < FILE_CACHE_MAX_ENTRIES) {
        FileCacheEntry *e = &cache->entries[cache->count];
        strncpy(e->path, filepath, MAX_PATH_LEN - 1);
        e->path[MAX_PATH_LEN - 1] = '\0';
        e->size = file_size;
        e->item_count = item_count;
        cache->count++;
    }
}
