
#ifndef WEB_LOOKUP_H
#define WEB_LOOKUP_H

// ============================================================================
// WEB LOOKUP — Internet-based mod attachment & stat discovery
// ============================================================================
//
// Uses WinINet (already linked, zero new dependencies) to fetch:
//   1. Steam Workshop mod descriptions via the public API
//   2. DayZ Fandom wiki pages for vanilla item attachment info
//   3. Workshop discussion pages for types.xml links (pastebin, etc.)
//
// Extracted classnames are classified (weapon / attachment / equipment / etc.)
// and stored with inferred parent relationships.  Item stats (protection
// multipliers, slot counts, weapon slots) are parsed from mod descriptions
// and used for tier promotion.
//
// Results are cached locally in config/web_cache/ with a 7-day TTL.
//
// The gap filler consults web lookup results as an additional tier of
// attachment discovery:
//
//   Tier 1  Static platform table  (hardcoded vanilla weapons)
//   Tier 2  Web lookup             (Workshop + wiki descriptions)
//   Tier 3  Naming convention      (classname pattern matching)
//   Tier 4  Mod-prefix heuristic   (shared first-segment prefix)
//
// The tier system uses web-discovered item stats for stat-based promotion:
//   - protection_mult ≥ 2.0 → T8 (Operator)
//   - protection_mult ≥ 1.5 → T7 (Spec-Ops)
//   - slot_count ≥ 120      → T7 (Spec-Ops)
//   - slot_count ≥ 80       → T6 (Infantry)
//   - has_weapon_slot        → +1 tier bump
//
// Configuration:  config/workshop_ids.ini maps @ModFolder → Workshop ID.
// ============================================================================

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Limits
#define WEB_MAX_MODS              64
#define WEB_MAX_ITEMS_PER_MOD    512
#define WEB_MAX_DESCRIPTION    32768
#define WEB_MAX_RESPONSE       65536
#define WEB_CACHE_MAX_AGE_SECS (7 * 24 * 3600)   // 7 days
#define WEB_MAX_DISCUSSION_LINKS  16

// ── Item Stats ───────────────────────────────────────────────────────────────
// Parsed from Workshop descriptions.  Used for stat-based tier promotion.
typedef struct {
    float protection_mult;   // e.g. 2.5 = "2.5x Vanilla Plate Carrier"
    int   slot_count;        // e.g. 150 = "150 Slots"
    bool  has_weapon_slot;   // e.g. "80 Slots + Weapon"
    char  stat_source[64];   // Where the stat came from: "workshop_desc", "types_xml", etc.
} ItemStats;

// A single item discovered from a web source
typedef struct {
    char classname[128];     // e.g. "SNAFU_AK_Suppressor"
    char category[32];       // "weapon", "attachment", "equip_attach", "equipment",
                             // "ammo", "unknown"
    char parent_hint[128];   // Inferred base item this attaches to (may be empty)
    ItemStats stats;         // Protection/slot stats parsed from description
} WebItem;

// A types.xml link found in Workshop discussions
typedef struct {
    char url[512];            // Full URL (pastebin, hastebin, raw text, etc.)
    char poster_name[128];    // Steam display name of poster
    bool is_author;           // true if poster is the mod author/uploader
    bool downloaded;          // true if we successfully fetched the content
} DiscussionLink;

// Parsed info for one mod (populated from Workshop API + cache)
typedef struct {
    char mod_name[128];               // e.g. "@SNAFU Weapons"
    unsigned long long workshop_id;
    unsigned long long creator_id;    // Steam ID of mod author (for verification)
    char title[256];                  // Workshop page title
    WebItem items[WEB_MAX_ITEMS_PER_MOD];
    int  item_count;
    DiscussionLink discussion_links[WEB_MAX_DISCUSSION_LINKS];
    int  discussion_link_count;
    time_t fetched_at;
    bool valid;
    bool has_stats;                   // true if any item has non-zero stats
} ModWebInfo;

// Web lookup state (heap-allocated, referenced from AuditorContext.web)
typedef struct WebLookupState {
#ifdef _WIN32
    void *h_internet;                 // WinINet HINTERNET session handle
#else
    void *h_internet;                 // Placeholder (no-op on non-Windows)
#endif
    ModWebInfo mods[WEB_MAX_MODS];
    int  mod_count;
    char cache_dir[512];              // e.g. "config/web_cache"
    bool initialized;
    int  fetch_count;                 // HTTP requests made this session
    int  cache_hits;                  // Cache hits this session
} WebLookupState;

// ── Lifecycle ────────────────────────────────────────────────────────────────
bool web_lookup_init(WebLookupState *state, const char *cache_dir);
void web_lookup_shutdown(WebLookupState *state);

// ── Fetching ─────────────────────────────────────────────────────────────────
// Fetch a single mod from Steam Workshop (or cache).
bool web_fetch_workshop_mod(WebLookupState *state,
                            unsigned long long workshop_id,
                            const char *mod_name);

// Fetch a DayZ Fandom wiki page (raw wikitext output).
bool web_fetch_wiki_page(WebLookupState *state,
                         const char *item_name,
                         char *text_out, int max_len);

// Fetch ALL mods listed in workshop_ids.ini (skip cached & fresh entries).
bool web_fetch_all_configured(WebLookupState *state,
                              const char *workshop_ids_path);

// ── Stat Extraction ──────────────────────────────────────────────────────────
// Extract item stats (protection multiplier, slot count, weapon slot) from
// a mod's Workshop description.  Called automatically during fetch.
// Can also be called manually to re-parse cached descriptions.
void web_extract_item_stats_from_description(ModWebInfo *mod,
                                             const char *plain_text);

// Look up stats for a specific classname across all loaded mods.
// Returns true if stats were found and written to out_stats.
bool web_get_item_stats(const WebLookupState *state,
                        const char *classname,
                        ItemStats *out_stats);

// Get the stat-based tier promotion for an item.
// Returns 0 if no promotion, or the recommended tier (5-8).
int  web_stat_tier_promotion(const ItemStats *stats);

// ── Config ───────────────────────────────────────────────────────────────────
// Parse workshop_ids.ini → arrays of mod names and IDs.  Returns entry count.
int  web_load_workshop_ids(const char *ini_path,
                           char mod_names[][128],
                           unsigned long long *ids,
                           int max_entries);

// ── Cache ────────────────────────────────────────────────────────────────────
bool web_load_cache(WebLookupState *state);
bool web_save_cache(WebLookupState *state);

// ── Queries ──────────────────────────────────────────────────────────────────
// Find web-discovered attachments compatible with a given base item.
// Returns the number of matches written to out_items.
int  web_find_attachments_for(const WebLookupState *state,
                              const char *base_classname,
                              WebItem *out_items, int max_out);

// Check if a specific classname was found on any web source.
bool web_item_exists(const WebLookupState *state, const char *classname);

#ifdef __cplusplus
}
#endif

#endif // WEB_LOOKUP_H
