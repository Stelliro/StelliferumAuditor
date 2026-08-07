
#include "auditor.h"
#include "loot_policy.h"
#include "loot_manager.h"
#include "web_lookup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static bool is_vanilla_source(const LootItem *item) {
    if (!item) return false;
    // Use mod_name (populated from filepath @ModName) if available
    if (item->mod_name[0] != '\0') {
        return (util_strcasecmp(item->mod_name, "vanilla") == 0 ||
                util_strcasecmp(item->mod_name, "server_root") == 0);
    }
    // Fallback: if mod_name not set, infer from mod_source filename
    return util_strcasecmp(item->mod_source, "types.xml") == 0;
}

// ============================================================================
// DEBUG / TEST / BROKEN ITEM DETECTION
// ============================================================================
static bool is_debug_classname(const char *classname) {
    if (!classname || !classname[0]) return false;
    // Patterns that indicate debug, test, broken, or placeholder items
    static const char *patterns[] = {
        "debug", "test_", "_test", "admin_", "dev_", "placeholder",
        "broken_", "_broken", "dummy", "template", "example",
        "deleteme", "remove_me", "todo_", "wip_", "prototype",
        "bp_test", "bp_debug", NULL
    };
    char lower[MAX_CLASSNAME_LEN];
    size_t len = strlen(classname);
    if (len >= MAX_CLASSNAME_LEN) len = MAX_CLASSNAME_LEN - 1;
    for (size_t i = 0; i < len; i++) lower[i] = (char)tolower((unsigned char)classname[i]);
    lower[len] = '\0';

    for (int i = 0; patterns[i]; i++) {
        if (strstr(lower, patterns[i])) return true;
    }
    return false;
}

// Compare two items to see if they represent the same real item
// or genuinely different items that happen to share a classname.
// Returns true if they look like the SAME item (safe to merge/dedup).
static bool items_are_same_thing(const LootItem *a, const LootItem *b) {
    if (!a || !b) return false;
    // If categories differ, they're likely different items
    if (a->category[0] && b->category[0] && util_strcasecmp(a->category, b->category) != 0)
        return false;
    // If nominal/lifetime differ dramatically, likely different items
    int nom_diff = abs(a->nominal - b->nominal);
    int life_a = a->lifetime > 0 ? a->lifetime : 3600;
    int life_b = b->lifetime > 0 ? b->lifetime : 3600;
    float life_ratio = (float)(life_a > life_b ? life_a : life_b) /
                       (float)(life_a < life_b ? life_a : life_b);
    // Same item: nominal within 50% and lifetime within 3x
    int nom_max = (a->nominal > b->nominal) ? a->nominal : b->nominal;
    if (nom_max == 0) nom_max = 1;
    float nom_pct = (float)nom_diff / (float)nom_max;
    if (nom_pct > 0.5f && nom_diff > 3) return false;  // nominal differs by >50%
    if (life_ratio > 3.0f) return false;  // lifetime differs by >3x
    return true;
}

static void add_unique(char arr[][MAX_USAGE_LEN], int *count, const char *value, int max) {
    if (!value || !*value) return;
    for (int i = 0; i < *count; i++) {
        if (util_strcasecmp(arr[i], value) == 0) return;
    }
    if (*count < max) {
        strncpy(arr[(*count)++], value, MAX_USAGE_LEN - 1);
    }
}

static void add_unique_value(char arr[][MAX_VALUE_LEN], int *count, const char *value, int max) {
    if (!value || !*value) return;
    for (int i = 0; i < *count; i++) {
        if (util_strcasecmp(arr[i], value) == 0) return;
    }
    if (*count < max) {
        strncpy(arr[(*count)++], value, MAX_VALUE_LEN - 1);
    }
}

static void merge_item_data(LootItem *dst, LootItem *src) {
    if (!dst || !src) return;

    if (dst->nominal < src->nominal) dst->nominal = src->nominal;
    if (dst->min < src->min) dst->min = src->min;
    if (dst->lifetime < src->lifetime) dst->lifetime = src->lifetime;
    if (dst->restock < src->restock) dst->restock = src->restock;
    if (dst->cost < src->cost) dst->cost = src->cost;

    if (dst->category[0] == '\0' && src->category[0] != '\0') {
        strncpy(dst->category, src->category, MAX_CATEGORY_LEN - 1);
    }

    dst->flags |= src->flags;

    for (int i = 0; i < src->usage_count; i++) {
        add_unique(dst->usages, &dst->usage_count, src->usages[i], MAX_USAGES_PER_TIER);
    }
    for (int i = 0; i < src->value_count; i++) {
        add_unique_value(dst->values, &dst->value_count, src->values[i], MAX_VALUES_PER_ITEM);
    }

    if (is_vanilla_source(dst) && !is_vanilla_source(src)) {
        strncpy(dst->mod_source, src->mod_source, sizeof(dst->mod_source) - 1);
        strncpy(dst->mod_name, src->mod_name, sizeof(dst->mod_name) - 1);
    }

    dst->modified = true;
}

