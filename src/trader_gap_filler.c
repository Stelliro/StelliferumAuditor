/**
 * STELLIFERUM TRADER GAP FILLER
 * -----------------------------
 * After the auditor has parsed all economy items and the file sorter has
 * organized trader files into sorted/trader/, this module:
 *
 * 1. Scans sorted/trader/ for existing TraderPlus JSON files
 * 2. Extracts every ClassName already listed in those trader files
 * 3. Compares against the auditor's item list
 * 4. For any item NOT in any trader file, generates store data (price/category)
 *    so the final TraderPlus export includes everything
 *
 * Decision: We use TraderPlus (JSON) format because:
 *   - The codebase already generates TraderPlusTrading.json
 *   - server_paths.ini points to profiles/TraderPlus/
 *   - Multiple mods ship TraderPlus files (SNAFU, Expansion, etc.)
 *   - JSON is more structured and easier to generate than Dr. Jones .txt
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
// JSON CLASSNAME EXTRACTOR
// ============================================================================
// Quick scan for "ClassName":"..." or "classname":"..." patterns in JSON data.
// We don't need a full JSON parser — just extract classname strings.
// ============================================================================

static void extract_classnames_from_json(const char *data, size_t data_len,
                                          char classnames[][MAX_CLASSNAME_LEN], int *count, int max_count) {
    if (!data || !classnames || !count) return;

    // Search for ClassName patterns (case-insensitive)
    const char *pos = data;
    while (pos && *pos && *count < max_count) {
        // Find next "ClassName" or "classname" key
        const char *key = NULL;
        const char *p = pos;
        while (*p) {
            if (*p == '"') {
                p++;
                // Check if this key is "ClassName" (case-insensitive)
                if ((p[0] == 'C' || p[0] == 'c') &&
                    (p[1] == 'l' || p[1] == 'L') &&
                    (p[2] == 'a' || p[2] == 'A') &&
                    (p[3] == 's' || p[3] == 'S') &&
                    (p[4] == 's' || p[4] == 'S') &&
                    (p[5] == 'N' || p[5] == 'n') &&
                    (p[6] == 'a' || p[6] == 'A') &&
                    (p[7] == 'm' || p[7] == 'M') &&
                    (p[8] == 'e' || p[8] == 'E') &&
                    p[9] == '"') {
                    key = p + 9; // past closing quote
                    break;
                }
                // Skip to end of string
                while (*p && *p != '"') {
                    if (*p == '\\') p++; // skip escaped char
                    if (*p) p++;
                }
                if (*p == '"') p++;
                continue;
            }
            p++;
        }

        if (!key) break;

        // Find the value after the colon
        const char *colon = strchr(key, ':');
        if (!colon) break;
        colon++;
        while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r') colon++;

        if (*colon == '"') {
            colon++; // skip opening quote
            const char *end = strchr(colon, '"');
            if (end && (size_t)(end - colon) < MAX_CLASSNAME_LEN - 1 && end > colon) {
                size_t len = (size_t)(end - colon);
                memcpy(classnames[*count], colon, len);
                classnames[*count][len] = '\0';
                (*count)++;
            }
            pos = end ? end + 1 : NULL;
        } else {
            pos = colon + 1;
        }
    }
}

// Also handle Dr. Jones trader XML format: extracts classnames from trader XML
static void extract_classnames_from_trader_xml(const char *data, size_t data_len,
                                                char classnames[][MAX_CLASSNAME_LEN], int *count, int max_count) {
    // Look for classname patterns in trader XML (various formats)
    // Dr. Jones: <Row ... ClassName="ItemName" .../>
    // Or simple CSV-like: ItemName,qty,buy,sell
    const char *pos = data;
    
    // Try to find ClassName= attributes in XML
    while (pos && *count < max_count) {
        const char *cn = strstr(pos, "ClassName=");
        if (!cn) cn = strstr(pos, "classname=");
        if (!cn) cn = strstr(pos, "className=");
        if (!cn) break;

        cn += 10; // skip "ClassName="
        if (*cn == '"') {
            cn++;
            const char *end = strchr(cn, '"');
            if (end && (size_t)(end - cn) < MAX_CLASSNAME_LEN - 1 && end > cn) {
                size_t len = (size_t)(end - cn);
                memcpy(classnames[*count], cn, len);
                classnames[*count][len] = '\0';
                (*count)++;
            }
            pos = end ? end + 1 : NULL;
        } else {
            pos = cn + 1;
        }
    }
}

// Extract classnames from Dr. Jones .txt format
// Format: <Category> CategoryName
//   ClassName, Quantity, BuyPrice, SellPrice
static void extract_classnames_from_trader_txt(const char *data, size_t data_len,
                                                char classnames[][MAX_CLASSNAME_LEN], int *count, int max_count) {
    const char *line = data;
    while (line && *line && *count < max_count) {
        // Skip whitespace
        while (*line == ' ' || *line == '\t') line++;

        // Skip lines starting with < (category headers), # (comments), or empty
        if (*line == '<' || *line == '#' || *line == '\n' || *line == '\r' || *line == '\0') {
            // Advance to next line
            const char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : NULL;
            continue;
        }

        // Extract the classname (first token before comma or whitespace)
        char name[MAX_CLASSNAME_LEN];
        int i = 0;
        while (line[i] && line[i] != ',' && line[i] != '\t' && line[i] != ' ' && 
               line[i] != '\n' && line[i] != '\r' && i < MAX_CLASSNAME_LEN - 1) {
            name[i] = line[i];
            i++;
        }
        name[i] = '\0';

        // Only add if it looks like a DayZ classname (starts with uppercase, contains letters)
        if (i > 2 && isupper((unsigned char)name[0])) {
            strncpy(classnames[*count], name, MAX_CLASSNAME_LEN - 1);
            classnames[*count][MAX_CLASSNAME_LEN - 1] = '\0';
            (*count)++;
        }

        const char *nl = strchr(line, '\n');
        line = nl ? nl + 1 : NULL;
    }
}

// ============================================================================
// LOAD TRADER CLASSNAMES FROM SORTED DIRECTORY
// ============================================================================

static void load_trader_classnames(AuditorContext *ctx, const char *sorted_root) {
    ctx->trader_classname_count = 0;

    char trader_dir[MAX_PATH_LEN];
    snprintf(trader_dir, sizeof(trader_dir), "%s\\trader", sorted_root);

    util_log(SEVERITY_INFO, "TraderGap: Scanning '%s' for existing trader files...", trader_dir);

#ifdef _WIN32
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*", trader_dir);

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        util_log(SEVERITY_WARNING, "TraderGap: No files found in sorted/trader/");
        return;
    }

    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        char filepath[MAX_PATH_LEN];
        snprintf(filepath, sizeof(filepath), "%s\\%s", trader_dir, fd.cFileName);

        // Read the file
        FILE *f = fopen(filepath, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        if (fsize <= 0 || fsize > 10 * 1024 * 1024) { fclose(f); continue; }
        fseek(f, 0, SEEK_SET);
        char *data = (char*)malloc(fsize + 1);
        if (!data) { fclose(f); continue; }
        fread(data, 1, fsize, f);
        data[fsize] = '\0';
        fclose(f);

        int before = ctx->trader_classname_count;

        // Determine format and extract
        const char *ext = strrchr(fd.cFileName, '.');
        if (ext && util_strcasecmp(ext, ".json") == 0) {
            extract_classnames_from_json(data, (size_t)fsize,
                ctx->trader_classnames, &ctx->trader_classname_count, MAX_ITEMS);
        } else if (ext && util_strcasecmp(ext, ".xml") == 0) {
            extract_classnames_from_trader_xml(data, (size_t)fsize,
                ctx->trader_classnames, &ctx->trader_classname_count, MAX_ITEMS);
        } else if (ext && util_strcasecmp(ext, ".txt") == 0) {
            extract_classnames_from_trader_txt(data, (size_t)fsize,
                ctx->trader_classnames, &ctx->trader_classname_count, MAX_ITEMS);
        }

        int loaded = ctx->trader_classname_count - before;
        if (loaded > 0) {
            util_log(SEVERITY_INFO, "TraderGap: Loaded %d classnames from '%s'", loaded, fd.cFileName);
        }

        free(data);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
#endif

    util_log(SEVERITY_INFO, "TraderGap: %d total classnames found in existing trader files.", ctx->trader_classname_count);
}

// ============================================================================
// COMPARE FOR QSORT / BSEARCH (case-insensitive)
// ============================================================================

static int cmp_classname(const void *a, const void *b) {
    return util_strcasecmp((const char *)a, (const char *)b);
}

// ============================================================================
// CHECK IF CLASSNAME EXISTS IN TRADER (binary search — requires sorted array)
// ============================================================================

static bool is_in_trader(AuditorContext *ctx, const char *classname) {
    if (ctx->trader_classname_count <= 0) return false;
    return bsearch(classname, ctx->trader_classnames, (size_t)ctx->trader_classname_count,
                   MAX_CLASSNAME_LEN, cmp_classname) != NULL;
}

// ============================================================================
// PUBLIC API
// ============================================================================

void trader_gap_fill(AuditorContext *ctx, const char *sorted_root) {
    if (!ctx || !sorted_root) return;

    util_log(SEVERITY_INFO, "=== TRADER GAP FILLER ===");

    // Step 1: Load existing trader classnames
    load_trader_classnames(ctx, sorted_root);

    // Sort trader classnames for O(log n) binary search in is_in_trader()
    if (ctx->trader_classname_count > 1) {
        qsort(ctx->trader_classnames, (size_t)ctx->trader_classname_count,
              MAX_CLASSNAME_LEN, cmp_classname);
    }

    // Step 2: Find items that are NOT in any trader file
    int missing_count = 0;
    int generated_count = 0;

    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (item->classname[0] == '\0') continue;
        if (item->is_debug_item) continue;

        // Currency items are NOT tradeable — skip them entirely.
        // They are the medium of exchange (Dollar, Euro, Ruble, Bitcoin, HeirloomToken).
        if (util_str_contains_ci(item->classname, "Money_Dollar") ||
            util_str_contains_ci(item->classname, "Money_Euro") ||
            util_str_contains_ci(item->classname, "Money_Ruble") ||
            util_str_contains_ci(item->classname, "Money_Bitcoin") ||
            util_str_contains_ci(item->classname, "HeirloomToken")) {
            continue;
        }

        if (!is_in_trader(ctx, item->classname)) {
            missing_count++;

            // Generate store data for this item if it hasn't been generated yet
            if (item->calculated_price <= 0) {
                // The store_generator runs in the AUDIT stage before EXPORT,
                // so this is a safety net for items it somehow missed.
                // Use tier-aware defaults instead of hardcoded values.
                int base = 100;
                if (item->assigned_tier >= 1 && item->assigned_tier <= 12) {
                    // Precomputed 1.8^(tier-1) lookup — same table as store_generator
                    static const double TIER_MULT[] = {
                        1.0, 1.0, 1.8, 3.24, 5.832, 10.4976,
                        18.89568, 34.012224, 61.2220032,
                        110.19960576, 198.35929037, 357.04672266, 642.68410079
                    };
                    base = (int)(100.0 * TIER_MULT[item->assigned_tier]);
                }
                item->calculated_price = base;
                item->buy_price = base;
                item->sell_price = (int)(base * 0.15); // 15% sell ratio (hardcore)
                strncpy(item->currency, "CJ187-Money-Dollars-Only", 63);
                item->black_market = false;
                item->admin_only = false;
                item->stock_override = 0;
                item->restock_override = 0;

                if (item->trader_cat[0] == '\0') {
                    strncpy(item->trader_cat, "Miscellaneous", 63);
                }

                generated_count++;
            }
        }
    }

    util_log(SEVERITY_INFO, "TraderGap: %d items exist in economy but NOT in any trader file.", missing_count);
    util_log(SEVERITY_INFO, "TraderGap: Generated store data for %d items that had none.", generated_count);
    util_log(SEVERITY_INFO, "TraderGap: All %d economy items will now appear in the TraderPlus export.", ctx->item_count);
    util_log(SEVERITY_INFO, "=== TRADER GAP FILLER COMPLETE ===");
}

// ============================================================================
// TRADERPLUS JSON PARSER (load existing trader file into context)
// ============================================================================

bool parser_load_traderplus_json(AuditorContext *ctx, const char *filepath) {
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

    extract_classnames_from_json(data, (size_t)fsize,
        ctx->trader_classnames, &ctx->trader_classname_count, MAX_ITEMS);

    free(data);
    util_log(SEVERITY_INFO, "Loaded TraderPlus JSON: %s (%d classnames total)", filepath, ctx->trader_classname_count);
    return true;
}
