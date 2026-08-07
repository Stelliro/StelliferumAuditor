
#include "auditor.h"
#include "web_lookup.h"
#include "loot_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ============================================================================
// PHOENIX CAPACITY CONFIG LOADER
// ============================================================================
// Reads .phoenix/item_capacity.json and populates ctx->phoenix.
// Minimal targeted JSON parser — no external dependency.
// Handles: "no_cargo".items[], "specific_only".items{}, "has_cargo".items[],
//          classname_patterns.no_cargo_patterns[], boot_patterns[], hat_patterns[]

#define PHOENIX_CONFIG_PATH ".phoenix/item_capacity.json"
#define PHOENIX_MAX_FILE_SIZE (512 * 1024) // 512KB max config

// Skip whitespace in JSON
static const char *pj_skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

// Extract a quoted string starting at p (must point to opening '"').
// Returns pointer past closing '"', or NULL on failure.
static const char *pj_read_string(const char *p, char *out, int max_len) {
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < max_len - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case '"': out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

// Find a JSON key in the text. Returns pointer to the value after "key":
static const char *pj_find_key(const char *json, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = json;
    while ((pos = strstr(pos, search)) != NULL) {
        pos += strlen(search);
        pos = pj_skip_ws(pos);
        if (*pos == ':') {
            pos++;
            pos = pj_skip_ws(pos);
            return pos;
        }
    }
    return NULL;
}

// Read a JSON array of strings: ["a", "b", "c"]
// Writes into out[max_count][MAX_CLASSNAME_LEN]. Returns count.
static int pj_read_string_array(const char *p, char out[][MAX_CLASSNAME_LEN], int max_count) {
    p = pj_skip_ws(p);
    if (*p != '[') return 0;
    p++;
    int count = 0;
    while (*p && *p != ']' && count < max_count) {
        p = pj_skip_ws(p);
        if (*p == '"') {
            p = pj_read_string(p, out[count], MAX_CLASSNAME_LEN);
            if (!p) break;
            count++;
        } else if (*p == ',') {
            p++;
        } else {
            p++;
        }
    }
    return count;
}

// Read specific_only items: { "ClassName": { "cargo": ["Item1"], "cargo_chance": 0.20, ... }, ... }
static int pj_read_specific_items(const char *p, PhoenixSpecificItem *out, int max_count) {
    p = pj_skip_ws(p);
    if (*p != '{') return 0;
    p++;
    int count = 0;
    while (*p && *p != '}' && count < max_count) {
        p = pj_skip_ws(p);
        if (*p == '"') {
            // Read classname key
            char classname[MAX_CLASSNAME_LEN];
            p = pj_read_string(p, classname, MAX_CLASSNAME_LEN);
            if (!p) break;
            p = pj_skip_ws(p);
            if (*p != ':') break;
            p++;
            p = pj_skip_ws(p);
            if (*p != '{') break;

            // Find matching closing brace (nested object)
            int depth = 1;
            const char *obj_start = p;
            p++;
            while (*p && depth > 0) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                if (depth > 0) p++;
            }
            if (*p == '}') p++;  // skip final }

            // Parse the inner object for "cargo" array and "cargo_chance"
            strncpy(out[count].classname, classname, MAX_CLASSNAME_LEN - 1);
            out[count].classname[MAX_CLASSNAME_LEN - 1] = '\0';
            out[count].cargo_item_count = 0;
            out[count].cargo_chance = 0.20f;

            const char *cargo_val = pj_find_key(obj_start, "cargo");
            if (cargo_val) {
                out[count].cargo_item_count = pj_read_string_array(
                    cargo_val, out[count].cargo_items, PHOENIX_MAX_CARGO_ITEMS);
            }

            // cargo_chance: find number after key
            const char *chance_val = pj_find_key(obj_start, "cargo_chance");
            if (chance_val) {
                out[count].cargo_chance = (float)atof(chance_val);
            }

            count++;
        } else if (*p == ',') {
            p++;
        } else {
            p++;
        }
    }
    return count;
}

void phoenix_load_capacity(AuditorContext *ctx) {
    if (!ctx) return;
    PhoenixCapacity *pc = &ctx->phoenix;
    memset(pc, 0, sizeof(PhoenixCapacity));

    FILE *f = fopen(PHOENIX_CONFIG_PATH, "r");
    if (!f) {
        util_log(SEVERITY_INFO, "Phoenix: No capacity config found at %s — using hardcoded fallback.", PHOENIX_CONFIG_PATH);
        return;
    }

    // Read entire file
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > PHOENIX_MAX_FILE_SIZE) {
        util_log(SEVERITY_WARNING, "Phoenix: Config file too large or empty (%ld bytes). Skipping.", fsize);
        fclose(f);
        return;
    }

    char *json = (char *)malloc(fsize + 1);
    if (!json) { fclose(f); return; }
    fread(json, 1, fsize, f);
    json[fsize] = '\0';
    fclose(f);

    // ── Parse rules.no_cargo.items ──
    const char *rules_pos = pj_find_key(json, "rules");
    if (rules_pos) {
        const char *no_cargo_pos = pj_find_key(rules_pos, "no_cargo");
        if (no_cargo_pos) {
            const char *items_pos = pj_find_key(no_cargo_pos, "items");
            if (items_pos) {
                pc->no_cargo_count = pj_read_string_array(items_pos, pc->no_cargo, PHOENIX_MAX_NO_CARGO);
            }
        }

        // ── Parse rules.specific_only.items ──
        const char *specific_pos = pj_find_key(rules_pos, "specific_only");
        if (specific_pos) {
            const char *items_pos = pj_find_key(specific_pos, "items");
            if (items_pos) {
                pc->specific_count = pj_read_specific_items(items_pos, pc->specific, PHOENIX_MAX_SPECIFIC);
            }
        }

        // ── Parse rules.has_cargo.items ──
        const char *has_cargo_pos = pj_find_key(rules_pos, "has_cargo");
        if (has_cargo_pos) {
            const char *items_pos = pj_find_key(has_cargo_pos, "items");
            if (items_pos) {
                pc->has_cargo_count = pj_read_string_array(items_pos, pc->has_cargo, PHOENIX_MAX_HAS_CARGO);
            }
        }
    }

    // ── Parse classname_patterns ──
    const char *patterns_pos = pj_find_key(json, "classname_patterns");
    if (patterns_pos) {
        const char *ncp = pj_find_key(patterns_pos, "no_cargo_patterns");
        if (ncp) {
            pc->no_cargo_pattern_count = pj_read_string_array(ncp, pc->no_cargo_patterns, PHOENIX_MAX_PATTERNS);
        }
        const char *bp = pj_find_key(patterns_pos, "no_cargo_boot_patterns");
        if (bp) {
            pc->boot_pattern_count = pj_read_string_array(bp, pc->boot_patterns, PHOENIX_MAX_PATTERNS);
        }
        const char *hp = pj_find_key(patterns_pos, "no_cargo_hat_patterns");
        if (hp) {
            pc->hat_pattern_count = pj_read_string_array(hp, pc->hat_patterns, PHOENIX_MAX_PATTERNS);
        }
    }

    free(json);

    pc->loaded = true;
    util_log(SEVERITY_INFO, "Phoenix: Loaded capacity config — no_cargo: %d, specific: %d, has_cargo: %d, patterns: %d+%d+%d",
             pc->no_cargo_count, pc->specific_count, pc->has_cargo_count,
             pc->no_cargo_pattern_count, pc->boot_pattern_count, pc->hat_pattern_count);
}

// ── Phoenix lookup functions ─────────────────────────────────────────────────

// Check if item is in the Phoenix "has_cargo" list (overrides no_cargo)
static bool phoenix_has_cargo(const PhoenixCapacity *pc, const char *cn) {
    for (int i = 0; i < pc->has_cargo_count; i++) {
        if (util_strcasecmp(pc->has_cargo[i], cn) == 0) return true;
    }
    return false;
}

// Check if item is in the Phoenix "no_cargo" list
static bool phoenix_is_no_cargo(const PhoenixCapacity *pc, const char *cn) {
    // Explicit no_cargo list
    for (int i = 0; i < pc->no_cargo_count; i++) {
        if (util_strcasecmp(pc->no_cargo[i], cn) == 0) return true;
    }
    // Fallback: pattern matching
    for (int i = 0; i < pc->no_cargo_pattern_count; i++) {
        if (util_str_contains_ci(cn, pc->no_cargo_patterns[i])) return true;
    }
    for (int i = 0; i < pc->boot_pattern_count; i++) {
        if (util_str_contains_ci(cn, pc->boot_patterns[i])) return true;
    }
    for (int i = 0; i < pc->hat_pattern_count; i++) {
        if (util_str_contains_ci(cn, pc->hat_patterns[i])) return true;
    }
    return false;
}

// Find a specific-only entry for this classname. Returns NULL if not found.
static const PhoenixSpecificItem *phoenix_find_specific(const PhoenixCapacity *pc, const char *cn) {
    for (int i = 0; i < pc->specific_count; i++) {
        if (util_strcasecmp(pc->specific[i].classname, cn) == 0) return &pc->specific[i];
    }
    return NULL;
}

// ── Generate specific-only cargo block ───────────────────────────────────────
// For items like AirborneMask that can only hold a GasMask_Filter.
// Produces: <cargo chance="X"><item name="Y" chance="Z" health="0.05,0.60" /></cargo>

static int generate_specific_cargo_block(char *buf, size_t buf_len,
                                          const PhoenixSpecificItem *spec) {
    int pos = 0;
    if (spec->cargo_item_count == 0) return 0;

    pos += snprintf(buf + pos, buf_len - pos,
        "    <cargo chance=\"%.2f\">\n", spec->cargo_chance);

    for (int i = 0; i < spec->cargo_item_count && (size_t)pos < buf_len - 128; i++) {
        float item_chance = 1.0f / (float)spec->cargo_item_count;
        if (item_chance < 0.10f) item_chance = 0.10f;
        pos += snprintf(buf + pos, buf_len - pos,
            "        <item name=\"%s\" chance=\"%.2f\" health=\"0.05,0.60\" />\n",
            spec->cargo_items[i], item_chance);
    }

    pos += snprintf(buf + pos, buf_len - pos, "    </cargo>\n");
    return (pos > 0 && (size_t)pos < buf_len) ? pos : 0;
}

// ============================================================================

static bool item_exists(AuditorContext *ctx, const char *name) {
    for (int i = 0; i < ctx->item_count; i++) {
        if (ctx->items[i].deleted) continue;
        if (util_strcasecmp(ctx->items[i].classname, name) == 0) return true;
    }
    return false;
}

static bool spawnable_exists(AuditorContext *ctx, const char *classname) {
    for (int i = 0; i < ctx->spawn_block_count; i++) {
        if (util_strcasecmp(ctx->spawn_blocks[i].classname, classname) == 0) return true;
    }
    return false;
}

// cn_contains consolidated into util_str_contains_ci() in util.c

// ============================================================================
// VANILLA / MOD BOUNDARY DETECTION
// ============================================================================
// Vanilla items should only get vanilla cargo and attachments.
// Mod items get same-mod + vanilla items in their pools.
// This prevents modded items leaking into vanilla containers and vice versa.

static bool gf_is_vanilla(const LootItem *item) {
    if (!item) return false;
    if (item->mod_name[0] != '\0') {
        return (util_strcasecmp(item->mod_name, "vanilla") == 0 ||
                util_strcasecmp(item->mod_name, "server_root") == 0);
    }
    // Fallback: infer from source filename — vanilla types.xml has no mod prefix
    return util_strcasecmp(item->mod_source, "types.xml") == 0;
}

// Check if two items share the same mod origin.
// Both must have mod_name set and matching (case-insensitive).
static bool gf_same_mod(const LootItem *a, const LootItem *b) {
    if (!a || !b) return false;
    if (a->mod_name[0] == '\0' || b->mod_name[0] == '\0') {
        // Fallback: compare mod_source prefixes (before "__" or first mismatch)
        // e.g. "SNAFU_types.xml" and "SNAFU_spawn.xml" share "SNAFU"
        const char *sa = a->mod_source;
        const char *sb = b->mod_source;
        if (sa[0] == '\0' || sb[0] == '\0') return false;
        int i = 0;
        while (sa[i] && sb[i] && sa[i] == sb[i] && sa[i] != '_') i++;
        return (i > 2 && (sa[i] == '_' || sa[i] == '\0') && (sb[i] == '_' || sb[i] == '\0'));
    }
    return util_strcasecmp(a->mod_name, b->mod_name) == 0;
}

// Determines if 'candidate' is allowed in 'container's cargo/attachment pool.
// Vanilla containers: vanilla items only.
// Mod containers: same-mod items + vanilla items.
static bool gf_is_pool_compatible(const LootItem *container, const LootItem *candidate) {
    if (!container || !candidate) return false;
    bool cont_vanilla = gf_is_vanilla(container);
    bool cand_vanilla = gf_is_vanilla(candidate);

    if (cont_vanilla) {
        // Vanilla items only get vanilla cargo/attachments
        return cand_vanilla;
    }
    // Mod items get same-mod + vanilla
    return cand_vanilla || gf_same_mod(container, candidate);
}

