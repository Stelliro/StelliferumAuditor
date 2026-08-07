// ============================================================================
// SFL_GENERATOR.C — Search For Loot Improved Config Generator
// ============================================================================
//
// Generates economy-aware SearchForLoot.json by:
// 1. Reading user's template (config/SearchForLoot.json) for buildings/proxies
// 2. Replacing SFLLootCategory with tier-matched items from the auditor
// 3. Applying tier-based rarity per building category
//
// Building-tier mapping:
//   Civilian (T1-3) rarity 65  |  Industrial (T2-4) rarity 55
//   Farm (T1-4)     rarity 60  |  Hunting (T3-5)    rarity 45
//   Police (T3-6)   rarity 35  |  Medical (T2-5)    rarity 50
//   Military (T5-9) rarity 20
//
// Each building has Food/Clothing/Tools sub-categories populated from
// the auditor's item database based on XML category + classname patterns.
// Medical buildings get medical consumables/tools in appropriate slots.
//
// Template path: config/SearchForLoot.json (must exist — opt-in feature)
// Output:        output/SearchForLoot.json
// Upload:        SearchForLoot/SearchForLoot.json (via REMOTE_SFL in INI)
// ============================================================================

#include "auditor.h"
#include "loot_manager.h"
#include "loot_policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Sub-type indices ---
#define SFL_FOOD     0
#define SFL_CLOTHING 1
#define SFL_TOOLS    2
#define SFL_NUM_TYPES     3

// --- Building categories ---
#define SFL_NUM_BUILDINGS 7
#define SFL_MAX_PER_CAT   15
#define SFL_MAX_CANDIDATES 1000

// Building-tier definitions: name, min tier, max tier, SFL rarity (0-100, higher = more common)
static const struct {
    const char *name;
    int tier_min;
    int tier_max;
    float rarity;
} sfl_bldg[SFL_NUM_BUILDINGS] = {
    {"Civilian",   1, 3, 65.0f},
    {"Industrial", 2, 4, 55.0f},
    {"Farm",       1, 4, 60.0f},
    {"Hunting",    3, 5, 45.0f},
    {"Police",     3, 6, 35.0f},
    {"Medical",    2, 5, 50.0f},
    {"Military",   5, 9, 20.0f},
};

static const char *sfl_type_names[SFL_NUM_TYPES] = {"Food", "Clothing", "Tools"};

// ============================================================================
// File I/O Helpers
// ============================================================================

static char *sfl_read_file_buf(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, len, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_len) *out_len = (long)rd;
    return buf;
}

// Find the byte offsets of a JSON array for a given key.
// Returns: arr_start = offset of '[', arr_end = offset past ']'
static bool sfl_find_array(const char *json, const char *key, int *arr_start, int *arr_end) {
    char needle[128];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;

    // Skip past the key and find the opening bracket
    p = strchr(p + strlen(needle), '[');
    if (!p) return false;
    *arr_start = (int)(p - json);

    // Count brackets to find matching close, respecting strings
    int depth = 0;
    bool in_str = false;
    for (; *p; p++) {
        if (*p == '"' && (p == json || *(p - 1) != '\\')) in_str = !in_str;
        if (!in_str) {
            if (*p == '[') depth++;
            else if (*p == ']') {
                depth--;
                if (depth == 0) {
                    *arr_end = (int)(p - json + 1);
                    return true;
                }
            }
        }
    }
    return false;
}

// ============================================================================
// Item Classification
// ============================================================================

static bool sfl_is_currency(const char *cn) {
    return util_str_contains_ci(cn, "Money_Dollar") || util_str_contains_ci(cn, "Money_Euro") ||
           util_str_contains_ci(cn, "Money_Ruble")  || util_str_contains_ci(cn, "Money_Bitcoin") ||
           util_str_contains_ci(cn, "Money-Dollar")  || util_str_contains_ci(cn, "Money-Euro") ||
           util_str_contains_ci(cn, "Money-Ruble")   || util_str_contains_ci(cn, "Money-Bitcoin") ||
           util_str_contains_ci(cn, "HeirloomToken");
}

// --- Medical item sub-classifiers (for Medical building override) ---