static int heuristic_tier_assignment(AuditorContext *ctx, LootItem *item) {
    int mod_tier = loot_get_mod_tier(item);
    if (mod_tier > 0) {
        // Stat-based promotion: if web lookup found item stats that suggest a
        // higher tier, promote.  This catches cases where classname-based rules
        // are too conservative (e.g., MMG items that look generic but have
        // 2.5x protection or 150-slot capacity).
        if (ctx->web) {
            ItemStats stats = {0};
            if (web_get_item_stats(ctx->web, item->classname, &stats)) {
                int stat_tier = web_stat_tier_promotion(&stats);
                if (stat_tier > mod_tier) {
                    util_log(SEVERITY_INFO,
                             "Tier promotion: %s T%d → T%d (prot=%.1fx, slots=%d%s)",
                             item->classname, mod_tier, stat_tier,
                             stats.protection_mult, stats.slot_count,
                             stats.has_weapon_slot ? "+wpn" : "");
                    return stat_tier;
                }
            }
        }
        return mod_tier;
    }

    // No mod-specific tier — try stat-based assignment from web lookup
    if (ctx->web) {
        ItemStats stats = {0};
        if (web_get_item_stats(ctx->web, item->classname, &stats)) {
            int stat_tier = web_stat_tier_promotion(&stats);
            if (stat_tier > 0) return stat_tier;
        }
    }

    // Nominal-based fallback
    if (item->nominal <= 3) return 4;
    if (item->nominal <= 10) return 3;
    if (item->nominal <= 30) return 2;
    return 1;
}

static bool item_is_boots(LootItem *item) {
    if (!item || !item->classname[0]) return false;
    if (util_str_contains_ci(item->classname, "boot")) return true;
    if (util_str_contains_ci(item->classname, "shoe")) return true;
    return false;
}

void auditor_assign_tier(AuditorContext *ctx, LootItem *item) {
    // Blacklisted classnames go straight to the contraband tier (no spawn, no trade).
    if (lp_is_blacklisted(item->classname)) {
        item->assigned_tier = lp_contraband_tier();
        return;
    }
    int t = heuristic_tier_assignment(ctx, item);
    int n = lp_tier_count();
    if (t < 1) t = 1;
    if (t > n) t = n;
    item->assigned_tier = t;
}

void auditor_validate_item(AuditorContext *ctx, int index) {
    LootItem *item = &ctx->items[index];
    // Validation only applies to Economy Items
    if (item->file_type != FILE_TYPE_ECONOMY) return;
    
    if (item->nominal < item->min) {
        ctx->issues[ctx->issue_count].severity = SEVERITY_WARNING;
        snprintf(ctx->issues[ctx->issue_count++].message, 255, 
            "[%s] Nominal (%d) < Min (%d)", item->classname, item->nominal, item->min);
    }
}

static int compare_items(const void *a, const void *b) {
    LootItem *itemA = (LootItem *)a;
    LootItem *itemB = (LootItem *)b;
    return util_strcasecmp(itemA->classname, itemB->classname);
}

void auditor_sort_items(AuditorContext *ctx) {
    util_log(SEVERITY_INFO, "Sorting %d items...", ctx->item_count);
    qsort(ctx->items, ctx->item_count, sizeof(LootItem), compare_items);
    ctx->selected_item = -1;
}