// ============================================================================
// ITEM CLASSIFICATION FOR SPAWNABLE GENERATION
// ============================================================================
// DayZ cfgspawnabletypes.xml defines what cargo/attachments an item spawns with.
// ANY item with inventory/attachment slots gets a spawnable entry:
//   - Weapons:    attachment discovery (platform table + web lookup + heuristics)
//   - Everything else with storage: tier-based cargo pool + web-discovered attachments
//   - Mods may add storage/attachment slots to armbands, boots, etc.
//     so we are deliberately INCLUSIVE — generate entries for anything
//     with FLAG_CARGO, clothes category, or known container patterns.

typedef enum {
    SPAWN_CAT_NONE = 0,
    SPAWN_CAT_ASSAULT_RIFLE,
    SPAWN_CAT_SNIPER_RIFLE,
    SPAWN_CAT_SMG,
    SPAWN_CAT_SHOTGUN,
    SPAWN_CAT_PISTOL,
    SPAWN_CAT_LAUNCHER,
    SPAWN_CAT_STORAGE_LARGE,    // Backpacks, containers, tents, vehicles, large cases
    SPAWN_CAT_STORAGE_MEDIUM,   // Vests, jackets, plate carriers, pants, helmets, holsters
    SPAWN_CAT_STORAGE_SMALL,    // Shirts, armbands, boots, gloves, hats, misc equipment
    SPAWN_CAT_MELEE_SHEATHED,
} SpawnCategory;

// Context pointer for web stat lookups (set by gap_fill_spawnable_types)
// Enables slot_count → storage size classification and cargo type affinity.
static const WebLookupState *s_web = NULL;

// Determine storage size classification for a non-weapon item.
// Controls tier-pool size and base cargo chance.
// Consults web-discovered slot_count first (if available), then falls back
// to classname heuristics.  This ensures modded items with known capacity
// (e.g. "MMPS Backpack (150 Slots)") get the correct cargo richness even
// if their classname doesn't match traditional keywords.
static SpawnCategory storage_size_for_item(const char *cn, const char *cat) {
    // ── Web stats override: use actual slot count if known ───────────────────
    // slot_count comes from Workshop description parsing (multi-language).
    // Thresholds: ≥80 slots → LARGE, ≥30 → MEDIUM, <30 → SMALL.
    if (s_web) {
        ItemStats stats = {0};
        if (web_get_item_stats(s_web, cn, &stats) && stats.slot_count > 0) {
            if (stats.slot_count >= 80)  return SPAWN_CAT_STORAGE_LARGE;
            if (stats.slot_count >= 30)  return SPAWN_CAT_STORAGE_MEDIUM;
            return SPAWN_CAT_STORAGE_SMALL;
        }
    }

    // ── Classname heuristic fallback ─────────────────────────────────────────
    // Large: backpacks, containers, tents, vehicles, large cases
    if (util_str_contains_ci(cn, "Mountain") || util_str_contains_ci(cn, "Alice") ||
        util_str_contains_ci(cn, "Coyote") || util_str_contains_ci(cn, "Tortilla") ||
        util_str_contains_ci(cn, "FieldPack") || util_str_contains_ci(cn, "AssaultPack") ||
        util_str_contains_ci(cn, "Drybag") || util_str_contains_ci(cn, "HuntingPack"))
        return SPAWN_CAT_STORAGE_LARGE;
    if (util_str_contains_ci(cn, "Barrel") || util_str_contains_ci(cn, "SeaChest") ||
        util_str_contains_ci(cn, "WoodenCrate") || util_str_contains_ci(cn, "Tent") ||
        util_str_contains_ci(cn, "Shelter") || util_str_contains_ci(cn, "Canopy"))
        return SPAWN_CAT_STORAGE_LARGE;
    if (util_str_contains_ci(cn, "FirstAidKit") || util_str_contains_ci(cn, "MedicalCase") ||
        util_str_contains_ci(cn, "Protector") || util_str_contains_ci(cn, "AmmoBox"))
        return SPAWN_CAT_STORAGE_LARGE;
    if (cat[0] && strstr(cat, "vehicles"))
        return SPAWN_CAT_STORAGE_LARGE;
    // Medium: vests, jackets, pants, plate carriers, helmets, holsters
    if (util_str_contains_ci(cn, "Vest") || util_str_contains_ci(cn, "PlateCarrier") ||
        util_str_contains_ci(cn, "ChestRig") || util_str_contains_ci(cn, "Smersh"))
        return SPAWN_CAT_STORAGE_MEDIUM;
    if (util_str_contains_ci(cn, "Jacket") || util_str_contains_ci(cn, "Coat") ||
        util_str_contains_ci(cn, "Hoodie") || util_str_contains_ci(cn, "Gorka") ||
        util_str_contains_ci(cn, "Parka") || util_str_contains_ci(cn, "Windbreaker") ||
        util_str_contains_ci(cn, "Firefighter") || util_str_contains_ci(cn, "Ghillie"))
        return SPAWN_CAT_STORAGE_MEDIUM;
    if (util_str_contains_ci(cn, "Pants") || util_str_contains_ci(cn, "Jeans") ||
        util_str_contains_ci(cn, "Trousers") || util_str_contains_ci(cn, "BDU") ||
        util_str_contains_ci(cn, "Cargopants"))
        return SPAWN_CAT_STORAGE_MEDIUM;
    if (util_str_contains_ci(cn, "Helmet") || util_str_contains_ci(cn, "Mich") ||
        util_str_contains_ci(cn, "BallisticHelm") || util_str_contains_ci(cn, "TacticalHelm"))
        return SPAWN_CAT_STORAGE_MEDIUM;
    if (util_str_contains_ci(cn, "Holster") || util_str_contains_ci(cn, "Pouch"))
        return SPAWN_CAT_STORAGE_MEDIUM;
    // Small: shirts, armbands, boots, hats, gloves, masks, everything else
    return SPAWN_CAT_STORAGE_SMALL;
}

// ── Clothing items that have NO general cargo inventory ──────────────────
// These items are category="clothes" but have either:
//   - No storage at all (gloves, boots, armbands, glasses, hats, headbands)
//   - Only a SPECIFIC attachment slot, not general cargo (gas masks → filter)
// Their vanilla cfgspawnabletypes.xml entries already define correct cargo/
// attachments (e.g. AirborneMask → GasMask_Filter only).  Generating tier
// cargo pools for these is WRONG — a gas mask can't hold a rifle mag.
//
// When Phoenix config is loaded (.phoenix/item_capacity.json), uses the
// AI-discovered rules.  Otherwise falls back to hardcoded patterns.
//
// If a mod adds real storage to one of these items, the mod's own
// cfgspawnabletypes.xml should define it, which gets parsed & kept via
// spawnable_exists().  Only items with FLAG_CARGO bypass this filter.

// Context pointer for Phoenix lookups (set by gap_fill_spawnable_types)
static const PhoenixCapacity *s_phoenix = NULL;

static bool is_no_cargo_clothing(const char *cn) {
    // ── Phoenix path: use AI-discovered rules ────────────────────────────────
    if (s_phoenix && s_phoenix->loaded) {
        // Items explicitly marked as having cargo override no_cargo
        if (phoenix_has_cargo(s_phoenix, cn)) return false;
        // Check no_cargo list and patterns
        return phoenix_is_no_cargo(s_phoenix, cn);
    }

    // ── Hardcoded fallback (no Phoenix config available) ─────────────────────
    // Gas masks — specific filter slot only, not general cargo
    if (util_str_contains_ci(cn, "GasMask") || util_str_contains_ci(cn, "Gas_Mask") ||
        util_str_contains_ci(cn, "AirborneMask") || util_str_contains_ci(cn, "GP5"))
        return true;
    // Other masks with no general cargo (decorative, protective face covers)
    if (util_str_contains_ci(cn, "HockeyMask") || util_str_contains_ci(cn, "SkullMask") ||
        util_str_contains_ci(cn, "PaydayMask") || util_str_contains_ci(cn, "EyeMask") ||
        util_str_contains_ci(cn, "MimeMask") || util_str_contains_ci(cn, "Plague_Mask") ||
        util_str_contains_ci(cn, "NioshFaceMask") || util_str_contains_ci(cn, "WeldingMask"))
        return true;
    // Regular masks, balaclavas, bandanas — no storage
    if (util_str_contains_ci(cn, "Balaclava") || util_str_contains_ci(cn, "SkiMask") ||
        util_str_contains_ci(cn, "Bandana") || util_str_contains_ci(cn, "SurgicalMask") ||
        util_str_contains_ci(cn, "MouthRag") || util_str_contains_ci(cn, "FaceCover"))
        return true;
    // Gloves — no storage at all
    if (util_str_contains_ci(cn, "Gloves") || util_str_contains_ci(cn, "NBCGloves"))
        return true;
    // Boots and shoes — no general cargo
    if (util_str_contains_ci(cn, "Boots") || util_str_contains_ci(cn, "Shoes") ||
        util_str_contains_ci(cn, "Sneakers") || util_str_contains_ci(cn, "JoggingShoes") ||
        util_str_contains_ci(cn, "NBCBoots") || util_str_contains_ci(cn, "Wellies") ||
        util_str_contains_ci(cn, "HikingBoots") || util_str_contains_ci(cn, "MilitaryBoots") ||
        util_str_contains_ci(cn, "WorkingBoots") || util_str_contains_ci(cn, "CombatBoots"))
        return true;
    // Armbands — vanilla has no storage (mods that add it will have FLAG_CARGO)
    if (util_str_contains_ci(cn, "Armband"))
        return true;
    // Glasses, goggles, eyewear — no cargo
    if (util_str_contains_ci(cn, "Glasses") || util_str_contains_ci(cn, "Goggles") ||
        util_str_contains_ci(cn, "AviatorGlasses") || util_str_contains_ci(cn, "SportGlasses") ||
        util_str_contains_ci(cn, "ThinFramesGlasses") || util_str_contains_ci(cn, "NVGoggles") ||
        util_str_contains_ci(cn, "Eyepatch"))
        return true;
    // Headbands, headwear with no storage
    if (util_str_contains_ci(cn, "Headband") || util_str_contains_ci(cn, "NVGHeadstrap") ||
        util_str_contains_ci(cn, "Headtorch"))
        return true;
    // Hats/caps that have no storage (most hats have 0 or negligible slots)
    if (util_str_contains_ci(cn, "BaseballCap") || util_str_contains_ci(cn, "Beret") ||
        util_str_contains_ci(cn, "Beanie") || util_str_contains_ci(cn, "Ushanka") ||
        util_str_contains_ci(cn, "Boonie") || util_str_contains_ci(cn, "ZSh3PilotHelmet") ||
        util_str_contains_ci(cn, "Wig") || util_str_contains_ci(cn, "NBCHood") ||
        util_str_contains_ci(cn, "SantasHat") || util_str_contains_ci(cn, "WitchHat") ||
        util_str_contains_ci(cn, "PumpkinHelmet") || util_str_contains_ci(cn, "DirtBikeHelmet"))
        return true;
    // NBC clothing — specific protective gear, no general cargo
    if (util_str_contains_ci(cn, "NBC") && !util_str_contains_ci(cn, "NBCPants") &&
        !util_str_contains_ci(cn, "NBCJacket"))
        return true;
    // Watches, rings, jewelry — worn items with no storage
    if (util_str_contains_ci(cn, "Watch") || util_str_contains_ci(cn, "Ring"))
        return true;
    return false;
}