static bool sfl_is_med_consumable(const char *cn) {
    return util_str_contains_ci(cn, "Tablet")        || util_str_contains_ci(cn, "Pills") ||
           util_str_contains_ci(cn, "Morphine")      || util_str_contains_ci(cn, "Epinephrine") ||
           util_str_contains_ci(cn, "SalineBag")     || util_str_contains_ci(cn, "Vitamin") ||
           util_str_contains_ci(cn, "CharcoalTablet") || util_str_contains_ci(cn, "Codeine") ||
           util_str_contains_ci(cn, "Disinfectant")  || util_str_contains_ci(cn, "Tetracycline") ||
           util_str_contains_ci(cn, "BloodBag")      || util_str_contains_ci(cn, "BandageDressing") ||
           util_str_contains_ci(cn, "Iodine")        || util_str_contains_ci(cn, "PainkillerTablets");
}

static bool sfl_is_med_tool(const char *cn) {
    return util_str_contains_ci(cn, "BloodTestKit") || util_str_contains_ci(cn, "FirstAidKit") ||
           util_str_contains_ci(cn, "StartKitIV")   || util_str_contains_ci(cn, "Thermometer") ||
           util_str_contains_ci(cn, "Defibrillator") || util_str_contains_ci(cn, "Splint") ||
           util_str_contains_ci(cn, "Syringe")       || util_str_contains_ci(cn, "MouthRag") ||
           util_str_contains_ci(cn, "Pen_");
}

static bool sfl_is_med_clothing(const char *cn) {
    return util_str_contains_ci(cn, "MedicalScrubs")  || util_str_contains_ci(cn, "SurgicalGloves") ||
           util_str_contains_ci(cn, "SurgicalMask")   || util_str_contains_ci(cn, "LabCoat") ||
           util_str_contains_ci(cn, "NioshFaceMask");
}

// --- Classname-based fallback classifiers (for items without XML category) ---

static bool sfl_is_food_cn(const char *cn) {
    // Avoid false positives: "Can" matches Canister/CanOpener, exclude those
    bool is_can = util_str_contains_ci(cn, "Can") &&
                  !util_str_contains_ci(cn, "Canister") &&
                  !util_str_contains_ci(cn, "CanOpener") &&
                  !util_str_contains_ci(cn, "Canteen");
    return is_can ||
           util_str_contains_ci(cn, "Meat")      || util_str_contains_ci(cn, "Apple") ||
           util_str_contains_ci(cn, "Pear")      || util_str_contains_ci(cn, "Plum") ||
           util_str_contains_ci(cn, "Tomato")    || util_str_contains_ci(cn, "Pepper") ||
           util_str_contains_ci(cn, "Potato")    || util_str_contains_ci(cn, "Mushroom") ||
           util_str_contains_ci(cn, "SodaCan")   || util_str_contains_ci(cn, "WaterBottle") ||
           util_str_contains_ci(cn, "Canteen")   || util_str_contains_ci(cn, "Cereal") ||
           util_str_contains_ci(cn, "Rice")      || util_str_contains_ci(cn, "Honey") ||
           util_str_contains_ci(cn, "Crackers")  || util_str_contains_ci(cn, "Chips") ||
           util_str_contains_ci(cn, "Bacon")     || util_str_contains_ci(cn, "Lunchmeat") ||
           util_str_contains_ci(cn, "Pajka")     || util_str_contains_ci(cn, "Zagorky") ||
           util_str_contains_ci(cn, "Sardine")   || util_str_contains_ci(cn, "Marmalade");
}

static bool sfl_is_clothing_cn(const char *cn) {
    return util_str_contains_ci(cn, "Jacket")    || util_str_contains_ci(cn, "Pants") ||
           util_str_contains_ci(cn, "Shirt")     || util_str_contains_ci(cn, "Hat") ||
           util_str_contains_ci(cn, "Boots")     || util_str_contains_ci(cn, "Shoes") ||
           util_str_contains_ci(cn, "Gloves")    || util_str_contains_ci(cn, "Vest") ||
           util_str_contains_ci(cn, "Coat")      || util_str_contains_ci(cn, "Helmet") ||
           util_str_contains_ci(cn, "Backpack")  || util_str_contains_ci(cn, "Balaclava") ||
           util_str_contains_ci(cn, "Belt")      || util_str_contains_ci(cn, "Hoodie") ||
           util_str_contains_ci(cn, "Armband")   || util_str_contains_ci(cn, "Mask") ||
           util_str_contains_ci(cn, "DryBag")    || util_str_contains_ci(cn, "Ushanka") ||
           util_str_contains_ci(cn, "Raincoat")  || util_str_contains_ci(cn, "Bandana");
}

