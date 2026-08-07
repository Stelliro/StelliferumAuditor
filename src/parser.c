
#include "auditor.h"
#include "loot_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ============================================================================
// MINI XML READER
// ============================================================================
typedef struct { const char *data; size_t pos; size_t len; } XmlReader;
static void xml_init(XmlReader *r, const char *d, size_t l) { r->data=d; r->pos=0; r->len=l; }
static char xml_peek(XmlReader *r) { return (r->pos >= r->len) ? '\0' : r->data[r->pos]; }
static void xml_advance(XmlReader *r) { if (r->pos < r->len) r->pos++; }

static void xml_read_tag_name(XmlReader *r, char *buf, int max) {
    int i = 0;
    while (r->pos < r->len && i < max - 1) {
        char c = r->data[r->pos];
        if (isspace((unsigned char)c) || c == '>' || c == '/') break;
        buf[i++] = c;
        r->pos++;
    }
    buf[i] = '\0';
}

static bool xml_find_attr(const char *tag_content, const char *attr_name, char *out_val, int max_len) {
    if (!tag_content || !attr_name || !out_val || max_len <= 0) return false;
    const char *found = strstr(tag_content, attr_name);
    if (!found) return false;
    found += strlen(attr_name);
    while (*found && *found != '"') found++;
    if (*found != '"') return false;
    found++; 
    int i = 0;
    while (*found && *found != '"' && i < max_len - 1) {
        out_val[i++] = *found++;
    }
    out_val[i] = '\0';
    return (i > 0);
}

static void xml_read_text(XmlReader *r, char *buf, int max) {
    int i = 0;
    while (r->pos < r->len && i < max - 1) {
        char c = r->data[r->pos];
        if (c == '<') break;
        buf[i++] = c;
        r->pos++;
    }
    buf[i] = '\0';
    // Trim whitespace
    while (i > 0 && isspace((unsigned char)buf[i-1])) buf[--i] = '\0';
    char *s = buf;
    while (*s && isspace((unsigned char)*s)) s++;
    if (s != buf) memmove(buf, s, strlen(s) + 1);
}

// Check if tag_content represents a self-closing tag (ends with /)
static bool xml_is_self_closing(const char *tag_content) {
    if (!tag_content) return false;
    size_t len = strlen(tag_content);
    while (len > 0 && isspace((unsigned char)tag_content[len-1])) len--;
    return (len > 0 && tag_content[len-1] == '/');
}

static const char* get_filename(const char* path) {
    const char *last_slash = strrchr(path, '/');
    const char *last_backslash = strrchr(path, '\\');
    const char *last = (last_slash > last_backslash) ? last_slash : last_backslash;
    return last ? last + 1 : path;
}

// Extract @ModName from filepath, e.g. "downloaded_mods/@SNAFU Weapons/types.xml" -> "@SNAFU Weapons"
// Also handles sorted file paths: "sorted/types/@Mass_sManyItemOverhaul__MMIO_types.xml" -> "@Mass_sManyItemOverhaul"
static void parser_extract_mod_name(const char *filepath, char *mod_name, size_t max) {
    mod_name[0] = '\0';
    const char *at = strchr(filepath, '@');
    if (at) {
        const char *end = at;
        while (*end && *end != '\\' && *end != '/') end++;
        size_t len = (size_t)(end - at);
        if (len >= max) len = max - 1;
        memcpy(mod_name, at, len);
        mod_name[len] = '\0';

        // Sorted file convention: @ModPrefix__filename.xml
        // Truncate at double-underscore to get the real mod name
        char *dunder = strstr(mod_name, "__");
        if (dunder) *dunder = '\0';

        // Sanitize apostrophes to underscores (matches file_sorter behavior)
        for (size_t i = 0; mod_name[i]; i++) {
            if (mod_name[i] == '\'') mod_name[i] = '_';
        }
        return;
    }
    // Check for vanilla mpmissions path
    const char *lower_check = filepath;
    bool found_mpmissions = false;
    for (const char *p = lower_check; *p; p++) {
        if ((*p == 'm' || *p == 'M') && util_strcasecmp(p, "mpmissions") >= 0) {
            char buf[12];
            for (int i = 0; i < 10 && p[i]; i++) buf[i] = (char)tolower((unsigned char)p[i]);
            buf[10] = '\0';
            if (strncmp(buf, "mpmissions", 10) == 0) { found_mpmissions = true; break; }
        }
    }
    if (found_mpmissions) { strncpy(mod_name, "vanilla", max - 1); mod_name[max-1] = '\0'; return; }
    strncpy(mod_name, "server_root", max - 1); mod_name[max-1] = '\0';
}

