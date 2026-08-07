/**
 * STELLIFERUM LOOT MANAGER (Implementation)
 * -----------------------------------------
 * Implements logic for Collectibles, Heirlooms, Mods, Boss Zombies, and Exceptions.
 *
 * v2: Added Heirloom collectable system (T5+ super-rares), Boss zombie tiers T5-T7.
 */

#include "loot_manager.h" // CMake will find this in /include automatically
#include "auditor.h"      // Access to LootItem struct
#include "loot_policy.h"  // Config-driven tiers + blacklist
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

// Helper: Case-insensitive classname substring check (delegates to shared util)
static bool name_contains(LootItem *item, const char *substr) {
    return util_str_contains_ci(item->classname, substr);
}

// Helper: Check if a short keyword appears as a standalone "word" in the classname.
// A word boundary is: start-of-string, '_', or a CamelCase transition (lower→upper).
// This prevents "GM" from matching "RabbitLegMeat" or "WeldingMask",
// and "Dev" from matching "Mass_TradeVest".
static bool name_contains_word(LootItem *item, const char *word) {
    if (!item || !word || !word[0]) return false;
    const char *cn = item->classname;
    size_t wlen = strlen(word);
    for (const char *p = cn; *p; p++) {
        // Case-insensitive match of `word` at position p
        if (util_strnicmp(p, word, wlen) != 0) continue;
        // Check left boundary: start of string, '_', or CamelCase transition
        if (p != cn) {
            char prev = *(p - 1);
            if (prev != '_' && !(islower((unsigned char)prev) && isupper((unsigned char)*p)))
                continue;
        }
        // Check right boundary: end of string, '_', digit, or CamelCase transition
        char next = p[wlen];
        if (next != '\0' && next != '_' && !isdigit((unsigned char)next) &&
            !(isupper((unsigned char)p[wlen - 1]) && islower((unsigned char)next)))
            continue;
        return true;
    }
    return false;
}

static void add_tier_values(LootItem *item, int min_tier, int max_tier) {
    item->value_count = 0;
    for (int t = min_tier; t <= max_tier && item->value_count < MAX_VALUES_PER_ITEM; t++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "Tier%d", t);
        strncpy(item->values[item->value_count++], buf, MAX_VALUE_LEN - 1);
    }
}

// --- MOD LOGIC ---

