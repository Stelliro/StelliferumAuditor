
#include "auditor.h"
#include "loot_policy.h"
#include "loot_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

// ============================================================================
// CE SAFETY — clamp values/usages at write time to prevent server crashes
// ============================================================================
// DayZ CE only recognises Tier1-4 + Unique for values, and 17 base usage flags.
// Writing invalid flags (Tier5+, "City", "Civilian", etc.) causes undefined
// behaviour; exceeding ~5 usage entries per item can overflow the CE's internal
// per-type usage array and corrupt the heap (C0000374).
// ============================================================================
#define MAX_CE_USAGES_PER_ITEM 5  /* vanilla max observed: 5 (Ammo_12gaPellets) */

static bool is_valid_ce_usage(const char *name) {
    static const char *valid[] = {
        "Coast","ContaminatedArea","Farm","Firefighter","Historical","Hunting",
        "Industrial","Lunapark","Medic","Military","Office","Police","Prison",
        "School","SeasonalEvent","Town","Village", NULL
    };
    for (int i = 0; valid[i]; i++) {
        if (strcmp(name, valid[i]) == 0) return true;
    }
    return false;
}

static bool is_valid_ce_value(const char *name) {
    if (strcmp(name, "Unique") == 0) return true;
    if (strncmp(name, "Tier", 4) == 0) {
        int n = atoi(name + 4);
        return (n >= 1 && n <= 4);
    }
    return false;
}

// Categories defined in cfglimitsdefinition.xml. Writing any other name
// (e.g. "vehiclesparts", "infected" passed through from mod XMLs) produces
// CE "Unknown category" errors at server boot. Vanilla zombie/animal
// entries carry no category at all.
static bool is_valid_ce_category(const char *name) {
    static const char *valid[] = {
        "books","clothes","containers","explosives","food",
        "lootdispatch","tools","weapons", NULL
    };
    for (int i = 0; valid[i]; i++) {
        if (strcmp(name, valid[i]) == 0) return true;
    }
    return false;
}
#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// HASH-SET DEDUP — bulletproof classname dedup that doesn't rely on sort order
// ============================================================================
// Open-addressing hash table with linear probing.  Capacity is a power of 2
// sized to stay under 50% load factor for ~9k items.
#define WRITER_DEDUP_CAP 16384  // must be power of 2

typedef struct {
    int  *indices;   // item index stored at each bucket (-1 = empty)
    int   capacity;
} WriterDedupSet;

static WriterDedupSet writer_dedup_create(void) {
    WriterDedupSet s;
    s.capacity = WRITER_DEDUP_CAP;
    s.indices = (int *)malloc(sizeof(int) * WRITER_DEDUP_CAP);
    if (s.indices) {
        for (int i = 0; i < WRITER_DEDUP_CAP; i++) s.indices[i] = -1;
    }
    return s;
}

static void writer_dedup_free(WriterDedupSet *s) {
    if (s && s->indices) { free(s->indices); s->indices = NULL; }
}

static unsigned int writer_dedup_hash(const char *name) {
    unsigned int h = 5381;
    for (; *name; name++)
        h = ((h << 5) + h) + (unsigned int)tolower((unsigned char)*name);
    return h;
}

// Returns true if classname was ALREADY in the set (duplicate).
// If new, inserts it and returns false.
static bool writer_dedup_check(WriterDedupSet *s, const LootItem *items,
                               int item_count, int item_idx) {
    if (!s || !s->indices) return false;
    const char *name = items[item_idx].classname;
    unsigned int mask = (unsigned int)(s->capacity - 1);
    unsigned int idx  = writer_dedup_hash(name) & mask;
    for (int p = 0; p < s->capacity; p++) {
        unsigned int slot = (idx + (unsigned int)p) & mask;
        if (s->indices[slot] == -1) {
            s->indices[slot] = item_idx;
            return false;  // new entry
        }
        if (util_strcasecmp(items[s->indices[slot]].classname, name) == 0) {
            return true;   // duplicate
        }
    }
    return false;  // table full — shouldn't happen at <50% load
}

// ----------------------------------------------------------------------------
// Generic case-insensitive string dedup set (stores caller-owned char* pointers).
// Used to suppress duplicate <type>/preset names when merging spawnabletypes and
// randompresets from vanilla + multiple mods. Duplicate names in those files make
// the DayZ CE log errors / reject entries on load.
// ----------------------------------------------------------------------------
typedef struct {
    const char **names;
    int capacity;
} WriterStrSet;

static WriterStrSet writer_strset_create(int capacity_pow2) {
    WriterStrSet s;
    s.capacity = capacity_pow2;
    s.names = (const char **)malloc(sizeof(const char *) * (size_t)capacity_pow2);
    if (s.names) {
        for (int i = 0; i < capacity_pow2; i++) s.names[i] = NULL;
    }
    return s;
}

static void writer_strset_free(WriterStrSet *s) {
    if (s && s->names) { free(s->names); s->names = NULL; }
}

// Returns true if 'name' was ALREADY present (duplicate). Inserts if new.
static bool writer_strset_add(WriterStrSet *s, const char *name) {
    if (!s || !s->names || !name) return false;
    unsigned int mask = (unsigned int)(s->capacity - 1);
    unsigned int idx  = writer_dedup_hash(name) & mask;
    for (int p = 0; p < s->capacity; p++) {
        unsigned int slot = (idx + (unsigned int)p) & mask;
        if (s->names[slot] == NULL) { s->names[slot] = name; return false; }
        if (util_strcasecmp(s->names[slot], name) == 0) return true;
    }
    return false;  // table full — shouldn't happen at <50% load
}

static const char* get_tier_name(int tier) {
    static char buf[96];
    const char *nm = lp_tier_name(tier);
    if (nm && nm[0]) {
        snprintf(buf, sizeof(buf), "Tier %d (%s)", tier, nm);
        return buf;
    }
    return "Untiered";
}

static const char* find_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle || !*needle) return NULL;
    size_t nlen = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nlen) return p;
    }
    return NULL;
}

static bool extract_currency_token(const char *data, const char *key, char *out, size_t out_len) {
    const char *pos = find_case_insensitive(data, key);
    if (!pos) return false;
    const char *quote = strchr(pos, '"');
    if (!quote) return false;
    quote++;
    const char *end = strchr(quote, '"');
    if (!end) return false;
    size_t len = (size_t)(end - quote);
    if (len == 0 || len >= out_len) return false;
    memcpy(out, quote, len);
    out[len] = '\0';
    return true;
}

static void try_read_currency_file(const char *path, char *standard_currency, size_t standard_len,
                                   char *black_market_currency, size_t black_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return; }

    long read_size = fsize > 65536 ? 65536 : fsize;
    char *buf = (char*)malloc((size_t)read_size + 1);
    if (!buf) { fclose(f); return; }
    size_t bytes_read = fread(buf, 1, (size_t)read_size, f);
    buf[bytes_read] = '\0';
    fclose(f);

    if (standard_currency[0] == '\0') {
        if (extract_currency_token(buf, "standard_currency", standard_currency, standard_len)) {
        } else if (extract_currency_token(buf, "CurrencyName", standard_currency, standard_len)) {
        } else if (extract_currency_token(buf, "currency", standard_currency, standard_len)) {
        }
    }

    if (black_market_currency[0] == '\0') {
        if (extract_currency_token(buf, "black_market_currency", black_market_currency, black_len)) {
        } else if (extract_currency_token(buf, "BlackMarketCurrency", black_market_currency, black_len)) {
        }
    }

    free(buf);
}

