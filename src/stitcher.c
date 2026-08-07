/**
 * STELLIFERUM STITCHER
 * --------------------
 * Intelligently merges XML files by type:
 * - Territory files: Mod territories OVERLAY vanilla (combined, larger file)
 * - Spawnable types: Mod spawnables APPEND to vanilla
 * - Types/Economy: Mod items MERGE with vanilla (already handled by auditor)
 * - Events/Globals: NOT merged (kept per-map, per-source)
 *
 * KEY RULE: Types and territory files are DIFFERENT and must NOT be stitched
 * together. Each file type has its own merge strategy.
 *
 * TERRITORY MERGE: The vanilla bear_territories.xml + any mod bear_territories.xml
 * entries = a combined output that is LARGER than vanilla alone.
 * Multiple territory files exist (bear, wolf, zombie, etc.) — each is merged separately.
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

static void str_lower(const char *src, char *dst, size_t max) {
    size_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = tolower((unsigned char)src[i]);
        i++;
    }
    dst[i] = '\0';
}

static const char* filename_only(const char *path) {
    const char *s = strrchr(path, '\\');
    const char *f = strrchr(path, '/');
    const char *last = (s > f) ? s : f;
    return last ? last + 1 : path;
}

// Return true if filename looks like a numbered backup artifact
// e.g. "003_remote_root_mpmissions_..._bear_territories.xml"
static bool is_backup_artifact(const char *filename) {
    if (!filename) return false;
    // Backup files start with digits followed by underscore(s)
    int digit_count = 0;
    for (int i = 0; filename[i] && filename[i] != '_'; i++) {
        if (filename[i] >= '0' && filename[i] <= '9') digit_count++;
        else return false;
    }
    return (digit_count >= 2 && filename[digit_count] == '_');
}

// Extract the "group key" from a territory/spawnable filename
// e.g. "bear_territories.xml" -> "bear_territories"
// e.g. "cfgspawnabletypes.xml" -> "cfgspawnabletypes"
static void extract_group_key(const char *filename, char *key, size_t max) {
    strncpy(key, filename, max - 1);
    key[max - 1] = '\0';
    char *dot = strrchr(key, '.');
    if (dot) *dot = '\0';
    // Lowercase for matching
    for (size_t i = 0; key[i]; i++) key[i] = tolower((unsigned char)key[i]);
}

// ============================================================================
// INIT
// ============================================================================

void stitcher_init(AuditorContext *ctx) {
    if (!ctx) return;
    ctx->stitch_group_count = 0;
    memset(ctx->stitch_groups, 0, sizeof(ctx->stitch_groups));
}

// ============================================================================
// BUILD STITCH GROUPS — Analyze the file index to create merge groups
// ============================================================================

static StitchGroup* find_or_create_group(AuditorContext *ctx, const char *group_name, FileType type, MapId map) {
    // Search existing
    for (int i = 0; i < ctx->stitch_group_count; i++) {
        StitchGroup *g = &ctx->stitch_groups[i];
        if (g->file_type == type && g->map_id == map && util_strcasecmp(g->group_name, group_name) == 0) {
            return g;
        }
    }
    // Create new
    if (ctx->stitch_group_count >= MAX_STITCH_GROUPS) return NULL;
    StitchGroup *g = &ctx->stitch_groups[ctx->stitch_group_count++];
    memset(g, 0, sizeof(StitchGroup));
    strncpy(g->group_name, group_name, 127);
    g->file_type = type;
    g->map_id = map;
    return g;
}

void stitcher_build_groups(AuditorContext *ctx) {
    if (!ctx) return;
    
    stitcher_init(ctx);
    FileIndex *idx = &ctx->file_index;
    
    util_log(SEVERITY_INFO, "Stitcher: Building merge groups from %d indexed files...", idx->count);
    
    for (int i = 0; i < idx->count; i++) {
        FileIndexEntry *e = &idx->entries[i];
        
        // Only process territory and spawnable files for stitching
        if (e->file_type != FILE_TYPE_TERRITORY && e->file_type != FILE_TYPE_SPAWNABLE) continue;

        // Skip backup artifacts that leaked into the index
        if (is_backup_artifact(e->filename)) {
            util_log(SEVERITY_WARNING, "Stitcher: Skipping backup artifact '%s'", e->filename);
            continue;
        }
        
        char key[128];
        extract_group_key(e->filename, key, sizeof(key));
        
        // Override map_id for territory files with known map-specific names.
        // Prevents cross-map contamination when files were downloaded to the wrong map dir.
        // reindeer_territories = Sakhal only; wildboar_territories (no underscore) = Enoch only.
        // wild_boar_territories (WITH underscore) IS Chernarus — different animal/file.
        MapId effective_map = e->map_id;
        if (e->file_type == FILE_TYPE_TERRITORY) {
            if (util_strcasecmp(key, "reindeer_territories") == 0) effective_map = MAP_SAKHAL;
            else if (util_strcasecmp(key, "wildboar_territories") == 0) effective_map = MAP_ENOCH;
        }
        
        StitchGroup *group = find_or_create_group(ctx, key, e->file_type, effective_map);
        if (!group) continue;
        
        if (e->is_vanilla) {
            // This is the base vanilla file
            strncpy(group->vanilla_path, e->filepath, MAX_PATH_LEN - 1);
        } else if (e->is_mod) {
            // This is a mod overlay
            if (group->mod_count < 32) {
                strncpy(group->mod_paths[group->mod_count++], e->filepath, MAX_PATH_LEN - 1);
            }
        }
    }
    
    // Build output paths
    for (int i = 0; i < ctx->stitch_group_count; i++) {
        StitchGroup *g = &ctx->stitch_groups[i];
        const char *map_dir = "chernarusplus";
        if (g->map_id == MAP_ENOCH) map_dir = "enoch";
        else if (g->map_id == MAP_SAKHAL) map_dir = "sakhal";
        else if (g->map_id == MAP_CHERNARUSPLUS) map_dir = "chernarusplus";
        
        if (g->file_type == FILE_TYPE_TERRITORY) {
            snprintf(g->output_path, MAX_PATH_LEN, "output/%s/env/%s.xml", map_dir, g->group_name);
        } else if (g->file_type == FILE_TYPE_SPAWNABLE) {
            snprintf(g->output_path, MAX_PATH_LEN, "output/%s/%s.xml", map_dir, g->group_name);
        }
    }
    
    util_log(SEVERITY_INFO, "Stitcher: Created %d merge groups.", ctx->stitch_group_count);
    
    // Log summary
    int territory_groups = 0, spawnable_groups = 0;
    for (int i = 0; i < ctx->stitch_group_count; i++) {
        StitchGroup *g = &ctx->stitch_groups[i];
        if (g->file_type == FILE_TYPE_TERRITORY) territory_groups++;
        else if (g->file_type == FILE_TYPE_SPAWNABLE) spawnable_groups++;
        
        if (g->mod_count > 0) {
            util_log(SEVERITY_INFO, "  [%s] vanilla:%s + %d mod overlays",
                     g->group_name, g->vanilla_path[0] ? "YES" : "NO", g->mod_count);
        }
    }
    util_log(SEVERITY_INFO, "Stitcher: %d territory groups, %d spawnable groups", territory_groups, spawnable_groups);
}

// ============================================================================
// XML MERGE — Appends content from overlay into base file
// ============================================================================

// Read file into malloc'd buffer, returns NULL on failure
static char* read_file_full(const char *path, long *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 100 * 1024 * 1024) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *data = (char*)malloc(fsize + 1);
    if (!data) { fclose(f); return NULL; }
    fread(data, 1, fsize, f);
    data[fsize] = '\0';
    fclose(f);
    if (out_size) *out_size = fsize;
    return data;
}

// Find the LAST occurrence of a closing tag like </territory-type>
static const char* find_last_close_tag(const char *data, const char *tag) {
    char search[128];
    snprintf(search, sizeof(search), "</%s>", tag);
    
    const char *last = NULL;
    const char *pos = data;
    while ((pos = strstr(pos, search)) != NULL) {
        last = pos;
        pos += strlen(search);
    }
    return last;
}

// Extract all inner content between <root_tag> and </root_tag>
static char* extract_inner_content(const char *data, const char *root_tag, long *out_len) {
    char open_tag1[128], open_tag2[128], close_tag[128];
    snprintf(open_tag1, sizeof(open_tag1), "<%s>", root_tag);
    snprintf(open_tag2, sizeof(open_tag2), "<%s ", root_tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", root_tag);
    
    const char *start = strstr(data, open_tag1);
    if (!start) start = strstr(data, open_tag2);
    if (!start) return NULL;
    
    // Move past the opening tag
    start = strchr(start, '>');
    if (!start) return NULL;
    start++;
    
    const char *end = find_last_close_tag(data, root_tag);
    if (!end || end <= start) return NULL;
    
    long len = (long)(end - start);
    char *content = (char*)malloc(len + 1);
    if (!content) return NULL;
    memcpy(content, start, len);
    content[len] = '\0';
    if (out_len) *out_len = len;
    return content;
}

bool stitcher_merge_xml_file(const char *base_path, const char *overlay_path, const char *output_path, const char *root_tag) {
    if (!base_path || !overlay_path || !output_path || !root_tag) return false;
    
    long base_size = 0, overlay_size = 0;
    char *base_data = read_file_full(base_path, &base_size);
    char *overlay_data = read_file_full(overlay_path, &overlay_size);
    
    if (!base_data && !overlay_data) return false;
    
    // If no base, just copy overlay
    if (!base_data && overlay_data) {
        FILE *f = fopen(output_path, "wb");
        if (f) { fwrite(overlay_data, 1, overlay_size, f); fclose(f); }
        free(overlay_data);
        return true;
    }
    
    // If no overlay, just copy base
    if (base_data && !overlay_data) {
        FILE *f = fopen(output_path, "wb");
        if (f) { fwrite(base_data, 1, base_size, f); fclose(f); }
        free(base_data);
        return true;
    }
    
    // Extract inner content from overlay
    long inner_len = 0;
    char *inner = extract_inner_content(overlay_data, root_tag, &inner_len);
    free(overlay_data);
    
    if (!inner || inner_len <= 0) {
        // Nothing to merge, just copy base
        FILE *f = fopen(output_path, "wb");
        if (f) { fwrite(base_data, 1, base_size, f); fclose(f); }
        free(base_data);
        free(inner);
        return true;
    }
    
    // Find insertion point in base (just before the closing tag)
    char close_tag[128];
    snprintf(close_tag, sizeof(close_tag), "</%s>", root_tag);
    const char *insert_point = find_last_close_tag(base_data, root_tag);
    
    if (!insert_point) {
        free(base_data);
        free(inner);
        return false;
    }
    
    // Write merged file
    FILE *f = fopen(output_path, "wb");
    if (!f) {
        free(base_data);
        free(inner);
        return false;
    }
    
    // Write base content up to the closing tag
    long prefix_len = (long)(insert_point - base_data);
    fwrite(base_data, 1, prefix_len, f);
    
    // Insert mod content
    fprintf(f, "\n    <!-- === MOD OVERLAY: %s === -->\n", filename_only(overlay_path));
    fwrite(inner, 1, inner_len, f);
    fprintf(f, "\n    <!-- === END MOD OVERLAY === -->\n");
    
    // Write the closing tag and anything after
    fwrite(insert_point, 1, base_size - prefix_len, f);
    
    fclose(f);
    free(base_data);
    free(inner);
    
    return true;
}

// ============================================================================
// MERGE TERRITORIES
// ============================================================================

static void ensure_dir_recursive(const char *path) {
#ifdef _WIN32
    char temp[MAX_PATH_LEN];
    strncpy(temp, path, MAX_PATH_LEN - 1);
    temp[MAX_PATH_LEN - 1] = '\0';
    
    for (char *p = temp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char saved = *p;
            *p = '\0';
            CreateDirectoryA(temp, NULL);
            *p = saved;
        }
    }
    CreateDirectoryA(temp, NULL);
#endif
}

void stitcher_merge_territories(AuditorContext *ctx, const char *output_dir) {
    if (!ctx || !output_dir) return;
    
    util_log(SEVERITY_INFO, "Stitcher: Merging territory files...");
    int merged = 0;
    
    for (int i = 0; i < ctx->stitch_group_count; i++) {
        StitchGroup *g = &ctx->stitch_groups[i];
        if (g->file_type != FILE_TYPE_TERRITORY) continue;
        if (g->vanilla_path[0] == '\0') continue; // No vanilla base
        if (g->mod_count == 0) continue; // Nothing to merge
        
        // Ensure output directory exists
        char dir_part[MAX_PATH_LEN];
        strncpy(dir_part, g->output_path, MAX_PATH_LEN - 1);
        char *last_sep = strrchr(dir_part, '/');
        if (!last_sep) last_sep = strrchr(dir_part, '\\');
        if (last_sep) { *last_sep = '\0'; ensure_dir_recursive(dir_part); }
        
        // Start with vanilla as base, merge each mod overlay sequentially
        // First mod merges with vanilla -> temp1
        // Second mod merges with temp1 -> temp2
        // etc.
        char current_base[MAX_PATH_LEN];
        strncpy(current_base, g->vanilla_path, MAX_PATH_LEN - 1);
        
        for (int m = 0; m < g->mod_count; m++) {
            char temp_out[MAX_PATH_LEN];
            if (m == g->mod_count - 1) {
                // Last merge goes to final output
                strncpy(temp_out, g->output_path, MAX_PATH_LEN - 1);
            } else {
                snprintf(temp_out, MAX_PATH_LEN, "%s_merge_temp_%d.xml", g->output_path, m);
            }
            
            if (stitcher_merge_xml_file(current_base, g->mod_paths[m], temp_out, "territory-type")) {
                util_log(SEVERITY_INFO, "  Merged: %s + %s -> %s",
                         filename_only(current_base), filename_only(g->mod_paths[m]), filename_only(temp_out));
                strncpy(current_base, temp_out, MAX_PATH_LEN - 1);
                merged++;
            } else {
                util_log(SEVERITY_WARNING, "  Failed to merge: %s + %s",
                         filename_only(current_base), filename_only(g->mod_paths[m]));
            }
        }
        
        // Clean up temp files
        for (int m = 0; m < g->mod_count - 1; m++) {
            char temp[MAX_PATH_LEN];
            snprintf(temp, MAX_PATH_LEN, "%s_merge_temp_%d.xml", g->output_path, m);
            remove(temp);
        }
    }
    
    // Also copy any vanilla territory files that have NO mod overlays (preserve them)
    for (int i = 0; i < ctx->stitch_group_count; i++) {
        StitchGroup *g = &ctx->stitch_groups[i];
        if (g->file_type != FILE_TYPE_TERRITORY) continue;
        if (g->vanilla_path[0] == '\0') continue;
        if (g->mod_count > 0) continue; // Already merged above
        
        // Just copy vanilla to output
        char dir_part[MAX_PATH_LEN];
        strncpy(dir_part, g->output_path, MAX_PATH_LEN - 1);
        char *last_sep = strrchr(dir_part, '/');
        if (!last_sep) last_sep = strrchr(dir_part, '\\');
        if (last_sep) { *last_sep = '\0'; ensure_dir_recursive(dir_part); }
        
        long size = 0;
        char *data = read_file_full(g->vanilla_path, &size);
        if (data) {
            FILE *f = fopen(g->output_path, "wb");
            if (f) { fwrite(data, 1, size, f); fclose(f); }
            free(data);
        }
    }
    
    util_log(SEVERITY_INFO, "Stitcher: Completed %d territory merges.", merged);
}

// ============================================================================
// MERGE SPAWNABLES
// ============================================================================

void stitcher_merge_spawnables(AuditorContext *ctx, const char *output_dir) {
    if (!ctx || !output_dir) return;
    
    util_log(SEVERITY_INFO, "Stitcher: Merging spawnable type files...");
    int merged = 0;
    
    for (int i = 0; i < ctx->stitch_group_count; i++) {
        StitchGroup *g = &ctx->stitch_groups[i];
        if (g->file_type != FILE_TYPE_SPAWNABLE) continue;
        if (g->vanilla_path[0] == '\0') continue;
        if (g->mod_count == 0) continue;
        
        char dir_part[MAX_PATH_LEN];
        strncpy(dir_part, g->output_path, MAX_PATH_LEN - 1);
        char *last_sep = strrchr(dir_part, '/');
        if (!last_sep) last_sep = strrchr(dir_part, '\\');
        if (last_sep) { *last_sep = '\0'; ensure_dir_recursive(dir_part); }
        
        char current_base[MAX_PATH_LEN];
        strncpy(current_base, g->vanilla_path, MAX_PATH_LEN - 1);
        
        for (int m = 0; m < g->mod_count; m++) {
            char temp_out[MAX_PATH_LEN];
            if (m == g->mod_count - 1) {
                strncpy(temp_out, g->output_path, MAX_PATH_LEN - 1);
            } else {
                snprintf(temp_out, MAX_PATH_LEN, "%s_merge_temp_%d.xml", g->output_path, m);
            }
            
            if (stitcher_merge_xml_file(current_base, g->mod_paths[m], temp_out, "spawnabletypes")) {
                util_log(SEVERITY_INFO, "  Merged spawnable: %s + %s",
                         filename_only(current_base), filename_only(g->mod_paths[m]));
                strncpy(current_base, temp_out, MAX_PATH_LEN - 1);
                merged++;
            }
        }
        
        for (int m = 0; m < g->mod_count - 1; m++) {
            char temp[MAX_PATH_LEN];
            snprintf(temp, MAX_PATH_LEN, "%s_merge_temp_%d.xml", g->output_path, m);
            remove(temp);
        }
    }
    
    util_log(SEVERITY_INFO, "Stitcher: Completed %d spawnable merges.", merged);
}

// ============================================================================
// MERGE ALL — Master entry point
// ============================================================================

void stitcher_merge_all(AuditorContext *ctx, const char *output_dir) {
    if (!ctx || !output_dir) return;
    
    util_log(SEVERITY_INFO, "Stitcher: === BEGIN FULL MERGE ===");
    
    // Ensure output directories exist
    util_ensure_directory(output_dir);
    
    char subdir[MAX_PATH_LEN];
    snprintf(subdir, sizeof(subdir), "%s/chernarusplus", output_dir);
    util_ensure_directory(subdir);
    snprintf(subdir, sizeof(subdir), "%s/chernarusplus/env", output_dir);
    util_ensure_directory(subdir);
    snprintf(subdir, sizeof(subdir), "%s/enoch", output_dir);
    util_ensure_directory(subdir);
    snprintf(subdir, sizeof(subdir), "%s/enoch/env", output_dir);
    util_ensure_directory(subdir);
    snprintf(subdir, sizeof(subdir), "%s/sakhal", output_dir);
    util_ensure_directory(subdir);
    snprintf(subdir, sizeof(subdir), "%s/sakhal/env", output_dir);
    util_ensure_directory(subdir);
    
    // 1. Merge territory files (mod territories overlay vanilla)
    stitcher_merge_territories(ctx, output_dir);
    
    // 2. Merge spawnable types
    stitcher_merge_spawnables(ctx, output_dir);
    
    // 3. Types merging is handled by the existing writer_export_merged_xml
    
    util_log(SEVERITY_INFO, "Stitcher: === MERGE COMPLETE ===");
}