int loot_get_mod_tier(LootItem *item) {
    int tier = 0;

    // ------------------------------------------------------------------
    // EXPLICIT WEAPON SPAWN-TIER OVERRIDES (user-configured)
    // ------------------------------------------------------------------
    // Several vanilla guns ship at nominal=0 ("defined in config but disabled").
    // Without an override they'd be swept into the Black Market tier by the
    // global nominal-0 rule at the bottom of this function and never world-spawn.
    // These entries force a real internal tier AND re-enable a nominal (which
    // survives balancing, since balance_economy_values only caps, never raises).
    // The internal tier -> CE zone (Tier1-4) mapping lives in
    // auditor_assign_ce_values(): T1->Tier1, T2->Tier1+Tier2, T4->Tier2, etc.
    if (name_contains(item, "Derringer")) {          // Black/Grey/Pink holdout pistol
        if (item->nominal < 8) item->nominal = 8;    // uncommon presence
        return 1;                                    // -> CE Tier1 (coast / towns)
    }
    if (util_strcasecmp(item->classname, "Glock19") == 0) {  // exact: not SN_Glock19
        if (item->nominal < 10) item->nominal = 10;
        return 4;                                    // -> CE Tier2 (inland towns)
    }

    if (name_contains(item, "Admin") || name_contains(item, "ADMIN") ||
        name_contains_word(item, "GM") || name_contains(item, "Debug") ||
        name_contains_word(item, "DEV") || name_contains_word(item, "Dev")) {
        int cb = lp_contraband_tier();
        return (cb > 0) ? cb : 11;
    }

    // PARAGON: Reclassified as Heirloom collectables (handled by loot_is_heirloom)
    // They skip this function entirely — processed in auditor_run_full_audit()
    // BITCOIN: rare high-tier world loot. Assigned to the top spawning tier within
    // the configured scaling range so it CE-spawns; broad zone tags are applied in
    // auditor_assign_ce_values so it appears across mid/high areas.
    if (name_contains(item, "Money_Bitcoin") || name_contains(item, "Money-Bitcoin")) {
        int hi = g_loot_policy.bitcoin_spawn_max_tier;
        if (hi <= 0 || hi > lp_tier_count()) hi = lp_tier_count();
        int bt = 0;
        for (int i = hi; i >= 1; i--) { if (lp_tier_spawns(i)) { bt = i; break; } }
        return (bt > 0) ? bt : 8;
    }

    if (name_contains(item, "Paragon") || name_contains(item, "PG_")) {
        return 5; // Heirloom tier (enforce_heirloom_properties handles details)
    }
    // TACTICAL FLAVA: Mid-High Tier (Tier 6-7)
    else if (name_contains(item, "TF_") || name_contains(item, "TacFlava")) {
        if (item->nominal <= 3) tier = 7; // Rare = Spec-Ops
        else tier = 6;                    // Common = Infantry
    }
    // SNAFU: Spread across Tier 7-10 (subcategory-aware)
    // Attachments and ammo are more common than the weapons themselves.
    // Prevents all 393 SNAFU items from piling up at T10 Mythic.
    else if (name_contains(item, "SNAFU") || name_contains(item, "Snafu")) {
        // Subcategory detection for SNAFU items
        bool snafu_is_ammo = name_contains(item, "Ammo_") ||
                             name_contains(item, "_Ammo") ||
                             name_contains(item, "AmmoBox");
        bool snafu_is_mag  = (name_contains(item, "Mag") ||
                              name_contains(item, "rdMag") ||
                              name_contains(item, "drum")) &&
                             !name_contains(item, "Magnum");
        bool snafu_is_attach = name_contains(item, "Bttstck") ||
                               name_contains(item, "Hndgrd") ||
                               name_contains(item, "Suppressor") ||
                               name_contains(item, "Compensator") ||
                               name_contains(item, "Bayonet") ||
                               name_contains(item, "Bipod") ||
                               name_contains(item, "Stock") ||
                               name_contains(item, "Grip") ||
                               name_contains(item, "Muzzel") ||
                               name_contains(item, "Muzzle") ||
                               name_contains(item, "Light") ||
                               name_contains(item, "Mount");
        bool snafu_is_optic = name_contains(item, "Optic") ||
                              name_contains(item, "Scope") ||
                              name_contains(item, "Aimpoint") ||
                              name_contains(item, "EOTech") ||
                              name_contains(item, "Trijicon") ||
                              name_contains(item, "Elcan") ||
                              name_contains(item, "Kahles") ||
                              name_contains(item, "Leupold") ||
                              name_contains(item, "Nightforce") ||
                              name_contains(item, "Walther") ||
                              name_contains(item, "Tango6T") ||
                              name_contains(item, "Hunting") ||
                              name_contains(item, "PKA") ||
                              name_contains(item, "Holo");

        if (item->nominal == 0) {
            int bm = lp_black_market_tier();
            tier = (bm > 0) ? bm : 11;  // Store Only -> Black Market tier (no spawn)
        }
        else if (snafu_is_ammo) {
            tier = 7;  // Ammo → Spec Ops (needs to be findable)
        }
        else if (snafu_is_mag) {
            tier = 8;  // Magazines → Operator
        }
        else if (snafu_is_optic || snafu_is_attach) {
            tier = 8;  // Optics/attachments → Operator
        }
        else {
            // Actual weapons: tier by nominal (Elite/Mythic, not BM — BM is store-only)
            if (item->nominal <= 1) tier = 10; // Very rare weapons → Mythic
            else if (item->nominal <= 3) tier = 9;  // Rare weapons → Elite
            else if (item->nominal <= 5) tier = 8;  // Uncommon weapons → Operator
            else tier = 7;                           // Common weapons → Spec Ops
        }
    }
    // MORTY'S WEAPONS: Mid-High Tier (Tier 5-7)
    else if (name_contains(item, "Morty") || name_contains(item, "MORTY")) {
        if (item->nominal <= 2) tier = 7;
        else if (item->nominal <= 5) tier = 6;
        else tier = 5;
    }
    // OSHKOSH / GunnerTruck: Tier 8 vehicle
    else if (name_contains(item, "Oshkosh") || name_contains(item, "Gunner")) {
        tier = 8;
    }
    // MMG — Mighty's Military Gear: Tier 5–8 (stat-driven promotion in auditor)
    // Base tier by category; stat-based promotion handled in heuristic_tier_assignment()
    else if (name_contains(item, "MMG") || name_contains(item, "Mighty")) {
        if (name_contains(item, "MKV") || name_contains(item, "MK_V") ||
            name_contains(item, "Mk_V") || name_contains(item, "MK5")) {
            tier = 8; // MK-V = 2.5x vanilla → Operator
        }
        else if (name_contains(item, "MKIII") || name_contains(item, "MK_III") ||
                 name_contains(item, "Mk_III") || name_contains(item, "MK3")) {
            tier = 7; // MK-III = 1.5x vanilla → Spec-Ops
        }
        else if (name_contains(item, "Armored") && name_contains(item, "Helmet")) {
            tier = 7; // Armored Helmet = 1.5x vanilla → Spec-Ops
        }
        else if (name_contains(item, "MMPS") || name_contains(item, "Supplybag")) {
            tier = 7; // MMPS 150 slot / Supplybag 120 slot → Spec-Ops
        }
        else if (name_contains(item, "PlateCarrier") || name_contains(item, "Vest") ||
                 name_contains(item, "JPC") || name_contains(item, "TEC")) {
            tier = 6; // Vanilla-equivalent vests → Infantry
        }
        else if (name_contains(item, "Helmet") || name_contains(item, "Striker") ||
                 name_contains(item, "Defcon")) {
            tier = 6; // Vanilla-equivalent helmets → Infantry
        }
        else if (name_contains(item, "Assault") || name_contains(item, "Carrier") ||
                 name_contains(item, "Camelback")) {
            tier = 6; // 80-90 slot + weapon backpacks → Infantry
        }
        else if (name_contains(item, "Pouch") || name_contains(item, "Holster")) {
            tier = 5; // Pouches → Insurgent (accessory)
        }
        else if (name_contains(item, "NVG")) {
            tier = 7; // NVGs are high-value → Spec-Ops
        }
        else if (name_contains(item, "Headphones") || name_contains(item, "Radio")) {
            tier = 6; // Function as radio transmitter → Infantry
        }
        else if (item->nominal <= 3) tier = 7;  // Unknown rare MMG → Spec-Ops
        else if (item->nominal <= 10) tier = 6;  // Unknown common MMG → Infantry
        else tier = 5;                           // Unknown abundant MMG → Insurgent
    }
    // WINDSTRIDE's Clothing: Mid tier (Tier 5-6)
    else if (name_contains(item, "WS_") || name_contains(item, "Windstride")) {
        if (item->nominal <= 5) tier = 6;
        else tier = 5;
    }
    // UNKNOWN GHILLIE: High tier (Tier 7-8)
    else if (name_contains(item, "Ghillie") && !name_contains(item, "Suit")) {
        if (item->nominal <= 2) tier = 8;
        else tier = 7;
    }
    // CJ187 MONEY DENOMINATIONS: Tier = denomination value
    // Low bills spawn everywhere (T1-T2), high bills only in high-tier areas (T7-T8).
    // Players in T8 areas find $50/$100 notes, early-game areas get $1/$2.
    // All three currencies (Dollar, Euro, Ruble) follow the same tier curve.
    else if (name_contains(item, "Money_Dollar")) {
        if (name_contains(item, "Dollar100"))     tier = 8;  // Operator-tier loot
        else if (name_contains(item, "Dollar50"))  tier = 7;  // Spec-Ops areas
        else if (name_contains(item, "Dollar20"))  tier = 5;  // Insurgent / mid areas
        else if (name_contains(item, "Dollar10"))  tier = 3;  // Militia / early-mid
        else if (name_contains(item, "Dollar5"))   tier = 2;  // Recruit+
        else                                       tier = 1;  // $1, $2 — everywhere
    }
    else if (name_contains(item, "Money_Euro")) {
        if (name_contains(item, "Euro500"))        tier = 8;
        else if (name_contains(item, "Euro200"))   tier = 8;
        else if (name_contains(item, "Euro100"))   tier = 7;
        else if (name_contains(item, "Euro50"))    tier = 5;
        else if (name_contains(item, "Euro20"))    tier = 3;
        else if (name_contains(item, "Euro10"))    tier = 2;
        else                                       tier = 1;  // €1, €2, €5
    }
    else if (name_contains(item, "Money_Ruble")) {
        if (name_contains(item, "Ruble5000"))      tier = 8;
        else if (name_contains(item, "Ruble2000")) tier = 8;
        else if (name_contains(item, "Ruble1000")) tier = 7;
        else if (name_contains(item, "Ruble500"))  tier = 7;
        else if (name_contains(item, "Ruble200"))  tier = 5;
        else if (name_contains(item, "Ruble100"))  tier = 3;
        else if (name_contains(item, "Ruble50"))   tier = 3;
        else if (name_contains(item, "Ruble10"))   tier = 2;
        else                                       tier = 1;  // ₽1, ₽2, ₽5
    }

    // HEIRLOOM: Collectables from specific mods get Tier 5 (Heirloom)
    if (loot_is_heirloom(item) && tier < 5) {
        tier = 5;
    }

    // STORE-ONLY CHECK (Global): nominal-0 items never world-spawn, so route them
    // to the Black Market tier (no spawn, purchasable in Bitcoin) rather than a
    // spawning tier. Falls back to 11 (default BM) if no black_market flag set.
    if (item->nominal == 0) {
        int bm = lp_black_market_tier();
        if (bm <= 0) bm = 11;
        if (tier < bm) tier = bm;
    }

    return tier;
}