bool writer_export_audit_report(AuditorContext *ctx, const char *filepath, bool include_duplicates) {
    FILE *f = fopen(filepath, "w");
    if (!f) return false;
    
    int total_active = 0;
    int orphans = 0;
    int tier_counts[MAX_TIERS + 1] = {0}; 
    int high_value_items = 0;
    int zombie_counts[5] = {0};  // [0]=untiered, [1-4]=tier
    int animal_counts[5] = {0};
    int total_zombies = 0, total_animals = 0;

    // ── Detailed counters for new scoring system ───────────────────────
    int weapon_nominal_total  = 0;   // Total nominal of all guns
    int ammo_nominal_total    = 0;   // Total nominal of ammo items
    int mag_nominal_total     = 0;   // Total nominal of magazines
    int weapon_type_count     = 0;   // Unique weapon classnames
    int ammo_type_count       = 0;   // Unique ammo classnames
    int mag_type_count        = 0;   // Unique magazine classnames
    int items_no_category     = 0;   // Items with empty category
    int items_no_usage        = 0;   // Active items with 0 usage zones
    int items_high_nominal    = 0;   // Items with nominal > 100 (server load)
    int items_zero_nominal    = 0;   // Active non-deleted items with nominal == 0
    int items_short_lifetime  = 0;   // Items with lifetime < 300 (5 min)
    int items_long_lifetime   = 0;   // Items with lifetime > 14400000 (~167 days)
    int total_category_counts[16] = {0}; // Aggregated broad categories
    // Broad categories: 0=Weapons, 1=Ammo, 2=Magazines, 3=Clothing, 4=Food,
    //                   5=Medical, 6=Tools, 7=Vehicles, 8=Building, 9=Backpacks, 10=Other
    int coastal_items  = 0;  // Tiers 1-2
    int midland_items  = 0;  // Tier 3
    int military_items = 0;  // Tier 4+
    
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        
        // The Switch: Skip deleted items ONLY if we want a clean report
        if (!include_duplicates && item->deleted) continue;
        
        total_active++;
        
        // Count zombies and animals separately
        if (loot_is_zombie(item) && !item->deleted) {
            total_zombies++;
            int zt = item->assigned_tier;
            if (zt >= 1 && zt <= 4) zombie_counts[zt]++;
            else zombie_counts[0]++;
            continue; // Don't count zombies in orphan/tier stats
        }
        if (loot_is_animal(item) && !item->deleted) {
            total_animals++;
            int at = item->assigned_tier;
            if (at >= 1 && at <= 4) animal_counts[at]++;
            else animal_counts[0]++;
            continue; // Don't count animals in orphan/tier stats
        }
        
        if (item->assigned_tier >= 1 && item->assigned_tier <= MAX_TIERS)
            tier_counts[item->assigned_tier]++;
        else 
            tier_counts[0]++;
            
        if (item->nominal > 0 && strlen(item->category) == 0 && item->usage_count == 0) orphans++;
        
        if (item->nominal > 50) high_value_items++;

        // ── Classify using trader_cat (set by determine_category) ──────
        const char *tc = item->trader_cat;
        bool is_weapon = (strstr(tc, "Rifle") || strstr(tc, "Pistol") ||
                          strstr(tc, "Shotgun") || strstr(tc, "Submachine") ||
                          strstr(tc, "Launcher") || strstr(tc, "High-Tier") ||
                          strstr(tc, "Sniper"));
        bool is_ammo   = (strstr(tc, "Ammunition") != NULL);
        bool is_mag    = (strstr(tc, "Magazine") != NULL);

        if (is_weapon) {
            weapon_nominal_total += item->nominal;
            weapon_type_count++;
        }
        if (is_ammo) {
            ammo_nominal_total += item->nominal;
            ammo_type_count++;
        }
        if (is_mag) {
            mag_nominal_total += item->nominal;
            mag_type_count++;
        }
        
        // Broad category aggregation for diversity
        if (is_weapon)                           total_category_counts[0]++;
        else if (is_ammo)                        total_category_counts[1]++;
        else if (is_mag)                         total_category_counts[2]++;
        else if (strstr(tc, "Clothing") || strstr(tc, "Ghillie") || strstr(tc, "Military Clothing"))
                                                 total_category_counts[3]++;
        else if (strstr(tc, "Food"))             total_category_counts[4]++;
        else if (strstr(tc, "Medical"))          total_category_counts[5]++;
        else if (strstr(tc, "Tool"))             total_category_counts[6]++;
        else if (strstr(tc, "Vehicle"))          total_category_counts[7]++;
        else if (strstr(tc, "Building") || strstr(tc, "Base"))
                                                 total_category_counts[8]++;
        else if (strstr(tc, "Backpack"))         total_category_counts[9]++;
        else                                     total_category_counts[10]++;

        // Quality checks
        if (strlen(item->category) == 0 && item->nominal > 0) items_no_category++;
        if (item->usage_count == 0 && item->nominal > 0)       items_no_usage++;
        if (item->nominal > 100)                                items_high_nominal++;
        if (item->nominal == 0 && !item->deleted)               items_zero_nominal++;
        if (item->lifetime > 0 && item->lifetime < 300)        items_short_lifetime++;
        if (item->lifetime > 14400000)                          items_long_lifetime++;

        // Tier balance
        if (item->assigned_tier >= 1 && item->assigned_tier <= 2) coastal_items++;
        else if (item->assigned_tier == 3)                        midland_items++;
        else if (item->assigned_tier >= 4)                        military_items++;
    }
    
    // ════════════════════════════════════════════════════════════════════
    //  PLAYABILITY SCORING — 8 weighted checks, 100 points total
    // ════════════════════════════════════════════════════════════════════
    typedef struct { const char *name; int max_pts; int earned; const char *detail; } ScoreCheck;
    char detail_buf[8][256];
    ScoreCheck checks[8];
    int check_count = 0;

    // 1. Orphan Rate (15 pts) — items with no spawn location
    {
        float orphan_pct = total_active > 0 ? (float)orphans / (float)total_active * 100.0f : 0.0f;
        int pts = 15;
        if (orphan_pct > 5.0f)       pts = 0;
        else if (orphan_pct > 2.0f)  pts = 5;
        else if (orphan_pct > 0.5f)  pts = 10;
        else if (orphans > 0)        pts = 13;
        snprintf(detail_buf[check_count], 256, "%d orphans (%.1f%% of %d items)", orphans, orphan_pct, total_active - total_zombies - total_animals);
        checks[check_count] = (ScoreCheck){"Orphan Rate", 15, pts, detail_buf[check_count]};
        check_count++;
    }

    // 2. Ammo-to-Weapon Ratio (15 pts) — enough ammo+mags for guns?
    {
        int pts = 15;
        // Use type counts (variety) + nominal (quantity)
        int supply = ammo_nominal_total + mag_nominal_total;
        float ratio = weapon_nominal_total > 0 ? (float)supply / (float)weapon_nominal_total : 99.0f;
        if (ratio < 0.5f)       pts = 0;   // Severe shortage
        else if (ratio < 1.0f)  pts = 5;   // Low
        else if (ratio < 1.5f)  pts = 10;  // Adequate
        else if (ratio < 2.0f)  pts = 13;  // Good
        // else full 15
        snprintf(detail_buf[check_count], 256, "Ammo+Mag nominal %d vs Weapon nominal %d (ratio %.1f:1)", supply, weapon_nominal_total, ratio);
        checks[check_count] = (ScoreCheck){"Ammo/Weapon Ratio", 15, pts, detail_buf[check_count]};
        check_count++;
    }

    // 3. Tier Balance (15 pts) — coastal shouldn't be empty vs military
    {
        int pts = 15;
        // Ratio: military-to-coastal. Modded servers naturally have more military,
        // so we use a generous 5:1 threshold before penalizing.
        float ratio = coastal_items > 0 ? (float)military_items / (float)coastal_items : 99.0f;
        if (ratio > 8.0f)       pts = 0;   // Extreme imbalance
        else if (ratio > 5.0f)  pts = 5;   // Heavy imbalance
        else if (ratio > 3.0f)  pts = 10;  // Moderate
        else if (ratio > 2.0f)  pts = 13;  // Slight
        // else full 15
        snprintf(detail_buf[check_count], 256, "Coastal(T1-2):%d  Midland(T3):%d  Military(T4+):%d  (ratio %.1f:1)",
                coastal_items, midland_items, military_items, ratio);
        checks[check_count] = (ScoreCheck){"Tier Balance", 15, pts, detail_buf[check_count]};
        check_count++;
    }

    // 4. Loot Diversity (15 pts) — at least 5 broad categories represented
    {
        int categories_present = 0;
        for (int c = 0; c < 11; c++) {
            if (total_category_counts[c] > 0) categories_present++;
        }
        int pts = 15;
        if (categories_present < 3)       pts = 0;
        else if (categories_present < 5)  pts = 5;
        else if (categories_present < 7)  pts = 10;
        else if (categories_present < 9)  pts = 13;
        snprintf(detail_buf[check_count], 256, "%d/11 broad categories have items", categories_present);
        checks[check_count] = (ScoreCheck){"Loot Diversity", 15, pts, detail_buf[check_count]};
        check_count++;
    }

    // 5. Data Quality (10 pts) — items should have category + usage zones
    {
        int pts = 10;
        int bad = items_no_category + items_no_usage;
        float bad_pct = total_active > 0 ? (float)bad / (float)total_active * 100.0f : 0.0f;
        if (bad_pct > 20.0f)      pts = 0;
        else if (bad_pct > 10.0f) pts = 3;
        else if (bad_pct > 5.0f)  pts = 6;
        else if (bad_pct > 1.0f)  pts = 8;
        snprintf(detail_buf[check_count], 256, "%d missing category, %d missing usage (%.1f%% incomplete)", items_no_category, items_no_usage, bad_pct);
        checks[check_count] = (ScoreCheck){"Data Quality", 10, pts, detail_buf[check_count]};
        check_count++;
    }

    // 6. Server Load (10 pts) — too many high-nominal items hurt perf
    {
        int pts = 10;
        float hi_pct = total_active > 0 ? (float)items_high_nominal / (float)total_active * 100.0f : 0.0f;
        if (hi_pct > 15.0f)      pts = 0;
        else if (hi_pct > 8.0f)  pts = 3;
        else if (hi_pct > 4.0f)  pts = 6;
        else if (hi_pct > 2.0f)  pts = 8;
        snprintf(detail_buf[check_count], 256, "%d items with nominal > 100 (%.1f%%)", items_high_nominal, hi_pct);
        checks[check_count] = (ScoreCheck){"Server Load", 10, pts, detail_buf[check_count]};
        check_count++;
    }

    // 7. Lifetime Sanity (10 pts) — no ultra-short or ultra-long lifetimes
    {
        int pts = 10;
        int bad_lt = items_short_lifetime + items_long_lifetime;
        float bad_pct = total_active > 0 ? (float)bad_lt / (float)total_active * 100.0f : 0.0f;
        if (bad_pct > 10.0f)      pts = 0;
        else if (bad_pct > 5.0f)  pts = 3;
        else if (bad_pct > 2.0f)  pts = 6;
        else if (bad_pct > 0.5f)  pts = 8;
        snprintf(detail_buf[check_count], 256, "%d too short (<5min), %d too long (>167d)", items_short_lifetime, items_long_lifetime);
        checks[check_count] = (ScoreCheck){"Lifetime Sanity", 10, pts, detail_buf[check_count]};
        check_count++;
    }

    // 8. Zero-Nominal Items (10 pts) — defined but never spawn
    {
        int pts = 10;
        float zero_pct = total_active > 0 ? (float)items_zero_nominal / (float)total_active * 100.0f : 0.0f;
        if (zero_pct > 15.0f)      pts = 0;
        else if (zero_pct > 8.0f)  pts = 3;
        else if (zero_pct > 3.0f)  pts = 6;
        else if (zero_pct > 1.0f)  pts = 8;
        snprintf(detail_buf[check_count], 256, "%d items with nominal=0 (%.1f%% wasted definitions)", items_zero_nominal, zero_pct);
        checks[check_count] = (ScoreCheck){"Zero-Nominal Items", 10, pts, detail_buf[check_count]};
        check_count++;
    }

    int score = 0;
    int score_max = 0;
    for (int c = 0; c < check_count; c++) {
        score     += checks[c].earned;
        score_max += checks[c].max_pts;
    }
    if (score < 0) score = 0;
    
    fprintf(f, "========================================================\n");
    if (include_duplicates)
        fprintf(f, " STELLIFERUM RAW REPORT (INCLUDES DUPLICATES/CONFLICTS)\n");
    else
        fprintf(f, " STELLIFERUM CLEAN REPORT (RESOLVED ECONOMY)\n");
    fprintf(f, "========================================================\n");
    
    fprintf(f, "OVERALL PLAYABILITY SCORE: %d / %d\n", score, score_max);
    
    if (score >= 90) fprintf(f, "RATING: EXCELLENT\n");
    else if (score >= 75) fprintf(f, "RATING: GOOD\n");
    else if (score >= 50) fprintf(f, "RATING: WARNING\n");
    else fprintf(f, "RATING: CRITICAL\n");

    // ── Score Breakdown ────────────────────────────────────────────────
    fprintf(f, "\n[SCORE BREAKDOWN]\n");
    for (int c = 0; c < check_count; c++) {
        const char *grade;
        float pct = checks[c].max_pts > 0 ? (float)checks[c].earned / (float)checks[c].max_pts * 100.0f : 0.0f;
        if (pct >= 100.0f)     grade = "PASS";
        else if (pct >= 80.0f) grade = "GOOD";
        else if (pct >= 50.0f) grade = "WARN";
        else                   grade = "FAIL";
        fprintf(f, "  [%s] %-22s %2d / %2d  — %s\n",
                grade, checks[c].name, checks[c].earned, checks[c].max_pts, checks[c].detail);
    }
    
    fprintf(f, "\n[SUMMARY STATS]\n");
    fprintf(f, "Total Item Count:     %d (+ %d Infected, %d Wildlife)\n", total_active - total_zombies - total_animals, total_zombies, total_animals);
    fprintf(f, "Orphaned Items:       %d\n", orphans);
    fprintf(f, "High-Load Items:      %d\n", high_value_items);
    
    fprintf(f, "\n[ITEM TIER DISTRIBUTION]\n");
    {
        int ntiers = lp_tier_count();
        if (ntiers < 1) ntiers = 12;
        if (ntiers > MAX_TIERS) ntiers = MAX_TIERS;
        for (int t = 1; t <= ntiers; t++) {
            if (tier_counts[t] > 0 || t <= 4) {
                fprintf(f, "%-22s: %d items\n", get_tier_name(t), tier_counts[t]);
            }
        }
    }
    fprintf(f, "%-22s: %d items\n", "Untiered/Global", tier_counts[0]);
    
    // Zombie tier distribution
    if (total_zombies > 0) {
        fprintf(f, "\n[INFECTED TIER DISTRIBUTION] (%d total)\n", total_zombies);
        fprintf(f, "  Tier 1 (Green Eyes)   : %d   — Towns, Villages, Coast\n", zombie_counts[1]);
        fprintf(f, "  Tier 2 (Yellow Eyes)  : %d   — Industrial, Farms\n", zombie_counts[2]);
        fprintf(f, "  Tier 3 (Red Eyes)     : %d   — Military Zones\n", zombie_counts[3]);
        fprintf(f, "  Tier 4 (Purple Eyes)  : %d   — Endgame / Contaminated\n", zombie_counts[4]);
        if (zombie_counts[0] > 0)
            fprintf(f, "  Untiered              : %d\n", zombie_counts[0]);
    }
    
    // Animal tier distribution
    if (total_animals > 0) {
        fprintf(f, "\n[WILDLIFE TIER DISTRIBUTION] (%d total)\n", total_animals);
        fprintf(f, "  Tier 1 (Farm)         : %d   — Hens, Pigs, Goats, Sheep\n", animal_counts[1]);
        fprintf(f, "  Tier 2 (Wild Game)    : %d   — Deer, Foxes, Boars\n", animal_counts[2]);
        fprintf(f, "  Tier 3 (Pack Hunters) : %d   — Wolves\n", animal_counts[3]);
        fprintf(f, "  Tier 4 (Apex)         : %d   — Bears\n", animal_counts[4]);
    }
    
    // [FIX] Unlimited Listing for Orphans
    if (orphans > 0) {
        fprintf(f, "\n[ORPHANS] - Defined but have no spawn location:\n");
        for (int i = 0; i < ctx->item_count; i++) {
            LootItem *item = &ctx->items[i];
            // Check orphan status (Active item with stats but no location)
            if (!item->deleted && item->nominal > 0 && strlen(item->category) == 0 && item->usage_count == 0) {
                fprintf(f, " - %s (Source: %s)\n", item->classname, item->mod_source);
            }
        }
    }
    
    if (!include_duplicates) {
        // [FIX] Group conflicts by source file — no more per-item spam
        int dupe_count = 0;
        for(int i=0; i<ctx->item_count; i++) if(ctx->items[i].deleted) dupe_count++;
        
        if (dupe_count > 0) {
            fprintf(f, "\n[CONFLICTS RESOLVED] - %d duplicate items merged (mod versions preferred):\n", dupe_count);
            
            // Group by source file
            char sources[128][64];
            int source_counts[128];
            int source_total = 0;
            memset(source_counts, 0, sizeof(source_counts));
            
            for (int i = 0; i < ctx->item_count; i++) {
                if (!ctx->items[i].deleted) continue;
                // Find or add source
                int found = -1;
                for (int s = 0; s < source_total; s++) {
                    if (util_strcasecmp(sources[s], ctx->items[i].mod_source) == 0) { found = s; break; }
                }
                if (found >= 0) {
                    source_counts[found]++;
                } else if (source_total < 128) {
                    strncpy(sources[source_total], ctx->items[i].mod_source, 63);
                    source_counts[source_total] = 1;
                    source_total++;
                }
            }
            
            for (int s = 0; s < source_total; s++) {
                fprintf(f, "  %s: %d items superseded by mod versions\n", sources[s], source_counts[s]);
            }
        }
    } else {
        fprintf(f, "\n[NOTE] This Raw Report includes duplicates. See Clean Report for resolution details.\n");
    }

    fclose(f);
    util_log(SEVERITY_INFO, "Generated Unlimited Report: %s", filepath);
    return true;
}