void auditor_resolve_duplicates(AuditorContext *ctx) {
    util_log(SEVERITY_INFO, "Smart Conflict Resolution (mod-aware)...");
    int merged = 0, renamed = 0;
    
    for (int i = 0; i < ctx->item_count; i++) {
        if (ctx->items[i].deleted) continue;
        if (ctx->items[i].file_type != FILE_TYPE_ECONOMY) continue;
        
        for (int j = i + 1; j < ctx->item_count; j++) {
            if (ctx->items[j].deleted) continue;
            if (ctx->items[j].file_type != FILE_TYPE_ECONOMY) continue;
            
            if (util_strcasecmp(ctx->items[i].classname, ctx->items[j].classname) != 0) break; // sorted: no more matches

            LootItem *a = &ctx->items[i];
            LootItem *b = &ctx->items[j];

            bool a_is_vanilla = is_vanilla_source(a);
            bool b_is_vanilla = is_vanilla_source(b);
            bool same_mod = (a->mod_name[0] && b->mod_name[0] &&
                             util_strcasecmp(a->mod_name, b->mod_name) == 0);

            // CASE 1: Same mod or vanilla vs mod — classic merge
            if (same_mod || a_is_vanilla || b_is_vanilla) {
                LootItem *keep = a;
                LootItem *drop = b;
                if (a_is_vanilla && !b_is_vanilla) {
                    keep = b; drop = a;
                } else if (!a_is_vanilla && b_is_vanilla) {
                    keep = a; drop = b;
                } else {
                    int a_score = a->nominal + (a->lifetime / 3600);
                    int b_score = b->nominal + (b->lifetime / 3600);
                    if (b_score > a_score) { keep = b; drop = a; }
                }
                merge_item_data(keep, drop);
                drop->deleted = true;
                merged++;
                continue;
            }

            // CASE 2: Different mods, same classname
            // Check if they represent the same real item or different items
            if (items_are_same_thing(a, b)) {
                // Same item redefined by two mods — keep the one with better data
                int a_score = a->nominal + (a->lifetime / 3600) + a->usage_count;
                int b_score = b->nominal + (b->lifetime / 3600) + b->usage_count;
                LootItem *keep = (a_score >= b_score) ? a : b;
                LootItem *drop = (keep == a) ? b : a;
                merge_item_data(keep, drop);
                drop->deleted = true;
                merged++;
                util_log(SEVERITY_INFO, "  Dedup [%s]: same item from %s and %s — kept %s",
                         a->classname, a->mod_name, b->mod_name, keep->mod_name);
            } else {
                // Different items sharing classname — rename the second one
                // Prefix with a short mod tag derived from mod_name
                // Guard: skip items already renamed to prevent double-prefixing
                if (b->renamed) continue;
                char prefix[32] = "";
                if (b->mod_name[0] == '@') {
                    // Extract short prefix: "@SNAFU Weapons" -> "SNAFU_"
                    // Stop at space, underscore, apostrophe, or other non-alnum chars
                    const char *start = b->mod_name + 1;  // skip @
                    int k = 0;
                    while (start[k] && start[k] != ' ' && start[k] != '_' &&
                           start[k] != '\'' && k < 20) {
                        prefix[k] = start[k]; k++;
                    }
                    prefix[k] = '_'; prefix[k + 1] = '\0';
                }
                if (prefix[0]) {
                    char new_name[MAX_CLASSNAME_LEN];
                    snprintf(new_name, sizeof(new_name), "%s%s", prefix, b->classname);
                    util_log(SEVERITY_WARNING, "  Rename [%s] -> [%s] (collision: %s vs %s)",
                             b->classname, new_name, a->mod_name, b->mod_name);
                    strncpy(b->classname, new_name, MAX_CLASSNAME_LEN - 1);
                    b->modified = true;
                    b->renamed = true;
                    renamed++;
                } else {
                    // Fallback: can't derive prefix, merge normally
                    merge_item_data(a, b);
                    b->deleted = true;
                    merged++;
                }
            }
        }
    }
    
    snprintf(ctx->status_message, 255, "Resolved %d duplicates, renamed %d collisions.", merged, renamed);
    util_log(SEVERITY_INFO, "Conflict Resolution: %d merged, %d renamed.", merged, renamed);
}

// ============================================================================
// DEBUG / TEST ITEM FILTER
// ============================================================================
void auditor_filter_debug_items(AuditorContext *ctx) {
    if (!ctx) return;
    int filtered = 0;
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        // Never filter items that were already renamed by the dedup pass.
        // A mod like @Test_Mod would produce prefix "Test_" which matches
        // the "test_" debug pattern, causing a false-positive deletion.
        if (item->renamed) continue;
        if (is_debug_classname(item->classname)) {
            item->is_debug_item = true;
            item->deleted = true;
            filtered++;
            util_log(SEVERITY_INFO, "  Filtered debug/test item: %s (from %s)", item->classname, item->mod_name);
        }
    }
    if (filtered > 0)
        util_log(SEVERITY_INFO, "Debug item filter: removed %d debug/test/broken items.", filtered);
}

// ============================================================================
// RENAME COLLISIONS — Standalone pass (can be called separately from dedup)
// ============================================================================
void auditor_rename_collisions(AuditorContext *ctx) {
    if (!ctx) return;
    // Already handled inside auditor_resolve_duplicates, 
    // but this provides a standalone entry point
    auditor_sort_items(ctx);
    int renamed = 0;
    for (int i = 0; i < ctx->item_count - 1; i++) {
        if (ctx->items[i].deleted) continue;
        if (ctx->items[i].file_type != FILE_TYPE_ECONOMY) continue;
        for (int j = i + 1; j < ctx->item_count; j++) {
            if (ctx->items[j].deleted) continue;
            if (ctx->items[j].file_type != FILE_TYPE_ECONOMY) continue;
            if (util_strcasecmp(ctx->items[i].classname, ctx->items[j].classname) != 0) break; // sorted
            // Still a collision after dedup — rename
            LootItem *b = &ctx->items[j];
            if (b->mod_name[0] == '@' && !b->renamed) {
                char prefix[32] = "";
                const char *start = b->mod_name + 1;
                int k = 0;
                while (start[k] && start[k] != ' ' && start[k] != '_' &&
                       start[k] != '\'' && k < 20) {
                    prefix[k] = start[k]; k++;
                }
                prefix[k] = '_'; prefix[k + 1] = '\0';
                char new_name[MAX_CLASSNAME_LEN];
                snprintf(new_name, sizeof(new_name), "%s%s", prefix, b->classname);
                strncpy(b->classname, new_name, MAX_CLASSNAME_LEN - 1);
                b->modified = true;
                b->renamed = true;
                renamed++;
            }
        }
    }
    if (renamed > 0)
        util_log(SEVERITY_INFO, "Rename pass: %d remaining collisions renamed.", renamed);
}