// --- EXCEPTIONS ---

bool loot_is_exception(const char *classname) {
    const char *exceptions[] = {
        "LandMineTrap", "BearTrap", "TripwireTrap", "Net", NULL
    };
    for (int i = 0; exceptions[i] != NULL; i++) {
        if (util_strcasecmp(classname, exceptions[i]) == 0) return true;
    }
    return false;
}

// ============================================================================
// HEIRLOOM COLLECTABLE SYSTEM
// ============================================================================
// Heirlooms are ultra-rare collectables from specific mods.
// They get their own economy tier (T5), currency (HeirloomToken),
// and very restricted spawn rules.
//
// Different collectable mods get different sub-tiers:
//   - @Bottle Cap Collectables (O12_Caps_): T5 Heirloom, nominal 2
//   - Other collectables by category: T5, standard heirloom rules
//
// Heirloom items are NEVER mixed into the general loot pool.
// They spawn in endgame/rare areas only.
// ============================================================================

bool loot_is_heirloom(LootItem *item) {
    if (!item || !item->classname[0]) return false;
    
    // @Bottle Cap Collectables (prefix O12_Caps_)
    if (util_str_contains_ci(item->classname, "O12_Caps")) return true;
    
    // Generic collectable patterns
    if (util_str_contains_ci(item->classname, "Collectable")) return true;
    if (util_str_contains_ci(item->classname, "Collectible")) return true;
    if (util_str_contains_ci(item->classname, "Heirloom")) return true;
    
    // Category-based detection
    if (item->category[0]) {
        if (util_strcasecmp(item->category, "collectables") == 0) return true;
        if (util_strcasecmp(item->category, "collections") == 0) return true;
    }
    
    // @Paragon Collectable Items (prefix Paragon_ or PG_)
    if (util_str_contains_ci(item->classname, "Paragon_")) return true;
    if (util_str_contains_ci(item->classname, "PG_")) return true;
    
    // Mod-source based detection
    if (util_str_contains_ci(item->mod_name, "Collectables")) return true;
    if (util_str_contains_ci(item->mod_name, "Collectable")) return true;
    if (util_str_contains_ci(item->mod_name, "Bottle Cap")) return true;
    if (util_str_contains_ci(item->mod_name, "Paragon")) return true;
    
    return false;
}