static bool sfl_is_tool_cn(const char *cn) {
    return util_str_contains_ci(cn, "Knife")     || util_str_contains_ci(cn, "Wrench") ||
           util_str_contains_ci(cn, "Hammer")    || util_str_contains_ci(cn, "Screwdriver") ||
           util_str_contains_ci(cn, "Hatchet")   || util_str_contains_ci(cn, "Axe") ||
           util_str_contains_ci(cn, "Crowbar")   || util_str_contains_ci(cn, "Flashlight") ||
           util_str_contains_ci(cn, "Ammo_")     || util_str_contains_ci(cn, "Grenade") ||
           util_str_contains_ci(cn, "Compass")   || util_str_contains_ci(cn, "RepairKit") ||
           util_str_contains_ci(cn, "DuctTape")  || util_str_contains_ci(cn, "Rope") ||
           util_str_contains_ci(cn, "NailBox")   || util_str_contains_ci(cn, "MetalWire") ||
           util_str_contains_ci(cn, "Matchbox")  || util_str_contains_ci(cn, "Lighter") ||
           util_str_contains_ci(cn, "Optic")     || util_str_contains_ci(cn, "Suppressor") ||
           util_str_contains_ci(cn, "Handcuffs") || util_str_contains_ci(cn, "Rangefinder") ||
           util_str_contains_ci(cn, "WeaponCleaningKit") || util_str_contains_ci(cn, "Shovel") ||
           util_str_contains_ci(cn, "Whetstone") || util_str_contains_ci(cn, "SewingKit") ||
           util_str_contains_ci(cn, "CanOpener");
}

// Classify an item for a given building index.
// Returns SFL_FOOD, SFL_CLOTHING, SFL_TOOLS, or -1 to skip.
static int sfl_classify(const LootItem *item, int bldg_idx) {
    // Skip non-economy, deleted, debug, zombie, animal, currency
    if (item->file_type != FILE_TYPE_ECONOMY || item->deleted || item->is_debug_item)
        return -1;
    if (loot_is_zombie((LootItem *)item) || loot_is_animal((LootItem *)item))
        return -1;
    if (sfl_is_currency(item->classname))
        return -1;
    // Skip zero-nominal items that are not on the Black Market (trader-only) tier.
    // Policy BM default is T11; Elite/Mythic remain spawning tiers.
    {
        int bm = lp_black_market_tier();
        if (bm <= 0) bm = 11;
        if (item->nominal <= 0 && item->assigned_tier < bm)
            return -1;
    }

    const char *cat = item->category;
    const char *cn  = item->classname;

    // Medical building override: reclassify medical items into correct SFL slots
    if (bldg_idx == 5) { // Medical
        if (sfl_is_med_consumable(cn)) return SFL_FOOD;
        if (sfl_is_med_clothing(cn))   return SFL_CLOTHING;
        if (sfl_is_med_tool(cn))       return SFL_TOOLS;
    }

    // Standard classification by XML category
    if (util_strcasecmp(cat, "food") == 0)       return SFL_FOOD;
    if (util_strcasecmp(cat, "clothes") == 0)    return SFL_CLOTHING;
    if (util_strcasecmp(cat, "containers") == 0) return SFL_CLOTHING;
    if (util_strcasecmp(cat, "tools") == 0)      return SFL_TOOLS;
    if (util_strcasecmp(cat, "weapons") == 0)    return SFL_TOOLS;

    // Classname fallback for uncategorized items
    if (cat[0] == '\0') {
        if (sfl_is_food_cn(cn))     return SFL_FOOD;
        if (sfl_is_clothing_cn(cn)) return SFL_CLOTHING;
        if (sfl_is_tool_cn(cn))     return SFL_TOOLS;
    }

    return -1;
}

// ============================================================================
// Loot Category Population
// ============================================================================

typedef struct {
    int indices[SFL_MAX_PER_CAT];
    int count;
} SFLCatItems;

// Populate all 21 loot categories (7 buildings × 3 types) from item database.
// Uses stride-based selection for diversity across alphabetically sorted items.
static void sfl_populate(AuditorContext *ctx, SFLCatItems cats[SFL_NUM_BUILDINGS][SFL_NUM_TYPES]) {
    static int candidates[SFL_MAX_CANDIDATES];

    for (int b = 0; b < SFL_NUM_BUILDINGS; b++) {
        for (int t = 0; t < SFL_NUM_TYPES; t++) {
            // Pass 1: collect all matching candidates
            int cand_count = 0;
            for (int i = 0; i < ctx->item_count && cand_count < SFL_MAX_CANDIDATES; i++) {
                LootItem *item = &ctx->items[i];
                int tier = item->assigned_tier;
                if (tier <= 0) tier = 1;

                // Tier range filter
                if (tier < sfl_bldg[b].tier_min || tier > sfl_bldg[b].tier_max)
                    continue;
                // Type classification filter
                if (sfl_classify(item, b) != t)
                    continue;

                candidates[cand_count++] = i;
            }

            // Pass 2: stride-select for diversity
            // Since items are sorted alphabetically, stride sampling avoids
            // clusters of color variants (e.g., 5 BeanieHat_* in a row).
            SFLCatItems *c = &cats[b][t];
            c->count = 0;
            if (cand_count == 0) continue;

            int stride = cand_count / SFL_MAX_PER_CAT;
            if (stride < 1) stride = 1;

            for (int i = 0; i < cand_count && c->count < SFL_MAX_PER_CAT; i += stride) {
                c->indices[c->count++] = candidates[i];
            }
        }
    }
}