/**
 * Map auditor's internal 12-tier policy to DayZ CE-compatible value tags (Tier1-4).
 * DayZ's Central Economy only recognizes Tier1-Tier4 + Unique as valueflags
 * (defined in cfglimitsdefinition.xml). Writing Tier5+ causes CE errors:
 *   "!!! [CE] :: Unknown value: 'Tier5'."
 *
 * Default progressive fantasy map (see config/loot_policy.ini):
 *   T1  Coast Scav      → low CE band (coast)
 *   T2  Town Survivor   → coast/inland transition
 *   T3  Constable       → towns
 *   T4  Outdoorsman     → towns/hunting overlap
 *   T5  Insurgent       → military checkpoints
 *   T6  Infantry        → major military bases
 *   T7  Spec Ops        → NWAF/Tisy
 *   T8  Operator        → heli / endgame
 *   T9  Elite           → ultra rare military
 *   T10 Mythic          → near impossible
 *   T11 Black Market    → (none — NOSPAWN, trader-only)
 *   T12 Contraband      → (none — NOSPAWN/NOTRADE)
 *
 * Actual CE tags are computed from spawning-tier rank (lp_tier_spawns), not
 * hard-coded indices. Usage flags further filter building types.
 */
static void auditor_assign_ce_values(LootItem *item, int tier) {
    if (!item || tier <= 0) return;
    item->value_count = 0;

    /* Heirloom/collectable items use 'Unique' — they're not geographic tier items */
    if (loot_is_heirloom(item)) {
        strncpy(item->values[item->value_count++], "Unique", MAX_VALUE_LEN - 1);
        item->modified = true;
        return;
    }

    /* Bitcoin: rare currency loot spread across mid/high zones (Tier2-4) so it
     * reads as high-tier loot rather than being pinned to a single band. */
    if (util_str_contains_ci(item->classname, "Bitcoin")) {
        strncpy(item->values[item->value_count++], "Tier2", MAX_VALUE_LEN - 1);
        strncpy(item->values[item->value_count++], "Tier3", MAX_VALUE_LEN - 1);
        strncpy(item->values[item->value_count++], "Tier4", MAX_VALUE_LEN - 1);
        item->modified = true;
        return;
    }

    /* No-spawn tiers (Black Market, Contraband, or any user-defined no-spawn
     * tier): emit no CE value tags. nominal 0 means they never CE-spawn anyway. */
    if (!lp_tier_spawns(tier)) {
        item->modified = true;
        return;
    }

    /* Compile this spawning tier's rank (among all spawning tiers) down to a CE
     * value tag. DayZ's CE only accepts Tier1-Tier4 + Unique, so N internal tiers
     * map proportionally: low tiers -> Tier1 (coast), high -> Tier4 (deep
     * military). Usage flags still filter by building type on top of this. */
    int n = lp_tier_count();
    int spawn_total = 0, rank = 0;
    for (int t = 1; t <= n; t++) {
        if (lp_tier_spawns(t)) {
            spawn_total++;
            if (t == tier) rank = spawn_total;
        }
    }
    if (spawn_total <= 0) { item->modified = true; return; }
    if (rank <= 0) rank = spawn_total;

    /* Spread across a small CE band so items appear across adjacent zones (like
     * vanilla) instead of a single hard tier. A continuous "center" runs 1..4
     * across the spawning range; lo/hi bracket it — one tag at the extremes, two
     * in the middle. With 10 spawning tiers this yields:
     *   T1->{Tier1}   T2,T3->{Tier1,Tier2}   T4->{Tier2}   T5,T6->{Tier2,Tier3}
     *   T7->{Tier3}   T8,T9->{Tier3,Tier4}   T10->{Tier4}
     * (center = 1 + 3*(rank-1)/(spawn_total-1); lo=floor, hi=ceil). */
    int den = spawn_total - 1;
    int lo_ce, hi_ce;
    if (den <= 0) {
        lo_ce = hi_ce = 1;                 /* single spawning tier -> everything Tier1 */
    } else {
        int num = 3 * (rank - 1);
        lo_ce = 1 + num / den;             /* floor(center) */
        hi_ce = 1 + (num + den - 1) / den; /* ceil(center)  */
    }
    if (lo_ce < 1) lo_ce = 1; if (lo_ce > 4) lo_ce = 4;
    if (hi_ce < 1) hi_ce = 1; if (hi_ce > 4) hi_ce = 4;
    if (hi_ce < lo_ce) hi_ce = lo_ce;

    for (int c = lo_ce; c <= hi_ce && item->value_count < MAX_VALUES_PER_ITEM; c++) {
        char cebuf[16];
        snprintf(cebuf, sizeof(cebuf), "Tier%d", c);
        strncpy(item->values[item->value_count++], cebuf, MAX_VALUE_LEN - 1);
    }
    item->modified = true;
}