int loot_get_heirloom_subtier(LootItem *item) {
    if (!item) return 5;
    
    // @Bottle Cap Collectables: T5 base (common caps) to T6 (rare/shiny caps)
    if (util_str_contains_ci(item->classname, "O12_Caps")) {
        // Ultra-rare cap variants (quantum, nuka, etc.)
        if (util_str_contains_ci(item->classname, "Quantum") ||
            util_str_contains_ci(item->classname, "Nuka") ||
            util_str_contains_ci(item->classname, "Atomic") ||
            util_str_contains_ci(item->classname, "Mythic") ||
            util_str_contains_ci(item->classname, "Golden")) {
            return 6; // Ultra-rare heirloom
        }
        return 5; // Standard heirloom
    }
    
    // @Paragon Collectable Items: trophy-class ultra-rares get T6
    if (util_str_contains_ci(item->classname, "Paragon_") ||
        util_str_contains_ci(item->classname, "PG_")) {
        if (util_str_contains_ci(item->classname, "Gold") ||
            util_str_contains_ci(item->classname, "Crown") ||
            util_str_contains_ci(item->classname, "Sword") ||
            util_str_contains_ci(item->classname, "Skull") ||
            util_str_contains_ci(item->classname, "Trophy") ||
            util_str_contains_ci(item->classname, "Diamond") ||
            util_str_contains_ci(item->classname, "Lion") ||
            util_str_contains_ci(item->classname, "Bitcoin") ||
            util_str_contains_ci(item->classname, "Rainbow")) {
            return 6; // Ultra-rare Paragon trophy
        }
        return 5; // Standard Paragon collectable
    }
    
    // Future collectable mods can be inserted here with their own sub-tiers
    return 5;
}