// Re-include others
bool writer_export_merged_xml(AuditorContext *ctx, const char *filepath) {
    if (!ctx) {
        util_log(SEVERITY_ERROR, "writer_export_merged_xml: ctx is NULL");
        return false;
    }

    util_log(SEVERITY_INFO, "writer_export_merged_xml: starting export to '%s' (%d items)", filepath, ctx->item_count);

    // Guarantee sort order before writing — the adjacent-classname dedup below
    // relies on duplicates being consecutive.  Without this, duplicates can
    // slip through if any pipeline step between the audit sort and this export
    // broke the order (e.g. auditor_run_audit_pipeline running before swarm).
    auditor_sort_items(ctx);

    /* Retry fopen up to 5 times — Compress-Archive / .NET GC can leave file
       handles open briefly after backup, causing EINVAL on Windows NTFS. */
    FILE *f = NULL;
    for (int retry = 0; retry < 5 && !f; retry++) {
        if (retry > 0) {
            util_log(SEVERITY_WARNING, "writer_export_merged_xml: fopen retry %d/5 for '%s' (errno=%d)", retry, filepath, errno);
#ifdef _WIN32
            Sleep(500);
#endif
        }
        remove(filepath); /* best-effort: clear pending-delete state */
        f = fopen(filepath, "w");
    }
    if (!f) {
        util_log(SEVERITY_ERROR, "writer_export_merged_xml: fopen('%s', 'w') FAILED after retries (errno=%d)", filepath, errno);
        return false;
    }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<types>\n");
    int written = 0, skipped_zombie_animal = 0, skipped_dupe = 0, skipped_bad_category = 0;
    int skipped_minclamp = 0, skipped_quantfix = 0;
    WriterDedupSet dedup = writer_dedup_create();
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        // Skip zombies/animals — they are exported separately to
        // types_infected.xml / types_wildlife.xml by writer_export_zombie_config().
        // Having them in BOTH files causes the DayZ CE to register duplicate
        // classnames during InitOffline(), corrupting the heap (C0000374).
        if (loot_is_zombie(item) || loot_is_animal(item)) {
            skipped_zombie_animal++;
            continue;
        }
        // Hash-set dedup: catches ALL duplicates regardless of sort adjacency.
        // Replaces the fragile prev_classname approach that relied on perfect
        // sort grouping.
        if (writer_dedup_check(&dedup, ctx->items, ctx->item_count, i)) {
            skipped_dupe++;
            continue;
        }
        // CE numeric guards (write-time): min must not exceed nominal, and the
        // quant pair must be -1/-1 (disabled) or 0<=quantmin<=quantmax<=100.
        // Emitting min>nominal or quantmin>quantmax makes CE log errors on load.
        int w_nominal = item->nominal < 0 ? 0 : item->nominal;
        int w_min = item->min < 0 ? 0 : item->min;
        if (w_min > w_nominal) { w_min = w_nominal; skipped_minclamp++; }
        int qmin = item->quantmin, qmax = item->quantmax;
        bool quant_ok = (qmin == -1 && qmax == -1) ||
                        (qmin >= 0 && qmax >= qmin && qmax <= 100);
        if (!quant_ok) { qmin = -1; qmax = -1; skipped_quantfix++; }
        fprintf(f, "    <type name=\"%s\">\n", item->classname);
        fprintf(f, "        <nominal>%d</nominal>\n", w_nominal);
        fprintf(f, "        <lifetime>%d</lifetime>\n", item->lifetime);
        fprintf(f, "        <restock>%d</restock>\n", item->restock);
        fprintf(f, "        <min>%d</min>\n", w_min);
        fprintf(f, "        <quantmin>%d</quantmin>\n", qmin);
        fprintf(f, "        <quantmax>%d</quantmax>\n", qmax);
        fprintf(f, "        <cost>%d</cost>\n", item->cost);
        fprintf(f, "        <flags count_in_cargo=\"%d\" count_in_hoarder=\"%d\" count_in_map=\"%d\" count_in_player=\"%d\" crafted=\"%d\" deloot=\"%d\"/>\n",
            (item->flags & 1)?1:0, (item->flags & 2)?1:0, (item->flags & 4)?1:0, 
            (item->flags & 8)?1:0, (item->flags & 16)?1:0, (item->flags & 32)?1:0);
        /* Category: write only categories defined in cfglimitsdefinition.xml */
        if (item->category[0] != '\0') {
            if (is_valid_ce_category(item->category))
                fprintf(f, "        <category name=\"%s\"/>\n", item->category);
            else
                skipped_bad_category++;
        }
        /* Usage: write only valid CE usage flags, capped at MAX_CE_USAGES_PER_ITEM */
        {
            int written_usages = 0;
            for (int u = 0; u < item->usage_count && written_usages < MAX_CE_USAGES_PER_ITEM; u++) {
                if (is_valid_ce_usage(item->usages[u])) {
                    fprintf(f, "        <usage name=\"%s\"/>\n", item->usages[u]);
                    written_usages++;
                }
            }
        }
        /* Values: write only valid CE value flags (Tier1-4, Unique) */
        for (int v = 0; v < item->value_count; v++) {
            if (is_valid_ce_value(item->values[v]))
                fprintf(f, "        <value name=\"%s\"/>\n", item->values[v]);
        }
        fprintf(f, "    </type>\n");
        written++;
    }
    fprintf(f, "</types>\n");
    fclose(f);
    writer_dedup_free(&dedup);
    if (skipped_zombie_animal > 0)
        util_log(SEVERITY_INFO, "Types export: excluded %d zombie/animal entries (in types_infected/types_wildlife)", skipped_zombie_animal);
    if (skipped_dupe > 0)
        util_log(SEVERITY_WARNING, "Types export: suppressed %d duplicate classname(s) at write time", skipped_dupe);
    if (skipped_bad_category > 0)
        util_log(SEVERITY_WARNING, "Types export: dropped %d category tag(s) not defined in cfglimitsdefinition.xml", skipped_bad_category);
    if (skipped_minclamp > 0)
        util_log(SEVERITY_WARNING, "Types export: clamped min>nominal on %d item(s)", skipped_minclamp);
    if (skipped_quantfix > 0)
        util_log(SEVERITY_WARNING, "Types export: normalized invalid quantmin/quantmax on %d item(s)", skipped_quantfix);
    util_log(SEVERITY_INFO, "Types export: wrote %d items to %s", written, filepath);
    return true;
}
bool writer_export_csv(AuditorContext *ctx, const char *filepath) {
    FILE *f = fopen(filepath, "w");
    if (!f) return false;
    fprintf(f, "Classname,SourceFile,Tier,Category,Nominal,Lifetime,Min,Restock\n");
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        fprintf(f, "%s,%s,%d,%s,%d,%d,%d,%d\n", item->classname, item->mod_source, item->assigned_tier, item->category, item->nominal, item->lifetime, item->min, item->restock);
    }
    fclose(f);
    return true;
}
bool writer_export_spawnable_types(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    fprintf(f, "<!-- Generated by StelliferumAuditor — Merged Spawnable Types -->\n");
    fprintf(f, "<spawnabletypes>\n");

    int written = 0;
    int skipped_deleted = 0;
    int skipped_zombie_animal = 0;
    int skipped_dupe = 0;
    // Dedup by classname: the same spawnable type can appear in vanilla AND one or
    // more mod cfgspawnabletypes.xml files. Duplicate <type name> entries make the
    // DayZ CE log "duplicate spawnable type" errors on load, so keep only the first.
    WriterStrSet spawn_dedup = writer_strset_create(8192);
    for (int i = 0; i < ctx->spawn_block_count; i++) {
        SpawnableBlock *blk = &ctx->spawn_blocks[i];
        if (blk->classname[0] == '\0') continue;

        // Skip spawnable blocks for zombies/infected and animals — these belong in
        // types_infected.xml / types_wildlife.xml and events.xml, NOT in
        // cfgspawnabletypes.xml. Having them here causes the CE to spawn zombies
        // and animals inside loot containers instead of actual loot items.
        LootItem probe = {0};
        strncpy(probe.classname, blk->classname, MAX_CLASSNAME_LEN - 1);
        if (loot_is_zombie(&probe) || loot_is_animal(&probe)) {
            skipped_zombie_animal++;
            continue;
        }

        // Skip spawnable blocks for items that were deleted (merged/deduped/debug)
        bool is_deleted = false;
        for (int j = 0; j < ctx->item_count; j++) {
            if (util_strcasecmp(ctx->items[j].classname, blk->classname) == 0) {
                if (ctx->items[j].deleted) { is_deleted = true; }
                else { is_deleted = false; break; }  // Found a live copy — keep it
            }
        }
        if (is_deleted) { skipped_deleted++; continue; }

        // Suppress duplicate classnames (vanilla + mod overlap).
        if (writer_strset_add(&spawn_dedup, blk->classname)) {
            skipped_dupe++;
            continue;
        }

        // Each raw_xml block already contains the full <type ...> inner XML.
        // If it starts with '<', write it directly. Otherwise wrap it.
        if (blk->raw_xml[0] == '<') {
            fprintf(f, "    %s\n", blk->raw_xml);
        } else {
            // Minimal fallback: write an empty type entry
            fprintf(f, "    <type name=\"%s\">\n", blk->classname);
            fprintf(f, "    </type>\n");
        }
        written++;
    }

    if (skipped_zombie_animal > 0)
        util_log(SEVERITY_INFO, "Spawnable export: stripped %d zombie/animal entries from cfgspawnabletypes", skipped_zombie_animal);
    if (skipped_deleted > 0)
        util_log(SEVERITY_INFO, "Spawnable export: skipped %d blocks for deleted items", skipped_deleted);
    if (skipped_dupe > 0)
        util_log(SEVERITY_WARNING, "Spawnable export: suppressed %d duplicate spawnable type(s) at write time", skipped_dupe);
    writer_strset_free(&spawn_dedup);

    fprintf(f, "</spawnabletypes>\n");
    fclose(f);

    util_log(SEVERITY_INFO, "Exported %d spawnable types -> %s", written, filepath);
    return true;
}

// ============================================================================
// DR. JONES ITEM FLAGS (shared by single-file and multi-shop writers)
// ============================================================================
// Flags: W = weapon, V = vehicle, VNK = vehicle no key, * = general item

static const char* drjones_item_flag(const char *trader_cat) {
    if (!trader_cat || !trader_cat[0]) return "*";
    if (util_str_contains_ci(trader_cat, "Rifle") ||
        util_str_contains_ci(trader_cat, "Weapon") ||
        util_str_contains_ci(trader_cat, "Pistol") ||
        util_str_contains_ci(trader_cat, "Shotgun") ||
        util_str_contains_ci(trader_cat, "Submachine") ||
        util_str_contains_ci(trader_cat, "Launcher") ||
        util_str_contains_ci(trader_cat, "Sniper"))
        return "W";
    if (util_str_contains_ci(trader_cat, "Cars") ||
        util_str_contains_ci(trader_cat, "Trucks") ||
        util_str_contains_ci(trader_cat, "Armoured") ||
        util_str_contains_ci(trader_cat, "Flight") ||
        util_str_contains_ci(trader_cat, "Boats"))
        return "VNK";
    return "*";
}

// ============================================================================
// MULTI-SHOP SYSTEM — each shop gets its own file for location-based trading
// ============================================================================
// Shops map trader_cat values to themed locations. Each shop outputs a
// separate file so server admins can place traders in different map locations.
//
// Shop definitions:
//   Weapons Shop    — Assault Rifles, Sniper Rifles, SMGs, Shotguns, Pistols,
//                     Launchers, Melee Weapons, High-Tier Weapons, Weapons
//   Weapon Parts    — Magazines, Ammunition, Optics, Suppressors,
//                     Weapon Attachments
//   Clothing & Gear — Clothing, Military Clothing, Tactical Gear, Helmets,
//                     Vests, Backpacks, Ghillie
//   Medical         — Medical
//   Food            — Food
//   Tools           — Tools, Navigation, Containers, Miscellaneous
//   Car Dealership  — Cars, Vehicle Parts
//   Truck Depot     — Trucks
//   Armoured Vehicles — Armoured Vehicles
//   Airfield        — Flight
//   Base Building   — Base Building, Storage
//   Harbour Master  — Boats, Fishing Gear (separate waterfront location)
//   Collectables    — Heirloom (HeirloomToken currency)
//   Black Market    — Black Market (Bitcoin currency; policy black_market tier)
//   Currency Exch.  — Bitcoin buy/sell with USD
// ============================================================================

#define MAX_SHOP_CATEGORIES 16

typedef struct {
    const char *shop_name;          // Human-readable shop name
    const char *filename_suffix;    // e.g. "Weapons" -> TraderWeapons.txt / TraderPlusWeapons.json
    const char *categories[MAX_SHOP_CATEGORIES]; // NULL-terminated list of trader_cat values
    bool is_black_market;           // true = all items priced in Bitcoin
    bool is_heirloom;               // true = all items priced in HeirloomToken
    bool is_exchange;               // true = Currency Exchange ATM (dynamic pricing)
} ShopDefinition;

static const ShopDefinition SHOP_DEFINITIONS[] = {
    {
        "Weapons",
        "Weapons",
        {"Assault Rifles", "Sniper Rifles", "Submachine Guns", "Shotguns",
         "Pistols", "Launchers", "Melee Weapons", "High-Tier Weapons", "Weapons", NULL},
        false, false, false
    },
    {
        "Weapon Parts & Attachments",
        "WeaponParts",
        {"Magazines", "Ammunition", "Optics", "Suppressors", "Weapon Attachments", NULL},
        false, false, false
    },
    {
        "Clothing & Gear",
        "Clothing",
        {"Clothing", "Military Clothing", "Tactical Gear", "Helmets",
         "Vests", "Backpacks", "Ghillie", NULL},
        false, false, false
    },
    {
        "Medical Supplies",
        "Medical",
        {"Medical", NULL},
        false, false, false
    },
    {
        "Food & Drink",
        "Food",
        {"Food", NULL},
        false, false, false
    },
    {
        "Tools & Navigation",
        "Tools",
        {"Tools", "Navigation", "Containers", "Miscellaneous", NULL},
        false, false, false
    },
    {
        "Car Dealership",
        "Cars",
        {"Cars", "Vehicle Parts", NULL},
        false, false, false
    },
    {
        "Truck Depot",
        "Trucks",
        {"Trucks", NULL},
        false, false, false
    },
    {
        "Armoured Vehicles",
        "ArmouredVehicles",
        {"Armoured Vehicles", NULL},
        false, false, false
    },
    {
        "Airfield",
        "Flight",
        {"Flight", NULL},
        false, false, false
    },
    {
        "Base Building & Storage",
        "BaseBuilding",
        {"Base Building", "Storage", NULL},
        false, false, false
    },
    {
        "Harbour Master",
        "HarbourMaster",
        {"Boats", "Fishing Gear", NULL},
        false, false, false
    },
    {
        "Collectables",
        "Collectables",
        {"Heirloom", NULL},
        false, true, false
    },
    {
        "Black Market",
        "BlackMarket",
        {"Black Market", NULL},
        true, false, false
    },
    {
        "Currency Exchange",
        "ATM",
        {"Currency", NULL},
        false, false, true
    },
    {NULL, NULL, {NULL}, false, false, false} // Sentinel
};

#define SHOP_COUNT 15  // Excluding sentinel

static bool item_belongs_to_shop(const LootItem *item, const ShopDefinition *shop) {
    // Contraband / no-trade tiers are excluded from every trader.
    if (!lp_tier_tradeable(item->assigned_tier)) return false;
    // Black market shop: match by black_market flag OR admin_only (T9-T11 items)
    if (shop->is_black_market) {
        return item->black_market || item->admin_only;
    }
    // Exchange shop: handled separately (currency items only)
    if (shop->is_exchange) {
        return false; // Exchange shop uses is_atm_exchangeable() directly
    }
    // Category-based match
    for (int c = 0; shop->categories[c] != NULL; c++) {
        if (util_strcasecmp(item->trader_cat, shop->categories[c]) == 0)
            return true;
    }
    return false;
}

static bool is_currency_item_for_export(const LootItem *item) {
    return (util_str_contains_ci(item->classname, "Money_Dollar") ||
            util_str_contains_ci(item->classname, "Money_Euro") ||
            util_str_contains_ci(item->classname, "Money_Ruble") ||
            util_str_contains_ci(item->classname, "Money_Bitcoin") ||
            util_str_contains_ci(item->classname, "HeirloomToken"));
}

// ── Currency Exchange (ATM) System ──────────────────────────────────────────
// Players find physical currency bills (Dollar, Euro, Ruble) in the world.
// At an ATM trader, they can exchange foreign currencies for USD wallet credit
// and purchase Bitcoin for use at the Black Market.
//
// Exchange rates (all relative to CJ187-Money-Dollars-Only / USD wallet):
//   Dollar deposits: 90% of face value (10% ATM fee)
//   Euro → USD:      buy at 85%, sell at 60% of face value
//   Ruble → USD:     buy at 1.5¢, sell at 1.0¢ per Ruble (heavily devalued)
//   Bitcoin:         buy $15,000, sell $10,000 (premium currency)
//   HeirloomToken:   NOT exchangeable (exclusive to Collectables shop)
// ────────────────────────────────────────────────────────────────────────────

typedef enum {
    EXCHG_DOLLAR,
    EXCHG_EURO,
    EXCHG_RUBLE,
    EXCHG_BITCOIN,
    EXCHG_HEIRLOOM,
    EXCHG_UNKNOWN
} ExchangeCurrencyType;

// Exchange rate constants
#define ATM_DOLLAR_SELL_RATE    0.90    // Deposit fee: lose 10%
#define ATM_EURO_BUY_RATE       0.85    // Buy 1 EUR for $0.85
#define ATM_EURO_SELL_RATE      0.60    // Sell 1 EUR for $0.60
#define ATM_RUBLE_BUY_RATE      0.015   // Buy 1 RUB for $0.015
#define ATM_RUBLE_SELL_RATE     0.010   // Sell 1 RUB for $0.010
#define ATM_BITCOIN_BUY         15000   // Buy 1 BTC for $15,000
#define ATM_BITCOIN_SELL        10000   // Sell 1 BTC for $10,000