/**
 * Assign CE usage flags based on item category, classname, and tier zone.
 * Creates diverse loot pools so items aren't all crammed into Military or Town.
 *
 * Military zone (T5+): primary Military, secondary by category/classname.
 * Civilian zone (T1-4): spread across Town/Village/Farm/Hunting/Industrial/etc.
 * Classname heuristics add Medic, Hunting, Police, Firefighter where appropriate.
 */
static void auditor_assign_usages_by_category(LootItem *item) {
    if (!item) return;
    item->usage_count = 0;

    Zone zone = util_tier_to_zone(item->assigned_tier);
    const char *cat = item->category;
    bool has_cat    = (cat[0] != '\0');
    bool is_weapon  = has_cat && strstr(cat, "weapons");
    bool is_clothes = has_cat && strstr(cat, "clothes");
    bool is_tools   = has_cat && strstr(cat, "tools");
    bool is_food    = has_cat && strstr(cat, "food");
    bool is_cont    = has_cat && strstr(cat, "containers");
    bool is_explo   = has_cat && strstr(cat, "explosives");
    bool is_vehicle = has_cat && strstr(cat, "vehicle");

    /* Classname-based refinements */
    bool cn_medical = util_str_contains_ci(item->classname, "bandage") ||
                      util_str_contains_ci(item->classname, "saline")  ||
                      util_str_contains_ci(item->classname, "morphine") ||
                      util_str_contains_ci(item->classname, "epinephrine") ||
                      util_str_contains_ci(item->classname, "tetracycline") ||
                      util_str_contains_ci(item->classname, "syringe") ||
                      util_str_contains_ci(item->classname, "bloodbag") ||
                      util_str_contains_ci(item->classname, "firstaid") ||
                      util_str_contains_ci(item->classname, "medkit");
    bool cn_optic   = util_str_contains_ci(item->classname, "optic")  ||
                      util_str_contains_ci(item->classname, "scope");
    bool cn_vest    = util_str_contains_ci(item->classname, "vest")   ||
                      util_str_contains_ci(item->classname, "plate");
    bool cn_fire    = util_str_contains_ci(item->classname, "firefighter") ||
                      util_str_contains_ci(item->classname, "fireman");

#define MAX_CE_USAGES 5  /* vanilla max: 5 (Ammo_12gaPellets) */
#define ADD_USAGE(u) do { \
    if (item->usage_count < MAX_CE_USAGES) \
        strncpy(item->usages[item->usage_count++], (u), MAX_USAGE_LEN - 1); \
} while(0)

    if (zone == ZONE_MILITARY || zone == ZONE_ENDGAME) {
        /* ---- Military / Endgame zone (T5-T10) ---- */
        ADD_USAGE("Military");

        if (is_weapon) {
            if (item->assigned_tier <= 6)
                ADD_USAGE("Hunting");          /* lower-mil weapons leak to hunting lodges */
        } else if (is_clothes) {
            ADD_USAGE("Police");               /* tactical clothing in police stations too */
        } else if (is_tools || is_vehicle) {
            ADD_USAGE("Industrial");           /* military gadgets / vehicle parts → garages */
        } else if (is_food) {
            ADD_USAGE("Town");                 /* MREs sometimes in residential */
        } else if (is_cont) {
            if (item->assigned_tier <= 6)
                ADD_USAGE("Police");           /* mid-mil packs also in police */
        }

        if (cn_optic)   ADD_USAGE("Hunting"); /* scopes can spawn at hunting lodges */
        if (cn_vest)    ADD_USAGE("Police");   /* plate carriers in police stations */
        if (cn_fire)    ADD_USAGE("Firefighter");
        if (cn_medical) ADD_USAGE("Medic");    /* medical items in hospitals */

    } else {
        /* ---- Civilian zone (T1-T4) ---- */
        if (is_weapon) {
            ADD_USAGE("Hunting");
            ADD_USAGE("Police");
            if (item->assigned_tier <= 2)
                ADD_USAGE("Town");             /* basic shotguns / .22s in houses */
        } else if (is_clothes) {
            ADD_USAGE("Town");
            ADD_USAGE("Village");
            if (item->assigned_tier <= 2)
                ADD_USAGE("Farm");             /* work clothes on farms */
        } else if (is_tools) {
            ADD_USAGE("Industrial");
            ADD_USAGE("Farm");
            ADD_USAGE("Town");
        } else if (is_food) {
            ADD_USAGE("Town");
            ADD_USAGE("Village");
            ADD_USAGE("Farm");
            if (item->assigned_tier <= 2)
                ADD_USAGE("Coast");            /* beach loot */
        } else if (is_cont) {
            ADD_USAGE("Town");
            ADD_USAGE("Village");
            ADD_USAGE("Office");               /* bags in school/office desks */
        } else if (is_explo) {
            ADD_USAGE("Hunting");              /* flares at hunting stands */
            ADD_USAGE("Industrial");
        } else if (is_vehicle) {
            ADD_USAGE("Industrial");
            ADD_USAGE("Town");
        } else {
            /* Unknown/missing category — default civilian spread */
            ADD_USAGE("Town");
            ADD_USAGE("Village");
        }

        /* T1 Scavenger items also appear on the coast */
        if (item->assigned_tier == 1 && !is_weapon)
            ADD_USAGE("Coast");

        /* Classname overrides for civilian */
        if (cn_medical) ADD_USAGE("Medic");
        if (cn_fire)    ADD_USAGE("Firefighter");
        if (cn_optic)   ADD_USAGE("Hunting");
        if (cn_vest)    ADD_USAGE("Police");
    }