// ============================================================================
// FILE TYPE DETECTION
// ============================================================================
FileType parser_detect_file_type(const char *filepath) {
    if (!filepath) return FILE_TYPE_UNKNOWN;
    FILE *f = fopen(filepath, "rb");
    if (!f) return FILE_TYPE_UNKNOWN;
    char data[2049];
    size_t bytes = fread(data, 1, 2048, f);
    data[bytes] = '\0';
    fclose(f);

    const char *source_name = get_filename(filepath);
    FileType detected_type = FILE_TYPE_UNKNOWN;

    // Content-based detection (most reliable)
    size_t scan_pos = 0;
    while (scan_pos < bytes && scan_pos + 1 < 2048) {
        if (data[scan_pos] == '<' && data[scan_pos + 1] != '?' && data[scan_pos + 1] != '!') {
            if (strncmp(data + scan_pos, "<types>", 7) == 0 || strncmp(data + scan_pos, "<types ", 7) == 0) {
                detected_type = FILE_TYPE_ECONOMY; break;
            }
            if (strncmp(data + scan_pos, "<spawnabletypes>", 16) == 0 || strncmp(data + scan_pos, "<spawnabletypes ", 16) == 0) {
                detected_type = FILE_TYPE_SPAWNABLE; break;
            }
            if (strncmp(data + scan_pos, "<territory-type>", 16) == 0 || strncmp(data + scan_pos, "<territory-type ", 16) == 0) {
                detected_type = FILE_TYPE_TERRITORY; break;
            }
            if (strncmp(data + scan_pos, "<events>", 8) == 0 || strncmp(data + scan_pos, "<events ", 8) == 0) {
                detected_type = FILE_TYPE_EVENTS; break;
            }
            if (strncmp(data + scan_pos, "<globals>", 9) == 0) {
                detected_type = FILE_TYPE_GLOBALS; break;
            }
            if (strncmp(data + scan_pos, "<randompresets>", 15) == 0 || strncmp(data + scan_pos, "<randompresets ", 15) == 0) {
                detected_type = FILE_TYPE_RANDOMPRESETS; break;
            }
            if (strncmp(data + scan_pos, "<Trader>", 8) == 0 || strncmp(data + scan_pos, "<Trader ", 8) == 0 ||
                strncmp(data + scan_pos, "<trader>", 8) == 0 || strncmp(data + scan_pos, "<trader ", 8) == 0) {
                detected_type = FILE_TYPE_TRADER; break;
            }
        }
        scan_pos++;
    }

    // Filename heuristic fallback
    if (detected_type == FILE_TYPE_UNKNOWN && source_name) {
        char lower[128];
        size_t slen = strlen(source_name);
        if (slen >= sizeof(lower)) slen = sizeof(lower) - 1;
        for (size_t i = 0; i < slen; i++) lower[i] = tolower((unsigned char)source_name[i]);
        lower[slen] = '\0';

        if (strstr(lower, "types") && !strstr(lower, "spawnable")) detected_type = FILE_TYPE_ECONOMY;
        else if (strstr(lower, "spawnabletype") || strstr(lower, "cfgspawnable")) detected_type = FILE_TYPE_SPAWNABLE;
        else if (strstr(lower, "territor")) detected_type = FILE_TYPE_TERRITORY;
        else if (strstr(lower, "events")) detected_type = FILE_TYPE_EVENTS;
        else if (strstr(lower, "globals")) detected_type = FILE_TYPE_GLOBALS;
        else if (strstr(lower, "trader")) detected_type = FILE_TYPE_TRADER;
        else if (strstr(lower, "randompreset")) detected_type = FILE_TYPE_RANDOMPRESETS;
    }

    // Extension fallback for non-XML files
    if (detected_type == FILE_TYPE_UNKNOWN) {
        const char *ext = strrchr(filepath, '.');
        if (ext) {
            if (util_strcasecmp(ext, ".json") == 0 || util_strcasecmp(ext, ".cfg") == 0 ||
                util_strcasecmp(ext, ".ini") == 0 || util_strcasecmp(ext, ".txt") == 0) {
                detected_type = FILE_TYPE_CONFIG;
            }
        }
    }

    return detected_type;
}