static SpawnCategory classify_for_spawnable(const LootItem *item) {
    const char *cn = item->classname;
    const char *cat = item->category;

    // Skip zombies, animals
    if (loot_is_zombie((LootItem*)item) || loot_is_animal((LootItem*)item)) return SPAWN_CAT_NONE;

    // ── Items that ARE cargo, not containers ─────────────────────────────────
    // Pure consumables, ammo, weapon attachments — these go IN cargo but don't
    // have their own storage.  Don't generate spawnables for these.
    if (util_str_contains_ci(cn, "Ammo_") || util_str_contains_ci(cn, "_Ammo")) return SPAWN_CAT_NONE;
    if (util_str_contains_ci(cn, "Mag_") ||
        (util_str_contains_ci(cn, "_Mag") && !util_str_contains_ci(cn, "_Magn"))) return SPAWN_CAT_NONE;
    if (util_str_contains_ci(cn, "Optic") || util_str_contains_ci(cn, "ACOG") || util_str_contains_ci(cn, "PSO") ||
        util_str_contains_ci(cn, "Scope") || util_str_contains_ci(cn, "RDS") || util_str_contains_ci(cn, "Reflex")) return SPAWN_CAT_NONE;
    if (util_str_contains_ci(cn, "Suppressor") || util_str_contains_ci(cn, "Silencer")) return SPAWN_CAT_NONE;
    if (util_str_contains_ci(cn, "Bttstck") || util_str_contains_ci(cn, "Hndgrd") || util_str_contains_ci(cn, "Compensator")) return SPAWN_CAT_NONE;
    if (util_str_contains_ci(cn, "Bayonet")) return SPAWN_CAT_NONE;
    if (util_str_contains_ci(cn, "Rail") && !util_str_contains_ci(cn, "Derringer")) return SPAWN_CAT_NONE;
    // Individual consumables (no storage)
    if (util_str_contains_ci(cn, "Tablet") || util_str_contains_ci(cn, "MetalPlate")) return SPAWN_CAT_NONE;
    // Vehicle parts — not vehicles themselves
    if (util_str_contains_ci(cn, "CarWheel") || util_str_contains_ci(cn, "HatchbackWheel") ||
        util_str_contains_ci(cn, "Tire") || util_str_contains_ci(cn, "CarDoor") ||
        util_str_contains_ci(cn, "CarHood") || util_str_contains_ci(cn, "SparkPlug") ||
        util_str_contains_ci(cn, "HeadlightH7") || util_str_contains_ci(cn, "CarRadiator") ||
        util_str_contains_ci(cn, "TruckBattery") || util_str_contains_ci(cn, "CarBattery"))
        return SPAWN_CAT_NONE;
    // Base building
    if (util_str_contains_ci(cn, "Fence") || util_str_contains_ci(cn, "Watchtower") ||
        util_str_contains_ci(cn, "CombinationLock") || util_str_contains_ci(cn, "CodeLock"))
        return SPAWN_CAT_NONE;

    // ── WEAPONS → attachment-based spawnables ────────────────────────────────
    if (cat[0] && strstr(cat, "weapons")) {
        if (util_str_contains_ci(cn, "Launcher") || util_str_contains_ci(cn, "RPG") || util_str_contains_ci(cn, "LAW"))
            return SPAWN_CAT_LAUNCHER;
        if (util_str_contains_ci(cn, "Shotgun") || util_str_contains_ci(cn, "Saiga") || util_str_contains_ci(cn, "Vaiga") ||
            util_str_contains_ci(cn, "BenelliM4") || util_str_contains_ci(cn, "AA12") || util_str_contains_ci(cn, "MP133") ||
            util_str_contains_ci(cn, "MP153"))
            return SPAWN_CAT_SHOTGUN;
        if (util_str_contains_ci(cn, "Pistol") || util_str_contains_ci(cn, "Glock") || util_str_contains_ci(cn, "CZ75") ||
            util_str_contains_ci(cn, "FNX") || util_str_contains_ci(cn, "Deagle") || util_str_contains_ci(cn, "Mlock") ||
            util_str_contains_ci(cn, "Magnum") || util_str_contains_ci(cn, "Derringer") || util_str_contains_ci(cn, "P1") ||
            util_str_contains_ci(cn, "MK2"))
            return SPAWN_CAT_PISTOL;
        if (util_str_contains_ci(cn, "SVD") || util_str_contains_ci(cn, "Mosin") || util_str_contains_ci(cn, "Winchester") ||
            util_str_contains_ci(cn, "CZ527") || util_str_contains_ci(cn, "CZ550") || util_str_contains_ci(cn, "Scout") ||
            util_str_contains_ci(cn, "Blaze") || util_str_contains_ci(cn, "SSG") || util_str_contains_ci(cn, "Repeater") ||
            util_str_contains_ci(cn, "LongHorn"))
            return SPAWN_CAT_SNIPER_RIFLE;
        if (util_str_contains_ci(cn, "MP5") || util_str_contains_ci(cn, "UMP") || util_str_contains_ci(cn, "AKS74U") ||
            util_str_contains_ci(cn, "Scorpion") || util_str_contains_ci(cn, "Bizon"))
            return SPAWN_CAT_SMG;
        return SPAWN_CAT_ASSAULT_RIFLE;
    }
    // Weapon-like items detected by classname without "weapons" category
    if (util_str_contains_ci(cn, "_Gun") || util_str_contains_ci(cn, "Rifle") || util_str_contains_ci(cn, "AK") ||
        util_str_contains_ci(cn, "M4A1") || util_str_contains_ci(cn, "M16") || util_str_contains_ci(cn, "FAL") ||
        util_str_contains_ci(cn, "VSS") || util_str_contains_ci(cn, "FAMAS") || util_str_contains_ci(cn, "Aug") ||
        util_str_contains_ci(cn, "M249") || util_str_contains_ci(cn, "PKM"))
        return SPAWN_CAT_ASSAULT_RIFLE;

    // ── MELEE with sheaths ───────────────────────────────────────────────────
    if (util_str_contains_ci(cn, "Machete") || util_str_contains_ci(cn, "Katana") ||
        util_str_contains_ci(cn, "Sword") || util_str_contains_ci(cn, "CombatKnife"))
        return SPAWN_CAT_MELEE_SHEATHED;

    // ── ANY ITEM WITH STORAGE → tier-based cargo + equipment attachments ─────
    // FLAG_CARGO from types.xml = item definitely has inventory space
    if (item->flags & FLAG_CARGO) return storage_size_for_item(cn, cat);

    // ── Clothing items that have NO general cargo inventory ──────────────────
    // Items like gas masks, gloves, boots, armbands, etc. are excluded from
    // the tier cargo engine.  Their vanilla spawnable entries (if any) define
    // correct cargo.  If a mod adds FLAG_CARGO, it bypasses this filter above.
    // Clothing without FLAG_CARGO: only classify as storage if it matches known
    // pocket-having patterns.
    if (cat[0] && strstr(cat, "clothes")) {
        if (is_no_cargo_clothing(cn)) return SPAWN_CAT_NONE;
        return storage_size_for_item(cn, cat);
    }
    // Container category — but check no-cargo patterns first in case a mod
    // miscategorized an item (e.g. Round_Glasses as "containers" instead of "clothes")
    if (cat[0] && strstr(cat, "containers")) {
        if (is_no_cargo_clothing(cn)) return SPAWN_CAT_NONE;
        return storage_size_for_item(cn, cat);
    }
    // Vehicle bodies (parts were filtered above)
    if (cat[0] && strstr(cat, "vehicles")) return SPAWN_CAT_STORAGE_LARGE;
    // Known storage patterns by classname
    if (util_str_contains_ci(cn, "Bag") || util_str_contains_ci(cn, "Pack") ||
        util_str_contains_ci(cn, "Barrel") || util_str_contains_ci(cn, "Crate") ||
        util_str_contains_ci(cn, "Tent") || util_str_contains_ci(cn, "Case") ||
        util_str_contains_ci(cn, "Vest") || util_str_contains_ci(cn, "Pouch") ||
        util_str_contains_ci(cn, "Holster") || util_str_contains_ci(cn, "Belt") ||
        util_str_contains_ci(cn, "Chest"))
        return storage_size_for_item(cn, cat);

    return SPAWN_CAT_NONE;
}

// ============================================================================
// ATTACHMENT COMPATIBILITY DATABASE
// ============================================================================
// Maps weapons to compatible attachments via platform prefixes.
// DayZ naming convention:
//   Buttstocks:  <platform>_<variant>Bttstck   (AK_WoodBttstck)
//   Handguards:  <platform>_<variant>Hndgrd    (AK_RailHndgrd)
//   Magazines:   Mag_<platform>_<capacity>     (Mag_AK74_30Rnd)
//   Muzzles:     <platform>_Suppressor         (AK_Suppressor)
//   Optics:      Named individually — mapped per platform
//   Lights:      Universal — any railed weapon can mount
//
// Equipment (backpacks, vests, helmets, belts):
//   Pouches:     <mod>_Pouch, <mod>_PouchMedic, GP_*Pouch*
//   Holsters:    *Holster*, *PistolHolster*
//   Armor:       *PlateCarrier*Plate, *ArmorPlate*
//   NVG:         NVGHeadstrap, NVG*, *_NVG*
//   Visors:      *Visor*, *Shield*, *FaceShield*
//   Wraps/camo:  *GhillieBushrag*, *Wrap*, *Camo*
//
// For modded items: the system checks the static platform table first,
// then falls back to mod-prefix heuristics (e.g. "SNAFU_AK47" shares a
// mod prefix with "SNAFU_AK_CustomStock", or "Spurgles_BigBag" with
// "Spurgles_Pouch_Small").

// ── Shared slot types ────────────────────────────────────────────────────────
// Weapon slots (0-5) and equipment slots (6-12) share a namespace so the
// same FoundAttach struct and rendering loop work for both.

typedef enum {
    // Weapon attachment slots
    ATTACH_MAG = 0,
    ATTACH_OPTIC,
    ATTACH_BUTTSTOCK,
    ATTACH_HANDGUARD,
    ATTACH_MUZZLE,
    ATTACH_LIGHT,
    WEAPON_SLOT_COUNT,       // = 6
    // Equipment attachment slots
    EQUIP_POUCH = 6,
    EQUIP_HOLSTER,
    EQUIP_ARMOR_PLATE,
    EQUIP_NVG,
    EQUIP_VISOR,
    EQUIP_HELMET_LIGHT,
    EQUIP_WRAP,
    ALL_SLOT_COUNT           // = 13
} AttachSlot;

typedef struct {
    char classname[MAX_CLASSNAME_LEN];
    AttachSlot slot;
    int nominal;    // for spawn-weight calculation (higher nominal = more common)
} FoundAttach;

#define MAX_FOUND_ATTACH  48
#define MAX_PER_SLOT      8

// Weapon platform definition
typedef struct {
    const char *weapon_pat;     // CI substring match (word-boundary-aware)
    const char *prefixes[8];    // Attachment classname prefixes (stocks, grips, mags, muzzles)
    const char *optics[8];      // Explicit compatible optic classnames
    bool has_rail;              // Can mount universal/weapon lights
} WeaponPlatform;

// Base slot-fill chances (before tier scaling)
static const float SLOT_BASE_CHANCE[WEAPON_SLOT_COUNT] = {
    0.30f,  // MAG        — magazine
    0.12f,  // OPTIC      — scope / red-dot
    0.40f,  // BUTTSTOCK  — stock
    0.35f,  // HANDGUARD  — handguard / rail
    0.06f,  // MUZZLE     — suppressor / compensator
    0.04f,  // LIGHT      — flashlight
};