#undef ADD_USAGE
#undef MAX_CE_USAGES

    item->modified = true;
}

void auditor_fill_missing_data(AuditorContext *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted || item->file_type != FILE_TYPE_ECONOMY) continue;

        if (item->usage_count == 0) {
            auditor_assign_usages_by_category(item);
        }

        if (item->value_count == 0 && item->assigned_tier > 0) {
            auditor_assign_ce_values(item, item->assigned_tier);
        }

        if (item->min == 0 && item->nominal > 0) {
            item->min = (int)(item->nominal * 0.6);
            item->modified = true;
        }

        if (item->lifetime == 0) {
            item->lifetime = 7200;
            item->modified = true;
        }

        /* Soft distribution from loot_policy.ini (NOMINAL/MIN/LIFETIME/RESTOCK targets) */
        if (item->assigned_tier > 0) {
            int nom = item->nominal;
            int mn  = item->min;
            int lt  = item->lifetime;
            int rs  = item->restock;
            lp_apply_distribution(item->assigned_tier, &nom, &mn, &lt, &rs);
            if (nom != item->nominal || mn != item->min || lt != item->lifetime || rs != item->restock) {
                item->nominal  = nom;
                item->min      = mn;
                item->lifetime = lt;
                item->restock  = rs;
                item->modified = true;
            }
        }
    }
}

/**
 * Sanitize ALL items' value tags to CE-compatible Tier1-4.
 * Catches invalid Tier5-Tier11 tags that may have come from:
 *   - Original mod XML files with non-standard tier values
 *   - Previous auditor runs before this mapping was added
 *   - Economy balancer output
 * Must run AFTER all tier assignment / gap filling is complete.
 */
void auditor_sanitize_ce_values(AuditorContext *ctx) {
    if (!ctx) return;
    int fixed = 0;
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (item->value_count == 0) continue;

        bool needs_fix = false;
        for (int v = 0; v < item->value_count; v++) {
            /* Check for Tier5-Tier99 (any tier beyond vanilla 4) */
            if (strncmp(item->values[v], "Tier", 4) == 0) {
                int tier_num = atoi(item->values[v] + 4);
                if (tier_num > 4) {
                    needs_fix = true;
                    break;
                }
            }
        }

        if (needs_fix) {
            /* Re-derive from assigned_tier using the proper CE mapping */
            if (item->assigned_tier > 0) {
                auditor_assign_ce_values(item, item->assigned_tier);
            } else {
                /* Fallback: clamp invalid tier values to Tier4 */
                item->value_count = 0;
                strncpy(item->values[item->value_count++], "Tier4", MAX_VALUE_LEN - 1);
                item->modified = true;
            }
            fixed++;
        }
    }
    if (fixed > 0) {
        util_log(SEVERITY_INFO, "CE Sanitizer: Remapped %d items with invalid Tier5+ value tags to Tier1-4.", fixed);
    }
}

/**
 * Rebalance usage pools for ALL economy items.
 * Re-derives usages using the category-aware mapper for items that currently
 * have only generic assignments (single "Military" or "Town"+"Village").
 * Items with hand-tuned or modder-supplied usages (e.g. "Coast"+"Hunting")
 * are left untouched.
 * Must run AFTER tier assignment and fill_missing_data.
 */
void auditor_rebalance_usage_pools(AuditorContext *ctx) {
    if (!ctx) return;
    int rebalanced = 0;
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted || item->file_type != FILE_TYPE_ECONOMY) continue;
        if (item->usage_count == 0) continue;

        /* Detect stale generic assignment: exactly "Military" only, or "Town"+"Village" only */
        bool is_stale = false;
        if (item->usage_count == 1 &&
            strcmp(item->usages[0], "Military") == 0) {
            is_stale = true;
        }
        if (item->usage_count == 2 &&
            strcmp(item->usages[0], "Town") == 0 &&
            strcmp(item->usages[1], "Village") == 0) {
            is_stale = true;
        }

        if (is_stale) {
            auditor_assign_usages_by_category(item);
            rebalanced++;
        }
    }
    if (rebalanced > 0) {
        util_log(SEVERITY_INFO, "Usage Rebalancer: Diversified %d items from generic Military/Town+Village to category-aware pools.", rebalanced);
    }
}