void loot_enforce_heirloom_properties(LootItem *item) {
    if (!item) return;
    
    int subtier = loot_get_heirloom_subtier(item);
    item->assigned_tier = subtier;
    
    // --- Economy: Ultra-rare ---
    // Only set economy defaults if the economy engine hasn't balanced yet
    if (item->calculated_price == 0) {
        switch (subtier) {
            case 6: // Ultra-rare heirloom (shiny/quantum caps, legendary collectables)
                item->nominal = 1;
                item->min = 0;
                item->lifetime = 28800;  // 8 hours
                item->restock = 3600;    // 1 hour restock
                break;
            case 5: // Standard heirloom
            default:
                item->nominal = 2;
                item->min = 1;
                item->lifetime = 21600;  // 6 hours
                item->restock = 1800;    // 30 min restock
                break;
        }
    }
    
    // --- Usage: Endgame/rare areas only ---
    item->usage_count = 0;
    strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
    strncpy(item->usages[item->usage_count++], "Hunting", MAX_USAGE_LEN - 1);
    if (subtier >= 6) {
        // Ultra-rares only in Military
        item->usage_count = 1;
    }
    
    // --- Values: Unique (heirlooms are collectables, not geographic tier) ---
    item->value_count = 0;
    strncpy(item->values[item->value_count++], "Unique", MAX_VALUE_LEN - 1);
    
    item->modified = true;
}

// --- COLLECTIBLES (non-heirloom, standard collectables) ---

bool loot_is_collectible(LootItem *item) {
    // Heirloom items are NOT treated as generic collectibles
    if (loot_is_heirloom(item)) return false;
    
    if (util_strcasecmp(item->category, "collectables") == 0 || 
        util_strcasecmp(item->category, "collections") == 0) {
        return true;
    }
    return false;
}

bool loot_is_craftable(LootItem *item) {
    if (item->flags & FLAG_CRAFTED) return true;
    return false;
}

void loot_enforce_collectible_properties(LootItem *item) {
    // 1. Force Usages
    item->usage_count = 0;
    strncpy(item->usages[item->usage_count++], "Town", MAX_USAGE_LEN-1);
    strncpy(item->usages[item->usage_count++], "Village", MAX_USAGE_LEN-1);
    strncpy(item->usages[item->usage_count++], "Hunting", MAX_USAGE_LEN-1); 

    // 2. Force Values (All vanilla CE tiers — Tier1-4 only)
    add_tier_values(item, 1, 4);

    // 3. Economy Settings — only apply defaults if economy engine hasn't run
    if (item->calculated_price == 0) {
        if (item->nominal < 10) item->nominal = 20;
        item->lifetime = 14400; 
        item->restock = 0;
        item->min = (int)(item->nominal * 0.7);
    }

    item->modified = true;
    if (item->assigned_tier <= 0) item->assigned_tier = 1;
}

