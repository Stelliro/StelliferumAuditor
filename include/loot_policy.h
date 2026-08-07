#ifndef LOOT_POLICY_H
#define LOOT_POLICY_H

// ============================================================================
// LOOT POLICY — user-configurable tier system + blacklist
// ============================================================================
// The compiled auditor historically hardcoded its tier table in loot_manager.c
// and never read config/tier_rules.json or config/known_items.json (that loader
// was a stub: parser_load_known_items() { return true; }). This module replaces
// that with a real, file-backed policy the generator consults at runtime and
// which the in-app "Loot Policy" tab edits.
//
// Source of truth: config/loot_policy.ini  — flat KEY=value, comma-separated
// lists (matches the comma-list inputs shown in the UI). If the file is absent,
// the built-in 12-tier default map is used.
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define LP_MAX_TIERS       16     // capacity; runtime tier_count <= this
#define LP_MAX_TIER_NAME   64
#define LP_MAX_BLACKLIST   2048
#define LP_MAX_NAME_LEN    128
#define LP_CONFIG_PATH     "config/loot_policy.ini"

typedef struct {
    char name[LP_MAX_TIER_NAME]; // display label, e.g. "Spec-Ops", "Black Market"
    bool spawns;                 // false -> nominal 0, no CE world spawn
    bool tradeable;              // false -> excluded from every trader (contraband)
    bool black_market;           // true  -> sold at the Black Market in Bitcoin
} LpTier;

typedef struct {
    int    tier_count;                 // active tiers (1..LP_MAX_TIERS)
    LpTier tiers[LP_MAX_TIERS];        // tiers[0] == tier 1
    char   blacklist[LP_MAX_BLACKLIST][LP_MAX_NAME_LEN];
    int    blacklist_count;
    int    contraband_tier;            // 1-based tier blacklisted items are forced into (0 = none)
    int    bitcoin_spawn_min_tier;     // Bitcoin world-spawn scaling range (0 = disabled)
    int    bitcoin_spawn_max_tier;
    bool   loaded;
} LootPolicy;

extern LootPolicy g_loot_policy;

// Seed g_loot_policy with the built-in 12-tier default map.
void loot_policy_init_defaults(void);

// Load policy from `path` (INI). Starts from defaults, overrides with file
// contents. Returns false if the file is missing (defaults remain in effect).
bool loot_policy_load(const char *path);

// Persist g_loot_policy back to `path` (INI). Used by the UI Save button.
bool loot_policy_save(const char *path);

// --- queries (all tier args are 1-based) ---
int         lp_tier_count(void);
const char *lp_tier_name(int tier);
bool        lp_tier_spawns(int tier);
bool        lp_tier_tradeable(int tier);
bool        lp_tier_black_market(int tier);
int         lp_black_market_tier(void);   // first tier flagged black_market, or 0
int         lp_contraband_tier(void);
bool        lp_is_blacklisted(const char *classname);

// Setters used by the in-app Loot Policy editor. CSV = comma-separated.
// set_tiers preserves each tier's spawn/trade/black_market flags by index.
void        loot_policy_set_tiers_csv(const char *csv);
void        loot_policy_set_blacklist_csv(const char *csv);

#ifdef __cplusplus
}
#endif

#endif // LOOT_POLICY_H