static ExchangeCurrencyType get_exchange_currency_type(const char *classname) {
    if (util_str_contains_ci(classname, "Bitcoin"))  return EXCHG_BITCOIN;
    if (util_str_contains_ci(classname, "Dollar"))   return EXCHG_DOLLAR;
    if (util_str_contains_ci(classname, "Euro"))     return EXCHG_EURO;
    if (util_str_contains_ci(classname, "Ruble"))    return EXCHG_RUBLE;
    if (util_str_contains_ci(classname, "Heirloom")) return EXCHG_HEIRLOOM;
    return EXCHG_UNKNOWN;
}

static const char *get_exchange_category_name(ExchangeCurrencyType type) {
    switch (type) {
        case EXCHG_DOLLAR:  return "Dollar Deposits";
        case EXCHG_EURO:    return "Euro Exchange";
        case EXCHG_RUBLE:   return "Ruble Exchange";
        case EXCHG_BITCOIN: return "Bitcoin Exchange";
        default:            return "Currency Exchange";
    }
}

static int extract_denomination(const char *classname) {
    // Extract trailing numeric suffix: "Money_Dollar100" -> 100
    int len = (int)strlen(classname);
    int end = len;
    int start = end;
    while (start > 0 && classname[start - 1] >= '0' && classname[start - 1] <= '9')
        start--;
    if (start == end) return 1; // No numeric suffix -> denomination 1
    return atoi(&classname[start]);
}

static void calculate_atm_prices(const char *classname, int *buy_price, int *sell_price) {
    ExchangeCurrencyType type = get_exchange_currency_type(classname);
    int denom = extract_denomination(classname);

    switch (type) {
        case EXCHG_DOLLAR:
            *buy_price = -1;  // Can't buy dollars from ATM (money printer protection)
            *sell_price = (int)(denom * ATM_DOLLAR_SELL_RATE);
            if (*sell_price < 1) *sell_price = 1;
            break;
        case EXCHG_EURO:
            *buy_price  = (int)(denom * ATM_EURO_BUY_RATE);
            *sell_price = (int)(denom * ATM_EURO_SELL_RATE);
            if (*buy_price < 1)  *buy_price = 1;
            if (*sell_price < 1) *sell_price = 1;
            break;
        case EXCHG_RUBLE:
            *buy_price  = (int)(denom * ATM_RUBLE_BUY_RATE);
            *sell_price = (int)(denom * ATM_RUBLE_SELL_RATE);
            if (*buy_price < 1)  *buy_price = 1;
            if (*sell_price < 1) *sell_price = 1;
            break;
        case EXCHG_BITCOIN:
            *buy_price  = ATM_BITCOIN_BUY;
            *sell_price = ATM_BITCOIN_SELL;
            break;
        default:
            *buy_price  = -1;
            *sell_price = -1;
            break;
    }
}

// Currency items eligible for ATM exchange (everything except HeirloomToken)
static bool is_atm_exchangeable(const LootItem *item) {
    if (util_str_contains_ci(item->classname, "HeirloomToken")) return false;
    return is_currency_item_for_export(item);
}

// Resolve currency strings for standard/black_market from config files
static void resolve_currencies(char *standard_currency, size_t standard_len,
                               char *black_market_currency, size_t black_len) {
    standard_currency[0] = '\0';
    black_market_currency[0] = '\0';

    const char *currency_paths[] = {
        "./profile/TraderPlus/TraderPlusConfig.json",
        "./profiles/TraderPlus/TraderPlusConfig.json",
        "./TraderPlusConfig.json",
        "./config/TraderPlusConfig.json",
        "./config/currency.json",
        NULL
    };
    for (int i = 0; currency_paths[i] != NULL; i++) {
        if (util_file_exists(currency_paths[i])) {
            try_read_currency_file(currency_paths[i], standard_currency, standard_len,
                                   black_market_currency, black_len);
        }
    }
    if (standard_currency[0] == '\0') strncpy(standard_currency, "CJ187-Money-Dollars-Only", standard_len - 1);
    if (black_market_currency[0] == '\0') strncpy(black_market_currency, "CJ187-Money-Bitcoin", black_len - 1);
}

// ── TraderPlus JSON: write a single shop file ──
static bool write_traderplus_shop_file(AuditorContext *ctx, const char *filepath,
                                       const ShopDefinition *shop,
                                       const char *standard_currency,
                                       const char *black_market_currency) {
    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    // Determine the shop's currency
    const char *shop_currency = standard_currency;
    if (shop->is_black_market) shop_currency = black_market_currency;
    if (shop->is_heirloom) shop_currency = "HeirloomToken";

    fprintf(f, "{\n");
    fprintf(f, "  \"generated_by\": \"StelliferumAuditor\",\n");
    fprintf(f, "  \"shop_name\": \"%s\",\n", shop->shop_name);
    fprintf(f, "  \"currency\": \"%s\",\n", shop_currency);
    fprintf(f, "  \"sell_ratio\": 0.15,\n");
    fprintf(f, "  \"items\": [\n");

    int written = 0;

    // ── Exchange shop: write currency items with ATM exchange prices ──
    if (shop->is_exchange) {
        for (int i = 0; i < ctx->item_count; i++) {
            LootItem *item = &ctx->items[i];
            if (item->deleted) continue;
            if (!is_atm_exchangeable(item)) continue;

            int buy, sell;
            calculate_atm_prices(item->classname, &buy, &sell);
            if (buy <= 0 && sell <= 0) continue; // Not exchangeable

            ExchangeCurrencyType etype = get_exchange_currency_type(item->classname);
            const char *ecat = get_exchange_category_name(etype);

            if (written > 0) fprintf(f, ",\n");
            fprintf(f, "    {\"ClassName\":\"%s\",\"Tier\":%d,\"Category\":\"%s\",",
                    item->classname, item->assigned_tier, ecat);
            fprintf(f, "\"Currency\":\"%s\",\"BuyPrice\":%d,\"SellPrice\":%d,",
                    standard_currency, buy, sell);
            fprintf(f, "\"Buyable\":%s,\"Sellable\":%s,",
                    buy > 0 ? "true" : "false", sell > 0 ? "true" : "false");
            fprintf(f, "\"Stock\":100,\"RestockSeconds\":900,\"BlackMarket\":false,\"AdminOnly\":false}");
            written++;
        }

        fprintf(f, "\n  ]\n}");
        fclose(f);
        return written > 0;
    }

    // ── Standard shop: write items by category ──
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (loot_is_zombie(item) || loot_is_animal(item)) continue;
        if (is_currency_item_for_export(item)) continue;
        if (!item_belongs_to_shop(item, shop)) continue;

        if (written > 0) fprintf(f, ",\n");
        // Resolve per-item currency
        const char *currency = item->currency;
        if (!currency[0] || strcmp(currency, "TBD") == 0) currency = standard_currency;
        if (item->black_market || item->admin_only) currency = black_market_currency;
        if (util_strcasecmp(item->currency, "HeirloomToken") == 0) currency = "HeirloomToken";

        fprintf(f, "    {\"ClassName\":\"%s\",\"Tier\":%d,\"Category\":\"%s\",", item->classname, item->assigned_tier, item->trader_cat);
        fprintf(f, "\"Currency\":\"%s\",\"BuyPrice\":%d,\"SellPrice\":%d,", currency, item->buy_price, item->sell_price);
        fprintf(f, "\"Buyable\":%s,\"Sellable\":%s,", item->buy_price < 0 ? "false" : "true", item->sell_price > 0 ? "true" : "false");
        fprintf(f, "\"Stock\":%d,\"RestockSeconds\":%d,\"BlackMarket\":%s,\"AdminOnly\":%s}",
            item->stock_override, item->restock_override,
            item->black_market ? "true" : "false", item->admin_only ? "true" : "false");
        written++;
    }

    // Black Market shop: include Bitcoin ↔ USD exchange entry
    if (shop->is_black_market) {
        if (written > 0) fprintf(f, ",\n");
        fprintf(f, "    {\"ClassName\":\"CJ187-Money-Bitcoin\",\"Tier\":1,\"Category\":\"Currency Exchange\",");
        fprintf(f, "\"Currency\":\"%s\",\"BuyPrice\":10000,\"SellPrice\":7500,", standard_currency);
        fprintf(f, "\"Buyable\":true,\"Sellable\":true,");
        fprintf(f, "\"Stock\":50,\"RestockSeconds\":1800,\"BlackMarket\":false,\"AdminOnly\":false}");
        written++;
    }

    fprintf(f, "\n  ]\n}");
    fclose(f);
    return written > 0;
}

// ── Dr. Jones TXT: write a single shop file ──
static bool write_drjones_shop_file(AuditorContext *ctx, const char *filepath,
                                    const ShopDefinition *shop,
                                    const char *standard_currency,
                                    const char *black_market_currency) {
    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    fprintf(f, "// Generated by StelliferumAuditor — Dr. Jones Trader Format\n");
    fprintf(f, "// Shop: %s\n", shop->shop_name);
    if (shop->is_exchange)
        fprintf(f, "// Currency Exchange ATM — deposit cash, exchange foreign currency, buy Bitcoin\n");
    else if (shop->is_black_market)
        fprintf(f, "// Currency: Bitcoin (CJ187-Money-Bitcoin)\n");
    else if (shop->is_heirloom)
        fprintf(f, "// Currency: HeirloomToken\n");
    else
        fprintf(f, "// Currency: CJ187-Money-Dollars-Only (USD)\n");
    fprintf(f, "\n");

    int written = 0;

    // ── Exchange shop: group currency items by exchange category ──
    if (shop->is_exchange) {
        // Write each exchange category in a logical order
        static const ExchangeCurrencyType exchange_order[] = {
            EXCHG_DOLLAR, EXCHG_EURO, EXCHG_RUBLE, EXCHG_BITCOIN
        };
        static const int exchange_order_count = 4;

        for (int eo = 0; eo < exchange_order_count; eo++) {
            ExchangeCurrencyType target_type = exchange_order[eo];
            const char *cat_name = get_exchange_category_name(target_type);
            bool header_written = false;

            for (int i = 0; i < ctx->item_count; i++) {
                LootItem *item = &ctx->items[i];
                if (item->deleted) continue;
                if (!is_atm_exchangeable(item)) continue;

                ExchangeCurrencyType etype = get_exchange_currency_type(item->classname);
                if (etype != target_type) continue;

                int buy, sell;
                calculate_atm_prices(item->classname, &buy, &sell);
                if (buy <= 0 && sell <= 0) continue;

                if (!header_written) {
                    fprintf(f, "<Category> %s\n", cat_name);
                    header_written = true;
                }

                fprintf(f, "\t%s,\t*,\t%d,\t%d\n", item->classname, buy, sell);
                written++;
            }
            if (header_written) fprintf(f, "\n");
        }

        fclose(f);
        return written > 0;
    }

    // ── Standard shop: collect unique categories then write items ──
    typedef struct { const char *name; int count; } CatEntry;
    CatEntry categories[64];
    int cat_count = 0;

    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (loot_is_zombie(item) || loot_is_animal(item)) continue;
        if (is_currency_item_for_export(item)) continue;
        if (item->trader_cat[0] == '\0') continue;
        if (!item_belongs_to_shop(item, shop)) continue;

        bool found = false;
        for (int c = 0; c < cat_count; c++) {
            if (util_strcasecmp(categories[c].name, item->trader_cat) == 0) {
                categories[c].count++;
                found = true;
                break;
            }
        }
        if (!found && cat_count < 64) {
            categories[cat_count].name = item->trader_cat;
            categories[cat_count].count = 1;
            cat_count++;
        }
    }

    for (int c = 0; c < cat_count; c++) {
        fprintf(f, "<Category> %s\n", categories[c].name);

        for (int i = 0; i < ctx->item_count; i++) {
            LootItem *item = &ctx->items[i];
            if (item->deleted) continue;
            if (loot_is_zombie(item) || loot_is_animal(item)) continue;
            if (is_currency_item_for_export(item)) continue;
            if (!item_belongs_to_shop(item, shop)) continue;
            if (util_strcasecmp(item->trader_cat, categories[c].name) != 0) continue;

            const char *flag = drjones_item_flag(item->trader_cat);
            int buy  = item->buy_price > 0 ? item->buy_price : -1;
            int sell = item->sell_price > 0 ? item->sell_price : -1;

            fprintf(f, "\t%s,\t%s,\t%d,\t%d\n", item->classname, flag, buy, sell);
            written++;
        }
        fprintf(f, "\n");
    }

    // Black Market shop: Currency Exchange category for Bitcoin ↔ USD
    if (shop->is_black_market) {
        fprintf(f, "<Category> Currency Exchange\n");
        fprintf(f, "\tCJ187-Money-Bitcoin,\t*,\t10000,\t7500\n");
        fprintf(f, "\n");
        written++;
    }

    fclose(f);
    return written > 0;
}

// ============================================================================
// MULTI-SHOP EXPORT: Generates all shop files
// ============================================================================

bool writer_export_trader_shops(AuditorContext *ctx, const char *output_dir) {
    if (!ctx || !output_dir) return false;

    char standard_currency[64];
    char black_market_currency[64];
    resolve_currencies(standard_currency, sizeof(standard_currency),
                       black_market_currency, sizeof(black_market_currency));

    char shops_dir[MAX_PATH_LEN];
    snprintf(shops_dir, sizeof(shops_dir), "%s/shops", output_dir);
    util_ensure_directory(shops_dir);

    int total_shops = 0, total_items = 0;
    bool is_drjones = (ctx->shop_mod == SHOP_MOD_DRJONES);
    const char *ext = is_drjones ? "txt" : "json";

    for (int s = 0; SHOP_DEFINITIONS[s].shop_name != NULL; s++) {
        const ShopDefinition *shop = &SHOP_DEFINITIONS[s];
        char filepath[MAX_PATH_LEN];

        if (is_drjones) {
            snprintf(filepath, sizeof(filepath), "%s/Trader%s.txt", shops_dir, shop->filename_suffix);
        } else {
            snprintf(filepath, sizeof(filepath), "%s/TraderPlus%s.json", shops_dir, shop->filename_suffix);
        }

        bool ok;
        if (is_drjones) {
            ok = write_drjones_shop_file(ctx, filepath, shop, standard_currency, black_market_currency);
        } else {
            ok = write_traderplus_shop_file(ctx, filepath, shop, standard_currency, black_market_currency);
        }

        if (ok) {
            // Count items for this shop
            int shop_items = 0;
            for (int i = 0; i < ctx->item_count; i++) {
                LootItem *item = &ctx->items[i];
                if (item->deleted) continue;
                if (shop->is_exchange) {
                    if (is_atm_exchangeable(item)) shop_items++;
                } else {
                    if (loot_is_zombie(item) || loot_is_animal(item)) continue;
                    if (is_currency_item_for_export(item)) continue;
                    if (item_belongs_to_shop(item, shop)) shop_items++;
                }
            }
            util_log(SEVERITY_INFO, "Shop export: %s -> %s (%d items)", shop->shop_name, filepath, shop_items);
            total_shops++;
            total_items += shop_items;
        }
    }

    util_log(SEVERITY_INFO, "Multi-shop export: %d shops, %d total items -> %s/*.%s",
             total_shops, total_items, shops_dir, ext);
    return total_shops > 0;
}