// ── Vanilla + common mod weapon platforms ────────────────────────────────────
static const WeaponPlatform WEAPON_PLATFORMS[] = {
    // === ASSAULT RIFLES ===
    { "AK74",    { "AK_", "Mag_AK74", NULL },
                 { "PSO1Scope", "PSO11Scope", "KobraOptic", "KazuarOptic", NULL }, true },
    { "AK101",   { "AK_", "Mag_AK101", NULL },
                 { "PSO1Scope", "PSO11Scope", "KobraOptic", "KazuarOptic", NULL }, true },
    { "AKM",     { "AK_", "Mag_AKM", NULL },
                 { "PSO1Scope", "PSO11Scope", "KobraOptic", "KazuarOptic", NULL }, true },
    { "AKS74U",  { "AK_", "Mag_AK74", NULL },
                 { "KobraOptic", "KazuarOptic", NULL }, true },
    { "M4A1",    { "M4_", "Mag_STANAG", "Mag_CMAG", NULL },
                 { "M68Optic", "ACOGOptic", "M4_T3NRDSOptic", "ReflexOptic", NULL }, true },
    { "M16",     { "M4_", "Mag_STANAG", NULL },
                 { "M68Optic", "ACOGOptic", "ReflexOptic", NULL }, true },
    { "FAL",     { "Fal_", "Mag_FAL", NULL },
                 { "M68Optic", "ACOGOptic", NULL }, true },
    { "FAMAS",   { "Mag_STANAG", "Mag_FAMAS", NULL },
                 { "M68Optic", "ACOGOptic", "ReflexOptic", NULL }, true },
    { "AUG",     { "AUG_", "Mag_AUG", "Mag_STANAG", NULL },
                 { "AUGOptic", "M68Optic", "ACOGOptic", NULL }, true },
    { "VSS",     { "Mag_VSS", NULL },
                 { "PSO1Scope", "KazuarOptic", NULL }, false },
    { "M249",    { "Mag_M249", NULL },
                 { "M68Optic", "ACOGOptic", NULL }, true },
    { "PKM",     { "Mag_PKM", NULL },
                 { NULL }, false },
    // === SNIPER RIFLES ===
    { "Mosin",   { "Mosin_", NULL },
                 { "PUScopeOptic", "HuntingOptic", NULL }, false },
    { "SVD",     { "Mag_SVD", NULL },
                 { "PSO1Scope", "PSO11Scope", "KazuarOptic", NULL }, false },
    { "CZ527",   { "Mag_CZ527", NULL },
                 { "HuntingOptic", "PUScopeOptic", NULL }, false },
    { "CZ550",   { NULL },
                 { "HuntingOptic", "PUScopeOptic", NULL }, false },
    { "Blaze",   { NULL },
                 { "HuntingOptic", "PUScopeOptic", NULL }, false },
    { "Scout",   { "Mag_Scout", NULL },
                 { "HuntingOptic", "ACOGOptic", NULL }, true },
    { "SSG",     { "Mag_SSG", NULL },
                 { "HuntingOptic", "ACOGOptic", NULL }, true },
    { "Winchester",{ NULL },
                 { "HuntingOptic", NULL }, false },
    { "Repeater",{ NULL },
                 { "HuntingOptic", NULL }, false },
    { "LongHorn",{ NULL },
                 { "HuntingOptic", NULL }, false },
    // === SMGS ===
    { "MP5",     { "MP5_", "Mag_MP5", NULL },
                 { "ReflexOptic", NULL }, true },
    { "UMP",     { "Mag_UMP", NULL },
                 { "ReflexOptic", NULL }, true },
    { "Scorpion",{ "Mag_CZ61", NULL },
                 { NULL }, false },
    { "Bizon",   { "Mag_Bizon", NULL },
                 { NULL }, false },
    // === SHOTGUNS ===
    { "Saiga",   { "Saiga_", "Mag_Saiga", NULL },
                 { NULL }, false },
    { "Vaiga",   { "Saiga_", "Mag_Saiga", NULL },
                 { NULL }, false },
    { "BenelliM4",{ NULL }, { NULL }, false },
    { "MP133",   { NULL }, { NULL }, false },
    { "MP153",   { NULL }, { NULL }, false },
    { "AA12",    { "Mag_AA12", NULL }, { NULL }, false },
    // === PISTOLS ===
    { "CZ75",    { "Mag_CZ75", NULL },
                 { "PistolOptic", NULL }, false },
    { "FNX",     { "Mag_FNX", NULL },
                 { "PistolOptic", "TLRLight", NULL }, false },
    { "Glock",   { "Mag_Glock", NULL },
                 { "PistolOptic", "TLRLight", NULL }, false },
    { "Mlock",   { "Mag_Glock", NULL },
                 { "PistolOptic", "TLRLight", NULL }, false },
    { "Deagle",  { "Mag_Deagle", NULL },
                 { NULL }, false },
    { "DesertEagle", { "Mag_Deagle", NULL },
                 { NULL }, false },
    { "MK2",     { "Mag_22", NULL },
                 { NULL }, false },
    { "Magnum",  { NULL }, { NULL }, false },
    { "Derringer",{ NULL }, { NULL }, false },
    { "P1",      { "Mag_P1", NULL }, { NULL }, false },
    { "Longhorn",{ NULL }, { NULL }, false },
    { NULL }  // sentinel
};

// ── Attachment slot classifier ───────────────────────────────────────────────
// Determines what slot type an item fills (or -1 if not an attachment).

static int classify_attachment_slot(const char *cn) {
    char lower[MAX_CLASSNAME_LEN];
    int i = 0;
    while (cn[i] && i < MAX_CLASSNAME_LEN - 1) {
        lower[i] = (char)tolower((unsigned char)cn[i]);
        i++;
    }
    lower[i] = '\0';

    if (strstr(lower, "mag_") || (strstr(lower, "_mag") && !strstr(lower, "_magnum")))
        return ATTACH_MAG;
    if (strstr(lower, "optic") || strstr(lower, "acog") || strstr(lower, "pso") ||
        strstr(lower, "scope") || strstr(lower, "rds")  || strstr(lower, "reflex") ||
        strstr(lower, "kobra") || strstr(lower, "kazuar"))
        return ATTACH_OPTIC;
    if (strstr(lower, "bttstck"))
        return ATTACH_BUTTSTOCK;
    if (strstr(lower, "hndgrd"))
        return ATTACH_HANDGUARD;
    if (strstr(lower, "suppressor") || strstr(lower, "silencer") || strstr(lower, "compensator"))
        return ATTACH_MUZZLE;
    if (strstr(lower, "universallight") || strstr(lower, "pistollight") ||
        strstr(lower, "tlrlight") || strstr(lower, "weaponflashlight"))
        return ATTACH_LIGHT;
    return -1;
}

// ── Word-boundary-aware substring match ──────────────────────────────────────
// Returns true if 'pattern' appears in 'text' preceded by '_' or start of string.
// Prevents "AK" from matching inside "BACKPACK".

static bool ci_prefix_at(const char *str, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix))
            return false;
        str++; prefix++;
    }
    return true;
}

static bool contains_weapon_id(const char *text, const char *pat) {
    for (const char *p = text; *p; p++) {
        if (ci_prefix_at(p, pat)) {
            if (p == text || *(p - 1) == '_')
                return true;
        }
    }
    return false;
}

// ── Mod-prefix heuristic ─────────────────────────────────────────────────────
// If weapon "SNAFU_AK47" and attachment "SNAFU_AK_Stock" share mod prefix
// before the first '_', they might be from the same mod and compatible.

static bool shares_mod_prefix(const char *weapon, const char *attach) {
    const char *u = strchr(weapon, '_');
    if (!u || u == weapon) return false;
    int len = (int)(u - weapon);
    for (int i = 0; i < len; i++) {
        if (tolower((unsigned char)weapon[i]) != tolower((unsigned char)attach[i]))
            return false;
    }
    return (attach[len] == '_');
}

// ── Find all compatible attachments for a weapon ─────────────────────────────
// Scans ctx->items, matches against the platform table + heuristics,
// and returns grouped attachment list.

static int find_weapon_attachments(AuditorContext *ctx, const char *weapon_cn,
                                   SpawnCategory scat,
                                   FoundAttach *out, int max_out) {
    int count = 0;

    // 1. Find matching platform from static table
    const WeaponPlatform *plat = NULL;
    for (int p = 0; WEAPON_PLATFORMS[p].weapon_pat; p++) {
        if (contains_weapon_id(weapon_cn, WEAPON_PLATFORMS[p].weapon_pat)) {
            plat = &WEAPON_PLATFORMS[p];
            break;
        }
    }

    // 2. Scan all items for compatible attachments
    // Retrieve the weapon item for vanilla/mod boundary checking
    const LootItem *weapon_item = NULL;
    for (int wi = 0; wi < ctx->item_count; wi++) {
        if (util_strcasecmp(ctx->items[wi].classname, weapon_cn) == 0 && !ctx->items[wi].deleted) {
            weapon_item = &ctx->items[wi];
            break;
        }
    }

    for (int i = 0; i < ctx->item_count && count < max_out; i++) {
        LootItem *it = &ctx->items[i];
        if (it->deleted) continue;

        int slot = classify_attachment_slot(it->classname);
        if (slot < 0) continue;

        // ── Vanilla/mod boundary filter for weapon attachments ───────────
        // Vanilla weapons: only vanilla attachments.
        // Mod weapons: same-mod + vanilla attachments.
        if (weapon_item && !gf_is_pool_compatible(weapon_item, it)) continue;

        bool compatible = false;

        if (plat) {
            // Check platform prefixes (mags, stocks, handguards, muzzles)
            for (int pr = 0; plat->prefixes[pr] && !compatible; pr++) {
                if (util_str_contains_ci(it->classname, plat->prefixes[pr]))
                    compatible = true;
            }
            // Check explicit optics list
            if (slot == ATTACH_OPTIC && !compatible) {
                for (int op = 0; plat->optics[op]; op++) {
                    if (util_strcasecmp(it->classname, plat->optics[op]) == 0) {
                        compatible = true;
                        break;
                    }
                }
            }
            // Universal lights for railed weapons
            if (slot == ATTACH_LIGHT && plat->has_rail)
                compatible = true;
        }

        // Heuristic fallback: mod-prefix matching for unknown platforms
        if (!compatible && !plat) {
            if (shares_mod_prefix(weapon_cn, it->classname))
                compatible = true;
        }

        // Web lookup fallback: check web-discovered attachment relationships
        if (!compatible && ctx->web) {
            WebItem web_match[4];
            int wcount = web_find_attachments_for(
                (const WebLookupState *)ctx->web,
                weapon_cn, web_match, 4);
            for (int w = 0; w < wcount && !compatible; w++) {
                if (util_strcasecmp(it->classname,
                                     web_match[w].classname) == 0)
                    compatible = true;
            }
        }

        if (compatible) {
            strncpy(out[count].classname, it->classname, MAX_CLASSNAME_LEN - 1);
            out[count].classname[MAX_CLASSNAME_LEN - 1] = '\0';
            out[count].slot = (AttachSlot)slot;
            out[count].nominal = it->nominal > 0 ? it->nominal : 5;
            count++;
        }
    }

    return count;
}

// ── Tier-based chance scaling ────────────────────────────────────────────────
// Higher tier → less likely to spawn fully kitted.
// T1 = 1.0, T5 ≈ 0.68, T10 ≈ 0.28

static float tier_chance_scale(int tier) {
    if (tier <= 1) return 1.0f;
    if (tier >= 10) return 0.10f;
    return 1.0f - 0.09f * (tier - 1);
}

// ── Dynamic weapon spawnable generator ───────────────────────────────────────
// Builds <type> XML with real compatible attachments (one <attachments> block
// per slot) and minimal cargo.  Falls back to generic cargo if no attachments
// were found by the platform/heuristic system.

static int generate_weapon_spawnable(char *buf, size_t buf_len,
                                     const LootItem *item, SpawnCategory scat,
                                     AuditorContext *ctx) {
    FoundAttach found[MAX_FOUND_ATTACH];
    int total = find_weapon_attachments(ctx, item->classname, scat,
                                        found, MAX_FOUND_ATTACH);

    float tscale = tier_chance_scale(item->assigned_tier);
    int pos = 0;

    pos += snprintf(buf + pos, buf_len - pos,
        "<type name=\"%s\">\n", item->classname);

    if (total > 0) {
        // One <attachments> block per slot that has entries
        for (int slot = 0; slot < WEAPON_SLOT_COUNT && (size_t)pos < buf_len - 128; slot++) {
            // Collect items for this slot (max MAX_PER_SLOT)
            const FoundAttach *picks[MAX_PER_SLOT];
            int pick_count = 0;
            float weight_sum = 0.0f;
            for (int f = 0; f < total && pick_count < MAX_PER_SLOT; f++) {
                if (found[f].slot == slot) {
                    picks[pick_count] = &found[f];
                    weight_sum += (float)found[f].nominal;
                    pick_count++;
                }
            }
            if (pick_count == 0) continue;

            float slot_chance = SLOT_BASE_CHANCE[slot] * tscale;
            if (slot_chance < 0.01f) slot_chance = 0.01f;

            pos += snprintf(buf + pos, buf_len - pos,
                "    <attachments chance=\"%.2f\">\n", slot_chance);

            for (int s = 0; s < pick_count && (size_t)pos < buf_len - 128; s++) {
                float w = (weight_sum > 0.0f)
                    ? (float)picks[s]->nominal / weight_sum
                    : 1.0f / (float)pick_count;
                if (w < 0.02f) w = 0.02f;
                pos += snprintf(buf + pos, buf_len - pos,
                    "        <item name=\"%s\" chance=\"%.2f\" health=\"0.20,0.85\" />\n",
                    picks[s]->classname, w);
            }

            pos += snprintf(buf + pos, buf_len - pos,
                "    </attachments>\n");
        }
    }

    // Weapons have attachment slots only — no cargo inventory.

    pos += snprintf(buf + pos, buf_len - pos, "</type>");
    return (pos > 0 && (size_t)pos < buf_len) ? pos : 0;
}

// ============================================================================
// SPAWNABLE XML BLOCK GENERATORS
// ============================================================================
// Weapon categories → dynamic attachment lookup via generate_weapon_spawnable().
// Non-weapon categories → TIER-BASED cargo pools + web-discovered attachments.
//
// Cargo philosophy (hardcore server):
//   - ALL items of the container's tier ±1 form the cargo pool
//   - Each gets a very low individual chance (~0.03-0.05)
//   - ~40-55% outer cargo chance → common to find empty containers
//   - Every ~2nd-3rd large container might have 1-2 items
//   - 3 empties in a row is common (~19% probability)
//   - Durability is mostly very low (0.02-0.45)
//   - Collectables are ultra-rare in cargo (0.03× weight, 0.08× chance)
//
// Attachment philosophy (universal):
//   - Web lookup discovers attachment slots for ALL items, not just weapons
//   - Mods add pouches to armbands, gun slots to vests, NVGs to helmets, etc.
//   - Color variants share attachment rules (strip suffixes before lookup)

// ── Cargo eligibility ────────────────────────────────────────────────────────
// Determines if an item can appear as cargo inside another item's inventory.
// Deliberately broad — the game engine handles physical size constraints.