// ============================================================================
// TYPES.XML PARSER — Fixed to correctly read attribute-based tags
// ============================================================================
bool parser_load_types_xml(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        util_log(SEVERITY_WARNING, "Parser: Cannot open '%s'", filepath);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 50 * 1024 * 1024) { // Safety: max 50MB
        fclose(f);
        util_log(SEVERITY_WARNING, "Parser: File too large or empty: '%s' (%ld bytes)", filepath, fsize);
        return false;
    }

    // File cache check — skip files whose size hasn't changed since last load.
    // This avoids re-parsing unchanged files entirely.
    if (ctx->file_cache.loaded && file_cache_is_unchanged(&ctx->file_cache, filepath, fsize)) {
        fclose(f);
        return true;  // Already loaded previously, nothing changed
    }

    fseek(f, 0, SEEK_SET);
    char *data = (char*)malloc(fsize + 1);
    if (!data) { fclose(f); return false; }
    size_t read_bytes = fread(data, 1, fsize, f);
    data[read_bytes] = '\0';
    fclose(f);
    
    const char *source_name = get_filename(filepath);
    FileType detected_type = parser_detect_file_type(filepath);
    
    if (detected_type != FILE_TYPE_ECONOMY) {
        util_index_touch(filepath, detected_type);
        free(data);
        return true;
    }

    XmlReader r;
    xml_init(&r, data, (size_t)fsize);
    LootItem *current_item = NULL;
    char tag[64];
    int items_loaded = 0;
    
    while (r.pos < r.len) {
        if (xml_peek(&r) == '<') {
            xml_advance(&r);
            
            // Skip processing instructions and comments
            if (xml_peek(&r) == '?' || xml_peek(&r) == '!') {
                while (r.pos < r.len && r.data[r.pos] != '>') r.pos++;
                xml_advance(&r);
                continue;
            }
            
            // Closing tag
            if (xml_peek(&r) == '/') { 
                xml_advance(&r);
                xml_read_tag_name(&r, tag, 64);
                if (strcmp(tag, "type") == 0) current_item = NULL;
                while (r.pos < r.len && r.data[r.pos] != '>') r.pos++;
                xml_advance(&r);
                continue;
            }
            
            // Opening tag — capture full tag content
            size_t start = r.pos;
            while (r.pos < r.len && r.data[r.pos] != '>') r.pos++;
            size_t end = r.pos;
            
            char tag_content[512];
            size_t len = (end - start < sizeof(tag_content) - 1) ? (end - start) : sizeof(tag_content) - 1;
            memcpy(tag_content, r.data + start, len);
            tag_content[len] = '\0';
            
            bool self_closing = xml_is_self_closing(tag_content);
            
            // Re-read tag name
            r.pos = start; 
            xml_read_tag_name(&r, tag, 64);
            
            // Advance past '>'
            r.pos = end;
            if (r.pos < r.len) r.pos++;
            
            // <type name="...">  —  INLINE CLASSNAME DEDUP
            // If the classname already exists in the hash map, point current_item
            // at the EXISTING entry for merging instead of creating a duplicate.
            if (strcmp(tag, "type") == 0) {
                char name_val[128];
                bool has_name = xml_find_attr(tag_content, "name", name_val, 128);

                // Check if classname already loaded via the hash map
                int existing_idx = CLASSNAME_MAP_EMPTY;
                if (has_name && ctx->classname_map) {
                    existing_idx = classname_map_find(ctx, name_val);
                }

                if (existing_idx != CLASSNAME_MAP_EMPTY) {
                    // Duplicate — merge into existing item instead of appending.
                    // Point current_item at the existing entry so subsequent tags
                    // (nominal, lifetime, flags, etc.) overwrite with latest values.
                    current_item = &ctx->items[existing_idx];
                    // Reset usage/value arrays — last file wins, consistent with
                    // scalar fields. Without this, N mod files defining the same
                    // classname accumulate N× duplicate usage/value tags, causing
                    // the DayZ CE to heap-corrupt on types.xml load.
                    current_item->usage_count = 0;
                    current_item->value_count = 0;
                    ctx->dedup_skipped++;
                } else if (ctx->item_count < MAX_ITEMS) {
                    current_item = &ctx->items[ctx->item_count];
                    memset(current_item, 0, sizeof(LootItem));
                    strncpy(current_item->mod_source, source_name, 63);
                    parser_extract_mod_name(filepath, current_item->mod_name, sizeof(current_item->mod_name));
                    current_item->file_type = detected_type;
                    
                    if (has_name) {
                        strncpy(current_item->classname, name_val, MAX_CLASSNAME_LEN - 1);
                        // Register in hash map for future dedup
                        if (ctx->classname_map) {
                            classname_map_insert(ctx, name_val, ctx->item_count);
                        }
                    }
                    ctx->item_count++;
                    items_loaded++;
                } else {
                    current_item = NULL;
                    util_log(SEVERITY_WARNING, "Parser: MAX_ITEMS (%d) reached, skipping remaining items in %s", MAX_ITEMS, source_name);
                }
                continue;
            }
            
            // Skip if we don't have an active item
            if (!current_item) continue;
            
            // === ATTRIBUTE-BASED TAGS (self-closing: <tag name="value"/>) ===
            if (strcmp(tag, "flags") == 0) {
                char val[16];
                if (xml_find_attr(tag_content, "count_in_cargo", val, 16) && atoi(val)) current_item->flags |= FLAG_CARGO;
                if (xml_find_attr(tag_content, "count_in_hoarder", val, 16) && atoi(val)) current_item->flags |= FLAG_HOARDER;
                if (xml_find_attr(tag_content, "count_in_map", val, 16) && atoi(val)) current_item->flags |= FLAG_MAP;
                if (xml_find_attr(tag_content, "count_in_player", val, 16) && atoi(val)) current_item->flags |= FLAG_PLAYER;
                if (xml_find_attr(tag_content, "crafted", val, 16) && atoi(val)) current_item->flags |= FLAG_CRAFTED;
                if (xml_find_attr(tag_content, "deloot", val, 16) && atoi(val)) current_item->flags |= FLAG_DELOOT;
                continue;
            }
            
            // [FIX] category, usage, value tags use name= attribute (self-closing)
            if (strcmp(tag, "category") == 0) {
                char attr_val[MAX_CATEGORY_LEN];
                if (xml_find_attr(tag_content, "name", attr_val, MAX_CATEGORY_LEN)) {
                    strncpy(current_item->category, attr_val, MAX_CATEGORY_LEN - 1);
                } else if (!self_closing) {
                    // Fallback: read text content for non-self-closing tags
                    char content[64];
                    xml_read_text(&r, content, 64);
                    if (content[0]) strncpy(current_item->category, content, MAX_CATEGORY_LEN - 1);
                }
                continue;
            }
            
            if (strcmp(tag, "usage") == 0) {
                char attr_val[MAX_USAGE_LEN];
                if (xml_find_attr(tag_content, "name", attr_val, MAX_USAGE_LEN)) {
                    if (current_item->usage_count < MAX_USAGES_PER_TIER)
                        strncpy(current_item->usages[current_item->usage_count++], attr_val, MAX_USAGE_LEN - 1);
                } else if (!self_closing) {
                    char content[64];
                    xml_read_text(&r, content, 64);
                    if (content[0] && current_item->usage_count < MAX_USAGES_PER_TIER)
                        strncpy(current_item->usages[current_item->usage_count++], content, MAX_USAGE_LEN - 1);
                }
                continue;
            }
            
            if (strcmp(tag, "value") == 0) {
                char attr_val[MAX_VALUE_LEN];
                if (xml_find_attr(tag_content, "name", attr_val, MAX_VALUE_LEN)) {
                    if (current_item->value_count < MAX_VALUES_PER_ITEM)
                        strncpy(current_item->values[current_item->value_count++], attr_val, MAX_VALUE_LEN - 1);
                } else if (!self_closing) {
                    char content[64];
                    xml_read_text(&r, content, 64);
                    if (content[0] && current_item->value_count < MAX_VALUES_PER_ITEM)
                        strncpy(current_item->values[current_item->value_count++], content, MAX_VALUE_LEN - 1);
                }
                continue;
            }
            
            // === TEXT-CONTENT TAGS (e.g. <nominal>10</nominal>) ===
            if (!self_closing) {
                char content[64];
                xml_read_text(&r, content, 64);
                
                if (strcmp(tag, "nominal") == 0) current_item->nominal = atoi(content);
                else if (strcmp(tag, "lifetime") == 0) current_item->lifetime = atoi(content);
                else if (strcmp(tag, "restock") == 0) current_item->restock = atoi(content);
                else if (strcmp(tag, "min") == 0) current_item->min = atoi(content);
                else if (strcmp(tag, "quantmin") == 0) current_item->quantmin = atoi(content);
                else if (strcmp(tag, "quantmax") == 0) current_item->quantmax = atoi(content);
                else if (strcmp(tag, "cost") == 0) current_item->cost = atoi(content);
            }
            continue;
        }
        xml_advance(&r);
    }
    
    free(data);
    util_index_touch(filepath, detected_type);

    // Update file fingerprint cache so this file is skipped on next load if unchanged
    file_cache_update(&ctx->file_cache, filepath, fsize, items_loaded);

    util_log(SEVERITY_INFO, "Loaded: %s (%d new items)", source_name, items_loaded);
    return true;
}