void loot_enforce_craftable_properties(LootItem *item) {
    // 1. Force Usages (craftables are practical and common)
    item->usage_count = 0;
    strncpy(item->usages[item->usage_count++], "Industrial", MAX_USAGE_LEN-1);
    strncpy(item->usages[item->usage_count++], "Town", MAX_USAGE_LEN-1);
    strncpy(item->usages[item->usage_count++], "Village", MAX_USAGE_LEN-1);

    // 2. Force Values (All vanilla CE tiers — Tier1-4 only)
    add_tier_values(item, 1, 4);

    // 3. Economy Settings — only apply defaults if economy engine hasn't run
    if (item->calculated_price == 0) {
        if (item->nominal < 5) item->nominal = 10;
        item->lifetime = 7200;
        item->restock = 0;
        item->min = (int)(item->nominal * 0.6);
    }

    item->modified = true;
    if (item->assigned_tier <= 0) item->assigned_tier = 1;
}

// ============================================================================
// ZOMBIE / INFECTED TIER SYSTEM
// ============================================================================

static bool starts_with_ci(const char *str, const char *prefix) {
    while (*prefix) {
        if (tolower((unsigned char)*str) != tolower((unsigned char)*prefix)) return false;
        str++; prefix++;
    }
    return true;
}

bool loot_is_zombie(LootItem *item) {
    if (!item || !item->classname[0]) return false;
    // DayZ zombie classnames: ZmbM_ (male), ZmbF_ (female), Zmb (generic)
    if (starts_with_ci(item->classname, "ZmbM_")) return true;
    if (starts_with_ci(item->classname, "ZmbF_")) return true;
    if (starts_with_ci(item->classname, "Zmb"))   return true;
    // Some mods use "Infected" prefix
    if (starts_with_ci(item->classname, "Infected")) return true;
    return false;
}

bool loot_is_animal(LootItem *item) {
    if (!item || !item->classname[0]) return false;
    if (starts_with_ci(item->classname, "Animal_")) return true;
    // Common vanilla animal classes
    const char *animals[] = {
        "Bear_", "Wolf_", "Hen_", "Rooster_", "Pig_", "Cow_", 
        "Goat_", "Sheep_", "Deer_", "Rabbit_", "Fox_", "Boar_",
        "GallusGallus", "SusDomesticus", "BosT", "CapraH", "OvisA",
        "CervusE", "VulpesV", "UrsusA", "CanisLupus",
        NULL
    };
    for (int i = 0; animals[i]; i++) {
        if (starts_with_ci(item->classname, animals[i])) return true;
    }
    return false;
}

/**
 * Determine zombie tier from its usage zones and classname patterns.
 *
 * Tier 1 (Green Eyes):   Civilian — towns, villages. Basic awareness.
 * Tier 2 (Yellow Eyes):  Industrial — factories, farms. Moderate awareness.
 * Tier 3 (Red Eyes):     Military — bases, checkpoints. High awareness + speed.
 * Tier 4 (Purple Eyes):  Endgame — tisy, NWAF, contaminated. Max stats.
 * Tier 5 (Cyan Eyes):    Mini-Boss — contaminated zones. Area denial.
 * Tier 6 (White Eyes):   Boss — unique spawns, event areas. Extreme danger.
 * Tier 7 (Gold Eyes):    Raid Boss — server events only. Requires squad.
 *
 * Modded zombies (SNAFU, GoreZ, etc.) get higher tiers based on mod origin.
 */