// ============================================================================
// LEGACY SINGLE-FILE EXPORT (kept for backward compatibility)
// ============================================================================

static bool write_trader_export(AuditorContext *ctx, const char *filepath) {
    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    char standard_currency[64];
    char black_market_currency[64];
    resolve_currencies(standard_currency, sizeof(standard_currency),
                       black_market_currency, sizeof(black_market_currency));

    fprintf(f, "{\n");
    fprintf(f, "  \"generated_by\": \"StelliferumAuditor\",\n");
    fprintf(f, "  \"standard_currency\": \"%s\",\n", standard_currency);
    fprintf(f, "  \"black_market_currency\": \"%s\",\n", black_market_currency);
    fprintf(f, "  \"heirloom_currency\": \"HeirloomToken\",\n");
    fprintf(f, "  \"sell_ratio\": 0.15,\n");
    fprintf(f, "  \"items\": [\n");

    int written = 0;
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (loot_is_zombie(item) || loot_is_animal(item)) continue;
        if (is_currency_item_for_export(item)) continue;

        if (written > 0) fprintf(f, ",\n");
        const char *currency = item->currency;
        if (!currency[0] || strcmp(currency, "TBD") == 0) currency = standard_currency;
        if (item->black_market || item->admin_only) currency = black_market_currency;
        if (util_strcasecmp(item->currency, "HeirloomToken") == 0) currency = "HeirloomToken";

        fprintf(f, "    {\"ClassName\":\"%s\",\"Tier\":%d,\"Category\":\"%s\",", item->classname, item->assigned_tier, item->trader_cat);
        fprintf(f, "\"Currency\":\"%s\",\"BuyPrice\":%d,\"SellPrice\":%d,", currency, item->buy_price, item->sell_price);
        fprintf(f, "\"Buyable\":%s,\"Sellable\":%s,", item->buy_price < 0 ? "false" : "true", item->sell_price > 0 ? "true" : "false");
        fprintf(f, "\"Stock\":%d,\"RestockSeconds\":%d,\"BlackMarket\":%s,\"AdminOnly\":%s}",
            item->stock_override, item->restock_override,
            item->black_market ? "true" : "false", item->admin_only ? "true" : "false");
        written++;
    }

    // Special entry: Bitcoin can be bought with USD
    if (written > 0) fprintf(f, ",\n");
    fprintf(f, "    {\"ClassName\":\"CJ187-Money-Bitcoin\",\"Tier\":1,\"Category\":\"Currency Exchange\",");
    fprintf(f, "\"Currency\":\"%s\",\"BuyPrice\":10000,\"SellPrice\":7500,", standard_currency);
    fprintf(f, "\"Buyable\":true,\"Sellable\":true,");
    fprintf(f, "\"Stock\":50,\"RestockSeconds\":1800,\"BlackMarket\":false,\"AdminOnly\":false}");
    written++;

    fprintf(f, "\n  ]\n}");
    fclose(f);
    return true;
}

bool writer_export_trader_config(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    if (!write_trader_export(ctx, filepath)) return false;
    return true;
}

// ============================================================================
// SHOP MOD HELPERS
// ============================================================================

const char* shop_mod_name(ShopMod mod) {
    switch (mod) {
        case SHOP_MOD_TRADERPLUS: return "TraderPlus";
        case SHOP_MOD_DRJONES:    return "Dr. Jones Trader";
        case SHOP_MOD_EXPANSION:  return "Expansion Market";
        default:                  return "Unknown";
    }
}

const char* shop_mod_output_filename(ShopMod mod) {
    switch (mod) {
        case SHOP_MOD_TRADERPLUS: return "output/TraderPlusTrading.json";
        case SHOP_MOD_DRJONES:    return "output/TraderConfig.txt";
        case SHOP_MOD_EXPANSION:  return "output/ExpansionMarket.json";
        default:                  return "output/trader_config.json";
    }
}

const char* shop_mod_default_remote_path(ShopMod mod) {
    switch (mod) {
        case SHOP_MOD_TRADERPLUS: return "profiles/TraderPlus/TraderPlusTrading.json";
        case SHOP_MOD_DRJONES:    return "profiles/Trader/TraderConfig.txt";
        case SHOP_MOD_EXPANSION:  return "profiles/ExpansionMod/Market/ExpansionMarket.json";
        default:                  return "profiles/TraderPlus/TraderPlusTrading.json";
    }
}

// Legacy single-file Dr. Jones export (still used for combined output)
static bool write_drjones_export(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    fprintf(f, "// Generated by StelliferumAuditor — Dr. Jones Trader Format\n");
    fprintf(f, "// Items: %d (excluding deleted, zombies, animals)\n", ctx->item_count);
    fprintf(f, "// NOTE: Per-shop files in output/shops/ for location-based traders\n\n");

    typedef struct { const char *name; int count; } CatEntry;
    CatEntry categories[64];
    int cat_count = 0;

    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (loot_is_zombie(item) || loot_is_animal(item)) continue;
        if (is_currency_item_for_export(item)) continue;
        if (item->trader_cat[0] == '\0') continue;

        bool found = false;
        for (int c = 0; c < cat_count; c++) {
            if (util_strcasecmp(categories[c].name, item->trader_cat) == 0) {
                categories[c].count++;
                found = true;
                break;
            }
        }
        if (!found && cat_count < 64) {
            categories[cat_count].name = item->trader_cat;
            categories[cat_count].count = 1;
            cat_count++;
        }
    }

    int written = 0;
    for (int c = 0; c < cat_count; c++) {
        fprintf(f, "<Category> %s\n", categories[c].name);
        for (int i = 0; i < ctx->item_count; i++) {
            LootItem *item = &ctx->items[i];
            if (item->deleted) continue;
            if (loot_is_zombie(item) || loot_is_animal(item)) continue;
            if (is_currency_item_for_export(item)) continue;
            if (util_strcasecmp(item->trader_cat, categories[c].name) != 0) continue;

            const char *flag = drjones_item_flag(item->trader_cat);
            int buy  = item->buy_price > 0 ? item->buy_price : -1;
            int sell = item->sell_price > 0 ? item->sell_price : -1;
            fprintf(f, "\t%s,\t%s,\t%d,\t%d\n", item->classname, flag, buy, sell);
            written++;
        }
        fprintf(f, "\n");
    }

    // Bitcoin exchange entry
    fprintf(f, "<Category> Currency Exchange\n");
    fprintf(f, "\tCJ187-Money-Bitcoin,\t*,\t10000,\t7500\n\n");
    written++;

    fclose(f);
    util_log(SEVERITY_INFO, "Dr. Jones export: %d items in %d categories -> %s", written, cat_count, filepath);
    return true;
}

bool writer_export_drjones_config(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    return write_drjones_export(ctx, filepath);
}

// ============================================================================
// SHOP MOD DISPATCH — exports BOTH legacy single file AND per-shop files
// ============================================================================

bool writer_export_trader_by_shop_mod(AuditorContext *ctx) {
    if (!ctx) return false;
    const char *output = shop_mod_output_filename(ctx->shop_mod);
    util_log(SEVERITY_INFO, "Trader export: using %s format -> %s", shop_mod_name(ctx->shop_mod), output);

    bool ok = false;
    switch (ctx->shop_mod) {
        case SHOP_MOD_DRJONES:
            ok = writer_export_drjones_config(ctx, output);
            break;
        case SHOP_MOD_EXPANSION:
            util_log(SEVERITY_WARNING, "Expansion Market export not yet implemented, falling back to TraderPlus.");
            ok = writer_export_trader_config(ctx, shop_mod_output_filename(SHOP_MOD_TRADERPLUS));
            break;
        case SHOP_MOD_TRADERPLUS:
        default:
            ok = writer_export_trader_config(ctx, output);
            break;
    }

    // Always generate per-shop files alongside the legacy combined file
    writer_export_trader_shops(ctx, "output");
    return ok;
}

bool writer_create_backup(const char *filepath, const char *backup_dir) { return true; }

// ============================================================================
// CFGECONOMYCORE.XML GENERATOR
// ============================================================================

/**
 * Generate cfgeconomycore.xml — the Central Economy manifest.
 *
 * IMPORTANT — the engine loads the default mission economy files implicitly:
 *   db/types.xml, db/globals.xml, db/events.xml, db/economy.xml,
 *   db/messages.xml, cfgspawnabletypes.xml, cfgrandompresets.xml,
 *   and the env/ territory files (referenced from cfgenvironment.xml).
 * Registering any of those here makes the CE load them a SECOND time —
 * duplicate type registration corrupts the heap (C0000374) during
 * Hive::InitOffline().  Vanilla cfgeconomycore.xml contains NO <ce>
 * entries at all (see BohemiaInteractive/DayZ-Central-Economy).
 *
 * Only ADDITIONAL files may be registered here.  The auditor adds exactly
 * two: db/types_infected.xml and db/types_wildlife.xml (both type="types").
 * Valid <file type="..."> identifiers are: types, spawnabletypes, globals,
 * economy, events, messages — "territories" and "randompresets" are NOT
 * valid and are silently ignored or rejected by the engine.
 */
bool writer_export_cfgeconomycore(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    fprintf(f, "<!-- Generated by StelliferumAuditor — Central Economy Manifest -->\n");
    fprintf(f, "<economycore>\n");

    // --- rootclass definitions: REQUIRED by the CE to recognize item hierarchies ---
    // Without these, the engine doesn't know what classes can spawn.
    fprintf(f, "    <classes>\n");
    fprintf(f, "        <rootclass name=\"DefaultWeapon\" /> <!-- weapons -->\n");
    fprintf(f, "        <rootclass name=\"DefaultMagazine\" /> <!-- magazines -->\n");
    fprintf(f, "        <rootclass name=\"Inventory_Base\" /> <!-- inventory items -->\n");
    fprintf(f, "        <rootclass name=\"HouseNoDestruct\" reportMemoryLOD=\"no\" /> <!-- houses, wrecks -->\n");
    fprintf(f, "        <rootclass name=\"SurvivorBase\" act=\"character\" reportMemoryLOD=\"no\" /> <!-- player characters -->\n");
    fprintf(f, "        <rootclass name=\"DZ_LightAI\" act=\"character\" reportMemoryLOD=\"no\" /> <!-- infected, animals -->\n");
    fprintf(f, "        <rootclass name=\"CarScript\" act=\"car\" reportMemoryLOD=\"no\" /> <!-- cars -->\n");
    fprintf(f, "        <rootclass name=\"BoatScript\" act=\"car\" reportMemoryLOD=\"no\" /> <!-- boats -->\n");
    fprintf(f, "    </classes>\n");

    // --- CE defaults: dynamic spawn tuning + logging flags ---
    fprintf(f, "    <defaults>\n");
    fprintf(f, "        <default name=\"dyn_radius\" value=\"30\" />\n");
    fprintf(f, "        <default name=\"dyn_smin\" value=\"0\" />\n");
    fprintf(f, "        <default name=\"dyn_smax\" value=\"0\" />\n");
    fprintf(f, "        <default name=\"dyn_dmin\" value=\"1\" />\n");
    fprintf(f, "        <default name=\"dyn_dmax\" value=\"5\" />\n");
    fprintf(f, "        <default name=\"log_ce_loop\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_ce_dynamicevent\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_ce_vehicle\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_ce_lootspawn\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_ce_lootcleanup\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_ce_lootrespawn\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_ce_statistics\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_ce_zombie\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_storageinfo\" value=\"false\" />\n");
    fprintf(f, "        <default name=\"log_hivewarning\" value=\"true\" />\n");
    fprintf(f, "        <default name=\"log_missionfilewarning\" value=\"true\" />\n");
    fprintf(f, "        <default name=\"save_events_startup\" value=\"true\" />\n");
    fprintf(f, "        <default name=\"save_types_startup\" value=\"true\" />\n");
    fprintf(f, "    </defaults>\n");

    // --- ce: register ONLY the auditor's ADDITIONAL type files ---
    // The default economy files (types.xml, globals.xml, events.xml,
    // cfgspawnabletypes.xml, cfgrandompresets.xml, env territories) are
    // loaded implicitly by the engine — never register them here (see
    // header comment: double registration = CE heap corruption C0000374).
    int extra_count = 0;
    bool has_infected = util_file_exists("output/zombie_tiers/types_infected.xml");
    bool has_wildlife = util_file_exists("output/zombie_tiers/types_wildlife.xml");
    if (has_infected || has_wildlife) {
        fprintf(f, "    <ce folder=\"db\">\n");
        if (has_infected) {
            fprintf(f, "        <file name=\"types_infected.xml\" type=\"types\" />\n");
            extra_count++;
        }
        if (has_wildlife) {
            fprintf(f, "        <file name=\"types_wildlife.xml\" type=\"types\" />\n");
            extra_count++;
        }
        fprintf(f, "    </ce>\n");
    }

    fprintf(f, "</economycore>\n");
    fclose(f);

    util_log(SEVERITY_INFO, "Exported cfgeconomycore.xml -> %s (%d custom file registration%s, default files load implicitly)",
             filepath, extra_count, extra_count == 1 ? "" : "s");
    return true;
}

// ============================================================================
// CFGLIMITSDEFINITIONUSER.XML GENERATOR
// ============================================================================

/**
 * Export cfglimitsdefinitionuser.xml — custom value/usage flag combinations.
 *
 * DayZ's CE only recognizes Tier1-Tier4 + Unique as base valueflags
 * (defined in cfglimitsdefinition.xml).  This file defines user combinations
 * that let single items span multiple tiers with one tag.
 *
 * The auditor's 11-tier internal system maps to these CE value flags:
 *   T1-T4  → Tier1-Tier4 directly (civilian progression)
 *   T5-T7  → Tier3-Tier4 (military progression via usage=Military)
 *   T8-T11 → Tier4 only   (endgame, restricted by low nominals + usage)
 *
 * Useful combos for multi-tier spanning items:
 *   Tier12   = Coast→Inland   (T1-T2 items)
 *   Tier23   = Towns→Hunting  (T3-T4 items)
 *   Tier34   = Military span  (T5-T6 items)
 *   Tier123  = Coast→Towns    (T1-T3 broad)
 *   Tier234  = Inland→Military(T2-T4 broad)
 *   Tier1234 = Everywhere     (universal items)
 */