// ============================================================================
// SPAWNABLE XML PARSER — Loads raw blocks for stitching
// ============================================================================
bool parser_load_spawnable_xml(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 50 * 1024 * 1024) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    char *data = (char*)malloc(fsize + 1);
    if (!data) { fclose(f); return false; }
    fread(data, 1, fsize, f);
    data[fsize] = '\0';
    fclose(f);
    
    const char *source_name = get_filename(filepath);
    
    // Find each <type ...>...</type> block and store raw XML
    const char *pos = data;
    int loaded = 0;
    while ((pos = strstr(pos, "<type ")) != NULL) {
        const char *end = strstr(pos, "</type>");
        if (!end) break;
        end += 7; // past </type>
        
        size_t block_len = (size_t)(end - pos);
        if (ctx->spawn_block_count < MAX_SPAWNABLE_BLOCKS && block_len < MAX_SPAWNABLE_BLOCK_LEN - 1) {
            SpawnableBlock *block = &ctx->spawn_blocks[ctx->spawn_block_count++];
            memset(block, 0, sizeof(SpawnableBlock));
            
            // Extract classname from name= attribute
            char name_buf[MAX_CLASSNAME_LEN];
            memset(name_buf, 0, sizeof(name_buf));
            const char *name_start = strstr(pos, "name=");
            if (name_start) {
                name_start = strchr(name_start, '"');
                if (name_start) {
                    name_start++;
                    const char *name_end = strchr(name_start, '"');
                    if (name_end) {
                        size_t nlen = (size_t)(name_end - name_start);
                        if (nlen >= MAX_CLASSNAME_LEN) nlen = MAX_CLASSNAME_LEN - 1;
                        memcpy(block->classname, name_start, nlen);
                        block->classname[nlen] = '\0';
                    }
                }
            }

            // Skip zombie/animal entries — they do not belong in cfgspawnabletypes.
            // Having them here causes the CE to spawn zombies/animals as loot items.
            LootItem probe = {0};
            strncpy(probe.classname, block->classname, MAX_CLASSNAME_LEN - 1);
            if (loot_is_zombie(&probe) || loot_is_animal(&probe)) {
                ctx->spawn_block_count--;  // undo the pre-increment
                pos = end;
                continue;
            }

            strncpy(block->source_file, source_name, 63);
            memcpy(block->raw_xml, pos, block_len);
            block->raw_xml[block_len] = '\0';
            loaded++;
        }
        
        pos = end;
    }
    
    free(data);
    util_log(SEVERITY_INFO, "Parsed spawnable: %s (%d blocks)", source_name, loaded);
    return true;
}

bool parser_load_known_items(AuditorContext *ctx, const char *filepath) { return true; }