static bool is_cargo_eligible(const LootItem *item) {
    if (item->deleted || item->is_debug_item) return false;
    if (loot_is_zombie((LootItem*)item) || loot_is_animal((LootItem*)item))
        return false;

    const char *cn = item->classname;
    const char *cat = item->category;

    // Storage containers — can't nest containers inside cargo
    if (util_str_contains_ci(cn, "Barrel") || util_str_contains_ci(cn, "SeaChest") ||
        util_str_contains_ci(cn, "WoodenCrate"))
        return false;
    if (util_str_contains_ci(cn, "Tent") || util_str_contains_ci(cn, "Shelter") ||
        util_str_contains_ci(cn, "Canopy"))
        return false;

    // Vehicle bodies and parts
    if (cat[0] && strstr(cat, "vehicles")) return false;
    if (util_str_contains_ci(cn, "CarWheel") || util_str_contains_ci(cn, "HatchbackWheel") ||
        util_str_contains_ci(cn, "Tire") || util_str_contains_ci(cn, "CarDoor") ||
        util_str_contains_ci(cn, "CarHood") || util_str_contains_ci(cn, "CarRadiator") ||
        util_str_contains_ci(cn, "TruckBattery") || util_str_contains_ci(cn, "CarBattery") ||
        util_str_contains_ci(cn, "SparkPlug") || util_str_contains_ci(cn, "HeadlightH7") ||
        util_str_contains_ci(cn, "CivSedanDoors") || util_str_contains_ci(cn, "CivSedanWheel") ||
        util_str_contains_ci(cn, "CivSedanHood") || util_str_contains_ci(cn, "CivSedanTrunk") ||
        util_str_contains_ci(cn, "OffroadDoors") || util_str_contains_ci(cn, "OffroadWheel") ||
        util_str_contains_ci(cn, "OffroadHood") || util_str_contains_ci(cn, "OffroadTrunk") ||
        util_str_contains_ci(cn, "HatchbackDoors") || util_str_contains_ci(cn, "HatchbackHood") ||
        util_str_contains_ci(cn, "HatchbackTrunk") ||
        util_str_contains_ci(cn, "Sedan02Doors") || util_str_contains_ci(cn, "Sedan02Hood") ||
        util_str_contains_ci(cn, "Sedan02Trunk") || util_str_contains_ci(cn, "Sedan02Wheel") ||
        util_str_contains_ci(cn, "Bus_Wheel") || util_str_contains_ci(cn, "Truck_01_Wheel") ||
        util_str_contains_ci(cn, "Truck_01_Door") ||
        util_str_contains_ci(cn, "GunnerDoors") || util_str_contains_ci(cn, "GunnerWheel") ||
        util_str_contains_ci(cn, "GunnerHood") ||
        // Expansion vehicle parts
        util_str_contains_ci(cn, "ExpansionWheel") || util_str_contains_ci(cn, "ExpansionDoor") ||
        util_str_contains_ci(cn, "ExpansionHood") || util_str_contains_ci(cn, "ExpansionRotor") ||
        util_str_contains_ci(cn, "ExpansionFueltank") ||
        // Expansion vehicle bodies (must not appear in cargo)
        util_str_contains_ci(cn, "ExpansionUh1h") || util_str_contains_ci(cn, "ExpansionMerlin") ||
        util_str_contains_ci(cn, "ExpansionMH6") || util_str_contains_ci(cn, "ExpansionGyro") ||
        util_str_contains_ci(cn, "ExpansionMi") || util_str_contains_ci(cn, "ExpansionCes") ||
        util_str_contains_ci(cn, "ExpansionAn2") || util_str_contains_ci(cn, "ExpansionBus") ||
        util_str_contains_ci(cn, "ExpansionTractor") || util_str_contains_ci(cn, "ExpansionVodnik") ||
        util_str_contains_ci(cn, "ExpansionUtility") || util_str_contains_ci(cn, "ExpansionZodiac") ||
        util_str_contains_ci(cn, "ExpansionLHD") || util_str_contains_ci(cn, "ExpansionSailboat") ||
        util_str_contains_ci(cn, "ExpansionV3S") || util_str_contains_ci(cn, "ExpansionUral") ||
        util_str_contains_ci(cn, "ExpansionSedan") || util_str_contains_ci(cn, "ExpansionHatchback") ||
        util_str_contains_ci(cn, "ExpansionOffroad") || util_str_contains_ci(cn, "ExpansionCivil"))
        return false;

    // Base building
    if (util_str_contains_ci(cn, "Fence") || util_str_contains_ci(cn, "Watchtower") ||
        util_str_contains_ci(cn, "Gate") || util_str_contains_ci(cn, "CodeLock") ||
        util_str_contains_ci(cn, "CombinationLock"))
        return false;

    return true;
}

// ── Container type compatibility (hard block) ───────────────────────────────
// Prevents wrong-type combinations: weapon attachments in shirts, random junk
// in medical kits, clothes in ammo boxes, etc.  This is a HARD filter.
// Items that fail are completely excluded from the container's cargo pool.
// The affinity system (cargo_type_affinity) provides additional SOFT weighting.

static bool is_item_wrong_type_for_container(const LootItem *item,
                                             const LootItem *container) {
    const char *icn = item->classname;
    const char *icat = item->category;
    const char *ccn = container->classname;

    // ── Medical containers: ONLY medical items allowed ───────────────────
    bool is_medical_container =
        util_str_contains_ci(ccn, "FirstAidKit") ||
        util_str_contains_ci(ccn, "MedicalCase") ||
        util_str_contains_ci(ccn, "Medkit") ||
        util_str_contains_ci(ccn, "medbag");
    if (is_medical_container) {
        bool is_medical_item =
            util_str_contains_ci(icn, "Bandage") ||
            util_str_contains_ci(icn, "Morphine") ||
            util_str_contains_ci(icn, "Epinephrine") ||
            util_str_contains_ci(icn, "Saline") ||
            util_str_contains_ci(icn, "Tourniquet") ||
            util_str_contains_ci(icn, "Tetracycline") ||
            util_str_contains_ci(icn, "Codeine") ||
            util_str_contains_ci(icn, "Charcoal") ||
            util_str_contains_ci(icn, "Vitamins") ||
            util_str_contains_ci(icn, "Vitamin") ||
            util_str_contains_ci(icn, "Syringe") ||
            util_str_contains_ci(icn, "BloodBag") ||
            util_str_contains_ci(icn, "BloodTest") ||
            util_str_contains_ci(icn, "Thermometer") ||
            util_str_contains_ci(icn, "Splint") ||
            util_str_contains_ci(icn, "Disinfectant") ||
            util_str_contains_ci(icn, "IodineTincture") ||
            util_str_contains_ci(icn, "PainKiller") ||
            util_str_contains_ci(icn, "Painkiller") ||
            util_str_contains_ci(icn, "AntiChemInjector") ||
            util_str_contains_ci(icn, "StartKit") ||
            util_str_contains_ci(icn, "Rag") ||
            util_str_contains_ci(icn, "SewingKit") ||
            util_str_contains_ci(icn, "PurificationTablets");
        if (!is_medical_item) return true;
    }

    // ── Ammo containers: ONLY ammo/mags/weapon-related items allowed ─────
    bool is_ammo_container =
        util_str_contains_ci(ccn, "AmmoBox") ||
        util_str_contains_ci(ccn, "AmmoCan");
    // Skip ammo BOX containers that are actually ammo items themselves
    // (e.g. AmmoBox_556x45_20Rnd IS an ammo item, not a storage container)
    if (is_ammo_container && !util_str_contains_ci(ccn, "_")) {
        // Only the plain "AmmoBox" or "AmmoCan" containers (no underscore suffix)
        bool is_ammo_item =
            util_str_contains_ci(icn, "Ammo") ||
            util_str_contains_ci(icn, "Mag_") ||
            util_str_contains_ci(icn, "_Mag") ||
            util_str_contains_ci(icn, "Drum") ||
            util_str_contains_ci(icn, "rdMag") ||
            util_str_contains_ci(icn, "Stanag") ||
            util_str_contains_ci(icn, "Clip_") ||
            util_str_contains_ci(icn, "_Clip");
        if (!is_ammo_item) return true;
    }

    // ── Weapon attachments in non-tactical clothing ──────────────────────
    // Buttstocks, handguards, bayonets, suppressors, bipods are weapon-specific
    // parts that should only appear in tactical gear (vests, plate carriers,
    // chest rigs), weapon cases, or large backpacks — not in random shirts,
    // pants, jackets, hats, boots, etc.
    bool is_weapon_attachment =
        util_str_contains_ci(icn, "Bttstck") ||
        util_str_contains_ci(icn, "Hndgrd") ||
        util_str_contains_ci(icn, "Bayonet") ||
        util_str_contains_ci(icn, "Bipod") ||
        util_str_contains_ci(icn, "_Suppressor") ||
        util_str_contains_ci(icn, "Compensator") ||
        util_str_contains_ci(icn, "RifleSling") ||
        util_str_contains_ci(icn, "PistolGrip") ||
        util_str_contains_ci(icn, "PBS1");
    if (is_weapon_attachment) {
        bool is_tactical_container =
            util_str_contains_ci(ccn, "Vest") ||
            util_str_contains_ci(ccn, "PlateCarrier") ||
            util_str_contains_ci(ccn, "ChestRig") ||
            util_str_contains_ci(ccn, "Smersh") ||
            util_str_contains_ci(ccn, "Carrier") ||
            util_str_contains_ci(ccn, "Rig") ||
            util_str_contains_ci(ccn, "Backpack") ||
            util_str_contains_ci(ccn, "AliceBag") ||
            util_str_contains_ci(ccn, "MountainBag") ||
            util_str_contains_ci(ccn, "CoyoteBag") ||
            util_str_contains_ci(ccn, "HuntingBag") ||
            util_str_contains_ci(ccn, "AssaultBag") ||
            util_str_contains_ci(ccn, "FieldPack") ||
            util_str_contains_ci(ccn, "DryBag") ||
            util_str_contains_ci(ccn, "Tent") ||
            util_str_contains_ci(ccn, "SeaChest") ||
            util_str_contains_ci(ccn, "WoodenCrate") ||
            util_str_contains_ci(ccn, "Barrel") ||
            util_str_contains_ci(ccn, "AmmoBox") ||
            util_str_contains_ci(ccn, "AmmoCan") ||
            util_str_contains_ci(ccn, "Ratnik") ||
            util_str_contains_ci(ccn, "Molle") ||
            util_str_contains_ci(ccn, "6sh112");
        if (!is_tactical_container) return true;
    }

    return false;
}

// ── Item-to-container size compatibility (hard block) ────────────────────────
// Prevents physically impossible combinations: rifles in shirts, brooms in
// pants, backpacks in pouches, etc.  This is a HARD filter — items that fail
// this check are completely excluded from the container's cargo pool.
// The affinity system (cargo_type_affinity) provides SOFT weighting on top.