bool writer_export_cfglimitsdefinitionuser(AuditorContext *ctx, const char *filepath) {
    if (!ctx || !filepath) return false;
    FILE *f = fopen(filepath, "w");
    if (!f) return false;

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    fprintf(f, "<!-- Generated by StelliferumAuditor — Custom CE Value/Usage Flag Combinations -->\n");
    fprintf(f, "<!-- These define shorthand tier combos so items can span multiple map zones -->\n");
    fprintf(f, "<user_lists>\n");

    // Usage flag combinations
    fprintf(f, "    <usageflags>\n");
    fprintf(f, "        <user name=\"TownVillage\">\n");
    fprintf(f, "            <usage name=\"Town\" />\n");
    fprintf(f, "            <usage name=\"Village\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"TownVillageOfficeSchool\">\n");
    fprintf(f, "            <usage name=\"Town\" />\n");
    fprintf(f, "            <usage name=\"Village\" />\n");
    fprintf(f, "            <usage name=\"Office\" />\n");
    fprintf(f, "            <usage name=\"School\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"TownVillageFarm\">\n");
    fprintf(f, "            <usage name=\"Town\" />\n");
    fprintf(f, "            <usage name=\"Village\" />\n");
    fprintf(f, "            <usage name=\"Farm\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"TownVillageFarmCoast\">\n");
    fprintf(f, "            <usage name=\"Town\" />\n");
    fprintf(f, "            <usage name=\"Village\" />\n");
    fprintf(f, "            <usage name=\"Farm\" />\n");
    fprintf(f, "            <usage name=\"Coast\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"HuntingPolice\">\n");
    fprintf(f, "            <usage name=\"Hunting\" />\n");
    fprintf(f, "            <usage name=\"Police\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"HuntingPoliceTown\">\n");
    fprintf(f, "            <usage name=\"Hunting\" />\n");
    fprintf(f, "            <usage name=\"Police\" />\n");
    fprintf(f, "            <usage name=\"Town\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"IndustrialFarmTown\">\n");
    fprintf(f, "            <usage name=\"Industrial\" />\n");
    fprintf(f, "            <usage name=\"Farm\" />\n");
    fprintf(f, "            <usage name=\"Town\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"MilitaryHunting\">\n");
    fprintf(f, "            <usage name=\"Military\" />\n");
    fprintf(f, "            <usage name=\"Hunting\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"MilitaryPolice\">\n");
    fprintf(f, "            <usage name=\"Military\" />\n");
    fprintf(f, "            <usage name=\"Police\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"MilitaryIndustrial\">\n");
    fprintf(f, "            <usage name=\"Military\" />\n");
    fprintf(f, "            <usage name=\"Industrial\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"HuntingIndustrial\">\n");
    fprintf(f, "            <usage name=\"Hunting\" />\n");
    fprintf(f, "            <usage name=\"Industrial\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "    </usageflags>\n");

    // Value flag combinations — span multiple geographic tiers
    fprintf(f, "    <valueflags>\n");
    fprintf(f, "        <user name=\"Tier12\">\n");
    fprintf(f, "            <value name=\"Tier1\" />\n");
    fprintf(f, "            <value name=\"Tier2\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"Tier23\">\n");
    fprintf(f, "            <value name=\"Tier2\" />\n");
    fprintf(f, "            <value name=\"Tier3\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"Tier34\">\n");
    fprintf(f, "            <value name=\"Tier3\" />\n");
    fprintf(f, "            <value name=\"Tier4\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"Tier123\">\n");
    fprintf(f, "            <value name=\"Tier1\" />\n");
    fprintf(f, "            <value name=\"Tier2\" />\n");
    fprintf(f, "            <value name=\"Tier3\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"Tier234\">\n");
    fprintf(f, "            <value name=\"Tier2\" />\n");
    fprintf(f, "            <value name=\"Tier3\" />\n");
    fprintf(f, "            <value name=\"Tier4\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "        <user name=\"Tier1234\">\n");
    fprintf(f, "            <value name=\"Tier1\" />\n");
    fprintf(f, "            <value name=\"Tier2\" />\n");
    fprintf(f, "            <value name=\"Tier3\" />\n");
    fprintf(f, "            <value name=\"Tier4\" />\n");
    fprintf(f, "        </user>\n");
    fprintf(f, "    </valueflags>\n");

    fprintf(f, "</user_lists>\n");
    fclose(f);

    util_log(SEVERITY_INFO, "Exported cfglimitsdefinitionuser.xml -> %s", filepath);
    return true;
}

// ============================================================================
// CFGRANDOMPRESETS.XML MERGER
// ============================================================================

#define RP_MAX_KEYS 2048

// Emit each top-level <cargo>/<attachments> preset element from `inner`, skipping
// any whose (kind + name) key was already written. Concatenating raw file bodies
// (vanilla + every mod) otherwise produces duplicate preset names, which the DayZ
// CE flags on load. Keys are owned copies tracked in seen_keys[].
static void emit_presets_deduped(FILE *out, const char *inner, size_t inner_len,
                                 char **seen_keys, int *seen_count,
                                 int *dup_count) {
    const char *p = inner;
    const char *limit = inner + inner_len;
    while (p < limit) {
        const char *c = strstr(p, "<cargo");
        const char *a = strstr(p, "<attachments");
        if (c && c >= limit) c = NULL;
        if (a && a >= limit) a = NULL;

        const char *tag, *kind, *closetag;
        if (c && (!a || c < a)) { tag = c; kind = "cargo";       closetag = "</cargo>"; }
        else if (a)             { tag = a; kind = "attachments"; closetag = "</attachments>"; }
        else break;

        const char *ce = strstr(tag, closetag);
        if (!ce || ce >= limit) break;
        const char *elem_end = ce + strlen(closetag);

        // Extract name="..." from within the opening tag only.
        char name[128] = {0};
        const char *gt = strchr(tag, '>');
        const char *np = strstr(tag, "name=");
        if (np && gt && np < gt) {
            const char *q = strchr(np, '"');
            if (q) {
                q++;
                const char *qe = strchr(q, '"');
                if (qe && (qe - q) < (long)sizeof(name)) {
                    memcpy(name, q, (size_t)(qe - q));
                    name[qe - q] = '\0';
                }
            }
        }

        bool write_it = true;
        if (name[0] != '\0') {
            char key[160];
            snprintf(key, sizeof(key), "%s:%s", kind, name);
            for (int i = 0; i < *seen_count; i++) {
                if (util_strcasecmp(seen_keys[i], key) == 0) { write_it = false; break; }
            }
            if (write_it && *seen_count < RP_MAX_KEYS) {
                size_t klen = strlen(key) + 1;
                char *owned = (char *)malloc(klen);
                if (owned) { memcpy(owned, key, klen); seen_keys[(*seen_count)++] = owned; }
            }
        }

        if (write_it) {
            fprintf(out, "    ");
            fwrite(tag, 1, (size_t)(elem_end - tag), out);
            fprintf(out, "\n");
        } else {
            (*dup_count)++;
        }
        p = elem_end;
    }
}

/**
 * Merge all randompresets files from sorted/randompresets/ into a single cfgrandompresets.xml.
 * Each file contains <cargo>/<attachments> presets inside a <randompresets> root.
 * Merge strategy: emit each preset element once, deduped by (kind + name).
 */
bool writer_merge_random_presets(AuditorContext *ctx, const char *sorted_root, const char *output_path) {
    if (!ctx || !sorted_root || !output_path) return false;

    char preset_dir[MAX_PATH_LEN];
    snprintf(preset_dir, sizeof(preset_dir), "%s/randompresets", sorted_root);

    FILE *out = fopen(output_path, "w");
    if (!out) return false;

    fprintf(out, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    fprintf(out, "<!-- Generated by StelliferumAuditor — Merged Random Presets -->\n");
    fprintf(out, "<randompresets>\n");

    int merged = 0;
    char *rp_seen[RP_MAX_KEYS];
    int   rp_seen_count = 0;
    int   rp_dupes = 0;

#ifdef _WIN32
    char search[MAX_PATH_LEN];
    snprintf(search, sizeof(search), "%s\\*.xml", preset_dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            char full_path[MAX_PATH_LEN];
            snprintf(full_path, sizeof(full_path), "%s\\%s", preset_dir, fd.cFileName);

            // Read the file and extract content between <randompresets> tags
            FILE *fp = fopen(full_path, "rb");
            if (!fp) continue;
            fseek(fp, 0, SEEK_END);
            long fsize = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (fsize <= 0 || fsize > 10 * 1024 * 1024) { fclose(fp); continue; }

            char *data = (char*)malloc((size_t)fsize + 1);
            if (!data) { fclose(fp); continue; }
            size_t bytes_read_rp = fread(data, 1, (size_t)fsize, fp);
            data[bytes_read_rp] = '\0';
            fclose(fp);

            // Find <randompresets> ... </randompresets>
            const char *start = strstr(data, "<randompresets>");
            if (!start) start = strstr(data, "<randompresets ");
            if (start) {
                start = strchr(start, '>');
                if (start) {
                    start++;
                    const char *end = strstr(start, "</randompresets>");
                    if (end && end > start) {
                        fprintf(out, "    <!-- Source: %s -->\n", fd.cFileName);
                        emit_presets_deduped(out, start, (size_t)(end - start),
                                             rp_seen, &rp_seen_count, &rp_dupes);
                        merged++;
                    }
                }
            }
            free(data);

        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
#endif

    // If no sorted presets exist, try to find and include the vanilla one
    if (merged == 0) {
        char local_root[256] = "downloaded_mods";
        util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));

        char vanilla_path[MAX_PATH_LEN];
        snprintf(vanilla_path, sizeof(vanilla_path), "%s/mpmissions/dayzOffline.chernarusplus/cfgrandompresets.xml", local_root);

        if (util_file_exists(vanilla_path)) {
            FILE *fp = fopen(vanilla_path, "rb");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long fsize = ftell(fp);
                fseek(fp, 0, SEEK_SET);
                if (fsize > 0 && fsize < 10 * 1024 * 1024) {
                    char *data = (char*)malloc((size_t)fsize + 1);
                    if (data) {
                        size_t bytes_read_vp = fread(data, 1, (size_t)fsize, fp);
                        data[bytes_read_vp] = '\0';
                        const char *start = strstr(data, "<randompresets>");
                        if (start) {
                            start = strchr(start, '>');
                            if (start) {
                                start++;
                                const char *end = strstr(start, "</randompresets>");
                                if (end && end > start) {
                                    fprintf(out, "    <!-- Source: vanilla cfgrandompresets.xml -->\n");
                                    emit_presets_deduped(out, start, (size_t)(end - start),
                                                         rp_seen, &rp_seen_count, &rp_dupes);
                                    merged++;
                                }
                            }
                        }
                        free(data);
                    }
                }
                fclose(fp);
            }
        }
    }

    fprintf(out, "</randompresets>\n");
    fclose(out);

    for (int i = 0; i < rp_seen_count; i++) free(rp_seen[i]);
    if (rp_dupes > 0)
        util_log(SEVERITY_WARNING, "Random presets: suppressed %d duplicate preset name(s) across sources", rp_dupes);
    util_log(SEVERITY_INFO, "Merged %d random preset files (%d unique presets) -> %s", merged, rp_seen_count, output_path);
    return merged > 0;
}

// ============================================================================
// ZOMBIE MOD CONFIGURATION EXPORT
// ============================================================================

/**
 * Export tiered zombie/animal configuration files:
 * 1. output/zombie_tiers/types_infected.xml      - Economy types for all zombies
 * 2. output/zombie_tiers/types_wildlife.xml       - Economy types for all animals
 * 3. output/zombie_tiers/ZOMBIE_MOD_DESIGN.md     - Mod design document for visual tiers
 * 4. output/zombie_tiers/zombie_tier_config.json  - Machine-readable tier definitions
 */
bool writer_export_zombie_config(AuditorContext *ctx, const char *output_dir) {
    if (!ctx || !output_dir) return false;

    // Guarantee sort order — zombie/wildlife adjacent-dedup requires it.
    auditor_sort_items(ctx);

    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/zombie_tiers", output_dir);
    util_ensure_directory(output_dir);
    util_ensure_directory(dir);
    
    // --- 1. Types XML for infected ---
    char path_infected[MAX_PATH_LEN];
    snprintf(path_infected, sizeof(path_infected), "%s/types_infected.xml", dir);
    FILE *fi = fopen(path_infected, "w");
    if (!fi) return false;
    
    fprintf(fi, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    fprintf(fi, "<!-- Generated by StelliferumAuditor - Tiered Infected Economy -->\n");
    fprintf(fi, "<!-- Tier 1=Green Eyes (Coast/Town) | Tier 2=Yellow (Industrial) | Tier 3=Red (Military) | Tier 4=Purple (Endgame) -->\n");
    fprintf(fi, "<types>\n");
    
    int zombie_count = 0, zombie_dupe = 0;
    WriterDedupSet zombie_dedup = writer_dedup_create();
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted || !loot_is_zombie(item)) continue;
        // Hash-set dedup: catches all duplicates regardless of sort adjacency
        if (writer_dedup_check(&zombie_dedup, ctx->items, ctx->item_count, i)) {
            zombie_dupe++;
            continue;
        }
        
        fprintf(fi, "    <type name=\"%s\">\n", item->classname);
        fprintf(fi, "        <nominal>%d</nominal>\n", item->nominal);
        fprintf(fi, "        <lifetime>%d</lifetime>\n", item->lifetime);
        fprintf(fi, "        <restock>%d</restock>\n", item->restock);
        fprintf(fi, "        <min>%d</min>\n", item->min);
        fprintf(fi, "        <quantmin>-1</quantmin>\n");
        fprintf(fi, "        <quantmax>-1</quantmax>\n");
        fprintf(fi, "        <cost>100</cost>\n");
        fprintf(fi, "        <flags count_in_cargo=\"0\" count_in_hoarder=\"0\" count_in_map=\"1\" count_in_player=\"0\" crafted=\"0\" deloot=\"0\"/>\n");
        /* Vanilla infected entries carry no <category>; "infected" is not a
           cfglimitsdefinition.xml category and triggers CE errors at boot.
           Only emit a category if it is actually a valid CE category. */
        if (item->category[0] != '\0' && is_valid_ce_category(item->category))
            fprintf(fi, "        <category name=\"%s\"/>\n", item->category);
        /* Same CE flag filtering as writer_export_merged_xml() */
        {
            int written_usages = 0;
            for (int u = 0; u < item->usage_count && written_usages < MAX_CE_USAGES_PER_ITEM; u++) {
                if (is_valid_ce_usage(item->usages[u])) {
                    fprintf(fi, "        <usage name=\"%s\"/>\n", item->usages[u]);
                    written_usages++;
                }
            }
        }
        for (int v = 0; v < item->value_count; v++) {
            if (is_valid_ce_value(item->values[v]))
                fprintf(fi, "        <value name=\"%s\"/>\n", item->values[v]);
        }
        fprintf(fi, "    </type>\n");
        zombie_count++;
    }
    fprintf(fi, "</types>\n");
    fclose(fi);
    writer_dedup_free(&zombie_dedup);
    
    // --- 2. Types XML for wildlife ---
    char path_wildlife[MAX_PATH_LEN];
    snprintf(path_wildlife, sizeof(path_wildlife), "%s/types_wildlife.xml", dir);
    FILE *fw = fopen(path_wildlife, "w");
    if (!fw) return false;
    
    fprintf(fw, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
    fprintf(fw, "<!-- Generated by StelliferumAuditor - Tiered Wildlife Economy -->\n");
    fprintf(fw, "<types>\n");
    
    int animal_count = 0, animal_dupe = 0;
    WriterDedupSet animal_dedup = writer_dedup_create();
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted || !loot_is_animal(item)) continue;
        // Hash-set dedup: catches all duplicates regardless of sort adjacency
        if (writer_dedup_check(&animal_dedup, ctx->items, ctx->item_count, i)) {
            animal_dupe++;
            continue;
        }
        
        fprintf(fw, "    <type name=\"%s\">\n", item->classname);
        fprintf(fw, "        <nominal>%d</nominal>\n", item->nominal);
        fprintf(fw, "        <lifetime>%d</lifetime>\n", item->lifetime);
        fprintf(fw, "        <restock>%d</restock>\n", item->restock);
        fprintf(fw, "        <min>%d</min>\n", item->min);
        fprintf(fw, "        <quantmin>-1</quantmin>\n");
        fprintf(fw, "        <quantmax>-1</quantmax>\n");
        fprintf(fw, "        <cost>100</cost>\n");
        fprintf(fw, "        <flags count_in_cargo=\"0\" count_in_hoarder=\"0\" count_in_map=\"1\" count_in_player=\"0\" crafted=\"0\" deloot=\"0\"/>\n");
        /* Same CE flag filtering as writer_export_merged_xml() */
        {
            int written_usages = 0;
            for (int u = 0; u < item->usage_count && written_usages < MAX_CE_USAGES_PER_ITEM; u++) {
                if (is_valid_ce_usage(item->usages[u])) {
                    fprintf(fw, "        <usage name=\"%s\"/>\n", item->usages[u]);
                    written_usages++;
                }
            }
        }
        for (int v = 0; v < item->value_count; v++) {
            if (is_valid_ce_value(item->values[v]))
                fprintf(fw, "        <value name=\"%s\"/>\n", item->values[v]);
        }
        fprintf(fw, "    </type>\n");
        animal_count++;
    }
    fprintf(fw, "</types>\n");
    fclose(fw);
    writer_dedup_free(&animal_dedup);
    