int loot_get_zombie_tier(LootItem *item) {
    if (!item) return 0;
    
    // Raid Boss / Boss patterns (T7/T6)
    if (util_str_contains_ci(item->classname, "RaidBoss") || util_str_contains_ci(item->classname, "Raid_Boss")) return 7;
    if (util_str_contains_ci(item->classname, "Boss_") || util_str_contains_ci(item->classname, "_Boss")) return 6;
    if (util_str_contains_ci(item->classname, "MiniBoss") || util_str_contains_ci(item->classname, "Mini_Boss")) return 5;
    
    // Modded zombies get higher tiers
    if (name_contains(item, "SNAFU") || name_contains(item, "Snafu")) {
        // SNAFU zombies: range from T4 to T5 depending on variant
        if (util_str_contains_ci(item->classname, "Heavy") || util_str_contains_ci(item->classname, "Elite")) return 5;
        return 4;
    }
    if (name_contains(item, "Gore") || name_contains(item, "GoreZ")) {
        if (util_str_contains_ci(item->classname, "Heavy") || util_str_contains_ci(item->classname, "Big")) return 5;
        return 3;
    }
    if (name_contains(item, "Heavy"))   return 4;
    if (name_contains(item, "Soldier") || name_contains(item, "Military")) return 3;
    if (name_contains(item, "Police") || name_contains(item, "Guard"))    return 2;
    
    // Check usage hints already on the item
    for (int i = 0; i < item->usage_count; i++) {
        if (util_strcasecmp(item->usages[i], "Military") == 0) return 3;
        if (util_strcasecmp(item->usages[i], "Industrial") == 0) return 2;
        if (util_strcasecmp(item->usages[i], "Coast") == 0) return 1;
        if (util_strcasecmp(item->usages[i], "Farm") == 0) return 1;
    }
    
    // Check value tier hints
    for (int i = 0; i < item->value_count; i++) {
        if (util_strcasecmp(item->values[i], "Tier4") == 0) return 4;
        if (util_strcasecmp(item->values[i], "Tier3") == 0) return 3;
        if (util_strcasecmp(item->values[i], "Tier2") == 0) return 2;
        if (util_strcasecmp(item->values[i], "Tier1") == 0) return 1;
    }
    
    // Default: civilian tier
    return 1;
}

/**
 * Enforce zombie tier properties.
 * Tiers 1-4: Standard infected.
 * Tiers 5-7: Boss variants (mini-boss, boss, raid boss).
 *
 * Higher tiers get:
 *   - Lower nominal (fewer but deadlier)
 *   - Higher lifetime (persist longer to guard areas)
 *   - Restricted usage zones matching the tier
 *   - Correct value tags
 */
void loot_enforce_zombie_tier(LootItem *item, int tier) {
    if (!item || tier < 1 || tier > 7) return;
    
    // --- Economy Scaling by Tier ---
    switch (tier) {
        case 1: // Green Eyes — abundant civilians
            if (item->nominal < 20) item->nominal = 30;
            item->min = (int)(item->nominal * 0.7);
            item->lifetime = 1800;
            item->restock = 0;
            break;
        case 2: // Yellow Eyes — moderate
            if (item->nominal < 10) item->nominal = 20;
            item->min = (int)(item->nominal * 0.6);
            item->lifetime = 2400;
            item->restock = 0;
            break;
        case 3: // Red Eyes — fewer, tougher
            if (item->nominal < 5) item->nominal = 12;
            item->min = (int)(item->nominal * 0.5);
            item->lifetime = 3600;
            item->restock = 300;
            break;
        case 4: // Purple Eyes — rare, terrifying
            if (item->nominal < 2) item->nominal = 6;
            item->min = (int)(item->nominal * 0.4);
            item->lifetime = 7200;
            item->restock = 600;
            break;
        case 5: // Gold Eyes — Super Zombie (rare, 0-1 on map at a time)
            item->nominal = 1;
            item->min = 0;
            item->lifetime = 10800; // 3 hours
            item->restock = 3600;   // 1 hour (slow respawn — keeps them rare)
            break;
        case 6: // White Eyes — Boss (unique spawns)
            item->nominal = 2;
            item->min = 0;
            item->lifetime = 14400; // 4 hours
            item->restock = 3600;   // 1 hour
            break;
        case 7: // Gold Eyes — Raid Boss (event only)
            item->nominal = 1;
            item->min = 0;
            item->lifetime = 21600; // 6 hours
            item->restock = 7200;   // 2 hours
            break;
    }
    
    // --- Usage zones ---
    item->usage_count = 0;
    switch (tier) {
        case 1:
            strncpy(item->usages[item->usage_count++], "Town", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Village", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Coast", MAX_USAGE_LEN - 1);
            break;
        case 2:
            strncpy(item->usages[item->usage_count++], "Industrial", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Farm", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Town", MAX_USAGE_LEN - 1);
            break;
        case 3:
            strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Industrial", MAX_USAGE_LEN - 1);
            break;
        case 4:
            strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
            break;
        case 5: // Mini-Boss: Military + Contaminated
            strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
            break;
        case 6: // Boss: Military only
            strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
            break;
        case 7: // Raid Boss: Military only (event spawn)
            strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
            break;
    }
    
    // --- Value tags ---
    item->value_count = 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "Tier%d", (tier <= 4) ? tier : 4);
    strncpy(item->values[item->value_count++], buf, MAX_VALUE_LEN - 1);
    // Higher tiers also bleed into adjacent tier
    if (tier >= 2 && tier <= 4) {
        snprintf(buf, sizeof(buf), "Tier%d", tier - 1);
        strncpy(item->values[item->value_count++], buf, MAX_VALUE_LEN - 1);
    }
    if (tier <= 3) {
        snprintf(buf, sizeof(buf), "Tier%d", tier + 1);
        strncpy(item->values[item->value_count++], buf, MAX_VALUE_LEN - 1);
    }
    
    // --- Category ---
    strncpy(item->category, "infected", MAX_CATEGORY_LEN - 1);
    
    item->assigned_tier = tier;
    item->modified = true;
}

