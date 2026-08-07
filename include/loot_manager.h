/**
 * STELLIFERUM LOOT MANAGER (Header)
 * ---------------------------------
 * Handles specific logic for Mods, Collectibles, Heirlooms, Boss Zombies, and Exceptions.
 * Decouples logic from the main Auditor loop.
 */

#ifndef LOOT_MANAGER_H
#define LOOT_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "auditor.h" // Access to LootItem struct

// --- MOD & TIER LOGIC ---
// Returns 0 if no specific mod rule applies, otherwise returns the forced tier.
int loot_get_mod_tier(LootItem *item);

// --- EXCEPTION HANDLING ---
// Returns true if the item is allowed to break category/zone rules (e.g., LandMines).
bool loot_is_exception(const char *classname);

// --- HEIRLOOM (COLLECTABLE) SYSTEM ---
// Returns true if the item is an "Heirloom" — a rare collectable from specific mods.
bool loot_is_heirloom(LootItem *item);

// Returns the heirloom sub-tier (5=standard, 6=ultra-rare)
int loot_get_heirloom_subtier(LootItem *item);

// Applies heirloom properties: ultra-rare nominal, endgame zones, HeirloomToken currency.
void loot_enforce_heirloom_properties(LootItem *item);

// --- COLLECTIBLES PROTOCOL (generic, non-heirloom) ---
// Returns true if the item belongs to the "Collectibles" group (but NOT heirlooms).
bool loot_is_collectible(LootItem *item);

// Applies the "All Tiers" properties (Tier 1-11, Town/Village/City/Hunting) to an item.
void loot_enforce_collectible_properties(LootItem *item);

// --- CRAFTABLES PROTOCOL ---
// Returns true if the item is flagged as craftable.
bool loot_is_craftable(LootItem *item);

// Applies the "All Tiers" properties (Tier 1-11, Industrial/Town/Village) to an item.
void loot_enforce_craftable_properties(LootItem *item);

// --- ZOMBIE / INFECTED TIER SYSTEM ---
// Tiers 1-4: Standard infected. Tiers 5-7: Boss variants.
bool loot_is_zombie(LootItem *item);

// Returns true if classname is an animal (Animal_, Bear, Wolf, etc.)
bool loot_is_animal(LootItem *item);

// Get the zombie tier based on classname and usage zones. Returns 1-7 or 0.
int loot_get_zombie_tier(LootItem *item);

// Apply tiered zombie properties (1-7, including boss tiers 5-7).
void loot_enforce_zombie_tier(LootItem *item, int tier);

// Apply tiered animal properties
void loot_enforce_animal_tier(LootItem *item, int tier);

// Get animal tier from classname
int loot_get_animal_tier(LootItem *item);

#ifdef __cplusplus
}
#endif

#endif // LOOT_MANAGER_H