static bool is_item_too_large_for_container(const LootItem *item,
                                            const LootItem *container,
                                            SpawnCategory container_scat) {
    const char *icn = item->classname;
    const char *icat = item->category;
    const char *ccn = container->classname;

    // ── Large tools that can't fit in any clothing cargo ─────────────────
    // These are 2-handed items (6+ inventory slots) that physically cannot
    // fit inside pockets, vests, pants, or small bags.
    bool is_large_tool =
        util_str_contains_ci(icn, "BarbedWire") ||
        util_str_contains_ci(icn, "MetalWire") ||
        util_str_contains_ci(icn, "Broom") ||
        util_str_contains_ci(icn, "BaseballBat") ||
        util_str_contains_ci(icn, "Crowbar") ||
        util_str_contains_ci(icn, "Pickaxe") ||
        util_str_contains_ci(icn, "Shovel") ||
        util_str_contains_ci(icn, "Sledgehammer") ||
        util_str_contains_ci(icn, "FarmingHoe") ||
        util_str_contains_ci(icn, "PipeWrench") ||
        util_str_contains_ci(icn, "Pipe") ||
        util_str_contains_ci(icn, "GardenLime") ||
        util_str_contains_ci(icn, "Fertilizer") ||
        util_str_contains_ci(icn, "WoodenPlank") ||
        util_str_contains_ci(icn, "LongWoodenStick") ||
        util_str_contains_ci(icn, "FireExtinguisher") ||
        util_str_contains_ci(icn, "PowerGenerator") ||
        util_str_contains_ci(icn, "Spotlight") ||
        util_str_contains_ci(icn, "BatteryCharger") ||
        util_str_contains_ci(icn, "Sword") ||
        util_str_contains_ci(icn, "Machete") ||
        util_str_contains_ci(icn, "OrientalMachete");

    // ── Rifles and large weapons (1x8+ slots, won't fit in clothing) ─────
    bool is_large_weapon = false;
    if (icat[0] && strstr(icat, "weapons")) {
        // Pistols, SMGs, and small weapons are OK in medium+ containers
        bool is_small_weapon =
            util_str_contains_ci(icn, "Pistol") || util_str_contains_ci(icn, "Glock") ||
            util_str_contains_ci(icn, "CZ75") || util_str_contains_ci(icn, "FNX") ||
            util_str_contains_ci(icn, "Deagle") || util_str_contains_ci(icn, "Mlock") ||
            util_str_contains_ci(icn, "Magnum") || util_str_contains_ci(icn, "Derringer") ||
            util_str_contains_ci(icn, "P1") || util_str_contains_ci(icn, "MK2") ||
            util_str_contains_ci(icn, "Flare") || util_str_contains_ci(icn, "Skorpion") ||
            util_str_contains_ci(icn, "MP5K") ||
            // Magazines, optics, attachments - these are small
            util_str_contains_ci(icn, "Mag_") || util_str_contains_ci(icn, "_Mag") ||
            util_str_contains_ci(icn, "Optic") || util_str_contains_ci(icn, "Bttstck") ||
            util_str_contains_ci(icn, "Hndgrd") || util_str_contains_ci(icn, "Suppressor") ||
            util_str_contains_ci(icn, "Compensator") || util_str_contains_ci(icn, "Bayonet") ||
            util_str_contains_ci(icn, "Bipod") || util_str_contains_ci(icn, "Sling") ||
            util_str_contains_ci(icn, "Grip") || util_str_contains_ci(icn, "Light") ||
            util_str_contains_ci(icn, "Ammo");
        if (!is_small_weapon)
            is_large_weapon = true;
    }
    // Weapon-like classnames
    if (!is_large_weapon) {
        if (util_str_contains_ci(icn, "_Gun") || util_str_contains_ci(icn, "Rifle") ||
            util_str_contains_ci(icn, "Shotgun") || util_str_contains_ci(icn, "Launcher")) {
            // But not magazine/attachment classnames containing these
            if (!util_str_contains_ci(icn, "Mag_") && !util_str_contains_ci(icn, "_Mag") &&
                !util_str_contains_ci(icn, "Optic") && !util_str_contains_ci(icn, "Drum") &&
                !util_str_contains_ci(icn, "Sling") && !util_str_contains_ci(icn, "Bttstck") &&
                !util_str_contains_ci(icn, "Hndgrd"))
                is_large_weapon = true;
        }
    }

    // ── Backpacks / large bags ───────────────────────────────────────────
    bool is_bag = util_str_contains_ci(icn, "Backpack") ||
                  util_str_contains_ci(icn, "AliceBag") ||
                  util_str_contains_ci(icn, "MountainBag") ||
                  util_str_contains_ci(icn, "CoyoteBag") ||
                  util_str_contains_ci(icn, "TortillaBag") ||
                  util_str_contains_ci(icn, "HuntingBag") ||
                  util_str_contains_ci(icn, "AssaultBag") ||
                  util_str_contains_ci(icn, "DryBag") ||
                  util_str_contains_ci(icn, "DrysackBag") ||
                  util_str_contains_ci(icn, "FieldPack") ||
                  util_str_contains_ci(icn, "CanvasBag") ||
                  util_str_contains_ci(icn, "CourierBag") ||
                  util_str_contains_ci(icn, "ChildBag") ||
                  util_str_contains_ci(icn, "SchoolBag") ||
                  util_str_contains_ci(icn, "TeddyBear") ||
                  // Modded bag patterns (MMG, Mass, etc.)
                  util_str_contains_ci(icn, "assault_pack") ||
                  util_str_contains_ci(icn, "mmps_bag") ||
                  util_str_contains_ci(icn, "supplybag") ||
                  util_str_contains_ci(icn, "mmps_Pack");

    // ── Apply size rules based on container category ─────────────────────
    if (container_scat == SPAWN_CAT_STORAGE_SMALL) {
        // SMALL containers (shirts, scarves, hats, belts, dresses):
        // Only very small items can fit. Block all large stuff.
        if (is_large_tool || is_large_weapon || is_bag)
            return true;
    }

    if (container_scat == SPAWN_CAT_STORAGE_MEDIUM) {
        // MEDIUM containers (vests, jackets, pants, helmets, pouches):
        // Block rifles, large tools, and backpacks.
        // Pistols, mags, ammo, and small tools are OK.
        if (is_large_tool || is_large_weapon || is_bag)
            return true;
    }

    // LARGE containers (backpacks, tents, barrels, crates):
    // Only block the truly impossible items (barrels in barrels etc.)
    // which is already handled by is_cargo_eligible().

    // ── Holster/pouch specific: strict cargo rules ─────────────────────
    if (util_str_contains_ci(ccn, "Holster") || util_str_contains_ci(ccn, "Pouch")) {
        if (is_bag) return true;
        // Small bags that also shouldn't be in pouches
        if (util_str_contains_ci(icn, "DuffelBag") ||
            util_str_contains_ci(icn, "FurImprovisedBag") ||
            util_str_contains_ci(icn, "AVSBag") ||
            util_str_contains_ci(icn, "CamelBakBag") ||
            util_str_contains_ci(icn, "MAPBag") ||
            util_str_contains_ci(icn, "AFAKMedPouch"))
            return true;
        // Drum magazines are too large for pouches
        if (util_str_contains_ci(icn, "Drum")) return true;
        // Large weapons can't go in pouches
        if (is_large_weapon) return true;
        // Weapon attachments (suppressor, compensator, bayonet, etc.) too big
        if (util_str_contains_ci(icn, "Bttstck") || util_str_contains_ci(icn, "Hndgrd") ||
            util_str_contains_ci(icn, "Suppressor") || util_str_contains_ci(icn, "Compensator") ||
            util_str_contains_ci(icn, "Bayonet") || util_str_contains_ci(icn, "RifleSling") ||
            util_str_contains_ci(icn, "PistolGrip") || util_str_contains_ci(icn, "PBS1"))
            return true;
        // Flags don't belong in pouches (check prefix "Flag_")
        if (_strnicmp(icn, "Flag_", 5) == 0) return true;
        // Food/steaks/blood bags don't belong in holsters
        if (util_str_contains_ci(icn, "SteakMeat") || util_str_contains_ci(icn, "BloodBagFull") ||
            util_str_contains_ci(icn, "Bark_") || util_str_contains_ci(icn, "BakedBeansCan"))
            return true;
    }

    return false;
}

static bool is_collectable_item(const LootItem *item) {
    if (item->assigned_tier == 5) return true;
    const char *cn = item->classname;
    const char *cat = item->category;
    if (util_str_contains_ci(cn, "O12_Caps") ||
        util_str_contains_ci(cn, "Collectable") ||
        util_str_contains_ci(cn, "Collectible") ||
        util_str_contains_ci(cn, "Heirloom"))
        return true;
    if (cat[0] && (strstr(cat, "collectables") || strstr(cat, "collections")))
        return true;
    return false;
}

// ── Color variant stripping ──────────────────────────────────────────────────
// Strips common color/camo suffixes so "GorkaHelmet_Black" and
// "GorkaHelmet_Green" share the same attachment rules discovered via web.

static void strip_color_suffix(const char *cn, char *base, int max_len) {
    static const char *suffixes[] = {
        "_Black", "_Green", "_Tan", "_Blue", "_Red", "_White", "_Grey", "_Gray",
        "_Camo", "_Woodland", "_Desert", "_Urban", "_Winter", "_Olive", "_Brown",
        "_Orange", "_Yellow", "_Pink", "_Navy", "_Khaki", "_DPM", "_TTsKO",
        "_Autumn", "_Spring", "_Summer", "_Mossy", "_Forest", "_Flecktarn",
        "_MultiCam", "_ATACS", "_Hex", "_M65", "_Checked", "_Netting",
        NULL
    };
    strncpy(base, cn, max_len - 1);
    base[max_len - 1] = '\0';
    int blen = (int)strlen(base);
    for (int i = 0; suffixes[i]; i++) {
        int slen = (int)strlen(suffixes[i]);
        if (blen > slen) {
            bool match = true;
            for (int c = 0; c < slen; c++) {
                if (tolower((unsigned char)base[blen - slen + c]) !=
                    tolower((unsigned char)suffixes[i][c])) {
                    match = false;
                    break;
                }
            }
            if (match) {
                base[blen - slen] = '\0';
                return;
            }
        }
    }
}

// ── Equipment attachment discovery via web lookup ────────────────────────────
// Queries web lookup for any non-weapon item to find attachment slots that
// mods may define (pouches on armbands, gun slots on vests, visors on helmets,
// NVG mounts, etc.).  Also tries color-stripped variant for broader matching.

static int classify_equip_web_slot(const char *wcn) {
    if (util_str_contains_ci(wcn, "Pouch") || util_str_contains_ci(wcn, "PouchMed"))
        return EQUIP_POUCH;
    if (util_str_contains_ci(wcn, "Holster") || util_str_contains_ci(wcn, "PistolHolster"))
        return EQUIP_HOLSTER;
    if (util_str_contains_ci(wcn, "ArmorPlate") || util_str_contains_ci(wcn, "BallisticPlate"))
        return EQUIP_ARMOR_PLATE;
    if (util_str_contains_ci(wcn, "NVG") || util_str_contains_ci(wcn, "Headstrap") ||
        util_str_contains_ci(wcn, "NVGoggles"))
        return EQUIP_NVG;
    if (util_str_contains_ci(wcn, "Visor") || util_str_contains_ci(wcn, "FaceShield"))
        return EQUIP_VISOR;
    if (util_str_contains_ci(wcn, "HeadLight") || util_str_contains_ci(wcn, "Headtorch") ||
        util_str_contains_ci(wcn, "HelmetLight"))
        return EQUIP_HELMET_LIGHT;
    if (util_str_contains_ci(wcn, "GhillieBushrag") || util_str_contains_ci(wcn, "GhillieAtt") ||
        util_str_contains_ci(wcn, "Wrap"))
        return EQUIP_WRAP;
    // Could be a weapon attachment on equipment (gun slot on armband)
    int wslot = classify_attachment_slot(wcn);
    return wslot;  // -1 if not recognized
}

static int find_equipment_attachments(AuditorContext *ctx, const char *equip_cn,
                                      FoundAttach *out, int max_out) {
    if (!ctx->web) return 0;
    int count = 0;

    // Find the equipment item for vanilla/mod boundary checking
    const LootItem *equip_item = NULL;
    for (int ei = 0; ei < ctx->item_count; ei++) {
        if (util_strcasecmp(ctx->items[ei].classname, equip_cn) == 0 && !ctx->items[ei].deleted) {
            equip_item = &ctx->items[ei];
            break;
        }
    }

    // Helper: try a classname against web, validate on server, add to out
    WebItem web_results[16];
    const char *queries[2];
    char base[MAX_CLASSNAME_LEN];
    int num_queries = 1;

    queries[0] = equip_cn;
    strip_color_suffix(equip_cn, base, MAX_CLASSNAME_LEN);
    if (strcmp(base, equip_cn) != 0) {
        queries[1] = base;
        num_queries = 2;
    }

    for (int q = 0; q < num_queries && count < max_out; q++) {
        int wcount = web_find_attachments_for(
            (const WebLookupState *)ctx->web, queries[q], web_results, 16);

        for (int w = 0; w < wcount && count < max_out; w++) {
            const char *wcn = web_results[w].classname;

            int slot = classify_equip_web_slot(wcn);
            if (slot < 0) continue;

            // Dedup
            bool dup = false;
            for (int d = 0; d < count && !dup; d++) {
                if (util_strcasecmp(out[d].classname, wcn) == 0) dup = true;
            }
            if (dup) continue;

            // Verify attachment actually exists on this server
            bool exists = false;
            int nom = 5;
            for (int i = 0; i < ctx->item_count; i++) {
                if (util_strcasecmp(ctx->items[i].classname, wcn) == 0 &&
                    !ctx->items[i].deleted) {
                    // ── Vanilla/mod boundary filter for equipment attachments ──
                    // Vanilla equipment: only vanilla attachments.
                    // Mod equipment: same-mod + vanilla attachments.
                    if (equip_item && !gf_is_pool_compatible(equip_item, &ctx->items[i]))
                        continue;
                    exists = true;
                    nom = ctx->items[i].nominal > 0 ? ctx->items[i].nominal : 5;
                    break;
                }
            }
            if (!exists) continue;

            strncpy(out[count].classname, wcn, MAX_CLASSNAME_LEN - 1);
            out[count].classname[MAX_CLASSNAME_LEN - 1] = '\0';
            out[count].slot = (AttachSlot)slot;
            out[count].nominal = nom;
            count++;
        }
    }

    return count;
}

// ── Tier-based cargo parameters per storage size ─────────────────────────────
// Hardcore server tuning: low chances, mostly empty, occasional small finds.
//
// Probability math for "every ~2nd large container has something":
//   outer_chance=0.55, 35 items at avg 0.04 each
//   P(at least one) when triggered = 1 - 0.96^35 ≈ 0.76
//   P(find something) overall = 0.55 × 0.76 ≈ 0.42  (~every 2nd-3rd)
//   P(3 empty in a row) = 0.58^3 ≈ 0.20  (common enough)