// --- 3. Tier config JSON (machine-readable) — now T1-T7 ---
    char path_json[MAX_PATH_LEN];
    snprintf(path_json, sizeof(path_json), "%s/zombie_tier_config.json", dir);
    FILE *fj = fopen(path_json, "w");
    if (fj) {
        fprintf(fj, "{\n");
        fprintf(fj, "  \"generated_by\": \"StelliferumAuditor\",\n");
        fprintf(fj, "  \"description\": \"Tiered infected variants for Stelliferrum Forge (T1-T4 normal + T5-T7 bosses)\",\n");
        fprintf(fj, "  \"tiers\": [\n");
        
        // T1 Shambler
        fprintf(fj, "    {\n      \"tier\": 1, \"name\": \"Shambler\",\n");
        fprintf(fj, "      \"eye_color\": \"#00FF00\", \"eye_glow_name\": \"Green\", \"aura\": \"none\",\n");
        fprintf(fj, "      \"zones\": [\"Town\", \"Village\", \"Coast\"],\n");
        fprintf(fj, "      \"awareness_multiplier\": 1.0, \"vision_range_multiplier\": 1.0, \"speed_multiplier\": 1.0,\n");
        fprintf(fj, "      \"stealth_detection\": 0.3, \"health_multiplier\": 1.0, \"damage_multiplier\": 1.0,\n");
        fprintf(fj, "      \"loot_tier\": \"Tier1\"\n    },\n");
        
        // T2 Prowler
        fprintf(fj, "    {\n      \"tier\": 2, \"name\": \"Prowler\",\n");
        fprintf(fj, "      \"eye_color\": \"#FFFF00\", \"eye_glow_name\": \"Yellow\", \"aura\": \"faint_yellow_mist\",\n");
        fprintf(fj, "      \"zones\": [\"Industrial\", \"Farm\", \"Town\"],\n");
        fprintf(fj, "      \"awareness_multiplier\": 1.25, \"vision_range_multiplier\": 1.3, \"speed_multiplier\": 1.05,\n");
        fprintf(fj, "      \"stealth_detection\": 0.45, \"health_multiplier\": 1.3, \"damage_multiplier\": 1.15,\n");
        fprintf(fj, "      \"loot_tier\": \"Tier2\"\n    },\n");
        
        // T3 Reaver
        fprintf(fj, "    {\n      \"tier\": 3, \"name\": \"Reaver\",\n");
        fprintf(fj, "      \"eye_color\": \"#FF0000\", \"eye_glow_name\": \"Red\", \"aura\": \"crimson_haze\",\n");
        fprintf(fj, "      \"zones\": [\"Military\", \"Industrial\"],\n");
        fprintf(fj, "      \"awareness_multiplier\": 1.6, \"vision_range_multiplier\": 1.7, \"speed_multiplier\": 1.15,\n");
        fprintf(fj, "      \"stealth_detection\": 0.65, \"health_multiplier\": 1.8, \"damage_multiplier\": 1.4,\n");
        fprintf(fj, "      \"loot_tier\": \"Tier3\"\n    },\n");
        
        // T4 Abomination
        fprintf(fj, "    {\n      \"tier\": 4, \"name\": \"Abomination\",\n");
        fprintf(fj, "      \"eye_color\": \"#9B00FF\", \"eye_glow_name\": \"Purple\", \"aura\": \"violet_corruption\",\n");
        fprintf(fj, "      \"zones\": [\"Military\"],\n");
        fprintf(fj, "      \"awareness_multiplier\": 2.0, \"vision_range_multiplier\": 2.2, \"speed_multiplier\": 1.25,\n");
        fprintf(fj, "      \"stealth_detection\": 0.85, \"health_multiplier\": 2.5, \"damage_multiplier\": 1.8,\n");
        fprintf(fj, "      \"loot_tier\": \"Tier4\"\n    },\n");
        
        // T5 Mini-Boss (Warden)
        fprintf(fj, "    {\n      \"tier\": 5, \"name\": \"Warden\", \"boss_type\": \"mini-boss\",\n");
        fprintf(fj, "      \"eye_color\": \"#00FFFF\", \"eye_glow_name\": \"Cyan\", \"aura\": \"frozen_mist\",\n");
        fprintf(fj, "      \"zones\": [\"Military\"],\n");
        fprintf(fj, "      \"awareness_multiplier\": 2.5, \"vision_range_multiplier\": 3.0, \"speed_multiplier\": 1.3,\n");
        fprintf(fj, "      \"stealth_detection\": 0.95, \"health_multiplier\": 5.0, \"damage_multiplier\": 2.5,\n");
        fprintf(fj, "      \"loot_tier\": \"Tier4\", \"loot_drops\": 3, \"aggro_radius\": 50,\n");
        fprintf(fj, "      \"special_abilities\": [\"area_denial\", \"enrage_below_50pct\"]\n    },\n");
        
        // T6 Boss (Overlord)
        fprintf(fj, "    {\n      \"tier\": 6, \"name\": \"Overlord\", \"boss_type\": \"boss\",\n");
        fprintf(fj, "      \"eye_color\": \"#FFFFFF\", \"eye_glow_name\": \"White\", \"aura\": \"blinding_radiance\",\n");
        fprintf(fj, "      \"zones\": [\"Military\"],\n");
        fprintf(fj, "      \"awareness_multiplier\": 3.0, \"vision_range_multiplier\": 4.0, \"speed_multiplier\": 1.4,\n");
        fprintf(fj, "      \"stealth_detection\": 1.0, \"health_multiplier\": 10.0, \"damage_multiplier\": 3.5,\n");
        fprintf(fj, "      \"loot_tier\": \"Tier4\", \"loot_drops\": 5, \"aggro_radius\": 75,\n");
        fprintf(fj, "      \"special_abilities\": [\"summon_minions\", \"enrage_below_50pct\", \"aoe_attack\"]\n    },\n");
        
        // T7 Raid Boss (Colossus)
        fprintf(fj, "    {\n      \"tier\": 7, \"name\": \"Colossus\", \"boss_type\": \"raid-boss\",\n");
        fprintf(fj, "      \"eye_color\": \"#FFD700\", \"eye_glow_name\": \"Gold\", \"aura\": \"golden_apocalypse\",\n");
        fprintf(fj, "      \"zones\": [\"Military\"],\n");
        fprintf(fj, "      \"awareness_multiplier\": 4.0, \"vision_range_multiplier\": 5.0, \"speed_multiplier\": 1.5,\n");
        fprintf(fj, "      \"stealth_detection\": 1.0, \"health_multiplier\": 25.0, \"damage_multiplier\": 5.0,\n");
        fprintf(fj, "      \"loot_tier\": \"Tier4\", \"loot_drops\": 10, \"aggro_radius\": 100,\n");
        fprintf(fj, "      \"special_abilities\": [\"summon_minions\", \"enrage_below_50pct\", \"aoe_attack\", \"phase_shift\", \"ground_slam\"],\n");
        fprintf(fj, "      \"requires_squad\": true, \"min_players\": 4\n    }\n");
        
        fprintf(fj, "  ]\n}\n");
        fclose(fj);
    }
    
    // --- 4. Mod Design Document (expanded with boss tiers) ---
    char path_md[MAX_PATH_LEN];
    snprintf(path_md, sizeof(path_md), "%s/ZOMBIE_MOD_DESIGN.md", dir);
    FILE *fm = fopen(path_md, "w");
    if (fm) {
        fprintf(fm, "# Stelliferrum Forge — Tiered Infected System\n\n");
        fprintf(fm, "## Overview\n");
        fprintf(fm, "All zombies and wildlife are assigned to tiers that correspond to loot zones.\n");
        fprintf(fm, "Higher-tier infected are more dangerous: better awareness, vision, speed, and\n");
        fprintf(fm, "stealth detection. Each tier has a distinct **eye glow color** so players can\n");
        fprintf(fm, "instantly gauge the threat level.\n\n");
        fprintf(fm, "**Tiers 1-4**: Standard infected. **Tiers 5-7**: Boss variants (squad recommended).\n\n");
        
        fprintf(fm, "## Infected Tiers\n\n");
        fprintf(fm, "| Tier | Name | Eye Color | Aura | Zones | HP | DMG | Notes |\n");
        fprintf(fm, "|------|------|-----------|------|-------|----|-----|-------|\n");
        fprintf(fm, "| 1 | Shambler | **GREEN** | None | Town/Village/Coast | 1.0x | 1.0x | Basic civilian |\n");
        fprintf(fm, "| 2 | Prowler | **YELLOW** | Faint mist | Industrial/Farm | 1.3x | 1.15x | Moderate threat |\n");
        fprintf(fm, "| 3 | Reaver | **RED** | Crimson haze | Military | 1.8x | 1.4x | High threat |\n");
        fprintf(fm, "| 4 | Abomination | **PURPLE** | Violet corruption | Endgame | 2.5x | 1.8x | Endgame threat |\n");
        fprintf(fm, "| 5 | **Warden** | **CYAN** | Frozen mist | Contaminated | 5x | 2.5x | **Mini-Boss** — area denial |\n");
        fprintf(fm, "| 6 | **Overlord** | **WHITE** | Blinding radiance | Unique spawn | 10x | 3.5x | **Boss** — summons adds |\n");
        fprintf(fm, "| 7 | **Colossus** | **GOLD** | Golden apocalypse | Event only | 25x | 5x | **Raid Boss** — 4+ players |\n\n");
        
        fprintf(fm, "## Boss Tier Details (T5-T7)\n\n");
        fprintf(fm, "### T5 — Warden (Mini-Boss)\n");
        fprintf(fm, "- **Eye Color**: Cyan (#00FFFF) — cold, piercing glow\n");
        fprintf(fm, "- **Aura**: Frozen mist — cold fog radiates from the creature\n");
        fprintf(fm, "- **Special**: Area denial (players take DOT near it), Enrage below 50%% HP\n");
        fprintf(fm, "- **Spawn**: Contaminated military zones, 3 max on map, 30min restock\n");
        fprintf(fm, "- **Loot**: 3 guaranteed T4 drops\n\n");
        
        fprintf(fm, "### T6 — Overlord (Boss)\n");
        fprintf(fm, "- **Eye Color**: White (#FFFFFF) — blinding radiance, visible at extreme range\n");
        fprintf(fm, "- **Aura**: Blinding radiance — white particles, screen bloom effect nearby\n");
        fprintf(fm, "- **Special**: Summons T2-T3 minions, AoE ground slam, Enrage below 50%%\n");
        fprintf(fm, "- **Spawn**: Unique locations (Tisy, NWAF tower, Prison Island), 2 max, 1h restock\n");
        fprintf(fm, "- **Loot**: 5 guaranteed T4 drops + chance for Black Market items\n\n");
        
        fprintf(fm, "### T7 — Colossus (Raid Boss)\n");
        fprintf(fm, "- **Eye Color**: Gold (#FFD700) — unmistakable golden glow\n");
        fprintf(fm, "- **Aura**: Golden apocalypse — dark sky, gold particle storm\n");
        fprintf(fm, "- **Special**: All boss abilities + Phase Shift (teleports), Ground Slam (knockback)\n");
        fprintf(fm, "- **Spawn**: SERVER EVENTS ONLY — 1 max, 2h restock, admin-triggered\n");
        fprintf(fm, "- **Loot**: 10 guaranteed T4 drops + guaranteed Black Market item + unique cosmetic\n");
        fprintf(fm, "- **Requires**: Squad of 4+ players recommended\n\n");
        
        fprintf(fm, "## Files Generated\n");
        fprintf(fm, "- `types_infected.xml` — %d zombie economy entries (ready for server db/)\n", zombie_count);
        fprintf(fm, "- `types_wildlife.xml` — %d animal economy entries\n", animal_count);
        fprintf(fm, "- `zombie_tier_config.json` — Tier 1-7 definitions with all multipliers\n");
        fprintf(fm, "- `ZOMBIE_MOD_DESIGN.md` — This document\n\n");
        
        fprintf(fm, "## Current Counts\n\n");
        
        // Count per tier (now T1-T7)
        int t_counts[8] = {0};
        for (int i = 0; i < ctx->item_count; i++) {
            LootItem *item = &ctx->items[i];
            if (item->deleted || !loot_is_zombie(item)) continue;
            int t = item->assigned_tier;
            if (t >= 1 && t <= 7) t_counts[t]++;
        }
        fprintf(fm, "| Tier | Name | Count |\n");
        fprintf(fm, "|------|------|-------|\n");
        fprintf(fm, "| T1 | Shambler | %d |\n", t_counts[1]);
        fprintf(fm, "| T2 | Prowler | %d |\n", t_counts[2]);
        fprintf(fm, "| T3 | Reaver | %d |\n", t_counts[3]);
        fprintf(fm, "| T4 | Abomination | %d |\n", t_counts[4]);
        fprintf(fm, "| T5 | Warden (Mini-Boss) | %d |\n", t_counts[5]);
        fprintf(fm, "| T6 | Overlord (Boss) | %d |\n", t_counts[6]);
        fprintf(fm, "| T7 | Colossus (Raid Boss) | %d |\n", t_counts[7]);
        fprintf(fm, "| **Total** | | **%d** |\n", zombie_count);
        
        fclose(fm);
    }
    
    if (zombie_dupe > 0)
        util_log(SEVERITY_WARNING, "Zombie export: suppressed %d duplicate infected classname(s) at write time", zombie_dupe);
    if (animal_dupe > 0)
        util_log(SEVERITY_WARNING, "Zombie export: suppressed %d duplicate wildlife classname(s) at write time", animal_dupe);
    util_log(SEVERITY_INFO, "Zombie config exported: %d infected, %d wildlife -> %s/", zombie_count, animal_count, dir);
    return true;
}