// ============================================================================
// JSON Writer
// ============================================================================

// Write the generated SFLLootCategory JSON array to file
static void sfl_write_categories(AuditorContext *ctx, FILE *f,
                                  SFLCatItems cats[SFL_NUM_BUILDINGS][SFL_NUM_TYPES]) {
    fprintf(f, "[\n");
    bool first = true;
    for (int b = 0; b < SFL_NUM_BUILDINGS; b++) {
        for (int t = 0; t < SFL_NUM_TYPES; t++) {
            if (!first) fprintf(f, ",\n");
            first = false;

            SFLCatItems *c = &cats[b][t];
            fprintf(f, "        {\n");
            fprintf(f, "            \"name\": \"%s_%s\",\n", sfl_bldg[b].name, sfl_type_names[t]);
            fprintf(f, "            \"rarity\": %.1f,\n", sfl_bldg[b].rarity);
            fprintf(f, "            \"loot\": [\n");
            for (int i = 0; i < c->count; i++) {
                fprintf(f, "                \"%s\"%s\n",
                        ctx->items[c->indices[i]].classname,
                        (i < c->count - 1) ? "," : "");
            }
            fprintf(f, "            ]\n");
            fprintf(f, "        }");
        }
    }
    fprintf(f, "\n    ]");
}

// ============================================================================
// Main Entry Point
// ============================================================================

bool sfl_generate_config(AuditorContext *ctx, const char *template_path, const char *output_path) {
    if (!ctx || !output_path) return false;

    // Template is required — this is an opt-in feature.
    // User must place their SearchForLoot.json in config/ to enable generation.
    if (!template_path || !util_file_exists(template_path)) {
        util_log(SEVERITY_INFO, "SFL: No template at %s — skipping SearchForLoot generation.",
                 template_path ? template_path : "(null)");
        return false;
    }

    // Read template file
    long tpl_len = 0;
    char *tpl = sfl_read_file_buf(template_path, &tpl_len);
    if (!tpl) {
        util_log(SEVERITY_WARNING, "SFL: Failed to read template %s", template_path);
        return false;
    }

    // Find SFLLootCategory array boundaries in the template
    int arr_start = 0, arr_end = 0;
    if (!sfl_find_array(tpl, "SFLLootCategory", &arr_start, &arr_end)) {
        util_log(SEVERITY_WARNING, "SFL: Template missing \"SFLLootCategory\" key");
        free(tpl);
        return false;
    }

    // Populate loot categories from auditor item database
    SFLCatItems cats[SFL_NUM_BUILDINGS][SFL_NUM_TYPES];
    memset(cats, 0, sizeof(cats));
    sfl_populate(ctx, cats);

    // Write output: template prefix + generated categories + template suffix
    // This preserves all user customization (settings, buildings, proxies)
    util_ensure_directory("output");
    FILE *f = fopen(output_path, "w");
    if (!f) {
        util_log(SEVERITY_WARNING, "SFL: Cannot write %s", output_path);
        free(tpl);
        return false;
    }

    // Prefix: everything up to the SFLLootCategory array value
    fwrite(tpl, 1, arr_start, f);

    // Generated loot categories
    sfl_write_categories(ctx, f, cats);

    // Suffix: everything after the original SFLLootCategory array
    fwrite(tpl + arr_end, 1, tpl_len - arr_end, f);

    fclose(f);
    free(tpl);

    // Summary
    int total = 0, empty = 0;
    for (int b = 0; b < SFL_NUM_BUILDINGS; b++)
        for (int t = 0; t < SFL_NUM_TYPES; t++) {
            total += cats[b][t].count;
            if (cats[b][t].count == 0) empty++;
        }

    util_log(SEVERITY_INFO, "SFL: Generated %s — %d items across %d categories (%d empty)",
             output_path, total, SFL_NUM_BUILDINGS * SFL_NUM_TYPES, empty);
    return true;
}