typedef struct {
    float outer_chance;       // <cargo chance="X"> — prob cargo generates at all
    int   max_pool;           // Max items picked from tier pool
    float item_chance_base;   // Base per-item chance (before rarity scaling)
} TierCargoParams;

static const TierCargoParams TIER_CARGO_PARAMS[] = {
    /* [0] NONE    */ { 0.00f,  0, 0.00f },
    /* [1] LARGE   */ { 0.55f, 35, 0.04f },   // ~42% find rate
    /* [2] MEDIUM  */ { 0.45f, 22, 0.035f },   // ~28% find rate
    /* [3] SMALL   */ { 0.30f, 10, 0.03f },    // ~12% find rate
};

static int tier_cargo_param_idx(SpawnCategory scat) {
    switch (scat) {
    case SPAWN_CAT_STORAGE_LARGE:  return 1;
    case SPAWN_CAT_STORAGE_MEDIUM: return 2;
    case SPAWN_CAT_STORAGE_SMALL:  return 3;
    default: return 0;
    }
}

#define MAX_TIER_POOL   400   // Pre-filtered eligible items per tier range
#define MAX_CARGO_PICKS  40   // Fits ~40 items in 4096 byte XML buffer

// ── Cargo type affinity ──────────────────────────────────────────────────────
// Containers prefer spawning contextually appropriate items.  A plate carrier
// should mostly hold mags/ammo/medical, not canned food.  A first aid kit
// should hold bandages, not rifle mags.
//
// Returns a weight multiplier applied during cargo pool construction:
//   >1.0 = preferred (more likely to appear)
//   1.0  = neutral (general containers, no bias)
//   <1.0 = discouraged (unlikely but not impossible)
//
// The affinity is SOFT — all items remain eligible.  This avoids empty pools
// while ensuring cargo contents make contextual sense.

static float cargo_type_affinity(const char *container_cn, const char *item_cn) {
    // ── Tactical containers (vests, plate carriers, chest rigs) ──────────────
    // These hold operational gear: mags, ammo, medical, tools
    bool is_tactical = util_str_contains_ci(container_cn, "Vest") ||
                       util_str_contains_ci(container_cn, "PlateCarrier") ||
                       util_str_contains_ci(container_cn, "ChestRig") ||
                       util_str_contains_ci(container_cn, "Smersh") ||
                       util_str_contains_ci(container_cn, "Carrier") ||
                       util_str_contains_ci(container_cn, "Rig");
    if (is_tactical) {
        // Strongly prefer tactical items
        if (util_str_contains_ci(item_cn, "Mag_") ||
            util_str_contains_ci(item_cn, "_Mag") ||
            util_str_contains_ci(item_cn, "Ammo") ||
            util_str_contains_ci(item_cn, "Bandage") ||
            util_str_contains_ci(item_cn, "Morphine") ||
            util_str_contains_ci(item_cn, "Epinephrine") ||
            util_str_contains_ci(item_cn, "Tourniquet") ||
            util_str_contains_ci(item_cn, "Saline") ||
            util_str_contains_ci(item_cn, "Grenade") ||
            util_str_contains_ci(item_cn, "Knife") ||
            util_str_contains_ci(item_cn, "Compass") ||
            util_str_contains_ci(item_cn, "Map") ||
            util_str_contains_ci(item_cn, "Radio"))
            return 2.0f;
        // Discourage food/drink — operators don't stuff sardines in plate carriers
        if (util_str_contains_ci(item_cn, "Food") ||
            util_str_contains_ci(item_cn, "Drink") ||
            util_str_contains_ci(item_cn, "Soda") ||
            util_str_contains_ci(item_cn, "Tuna") ||
            util_str_contains_ci(item_cn, "Beans") ||
            util_str_contains_ci(item_cn, "Sardines") ||
            util_str_contains_ci(item_cn, "Peach") ||
            util_str_contains_ci(item_cn, "Apple"))
            return 0.3f;
        return 1.0f;
    }

    // ── Medical containers → strongly prefer medical items ───────────────────
    bool is_medical = util_str_contains_ci(container_cn, "FirstAidKit") ||
                      util_str_contains_ci(container_cn, "MedicalCase") ||
                      util_str_contains_ci(container_cn, "Medkit") ||
                      util_str_contains_ci(container_cn, "MedBag");
    if (is_medical) {
        if (util_str_contains_ci(item_cn, "Bandage") ||
            util_str_contains_ci(item_cn, "Morphine") ||
            util_str_contains_ci(item_cn, "Epinephrine") ||
            util_str_contains_ci(item_cn, "Saline") ||
            util_str_contains_ci(item_cn, "Tourniquet") ||
            util_str_contains_ci(item_cn, "Tetracycline") ||
            util_str_contains_ci(item_cn, "Codeine") ||
            util_str_contains_ci(item_cn, "Charcoal") ||
            util_str_contains_ci(item_cn, "Vitamins") ||
            util_str_contains_ci(item_cn, "Syringe") ||
            util_str_contains_ci(item_cn, "BloodBag") ||
            util_str_contains_ci(item_cn, "BloodTest") ||
            util_str_contains_ci(item_cn, "Thermometer") ||
            util_str_contains_ci(item_cn, "Splint") ||
            util_str_contains_ci(item_cn, "Disinfectant") ||
            util_str_contains_ci(item_cn, "IodineTincture") ||
            util_str_contains_ci(item_cn, "PainKiller") ||
            util_str_contains_ci(item_cn, "AntiChemInjector"))
            return 3.0f;
        return 0.2f;  // Medical containers almost never have random junk
    }

    // ── Ammo boxes → strongly prefer ammunition ─────────────────────────────
    bool is_ammo_box = util_str_contains_ci(container_cn, "AmmoBox") ||
                       util_str_contains_ci(container_cn, "AmmoCan");
    if (is_ammo_box) {
        if (util_str_contains_ci(item_cn, "Ammo") ||
            util_str_contains_ci(item_cn, "Mag_") ||
            util_str_contains_ci(item_cn, "_Ammo"))
            return 3.0f;
        return 0.1f;
    }

    // ── Holsters/pouches → prefer small tactical items ──────────────────────
    bool is_holster = util_str_contains_ci(container_cn, "Holster") ||
                      util_str_contains_ci(container_cn, "Pouch");
    if (is_holster) {
        if (util_str_contains_ci(item_cn, "Mag_") ||
            util_str_contains_ci(item_cn, "Bandage") ||
            util_str_contains_ci(item_cn, "Knife") ||
            util_str_contains_ci(item_cn, "Compass") ||
            util_str_contains_ci(item_cn, "Flashlight") ||
            util_str_contains_ci(item_cn, "Battery") ||
            util_str_contains_ci(item_cn, "Lockpick"))
            return 1.8f;
        // Discourage large items that can't physically fit in a pouch
        if (util_str_contains_ci(item_cn, "Rifle") ||
            util_str_contains_ci(item_cn, "AK") ||
            util_str_contains_ci(item_cn, "Barrel") ||
            util_str_contains_ci(item_cn, "Tent") ||
            util_str_contains_ci(item_cn, "Backpack") ||
            util_str_contains_ci(item_cn, "Pack"))
            return 0.05f;
        return 0.7f;
    }

    // ── General containers / backpacks / clothing → no bias ─────────────────
    return 1.0f;
}