// ============================================================================
// BLACK MARKET WORLD SPAWN WHITELIST
// ============================================================================
// Curated list of ~20 iconic weapons that spawn in the world as ultra-rare
// Black Market finds (policy BM tier, nominal=1, min=0). All other BM-tier
// items remain trader-only (nominal=0). Total expected world spawns: ~20.
// ============================================================================

static const char *BLACK_MARKET_SPAWN_WHITELIST[] = {
    // Vanilla endgame weapons
    "FAL",
    "SVD",
    "ASVAL",
    "Aug",
    "AugShort",
    "Deagle",
    "Deagle_Gold",
    "MP5K",
    "Magnum",
    "FNX45",
    "Scout",
    "Repeater",
    // Vanilla endgame ammo (1 box each)
    "Ammo_357",
    "AmmoBox_357_20Rnd",
    // Morty's endgame weapons
    "Mass_MassM110",
    // Melee collectables
    "Sword",
    // High-value misc
    "Crossbow_Autumn",
    "Glock19",
    "Derringer_Black",
    "Derringer_Grey",
    NULL
};

static bool is_black_market_spawn(const char *classname) {
    for (int i = 0; BLACK_MARKET_SPAWN_WHITELIST[i]; i++) {
        if (util_strcasecmp(classname, BLACK_MARKET_SPAWN_WHITELIST[i]) == 0)
            return true;
    }
    return false;
}

static void auditor_enforce_black_market_spawns(AuditorContext *ctx) {
    int bm_tier = lp_black_market_tier();
    if (bm_tier <= 0) return; /* no BM tier configured — nothing to promote */

    int promoted = 0;
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (item->assigned_tier != bm_tier) continue;
        if (item->nominal > 0) continue; // Already spawns — don't touch

        if (is_black_market_spawn(item->classname)) {
            item->nominal = 1;
            item->min = 0;
            item->lifetime = 10800;  // 3 hours
            item->restock = 3600;    // 1 hour restock
            item->modified = true;
            promoted++;
        }
    }
    if (promoted > 0) {
        util_log(SEVERITY_INFO, "Black Market: Promoted %d items to world spawn (nominal=1)", promoted);
    }
}