// ============================================================================
// BUILDING SPAWN LOCATION TEMPLATES
// ============================================================================
// Generates mapgrouppos XML templates that define WHERE items can spawn
// inside buildings. Each building type gets spawn positions tailored to
// the tier of items that should appear there.
//
// Output: output/spawn_templates/
//   - mapgrouppos_civilian.xml      (T1-T2 items in houses, shops)
//   - mapgrouppos_industrial.xml    (T2-T3 items in factories)
//   - mapgrouppos_military.xml      (T3-T4 items in barracks, bunkers)
//   - mapgrouppos_medical.xml       (Medical items in hospitals)
//   - mapgrouppos_police.xml        (T2-T3 items in police stations)
//   - mapgrouppos_hunting.xml       (T2 items in hunting lodges)
//   - SPAWN_TEMPLATE_GUIDE.md       (Documentation)

typedef struct {
    const char *name;
    const char *building_class;
    int tier_min;
    int tier_max;
    const char *loot_zones[8];
    int zone_count;
    // Spawn positions (x, y, z offsets inside building)
    struct { float x, y, z; const char *description; } positions[16];
    int pos_count;
} BuildingTemplate;

static const BuildingTemplate BUILDING_TEMPLATES[] = {
    // --- CIVILIAN ---
    {"Land_House_1W01", "Land_House_1W01", 1, 2, {"Town", "Village"}, 2,
     {{0.5f, 0.1f, 1.2f, "Kitchen counter"}, {-1.0f, 0.1f, 2.5f, "Living room table"},
      {0.3f, 2.8f, 1.0f, "Upstairs bedroom"}, {-0.8f, 2.8f, 3.0f, "Upstairs hallway"}}, 4},
    {"Land_House_1W03", "Land_House_1W03", 1, 2, {"Town", "Village"}, 2,
     {{1.2f, 0.1f, 0.8f, "Entrance hall"}, {-0.5f, 0.1f, 3.2f, "Back room"},
      {0.8f, 2.6f, 1.5f, "Bedroom"}, {-1.2f, 2.6f, 0.5f, "Bathroom shelf"}}, 4},
    {"Land_House_2W01", "Land_House_2W01", 1, 2, {"Town", "Village", "Coast"}, 3,
     {{2.0f, 0.1f, 1.0f, "Kitchen"}, {-1.5f, 0.1f, 4.0f, "Garage"},
      {1.0f, 3.0f, 2.0f, "Master bedroom"}, {-0.5f, 3.0f, 0.8f, "Closet"}}, 4},
    
    // --- INDUSTRIAL ---
    {"Land_Factory_Main", "Land_Factory_Main", 2, 3, {"Industrial"}, 1,
     {{3.0f, 0.3f, 5.0f, "Factory floor"}, {-2.0f, 0.3f, 8.0f, "Workbench"},
      {1.5f, 3.5f, 2.0f, "Catwalk"}, {4.0f, 0.3f, 12.0f, "Loading bay"},
      {-3.0f, 3.5f, 6.0f, "Office loft"}, {0.0f, 0.3f, 15.0f, "Storage room"}}, 6},
    {"Land_Shed_W4", "Land_Shed_W4", 1, 2, {"Industrial", "Farm"}, 2,
     {{0.5f, 0.1f, 0.5f, "Shelf"}, {-0.3f, 0.1f, 1.5f, "Workbench"}, {0.8f, 0.1f, 2.5f, "Corner"}}, 3},
    
    // --- MILITARY ---
    {"Land_Mil_Barracks", "Land_Mil_Barracks", 3, 4, {"Military"}, 1,
     {{1.0f, 0.1f, 2.0f, "Bunk area"}, {-1.5f, 0.1f, 5.0f, "Footlocker"},
      {2.0f, 0.1f, 8.0f, "Weapons rack"}, {-0.5f, 0.1f, 11.0f, "Supply closet"},
      {1.5f, 0.1f, 14.0f, "Commander desk"}, {3.0f, 0.1f, 3.0f, "Under bunk"},
      {-2.0f, 0.1f, 7.0f, "Locker"}, {0.0f, 0.1f, 16.0f, "Back storage"}}, 8},
    {"Land_Mil_Guardhouse", "Land_Mil_Guardhouse", 2, 3, {"Military"}, 1,
     {{0.3f, 0.1f, 0.5f, "Desk"}, {-0.5f, 0.1f, 1.2f, "Wall shelf"}, {0.8f, 0.1f, 2.0f, "Under desk"}}, 3},
    {"Land_Mil_ATC_Tower", "Land_Mil_ATC_Tower", 3, 4, {"Military"}, 1,
     {{0.5f, 12.0f, 0.5f, "Top floor console"}, {-0.3f, 12.0f, 1.0f, "Radar desk"},
      {0.0f, 8.0f, 0.8f, "Mid level shelf"}, {1.0f, 4.0f, 0.3f, "Ground floor locker"},
      {-0.5f, 0.1f, 1.5f, "Basement"}}, 5},
    
    // --- MEDICAL ---
    {"Land_Hospital", "Land_Hospital", 1, 3, {"Town", "City"}, 2,
     {{1.0f, 0.1f, 2.0f, "Reception desk"}, {-2.0f, 0.1f, 5.0f, "Pharmacy shelf"},
      {0.5f, 3.0f, 3.0f, "Patient room"}, {-1.0f, 3.0f, 8.0f, "Supply room"},
      {2.0f, 6.0f, 2.0f, "Operating theater"}, {-0.5f, 6.0f, 6.0f, "Storage cabinet"}}, 6},
    
    // --- POLICE ---
    {"Land_PoliceStation", "Land_PoliceStation", 2, 3, {"Town", "City"}, 2,
     {{0.8f, 0.1f, 1.0f, "Front desk"}, {-1.0f, 0.1f, 3.0f, "Armory"},
      {1.5f, 0.1f, 5.0f, "Evidence locker"}, {-0.5f, 3.0f, 2.0f, "Office upstairs"},
      {0.3f, 3.0f, 4.0f, "Rooftop access"}}, 5},
    
    // --- HUNTING ---
    {"Land_HuntingLodge", "Land_HuntingLodge", 1, 2, {"Hunting", "Village"}, 2,
     {{0.5f, 0.1f, 1.0f, "Fireplace mantle"}, {-1.0f, 0.1f, 2.5f, "Gun cabinet"},
      {0.8f, 2.5f, 0.5f, "Loft"}, {-0.3f, 0.1f, 4.0f, "Back porch"}}, 4},
};

static const int BUILDING_TEMPLATE_COUNT = sizeof(BUILDING_TEMPLATES) / sizeof(BUILDING_TEMPLATES[0]);

static void write_mapgrouppos_xml(FILE *f, const BuildingTemplate *tmpl) {
    fprintf(f, "    <!-- %s (Tier %d-%d) -->\n", tmpl->name, tmpl->tier_min, tmpl->tier_max);
    fprintf(f, "    <group name=\"%s\" pos=\"0 0 0\" rpy=\"0 0 0\" a=\"0\">\n", tmpl->building_class);
    for (int p = 0; p < tmpl->pos_count; p++) {
        fprintf(f, "        <container name=\"lootFloor\" lootmax=\"3\">\n");
        fprintf(f, "            <point pos=\"%.1f %.1f %.1f\" range=\"0.5\" height=\"0.3\" /> <!-- %s -->\n",
                tmpl->positions[p].x, tmpl->positions[p].y, tmpl->positions[p].z, tmpl->positions[p].description);
        fprintf(f, "        </container>\n");
    }
    fprintf(f, "    </group>\n\n");
}

bool writer_export_spawn_templates(AuditorContext *ctx, const char *output_dir) {
    if (!ctx || !output_dir) return false;
    
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/spawn_templates", output_dir);
    util_ensure_directory(output_dir);
    util_ensure_directory(dir);
    
    // Generate per-category mapgrouppos files
    typedef struct { const char *filename; const char *label; int tier_min; int tier_max; } CategoryFile;
    CategoryFile cats[] = {
        {"mapgrouppos_civilian.xml",   "Civilian",   1, 2},
        {"mapgrouppos_industrial.xml", "Industrial",  2, 3},
        {"mapgrouppos_military.xml",   "Military",    3, 4},
        {"mapgrouppos_medical.xml",    "Medical",     1, 3},
        {"mapgrouppos_police.xml",     "Police",      2, 3},
        {"mapgrouppos_hunting.xml",    "Hunting",     1, 2},
        {NULL, NULL, 0, 0}
    };
    
    int total_templates = 0;
    
    for (int c = 0; cats[c].filename; c++) {
        char path[MAX_PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", dir, cats[c].filename);
        FILE *f = fopen(path, "w");
        if (!f) continue;
        
        fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
        fprintf(f, "<!-- Generated by StelliferumAuditor — %s Spawn Templates (Tier %d-%d) -->\n",
                cats[c].label, cats[c].tier_min, cats[c].tier_max);
        fprintf(f, "<!-- These define WHERE items can physically spawn inside buildings -->\n");
        fprintf(f, "<mapgrouppos>\n\n");
        
        int written = 0;
        for (int t = 0; t < BUILDING_TEMPLATE_COUNT; t++) {
            const BuildingTemplate *tmpl = &BUILDING_TEMPLATES[t];
            // Include if tier ranges overlap
            if (tmpl->tier_max >= cats[c].tier_min && tmpl->tier_min <= cats[c].tier_max) {
                write_mapgrouppos_xml(f, tmpl);
                written++;
            }
        }
        
        fprintf(f, "</mapgrouppos>\n");
        fclose(f);
        total_templates += written;
    }
    
    // --- Generate guide document ---
    char guide_path[MAX_PATH_LEN];
    snprintf(guide_path, sizeof(guide_path), "%s/SPAWN_TEMPLATE_GUIDE.md", dir);
    FILE *gf = fopen(guide_path, "w");
    if (gf) {
        fprintf(gf, "# Stelliferrum Forge — Building Spawn Location Templates\n\n");
        fprintf(gf, "## What Are These?\n");
        fprintf(gf, "These XML files define **where items physically appear inside buildings**.\n");
        fprintf(gf, "DayZ's Central Economy uses `mapgrouppos.xml` to know the exact XYZ positions\n");
        fprintf(gf, "within each building type where loot can spawn.\n\n");
        
        fprintf(gf, "## How to Use\n");
        fprintf(gf, "1. Open your mission's `mapgrouppos.xml` (mission root, e.g. `mpmissions/dayzOffline.chernarusplus/mapgrouppos.xml`)\n");
        fprintf(gf, "2. Merge the `<group>` entries from the relevant `mapgrouppos_*.xml` template into it\n");
        fprintf(gf, "   (the engine only loads `mapgrouppos.xml` itself — these template files CANNOT be\n");
        fprintf(gf, "   registered via `cfgeconomycore.xml`; `mapgrouppos` is not a valid `<ce>` file type,\n");
        fprintf(gf, "   and registering extra files there risks double-loading)\n");
        fprintf(gf, "3. Items matching the building's tier range will spawn at these positions\n\n");
        
        fprintf(gf, "## Files\n\n");
        fprintf(gf, "| File | Tier Range | Buildings | Description |\n");
        fprintf(gf, "|------|-----------|-----------|-------------|\n");
        fprintf(gf, "| mapgrouppos_civilian.xml | T1-T2 | Houses, shops | Common civilian loot |\n");
        fprintf(gf, "| mapgrouppos_industrial.xml | T2-T3 | Factories, sheds | Tools, industrial gear |\n");
        fprintf(gf, "| mapgrouppos_military.xml | T3-T4 | Barracks, towers | Military weapons/gear |\n");
        fprintf(gf, "| mapgrouppos_medical.xml | T1-T3 | Hospitals | Medical supplies |\n");
        fprintf(gf, "| mapgrouppos_police.xml | T2-T3 | Police stations | Police gear, weapons |\n");
        fprintf(gf, "| mapgrouppos_hunting.xml | T1-T2 | Hunting lodges | Hunting/outdoor gear |\n\n");
        
        fprintf(gf, "## Customization\n");
        fprintf(gf, "- **Adding positions**: Add more `<point>` entries inside `<container>` blocks\n");
        fprintf(gf, "- **Changing loot density**: Adjust `lootmax` attribute (default: 3)\n");
        fprintf(gf, "- **Range**: The `range` attribute sets how far from the point items can scatter\n");
        fprintf(gf, "- **Height**: The `height` attribute controls vertical spawn variance\n\n");
        
        fprintf(gf, "## Important Notes\n");
        fprintf(gf, "- These are **template** positions — they work best combined with custom building classes\n");
        fprintf(gf, "- The actual item TYPE that spawns is controlled by `types.xml` usage/value tags\n");
        fprintf(gf, "- These positions define where items CAN appear, not WHAT appears\n");
        fprintf(gf, "- For fine-grained control, adjust the building floor positions with a map editor\n");
        
        fclose(gf);
    }
    
    util_log(SEVERITY_INFO, "Spawn templates exported: %d building templates -> %s/", total_templates, dir);
    return true;
}