// Deterministic hash for reproducible cargo selection.
// Same container always gets the same cargo across builds.
static unsigned int cargo_hash(const char *s) {
    unsigned int h = 2166136261u;  // FNV-1a offset basis
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

// ── Tier-based cargo generator ───────────────────────────────────────────────
// For any storage item: collects ALL eligible items within tier ±1,
// deterministically selects a pool subset, assigns very low per-item chances.
// Hardcore tuning: low durability, scarce finds, collectables ultra-rare.
// Returns bytes written for <cargo>...</cargo> content (no <type> wrapper).

static int generate_tier_cargo_content(char *buf, size_t buf_len,
                                       const LootItem *container,
                                       SpawnCategory scat,
                                       AuditorContext *ctx) {
    int pidx = tier_cargo_param_idx(scat);
    if (pidx == 0) return 0;

    const TierCargoParams *params = &TIER_CARGO_PARAMS[pidx];
    int ctier = container->assigned_tier;
    if (ctier < 1) ctier = 1;

    // Tier range: same tier ±1 (bounded to 1–11)
    int min_tier = ctier - 1; if (min_tier < 1)  min_tier = 1;
    int max_tier = ctier + 1; if (max_tier > 11) max_tier = 11;

    // 1. Collect eligible items in tier range
    int   pool_idx[MAX_TIER_POOL];
    float pool_wt[MAX_TIER_POOL];
    int   pool_n = 0;

    for (int i = 0; i < ctx->item_count && pool_n < MAX_TIER_POOL; i++) {
        LootItem *it = &ctx->items[i];
        if (!is_cargo_eligible(it)) continue;

        int itier = it->assigned_tier;
        if (itier < 1) itier = 1;
        if (itier < min_tier || itier > max_tier) continue;

        // Don't put item in its own cargo
        if (util_strcasecmp(it->classname, container->classname) == 0) continue;

        // ── Type compatibility: hard-block wrong item types ─────────────
        // Non-medical items can't go in first aid kits, non-ammo in ammo boxes,
        // weapon attachments can't go in random shirts/pants/hats.
        if (is_item_wrong_type_for_container(it, container)) continue;

        // ── Size compatibility: hard-block oversized items ───────────────
        // Rifles can't fit in shirts, brooms can't fit in pants, etc.
        if (is_item_too_large_for_container(it, container, scat)) continue;

        // ── Vanilla/mod boundary filter ──────────────────────────────────
        // Vanilla containers: only vanilla items in cargo pool.
        // Mod containers: same-mod + vanilla items in cargo pool.
        if (!gf_is_pool_compatible(container, it)) continue;

        float w = (float)(it->nominal > 0 ? it->nominal : 3);
        // Tier distance penalty (items 1 tier away are half as likely)
        if (abs(itier - ctier) > 0) w *= 0.5f;

        // Cargo type affinity: contextual weighting based on container type.
        // Tactical vests prefer mags/ammo, medical kits prefer medical items, etc.
        w *= cargo_type_affinity(container->classname, it->classname);

        // Collectables: massively reduced weight (ultra-rare in cargo)
        if (is_collectable_item(it)) w *= 0.03f;

        pool_idx[pool_n] = i;
        pool_wt[pool_n]  = w;
        pool_n++;
    }

    if (pool_n == 0) return 0;

    // 2. Deterministic weighted selection of pool subset
    unsigned int seed = cargo_hash(container->classname);

    float total_wt = 0.0f;
    for (int i = 0; i < pool_n; i++) total_wt += pool_wt[i];

    int   sel_idx[MAX_CARGO_PICKS];
    float sel_chance[MAX_CARGO_PICKS];
    int   sel_n = 0;
    bool  picked[MAX_TIER_POOL];
    memset(picked, 0, sizeof(bool) * (pool_n < MAX_TIER_POOL ? pool_n : MAX_TIER_POOL));

    int picks = pool_n < params->max_pool ? pool_n : params->max_pool;
    if (picks > MAX_CARGO_PICKS) picks = MAX_CARGO_PICKS;

    for (int p = 0; p < picks && total_wt > 0.0f; p++) {
        seed = seed * 1103515245u + 12345u;
        float r = (float)(seed % 10000) / 10000.0f * total_wt;

        float acc = 0.0f;
        int pick = -1;
        for (int i = 0; i < pool_n; i++) {
            if (picked[i]) continue;
            acc += pool_wt[i];
            if (acc >= r) { pick = i; break; }
        }
        if (pick < 0) {
            for (int i = 0; i < pool_n; i++) {
                if (!picked[i]) { pick = i; break; }
            }
        }
        if (pick < 0) break;

        picked[pick] = true;
        total_wt -= pool_wt[pick];
        if (total_wt < 0.0f) total_wt = 0.0f;

        LootItem *it = &ctx->items[pool_idx[pick]];

        // Per-item chance: base × rarity scaling
        float chance = params->item_chance_base;

        // Rarity: lower nominal = rarer in cargo too
        int nom = it->nominal > 0 ? it->nominal : 3;
        float rarity = (float)nom / 20.0f;
        if (rarity > 1.0f) rarity = 1.0f;
        if (rarity < 0.005f) rarity = 0.005f;
        chance *= rarity;

        // Collectable extra penalty — finding one in cargo should feel special
        if (is_collectable_item(it)) chance *= 0.08f;

        // Higher tier items in lower tier containers: extra penalty
        if (it->assigned_tier > ctier) chance *= 0.6f;

        // Clamp
        if (chance > 0.12f) chance = 0.12f;
        if (chance < 0.01f) chance = 0.01f;

        sel_idx[sel_n]    = pool_idx[pick];
        sel_chance[sel_n] = chance;
        sel_n++;
    }

    if (sel_n == 0) return 0;

    // 3. Render <cargo>...</cargo> — hardcore: very low durability
    int pos = 0;
    float tscale = tier_chance_scale(ctier);
    float outer = params->outer_chance * tscale;
    if (outer > params->outer_chance) outer = params->outer_chance;
    if (outer < 0.15f) outer = 0.15f;

    pos += snprintf(buf + pos, buf_len - pos,
        "    <cargo chance=\"%.2f\">\n", outer);

    for (int s = 0; s < sel_n && (size_t)pos < buf_len - 128; s++) {
        LootItem *it = &ctx->items[sel_idx[s]];
        // Hardcore durability: mostly beaten up
        // Range 0.02–0.45 with slight tier influence
        float hmin = 0.02f + (float)it->assigned_tier * 0.01f;
        float hmax = 0.30f + (float)it->assigned_tier * 0.015f;
        if (hmin > 0.20f) hmin = 0.20f;
        if (hmax > 0.50f) hmax = 0.50f;

        pos += snprintf(buf + pos, buf_len - pos,
            "        <item name=\"%s\" chance=\"%.2f\" health=\"%.2f,%.2f\" />\n",
            it->classname, sel_chance[s], hmin, hmax);
    }

    pos += snprintf(buf + pos, buf_len - pos, "    </cargo>\n");
    return pos;
}

// ── Equipment attachment block renderer ──────────────────────────────────────
// If web lookup found equipment attachments for this item, renders
// <attachments> blocks grouped by slot type.

static int render_equipment_attachments(char *buf, size_t buf_len,
                                        int start_pos,
                                        const LootItem *item,
                                        FoundAttach *found, int found_count) {
    if (found_count == 0) return start_pos;

    float tscale = tier_chance_scale(item->assigned_tier);
    int pos = start_pos;

    // Group by slot and render one <attachments> block per slot
    for (int slot = 0; slot < ALL_SLOT_COUNT && (size_t)pos < buf_len - 128; slot++) {
        const FoundAttach *picks[MAX_PER_SLOT];
        int pick_count = 0;
        float weight_sum = 0.0f;

        for (int f = 0; f < found_count && pick_count < MAX_PER_SLOT; f++) {
            if (found[f].slot == slot) {
                picks[pick_count] = &found[f];
                weight_sum += (float)found[f].nominal;
                pick_count++;
            }
        }
        if (pick_count == 0) continue;

        // Equipment attachment base chances (lower than weapons — hardcore)
        float slot_chance = 0.06f * tscale;
        if (slot == EQUIP_NVG)          slot_chance = 0.02f * tscale;
        if (slot == EQUIP_ARMOR_PLATE)  slot_chance = 0.04f * tscale;
        if (slot == EQUIP_POUCH)        slot_chance = 0.08f * tscale;
        if (slot_chance < 0.01f) slot_chance = 0.01f;

        pos += snprintf(buf + pos, buf_len - pos,
            "    <attachments chance=\"%.2f\">\n", slot_chance);

        for (int s = 0; s < pick_count && (size_t)pos < buf_len - 128; s++) {
            float w = (weight_sum > 0.0f)
                ? (float)picks[s]->nominal / weight_sum
                : 1.0f / (float)pick_count;
            if (w < 0.02f) w = 0.02f;

            pos += snprintf(buf + pos, buf_len - pos,
                "        <item name=\"%s\" chance=\"%.2f\" health=\"0.05,0.40\" />\n",
                picks[s]->classname, w);
        }

        pos += snprintf(buf + pos, buf_len - pos, "    </attachments>\n");
    }

    return pos;
}

static int generate_spawnable_block(char *buf, size_t buf_len,
                                    const LootItem *item, SpawnCategory scat,
                                    AuditorContext *ctx) {
    // ── Weapon categories → dynamic attachment system ────────────────────────
    switch (scat) {
    case SPAWN_CAT_ASSAULT_RIFLE:
    case SPAWN_CAT_SNIPER_RIFLE:
    case SPAWN_CAT_SMG:
    case SPAWN_CAT_SHOTGUN:
    case SPAWN_CAT_PISTOL:
    case SPAWN_CAT_LAUNCHER:
        return generate_weapon_spawnable(buf, buf_len, item, scat, ctx);
    default:
        break;
    }

    // ── Melee with sheaths: minimal entry ────────────────────────────────────
    if (scat == SPAWN_CAT_MELEE_SHEATHED) {
        int n = snprintf(buf, buf_len,
            "<type name=\"%s\">\n</type>", item->classname);
        return (n > 0 && (size_t)n < buf_len) ? n : 0;
    }

    // ── Phoenix specific-only items (e.g. gas masks → filter only) ───────────
    // If Phoenix config has a specific_only rule for this item, generate a
    // <cargo> block with ONLY the allowed items, no tier pool.
    if (s_phoenix && s_phoenix->loaded) {
        const PhoenixSpecificItem *spec = phoenix_find_specific(s_phoenix, item->classname);
        if (spec && spec->cargo_item_count > 0) {
            int pos = 0;
            pos += snprintf(buf + pos, buf_len - pos, "<type name=\"%s\">\n", item->classname);
            int cargo_written = generate_specific_cargo_block(buf + pos, buf_len - pos, spec);
            if (cargo_written > 0) pos += cargo_written;
            pos += snprintf(buf + pos, buf_len - pos, "</type>");
            return (pos > 0 && (size_t)pos < buf_len) ? pos : 0;
        }
    }

    // ── Storage items → tier-based cargo + web-discovered attachments ────────
    int pos = 0;
    int header_end;

    // Write <type name="..."> header
    header_end = snprintf(buf, buf_len, "<type name=\"%s\">\n", item->classname);
    if (header_end <= 0 || (size_t)header_end >= buf_len) return 0;
    pos = header_end;

    // Generate tier-based cargo content
    int cargo_written = generate_tier_cargo_content(buf + pos, buf_len - pos,
                                                     item, scat, ctx);
    if (cargo_written > 0) pos += cargo_written;

    // Discover and render equipment attachments via web lookup
    FoundAttach equip_att[MAX_FOUND_ATTACH];
    int equip_count = find_equipment_attachments(ctx, item->classname,
                                                  equip_att, MAX_FOUND_ATTACH);
    if (equip_count > 0) {
        pos = render_equipment_attachments(buf, buf_len, pos, item,
                                           equip_att, equip_count);
    }

    // If nothing was generated (no cargo, no attachments), don't create block
    if (pos == header_end) return 0;

    // Close </type>
    pos += snprintf(buf + pos, buf_len - pos, "</type>");
    return (pos > 0 && (size_t)pos < buf_len) ? pos : 0;
}

// ============================================================================
// WEB LOOKUP: Lazy initialization from config/workshop_ids.ini
// ============================================================================
// Called once at the start of gap_fill_spawnable_types().  If workshop_ids.ini
// exists and has entries, initializes WinINet, fetches configured Workshop
// pages (or reads cache), and stores the state in ctx->web for the attachment
// pipeline to query.

void web_lookup_ensure_ready(AuditorContext *ctx) {
    if (!ctx || ctx->web) return;  // Already initialized or NULL ctx

    char workshop_path[MAX_PATH_LEN];
    snprintf(workshop_path, sizeof(workshop_path),
             "%s/workshop_ids.ini", ctx->config_dir);

    // Quick check: does the config file exist?
    if (!util_file_exists(workshop_path)) {
        util_log(SEVERITY_INFO,
                 "WebLookup: No workshop_ids.ini — skipping web lookup");
        return;
    }

    WebLookupState *web = (WebLookupState *)malloc(sizeof(WebLookupState));
    if (!web) return;

    char cache_dir[MAX_PATH_LEN];
    snprintf(cache_dir, sizeof(cache_dir), "%s/web_cache", ctx->config_dir);

    if (!web_lookup_init(web, cache_dir)) {
        free(web);
        return;
    }

    // Load any existing cache entries first
    web_load_cache(web);

    // Fetch all configured mods (skips already-cached & fresh entries)
    web_fetch_all_configured(web, workshop_path);

    ctx->web = (struct WebLookupState *)web;
}

// ============================================================================
// PUBLIC API: Generate spawnable entries for items missing from cfgspawnabletypes
// ============================================================================

void gap_fill_spawnable_types(AuditorContext *ctx) {
    if (!ctx) return;

    // Load Phoenix capacity config (no-op if already loaded or not present)
    if (!ctx->phoenix.loaded) {
        phoenix_load_capacity(ctx);
    }
    s_phoenix = &ctx->phoenix;

    // Initialize web lookup (no-op if already done or no config)
    web_lookup_ensure_ready(ctx);

    // Set static web pointer for storage_size_for_item() and cargo affinity
    s_web = (const WebLookupState *)ctx->web;

    util_log(SEVERITY_INFO, "Spawnable Generator: Scanning %d items for missing cfgspawnabletypes entries...", ctx->item_count);

    int generated = 0;
    int skipped_exists = 0;
    int skipped_none = 0;
    int skipped_full = 0;
    int by_cat[20] = {0};

    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (loot_is_zombie(item) || loot_is_animal(item)) continue;

        // Already has a spawnable entry
        if (spawnable_exists(ctx, item->classname)) {
            skipped_exists++;
            continue;
        }

        SpawnCategory scat = classify_for_spawnable(item);
        if (scat == SPAWN_CAT_NONE) {
            skipped_none++;
            continue;
        }

        if (ctx->spawn_block_count >= MAX_SPAWNABLE_BLOCKS) {
            skipped_full++;
            continue;
        }

        SpawnableBlock *block = &ctx->spawn_blocks[ctx->spawn_block_count];
        memset(block, 0, sizeof(SpawnableBlock));

        int written = generate_spawnable_block(block->raw_xml, MAX_SPAWNABLE_BLOCK_LEN, item, scat, ctx);
        if (written > 0) {
            strncpy(block->classname, item->classname, MAX_CLASSNAME_LEN - 1);
            strncpy(block->source_file, "generated", 63);
            ctx->spawn_block_count++;
            generated++;
            if ((int)scat < 20) by_cat[(int)scat]++;
        }
    }

    const char *cat_names[] = {
        "none", "assault_rifle", "sniper_rifle", "smg", "shotgun", "pistol", "launcher",
        "storage_lg", "storage_md", "storage_sm", "melee"
    };

    util_log(SEVERITY_INFO, "Spawnable Generator: Generated %d new entries. (Already existed: %d, No storage: %d, Buffer full: %d)",
             generated, skipped_exists, skipped_none, skipped_full);

    for (int c = 1; c < 11; c++) {
        if (by_cat[c] > 0)
            util_log(SEVERITY_INFO, "  %s: %d", cat_names[c], by_cat[c]);
    }
}

void gap_fill_missing_data(AuditorContext *ctx) {
    util_log(SEVERITY_INFO, "Gap Filler: Checking %d spawnables against types...", ctx->spawn_block_count);
    int created = 0;
    
    for (int i = 0; i < ctx->spawn_block_count; i++) {
        if (!item_exists(ctx, ctx->spawn_blocks[i].classname)) {
            if (ctx->item_count >= MAX_ITEMS) break;
            LootItem *item = &ctx->items[ctx->item_count++];
            memset(item, 0, sizeof(LootItem));
            
            strncpy(item->classname, ctx->spawn_blocks[i].classname, MAX_NAME_LEN-1);
            item->file_type = FILE_TYPE_ECONOMY;  // Tag as economy so dedup/audit see it
            item->nominal = 10;
            item->lifetime = 3600;
            item->min = 5;
            item->cost = 100;
            item->assigned_tier = 1;
            strncpy(item->category, "generated", MAX_CATEGORY_LEN-1);
            created++;
        }
    }
    util_log(SEVERITY_INFO, "Gap Filler: Created %d missing items.", created);
}

void run_ai_balancer_script(AuditorContext *ctx) {
    util_log(SEVERITY_INFO, "Exporting CSV for AI...");
    writer_export_csv(ctx, "output/items.csv");
    util_log(SEVERITY_INFO, "Launching economy_balancer.py...");
    system("python economy_balancer.py");
}

void run_phoenix_scanner(AuditorContext *ctx) {
    util_log(SEVERITY_INFO, "Phoenix: Exporting CSV for item scanner...");
    writer_export_csv(ctx, "output/items.csv");
    util_log(SEVERITY_INFO, "Phoenix: Launching phoenix_item_scanner.py (model: qwen2.5)...");
    int result = system("python phoenix_item_scanner.py");
    if (result == 0) {
        util_log(SEVERITY_INFO, "Phoenix: Scanner completed successfully. Reloading capacity config...");
        ctx->phoenix.loaded = false;
        phoenix_load_capacity(ctx);
        s_phoenix = &ctx->phoenix;
    } else {
        util_log(SEVERITY_WARNING, "Phoenix: Scanner exited with code %d. Capacity config unchanged.", result);
    }
}

bool parser_import_balanced_json(AuditorContext *ctx, const char *filepath) {
    // Basic stub for reading back JSON (would use a JSON parser in real impl)
    // For now, let's assume successful if file exists
    FILE *f = fopen(filepath, "r");
    if (!f) return false;
    fclose(f);
    return true; 
}