void auditor_run_full_audit(AuditorContext *ctx) {
    ctx->issue_count = 0;
    memset(&ctx->summary, 0, sizeof(AuditSummary));

    int boot_counts[MAX_TIERS + 1] = {0};
    int zombie_tier_counts[8] = {0};  // [0]=untiered, [1-7]=tier counts
    int animal_tier_counts[5] = {0};
    int heirloom_count = 0;
    
    for (int i = 0; i < ctx->item_count; i++) {
        if (ctx->items[i].deleted) continue;
        LootItem *item = &ctx->items[i];

        // --- Zombie Tier Processing (T1-T7 including bosses) ---
        if (loot_is_zombie(item)) {
            int ztier = loot_get_zombie_tier(item);
            loot_enforce_zombie_tier(item, ztier);
            if (ztier >= 1 && ztier <= 7) zombie_tier_counts[ztier]++;
            else zombie_tier_counts[0]++;
            continue; // Zombies skip normal item audit
        }
        
        // --- Animal Tier Processing ---
        if (loot_is_animal(item)) {
            int atier = loot_get_animal_tier(item);
            loot_enforce_animal_tier(item, atier);
            if (atier >= 1 && atier <= 4) animal_tier_counts[atier]++;
            else animal_tier_counts[0]++;
            continue; // Animals skip normal item audit
        }

        // --- Heirloom Processing (before generic collectibles) ---
        if (loot_is_heirloom(item)) {
            loot_enforce_heirloom_properties(item);
            heirloom_count++;
            continue; // Heirlooms get their own economy rules
        }

        if (loot_is_collectible(item)) {
            loot_enforce_collectible_properties(item);
        } else if (loot_is_craftable(item)) {
            loot_enforce_craftable_properties(item);
        }

        auditor_assign_tier(ctx, item);
        auditor_validate_item(ctx, i);

        if (item_is_boots(item) && item->assigned_tier >= 1 && item->assigned_tier <= MAX_TIERS) {
            boot_counts[item->assigned_tier]++;
        }
    }

    // Log zombie/animal/heirloom tier distribution
    int total_zombies = 0;
    for (int t = 1; t <= 7; t++) total_zombies += zombie_tier_counts[t];
    int total_animals = animal_tier_counts[1] + animal_tier_counts[2] + animal_tier_counts[3] + animal_tier_counts[4];
    if (total_zombies > 0) {
        util_log(SEVERITY_INFO, "Audit: %d zombies tiered — T1(Green):%d T2(Yellow):%d T3(Red):%d T4(Purple):%d T5(Cyan/MiniBoss):%d T6(White/Boss):%d T7(Gold/Raid):%d",
                 total_zombies, zombie_tier_counts[1], zombie_tier_counts[2], zombie_tier_counts[3], zombie_tier_counts[4],
                 zombie_tier_counts[5], zombie_tier_counts[6], zombie_tier_counts[7]);
    }
    if (total_animals > 0) {
        util_log(SEVERITY_INFO, "Audit: %d animals tiered — T1(Farm):%d T2(Wild):%d T3(Wolf):%d T4(Bear):%d",
                 total_animals, animal_tier_counts[1], animal_tier_counts[2], animal_tier_counts[3], animal_tier_counts[4]);
    }
    if (heirloom_count > 0) {
        util_log(SEVERITY_INFO, "Audit: %d heirloom collectables identified and enforced", heirloom_count);
    }

    /* Black Market: promote ~20 iconic weapons from nominal=0 to nominal=1 */
    auditor_enforce_black_market_spawns(ctx);

    /* AUTHORITATIVE CE ZONE-TAG PASS.
     * auditor_fill_missing_data() runs BEFORE this audit (see the pipeline in
     * auditor_run_swarm / swarm.c / integrity.c), so at that point assigned_tier
     * was still 0 and its `value_count == 0 && assigned_tier > 0` guard skipped
     * CE assignment entirely — leaving every item without source <value> tags
     * (Makarov, CZ75, Glock19, AKM, ...) with no zone tags at all. Now that
     * every item's final tier is set, (re)derive CE tags here:
     *   - Weapons ALWAYS follow their internal tier, so the 1-12 ladder drives
     *     the CE Tier1-4 spawn zone (what "spread weapons accordingly" needs).
     *   - Other items only get tags if they still lack them, preserving curated
     *     source values. */
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted || item->file_type != FILE_TYPE_ECONOMY) continue;
        if (item->assigned_tier <= 0) continue;
        if (loot_is_zombie(item) || loot_is_animal(item)) continue;
        bool is_weapon = item->category[0] && strstr(item->category, "weapons");
        if (is_weapon || item->value_count == 0) {
            auditor_assign_ce_values(item, item->assigned_tier);
        }
    }

    for (int t = 6; t <= MAX_TIERS; t++) {
        if (boot_counts[t] == 0 && ctx->issue_count < MAX_ISSUES) {
            ctx->issues[ctx->issue_count].severity = SEVERITY_WARNING;
            snprintf(ctx->issues[ctx->issue_count++].message, 255,
                "[Gap] No boots found in Tier %d+. Consider adding mid/high-tier footwear.", t);
        }
    }
    // Note: caller is responsible for setting ctx->status_message after audit completes.
    // Writing it here would overwrite pipeline progress messages in the status bar.
    util_log(SEVERITY_INFO, "Audit Complete. %d Issues.", ctx->issue_count);
}

void auditor_run_swarm(AuditorContext *ctx) {
    if (!ctx) return;
    util_log(SEVERITY_INFO, "Swarm: Filter -> Dedup -> Rename -> Gap Fill -> Fill Missing -> Sanitize CE -> Rebalance Usages -> Audit -> Manifest -> Store");
    auditor_filter_debug_items(ctx);
    auditor_resolve_duplicates(ctx);
    auditor_rename_collisions(ctx);
    gap_fill_missing_data(ctx);
    auditor_fill_missing_data(ctx);
    auditor_sanitize_ce_values(ctx);
    auditor_rebalance_usage_pools(ctx);
    auditor_run_full_audit(ctx);
    
    // Refresh mod manifest so LLM always has current state
    char local_root[256] = "downloaded_mods";
    util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
    util_generate_mod_manifest(local_root, "output/mod_manifest.md");
    
    auditor_generate_store_data(ctx);
    ctx->audit_complete = true;
    // Note: caller is responsible for setting ctx->status_message.
    util_log(SEVERITY_INFO, "Swarm Complete. %d Issues. Manifest updated.", ctx->issue_count);
}

void auditor_apply_all_fixes(AuditorContext *ctx) {
    auditor_filter_debug_items(ctx);
    auditor_resolve_duplicates(ctx);
    auditor_rename_collisions(ctx);
}

void auditor_snapshot_originals(AuditorContext *ctx) {
    if (!ctx || ctx->item_count == 0) return;
    
    // Free previous snapshot
    if (ctx->original_items) {
        free(ctx->original_items);
        ctx->original_items = NULL;
    }
    
    ctx->original_items = (LootItem*)malloc(sizeof(LootItem) * ctx->item_count);
    if (!ctx->original_items) {
        util_log(SEVERITY_ERROR, "Failed to allocate memory for item snapshot (%d items)", ctx->item_count);
        ctx->original_item_count = 0;
        return;
    }
    
    memcpy(ctx->original_items, ctx->items, sizeof(LootItem) * ctx->item_count);
    ctx->original_item_count = ctx->item_count;
    ctx->data_loaded = true;
    
    util_log(SEVERITY_INFO, "Snapshot: Saved %d original items for diff comparison", ctx->original_item_count);
}