/**
 * Enforce animal tier properties.
 * Predators (bears, wolves) -> higher tiers (military/endgame areas)
 * Prey (deer, rabbits, hens) -> lower tiers (farms, villages)
 */
void loot_enforce_animal_tier(LootItem *item, int tier) {
    if (!item || tier < 1 || tier > 4) return;
    
    // Economy scaling
    switch (tier) {
        case 1: // Passive farm animals
            if (item->nominal < 10) item->nominal = 15;
            item->min = (int)(item->nominal * 0.7);
            item->lifetime = 3600;
            break;
        case 2: // Deer, foxes, boars — wilderness
            if (item->nominal < 5) item->nominal = 10;
            item->min = (int)(item->nominal * 0.6);
            item->lifetime = 3600;
            break;
        case 3: // Wolves — packs in military zones
            if (item->nominal < 3) item->nominal = 6;
            item->min = (int)(item->nominal * 0.5);
            item->lifetime = 7200;
            break;
        case 4: // Bears — endgame areas
            if (item->nominal < 1) item->nominal = 3;
            item->min = (int)(item->nominal * 0.4);
            item->lifetime = 7200;
            break;
    }
    item->restock = 0;
    
    // Usage zones
    item->usage_count = 0;
    switch (tier) {
        case 1:
            strncpy(item->usages[item->usage_count++], "Farm", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Village", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Coast", MAX_USAGE_LEN - 1);
            break;
        case 2:
            strncpy(item->usages[item->usage_count++], "Hunting", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Farm", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Village", MAX_USAGE_LEN - 1);
            break;
        case 3:
            strncpy(item->usages[item->usage_count++], "Hunting", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
            break;
        case 4:
            strncpy(item->usages[item->usage_count++], "Military", MAX_USAGE_LEN - 1);
            strncpy(item->usages[item->usage_count++], "Hunting", MAX_USAGE_LEN - 1);
            break;
    }
    
    // Value tags
    item->value_count = 0;
    char buf[16];
    snprintf(buf, sizeof(buf), "Tier%d", tier);
    strncpy(item->values[item->value_count++], buf, MAX_VALUE_LEN - 1);
    
    item->assigned_tier = tier;
    item->modified = true;
}

/**
 * Determine animal tier from classname.
 * Predators get higher tiers, prey gets lower.
 */
int loot_get_animal_tier(LootItem *item) {
    if (!item) return 1;
    
    // Tier 4: Bears
    if (name_contains(item, "Bear") || name_contains(item, "UrsusA")) return 4;
    
    // Tier 3: Wolves  
    if (name_contains(item, "Wolf") || name_contains(item, "CanisLupus")) return 3;
    
    // Tier 2: Wild game (deer, boar, fox)
    if (name_contains(item, "Deer") || name_contains(item, "CervusE")) return 2;
    if (name_contains(item, "Boar") || name_contains(item, "Fox") || name_contains(item, "VulpesV")) return 2;
    
    // Modded dangerous animals
    if (name_contains(item, "SNAFU") || name_contains(item, "Gore")) return 4;
    
    // Tier 1: Farm animals
    return 1;
}
