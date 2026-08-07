#include "auditor.h"
#include "loot_policy.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ============================================================================
// STELLIFERRUM ECONOMY ENGINE v3 — HARDCORE
// ============================================================================
// Complete pricing, categorization, and economy balancing system.
// Tuned for HARDCORE difficulty: high buy prices, punishing sell margins.
//
// PRICING FORMULA:
//   base_price(category) * tier_multiplier^(tier-1) * rarity_mult * volatility
//
// SELL = 15% of BUY (hardcore — selling is a grind, not a shortcut)
//
// DESIGN INTENT:
//   - A player should need to sell ~7 items to afford a basic medical supply.
//   - A gas mask should require multiple loot runs' worth of selling.
//   - Endgame gear is aspirational — weeks of trading, not hours.
//
// CURRENCY:
//   All tiers:   CJ187-Money-Dollars-Only (USD — standard currency)
//   Tier 9-10:   CJ187-Money-Bitcoin (Black Market — bought/sold for BTC)
//   Tier 11:     CJ187-Money-Bitcoin (Admin Only — sell only for BTC)
//   Heirloom:    HeirloomToken (collectables / ultra-rare)
//   Zombies:     NOT in store (excluded entirely)
// ============================================================================

// Hardcore sell ratio: players get back 15% of an item's buy price.
// This is the #1 lever for economy difficulty. Lower = harder.
// 0.25 = vanilla-like, 0.15 = hardcore, 0.10 = ultra-punishing
#define SELL_RATIO 0.15

// Using util_str_contains_ci() from util.c — no local duplicate needed
// Using util_hash_string() from util.c — no local duplicate needed

// ============================================================================
// CATEGORY BASE PRICES
// ============================================================================
// Each category has a different base price. This creates natural variation:
// a T3 weapon costs more than a T3 hat, which costs more than T3 food.

typedef struct {
    const char *category;   // trader_cat name
    int base_price;         // base price at tier 1
} CategoryPriceEntry;

static const CategoryPriceEntry CATEGORY_PRICES[] = {
    // Weapons & Combat — HARDCORE: 2.5-3x vanilla pricing
    {"Assault Rifles",    2000},
    {"Sniper Rifles",     3000},
    {"Submachine Guns",   1200},
    {"Shotguns",          1000},
    {"Pistols",           600},
    {"Launchers",         12000},
    {"Melee Weapons",     200},
    {"Weapons",           1500},
    {"High-Tier Weapons", 5000},
    {"Magazines",         250},
    {"Ammunition",        75},
    {"Optics",            750},
    {"Suppressors",       1200},
    {"Weapon Attachments",500},
    
    // Gear & Clothing — HARDCORE: gear is precious
    {"Tactical Gear",     1000},
    {"Military Clothing", 750},
    {"Clothing",          120},
    {"Helmets",           500},
    {"Vests",             850},
    {"Backpacks",         600},
    {"Ghillie",           4000},
    
    // Survival — HARDCORE: even basics cost real money
    {"Food",              30},
    {"Medical",           100},
    {"Tools",             150},
    {"Navigation",        200},
    {"Containers",        250},
    
    // Vehicles — HARDCORE: vehicles are serious investments
    {"Cars",              10000},
    {"Trucks",            15000},
    {"Armoured Vehicles", 35000},
    {"Flight",            50000},
    {"Boats",             8000},
    {"Vehicle Parts",     500},
    {"Fishing Gear",      150},
    
    // Building — HARDCORE: base building is expensive
    {"Base Building",     400},
    {"Storage",           750},
    
    // Special
    {"Heirloom",          5000},
    {"Black Market",      8000},
    {"Miscellaneous",     100},
    {NULL, 0}
};

static int get_category_base_price(const char *trader_cat) {
    if (!trader_cat || !trader_cat[0]) return 50;
    for (int i = 0; CATEGORY_PRICES[i].category; i++) {
        if (util_strcasecmp(trader_cat, CATEGORY_PRICES[i].category) == 0)
            return CATEGORY_PRICES[i].base_price;
    }
    return 50; // fallback
}

// ============================================================================
// TIER MULTIPLIER
// ============================================================================
// Exponential scaling: each tier roughly doubles the price.
// T1=1x  T2=1.8x  T3=3.24x  T4=5.83x  T5=10.5x  T6=18.9x  T7=34x  ...

static double get_tier_multiplier(int tier) {
    // Precomputed 1.8^(tier-1) — avoids loop per call
    static const double TABLE[] = {
        1.0,          // T0 (fallback)
        1.0,          // T1
        1.8,          // T2
        3.24,         // T3
        5.832,        // T4
        10.4976,      // T5
        18.89568,     // T6
        34.012224,    // T7
        61.2220032,   // T8
        110.19960576, // T9
        198.35929037, // T10
        357.04672266, // T11
        642.68410079  // T12
    };
    if (tier < 0)  return 1.0;
    if (tier > 12) return TABLE[12];
    return TABLE[tier];
}

// ============================================================================
// RARITY MULTIPLIER (based on nominal count)
// ============================================================================

static double get_rarity_multiplier(const LootItem *item) {
    if (item->nominal == 0)  return 2.0;  // Store-only / unobtainable
    if (item->nominal <= 2)  return 1.5;  // Mythic rarity
    if (item->nominal <= 5)  return 1.25; // Very rare
    if (item->nominal <= 10) return 1.1;  // Rare
    if (item->nominal <= 25) return 1.0;  // Normal
    if (item->nominal <= 50) return 0.9;  // Common
    return 0.8;                           // Very common (bulk discount)
}

// ============================================================================
// BLACK MARKET VOLATILITY (pseudo-random price jitter for BM items)
// ============================================================================

static double get_bm_volatility(const LootItem *item) {
    if (!item->black_market) return 1.0;
    unsigned int h = util_hash_string(item->classname);
    double t = (double)(h % 1000) / 1000.0;
    return 0.85 + (t * 0.4); // 0.85x - 1.25x
}

// ============================================================================
// PRICE CALCULATOR
// ============================================================================

static int calculate_price(LootItem *item) {
    if (item->assigned_tier <= 0) return 50;
    
    int effective_tier = item->assigned_tier;
    
    // ── Vehicle tier cap ──
    // DayZ assigns Tier 9 to ALL vehicles in types.xml because they spawn
    // via events.xml, not the CE loot system.  Tier 9 is a technical marker,
    // not a rarity indicator.  Cap pricing tier so vehicles get sane prices.
    const char *tc = item->trader_cat;
    if (util_strcasecmp(tc, "Cars") == 0 ||
        util_strcasecmp(tc, "Boats") == 0) {
        if (effective_tier > 4) effective_tier = 4;
    } else if (util_strcasecmp(tc, "Trucks") == 0) {
        if (effective_tier > 5) effective_tier = 5;
    } else if (util_strcasecmp(tc, "Armoured Vehicles") == 0 ||
               util_strcasecmp(tc, "Flight") == 0) {
        if (effective_tier > 7) effective_tier = 7;
    } else if (util_strcasecmp(tc, "Vehicle Parts") == 0) {
        if (effective_tier > 3) effective_tier = 3;
    }
    
    int base = get_category_base_price(item->trader_cat);
    double price = (double)base;
    
    price *= get_tier_multiplier(effective_tier);
    price *= get_rarity_multiplier(item);
    price *= get_bm_volatility(item);
    
    // Admin items premium
    if (effective_tier == 11) price *= 2.0;
    
    int final_price = (int)price;
    if (final_price < 5) final_price = 5;
    if (final_price > 2000000) final_price = 2000000;
    
    // Round to nearest clean number based on magnitude
    if (final_price >= 10000) final_price = ((final_price + 500) / 1000) * 1000;
    else if (final_price >= 1000) final_price = ((final_price + 50) / 100) * 100;
    else if (final_price >= 100) final_price = ((final_price + 5) / 10) * 10;
    else if (final_price >= 10) final_price = ((final_price + 2) / 5) * 5;
    
    return final_price;
}

// ============================================================================
// CATEGORY DETECTION (expanded with many sub-categories)
// ============================================================================

static void determine_category(LootItem *item) {
    // Precompute lowercase classname once — avoids repeated tolower() in ~80 subcalls
    char cn[MAX_CLASSNAME_LEN];
    {
        const char *s = item->classname;
        char *d = cn;
        while (*s) { *d++ = (char)tolower((unsigned char)*s++); }
        *d = '\0';
    }
    int cnlen = (int)strlen(cn);
    // Macro: fast lowercase-only substring match (needle MUST be lowercase literal)
    #define CN(pat) strstr(cn, pat)

    // Start with default
    strncpy(item->trader_cat, "Miscellaneous", 63);
    
    // --- CURRENCY ITEMS (absolute highest priority) ---
    // Money items are the medium of exchange, not tradeable goods.
    // Match both underscore and hyphen variants (Money_Dollar vs Money-Dollar)
    if (CN("money_dollar") || CN("money-dollar") ||
        CN("money_euro")   || CN("money-euro")   ||
        CN("money_ruble")  || CN("money-ruble")  ||
        CN("money_bitcoin")|| CN("money-bitcoin")||
        CN("heirloomtoken")) {
        strncpy(item->trader_cat, "Currency", 63);
        return;
    }

    // --- PARAGON MOD ITEMS (before heirloom/weapon checks) ---
    // The Paragon mod family has two types:
    //   1. Paragon Collectables (T9, wrongly tagged category=weapons)
    //      Treasure items: Bitcoin, Crown, Diamonds, Crystals, GoldBar, etc.
    //   2. Paragon Storage (T8, no category)
    //      Safes, containers, lockers, fridges, gun racks, tents, etc.
    // Detect by "paragon_" prefix and route correctly.
    if (CN("paragon_")) {
        // Paragon Collectable items — treasure loot that spawns in the world
        if (CN("paragon_bitcoin") || CN("paragon_crown") ||
            CN("paragon_diamond") || CN("paragon_crystal") ||
            CN("paragon_cube") || CN("paragon_glowrock") ||
            CN("paragon_goldbar") || CN("paragon_gameboy") ||
            CN("paragon_lion") || CN("paragon_kaws") ||
            CN("paragon_axe") || CN("paragon_documents") ||
            CN("paragon_ds") || CN("paragon_intel") ||
            CN("paragon_canister")) {
            strncpy(item->trader_cat, "Heirloom", 63);
            return;
        }
        // Paragon Storage — safes, containers, lockers, fridges, gun racks
        if (CN("paragon_bigsafe") || CN("paragon_container") ||
            CN("paragon_locker") || CN("paragon_fridge") ||
            CN("paragon_guncase") || CN("paragon_dguncase") ||
            CN("paragon_gunrack") || CN("paragon_dgunrack") ||
            CN("paragon_gunwall") || CN("paragon_dumpster") ||
            CN("paragon_icebox") || CN("paragon_bigtent") ||
            CN("paragon_ic_freezer") || CN("paragon_mcabinet") ||
            CN("paragon_hotdog") || CN("paragon_greenhouse") ||
            CN("paragon_largegreenhouse") || CN("paragon_gearstand")) {
            strncpy(item->trader_cat, "Storage", 63);
            return;
        }
        // Paragon Base Building — doors, gates, walls, helipads
        if (CN("paragon_adoor") || CN("paragon_bdoor") ||
            CN("paragon_compound") || CN("paragon_helipad") ||
            CN("paragon_graffi")) {
            strncpy(item->trader_cat, "Base Building", 63);
            return;
        }
        // Catch-all for any remaining Paragon_ items
        strncpy(item->trader_cat, "Storage", 63);
        return;
    }

    // --- HEIRLOOM / COLLECTABLE DETECTION (highest priority) ---
    if (item->assigned_tier == 5 ||
        CN("o12_caps") ||
        CN("collectable") ||
        CN("collectible") ||
        CN("heirloom") ||
        (item->category[0] && (util_strcasecmp(item->category, "collectables") == 0 ||
                               util_strcasecmp(item->category, "collections") == 0))) {
        strncpy(item->trader_cat, "Heirloom", 63);
        return; // Don't override with anything else
    }
    
    // --- WEAPONS ---
    // Only match _gun suffix at END of classname to prevent false positives
    // (e.g. Mass_GunMittens contains _gun mid-string but is clothing)
    bool gun_suffix = (cnlen >= 4 && strcmp(cn + cnlen - 4, "_gun") == 0);
    if (strstr(item->category, "weapons") || gun_suffix) {
        // ── Non-weapon redirects ──
        // Items tagged "weapons" in XML but are actually clothing/gear/tools
        if (CN("ghillie"))
            strncpy(item->trader_cat, "Ghillie", 63);
        else if (CN("nvgoggles") || (cnlen >= 3 && CN("nvg") && !CN("nvga")))
            strncpy(item->trader_cat, "Navigation", 63);
        else if (CN("binoc"))
            strncpy(item->trader_cat, "Navigation", 63);
        else if (CN("iceaxe"))
            strncpy(item->trader_cat, "Melee Weapons", 63);
        else if (CN("mitten") || CN("clothing_"))
            strncpy(item->trader_cat, "Clothing", 63);
        // ── Ammunition ──
        else if (CN("ammo_") || CN("_ammo") || CN("ammobox"))
            strncpy(item->trader_cat, "Ammunition", 63);
        // ── Magazines ──
        // Catches: mag_ prefix, _mag suffix, ends-in-"mag", STANAG, drum,
        // round-count items (30rnd, 45rnd, etc.), ends-in-"box" (ppshbox)
        else if (CN("mag_") || CN("_mag") ||
                 (cnlen > 3 && strcmp(cn + cnlen - 3, "mag") == 0) ||
                 CN("stanag") || CN("drum") ||
                 CN("0rnd") || CN("5rnd") ||
                 (cnlen > 3 && strcmp(cn + cnlen - 3, "box") == 0))
            strncpy(item->trader_cat, "Magazines", 63);
        // ── Optics ──
        else if (CN("optic") || CN("acog") || CN("pso") || CN("scope") ||
                 CN("rds") || CN("reflex") || CN("holo") || CN("elcan") ||
                 CN("hamr") || CN("aimpoint") || CN("pka") || CN("1p69") ||
                 CN("bugbust") || CN("razor"))
            strncpy(item->trader_cat, "Optics", 63);
        // ── Suppressors ──
        else if (CN("suppressor") || CN("silencer") ||
                 (cnlen >= 5 && strcmp(cn + cnlen - 5, "_supp") == 0))
            strncpy(item->trader_cat, "Suppressors", 63);
        // ── Weapon Attachments ──
        // buttstock/bttstck, hndguard/hndgrd, grips, barrels, lights, etc.
        else if (CN("buttstock") || CN("bttstck") || CN("bttstk") ||
                 CN("hndguard") || CN("hndgrd") ||
                 CN("rail") || CN("bayonet") || CN("compensator") || CN("wrap") ||
                 CN("muzzle") || CN("bipod") || CN("fgrip") || CN("_afg") ||
                 CN("_vfg") || CN("_kac") || CN("light") || CN("stock") ||
                 CN("barrel_") || CN("rifleframe") || CN("flashhider"))
            strncpy(item->trader_cat, "Weapon Attachments", 63);
        // ── Launchers ──
        else if (CN("launcher") || CN("rpg") || CN("law") || CN("m79"))
            strncpy(item->trader_cat, "Launchers", 63);
        // ── Shotguns ──
        else if (CN("shotgun") || CN("saiga") || CN("vaiga") || CN("benellim4") ||
                 CN("aa12") || CN("mp133") || CN("mp153") || CN("spas") ||
                 CN("m870") || CN("maverick") || CN("mossberg") || CN("vr80"))
            strncpy(item->trader_cat, "Shotguns", 63);
        // ── Pistols ──
        else if (CN("pistol") || CN("glock") || CN("cz75") || CN("fnx") ||
                 CN("deagle") || CN("mlock") || CN("magnum") || CN("derringer") ||
                 CN("1911") || CN("makarov") || CN("flaregun") || CN("tec9") ||
                 CN("_m9") || CN("_usp") || CN("mp443") ||
                 CN("p320") || CN("kimber") ||
                 (strcmp(cn, "p1") == 0) ||
                 (strcmp(cn, "mkii") == 0))
            strncpy(item->trader_cat, "Pistols", 63);
        // ── Sniper Rifles / DMRs ──
        else if (CN("svd") || CN("mosin") || CN("winchester") || CN("cz527") ||
                 CN("cz550") || CN("scout") || CN("blaze") || CN("ssg") ||
                 CN("repeater") || CN("longhorn") || CN("springfield") ||
                 CN("kar98") || CN("enfield") || CN("garand") || CN("b95") ||
                 CN("awm") || CN("mrad") || CN("r700") || CN("m82") ||
                 CN("xm2010") || CN("sv98") || CN("m1903") || CN("mas36") ||
                 CN("m700") || CN("m200") || CN("m110") || CN("m14") ||
                 CN("hkg28") || CN("svt") || CN("svu") || CN("vss") ||
                 (CN("m24") && !CN("m249")))
            strncpy(item->trader_cat, "Sniper Rifles", 63);
        // ── Submachine Guns ──
        else if (CN("mp5") || CN("ump") || CN("aks74u") || CN("scorpion") ||
                 CN("bizon") || CN("pp19") || CN("ppsh") || CN("cz61") ||
                 CN("pm73") || CN("mp7") || CN("mpx") || CN("uzi") ||
                 CN("pp91") || CN("vityaz") || CN("mp40"))
            strncpy(item->trader_cat, "Submachine Guns", 63);
        // ── High-tier weapons ──
        else if (item->assigned_tier >= 8)
            strncpy(item->trader_cat, "High-Tier Weapons", 63);
        // ── Catch-all: Assault Rifles ──
        else
            strncpy(item->trader_cat, "Assault Rifles", 63);
    }
    // --- MELEE ---
    // Use specific patterns to avoid false positives:
    // "baseballbat" instead of "bat" (was matching Battery*, CombatBoots*)
    else if (CN("axe") || CN("machete") ||
             CN("baseballbat") || CN("cricketbat") ||
             CN("sword") || CN("knife") || CN("crowbar") ||
             CN("shovel") || CN("hammer") ||
             CN("pipe") || CN("wrench")) {
        strncpy(item->trader_cat, "Melee Weapons", 63);
    }
    // --- CLOTHING & GEAR ---
    else if (strstr(item->category, "clothes")) {
        if (CN("bag") || CN("pack") ||
            CN("pouch") || CN("alice") ||
            CN("tortilla") || CN("drybag") ||
            CN("smersh"))
            strncpy(item->trader_cat, "Backpacks", 63);
        else if (CN("helmet") || CN("mich") ||
                 CN("ballistichelm") || CN("greathelm") ||
                 CN("tacticalhelm"))
            strncpy(item->trader_cat, "Helmets", 63);
        else if (CN("vest") || CN("platecarrier") ||
                 CN("pressvest") || CN("highcap"))
            strncpy(item->trader_cat, "Vests", 63);
        else if (CN("ghillie"))
            strncpy(item->trader_cat, "Ghillie", 63);
        else if (item->assigned_tier >= 7)
            strncpy(item->trader_cat, "Military Clothing", 63);
        else
            strncpy(item->trader_cat, "Clothing", 63);
    }
    // --- VEHICLES ---
    // DayZ vehicles often have NO <category> tag in types.xml because they
    // spawn via events.xml, not the CE loot system.  We detect them by BOTH
    // the XML category tag AND classname patterns so nothing slips through.
    //
    // NOTE: Expansion mod classnames start with "Expansion" prefix.
    else if (strstr(item->category, "vehicles") ||
             // ── Vanilla vehicle bodies (no category tag) ──
             CN("civiliansedan") || CN("hatchback_02") ||
             CN("offroadhatchback") || CN("sedan_02") ||
             CN("truck_01") || CN("v3schassis") ||
             // ── Vanilla vehicle parts (doors/hoods/trunks with no category) ──
             CN("civsedandoors") || CN("civsedanhood") || CN("civsedantrunk") ||
             CN("hatchback_02_door") || CN("hatchback_02_hood") || CN("hatchback_02_trunk") ||
             CN("offroadhatchback_door") || CN("offroadhatchback_hood") || CN("offroadhatchback_trunk") ||
             CN("sedan_02_door") || CN("sedan_02_hood") || CN("sedan_02_trunk") ||
             CN("truck_01_door") || CN("truck_01_hood") ||
             // ── Modded vehicles ──
             CN("gunnertruck") || CN("oshkosh") ||
             // ── Expansion vehicles ──
             CN("expansionbus") || CN("expansiontractor") ||
             CN("expansionv3s") || CN("expansionural") ||
             CN("expansiontruck") || CN("expansionuh1h") ||
             CN("expansionmerlin") || CN("expansionmh6") ||
             CN("expansiongyro") || CN("expansionmi8") ||
             CN("expansionmi24") || CN("expansionces") ||
             CN("expansionan2") || CN("expansionvodnik") ||
             CN("expansionutility") || CN("expansionzodiac") ||
             CN("expansionlhd") || CN("expansionsailboat") ||
             CN("expansionhatchback") || CN("expansionseda") ||
             CN("expansionoffroad") || CN("expansionuaz") ||
             CN("expansionlandrover")) {
        // ── World wrecks — NOT tradeable ──
        if (CN("wreck_") || CN("_wreck")) {
            strncpy(item->trader_cat, "Miscellaneous", 63);
            item->buy_price = -1;
            item->sell_price = -1;
            return;
        }
        // Vehicle parts (wheels, doors, batteries, etc.)
        if (CN("wheel") || CN("tire") ||
            CN("door") || CN("hood") ||
            CN("trunk") || CN("battery") ||
            CN("plug") || CN("radiator") ||
            CN("sparkplug") || CN("headlight") ||
            CN("enginebelt") || CN("brakefluid") ||
            CN("motoroil") || CN("windscreen") ||
            CN("rotor") || CN("fueltank") ||
            CN("carcover") || CN("seatcover"))
            strncpy(item->trader_cat, "Vehicle Parts", 63);
        // Armoured vehicles (APCs, armoured cars, military vehicles)
        else if (CN("btr") || CN("brdm") || CN("bmp") ||
                 CN("humvee") || CN("mrap") || CN("apc") ||
                 CN("vodnik") || CN("lav") || CN("stryker") ||
                 CN("armou") ||
                 CN("t72") || CN("abrams") || CN("leopard") ||
                 CN("expansionvodnik"))
            strncpy(item->trader_cat, "Armoured Vehicles", 63);
        // Flight (helicopters, planes, gyrocopters)
        else if (CN("heli") || CN("uh1h") || CN("merlin") ||
                 CN("mh6") || CN("littlebird") || CN("mi8") ||
                 CN("mi24") || CN("gyro") || CN("cessna") ||
                 CN("an2") || CN("plane") || CN("aircraft") ||
                 CN("chinook") || CN("blackhawk") || CN("apache") ||
                 CN("huey") || CN("chopper") || CN("piper") ||
                 CN("expansionuh1h") || CN("expansionmerlin") ||
                 CN("expansionmh6") || CN("expansiongyro") ||
                 CN("expansionmi8") || CN("expansionmi24") ||
                 CN("expansionces") || CN("expansionan2"))
            strncpy(item->trader_cat, "Flight", 63);
        // Boats (all watercraft)
        else if (CN("boat") || CN("rib") || CN("sailboat") ||
                 CN("canoe") || CN("kayak") || CN("dinghy") ||
                 CN("yacht") || CN("fishing") || CN("zodiac") ||
                 CN("skiff") || CN("jetski") || CN("pontoon") ||
                 CN("raft") || CN("ship") || CN("trawler") ||
                 CN("speedboat") || CN("watercraft") ||
                 CN("expansionutility") || CN("expansionzodiac") ||
                 CN("expansionlhd") || CN("expansionsailboat"))
            strncpy(item->trader_cat, "Boats", 63);
        // Trucks (large transport, military trucks, buses)
        else if (CN("truck") || CN("v3s") || CN("ural") ||
                 CN("kamaz") || CN("oshkosh") || CN("gunner") ||
                 CN("apocalypse") || CN("mbm") ||
                 CN("bus") || CN("tractor") || CN("multicar") ||
                 CN("transit") || CN("van") || CN("cargo") ||
                 CN("flatbed") || CN("trailer") ||
                 CN("expansionbus") || CN("expansiontractor") ||
                 CN("expansionv3s") || CN("expansionural") ||
                 CN("expansiontruck"))
            strncpy(item->trader_cat, "Trucks", 63);
        // Cars (default — sedans, hatchbacks, SUVs, offroads, Expansion cars)
        else
            strncpy(item->trader_cat, "Cars", 63);
    }
    // --- EXPANSION NON-VEHICLE ITEMS ---
    // Expansion adds base building, navigation, and misc items that may not
    // have standard DayZ categories. Catch them by classname prefix.
    else if (CN("expansion")) {
        if (CN("expansioncodelocksmall") || CN("expansioncodelockbig") ||
            CN("expansionsafe") || CN("expansionbasebuilding") ||
            CN("expansionflag") || CN("expansionterritoryflg") ||
            CN("expansionwall") || CN("bb_") || CN("expansionfloor") ||
            CN("expansionstair") || CN("expansionramp") ||
            CN("expansionroof") || CN("expansiongate") ||
            CN("expansiondoor") || CN("expansionfence"))
            strncpy(item->trader_cat, "Base Building", 63);
        else if (CN("expansiongps") || CN("expansioncompass") ||
                 CN("expansionmap") || CN("expansionbinocular") ||
                 CN("personalstoragecontainer"))
            strncpy(item->trader_cat, "Navigation", 63);
        else if (CN("expansionspraypaint") || CN("expansioncarkey") ||
                 CN("expansionheroset") || CN("expansionbanditset"))
            strncpy(item->trader_cat, "Tools", 63);
        // Expansion weapons (if Expansion-Weapons was accidentally added)
        else if (CN("expansion_m16") || CN("expansion_ak") ||
                 CN("expansion_g36") || CN("expansion_mp") ||
                 CN("expansion_law") || CN("expansion_rpg"))
            strncpy(item->trader_cat, "Weapons", 63);
        // Catch-all for unclassified Expansion items
        else
            strncpy(item->trader_cat, "Miscellaneous", 63);
    }
    // --- FOOD ---
    else if (strstr(item->category, "food")) {
        // Bottle caps from Bottle Cap Collectables mod are in "food" category
        if (CN("o12_caps"))
            strncpy(item->trader_cat, "Heirloom", 63);
        // Bait, caught fish, worms — Harbour Master territory
        else if (CN("bait") || CN("worm") || CN("carp") ||
                 CN("mackerel") || CN("sardine") || CN("trout") ||
                 CN("salmon") || CN("catfish") || CN("pike"))
            strncpy(item->trader_cat, "Fishing Gear", 63);
        else
            strncpy(item->trader_cat, "Food", 63);
    }
    // --- TOOLS ---
    else if (strstr(item->category, "tools")) {
        if (CN("compass") || CN("map") ||
            CN("gps") || CN("binocular") ||
            CN("rangefinder"))
            strncpy(item->trader_cat, "Navigation", 63);
        else if (CN("carbattery") || CN("truckbattery"))
            strncpy(item->trader_cat, "Vehicle Parts", 63);
        // Fishing rods, hooks, tackle — Harbour Master territory
        else if (CN("fishingrod") || CN("hook") || CN("tackle") ||
                 CN("fishing") || CN("lure") || CN("bobber") ||
                 CN("net") || CN("sinker") || CN("reel"))
            strncpy(item->trader_cat, "Fishing Gear", 63);
        else
            strncpy(item->trader_cat, "Tools", 63);
    }
    // --- MEDICAL ---
    else if (CN("bandage") || CN("morphine") ||
             CN("epinephrine") || CN("saline") ||
             CN("blood") || CN("splint") ||
             CN("tetracycline") || CN("charcoal") ||
             CN("codeine") || CN("vitamins") ||
             CN("firstaid") || CN("syringe") ||
             CN("defibrillator") ||
             (item->category[0] && strstr(item->category, "medical"))) {
        strncpy(item->trader_cat, "Medical", 63);
    }
    // --- BASE BUILDING ---
    else if (CN("fence") || CN("watchtower") ||
             CN("gate") || CN("codelock") ||
             CN("combinationlock") || CN("nail") ||
             CN("metalplate")) {
        strncpy(item->trader_cat, "Base Building", 63);
    }
    // --- STORAGE ---
    else if (CN("barrel") || CN("seachest") ||
             CN("woodencrate") || CN("tent")) {
        strncpy(item->trader_cat, "Storage", 63);
    }
    // --- FALLBACK: Items with empty/wrong XML category ---
    // Catches items that slipped through all category-based branches
    else if (CN("mp153") || CN("mp133") || CN("shotgun") || CN("saiga") ||
             CN("m870") || CN("spas") || CN("vr80")) {
        strncpy(item->trader_cat, "Shotguns", 63);
    }
    else if (CN("boots") || CN("shoes") || CN("pants") || CN("jacket") ||
             CN("hoodie") || CN("shirt") || CN("gloves") || CN("mittens")) {
        strncpy(item->trader_cat, "Clothing", 63);
    }
    
    // --- TIER OVERRIDES (Black Market for T9+) ---
    // Vehicles are intentionally Tier 9 in DayZ types.xml but should stay
    // in their vehicle shops, not get dumped into Black Market.
    if (item->assigned_tier >= 9 &&
        util_strcasecmp(item->trader_cat, "Heirloom") != 0 &&
        util_strcasecmp(item->trader_cat, "Cars") != 0 &&
        util_strcasecmp(item->trader_cat, "Trucks") != 0 &&
        util_strcasecmp(item->trader_cat, "Armoured Vehicles") != 0 &&
        util_strcasecmp(item->trader_cat, "Flight") != 0 &&
        util_strcasecmp(item->trader_cat, "Boats") != 0 &&
        util_strcasecmp(item->trader_cat, "Vehicle Parts") != 0) {
        strncpy(item->trader_cat, "Black Market", 63);
    }
    
    // --- MOD-SPECIFIC OVERRIDES ---
    if (CN("mmg") && item->assigned_tier < 8 &&
        util_strcasecmp(item->trader_cat, "Heirloom") != 0) {
        strncpy(item->trader_cat, "Tactical Gear", 63);
    }
    if (CN("tf_") && item->assigned_tier < 8 &&
        util_strcasecmp(item->trader_cat, "Heirloom") != 0) {
        strncpy(item->trader_cat, "Military Clothing", 63);
    }

    #undef CN
}

// ============================================================================
// CURRENCY ITEM DETECTION
// ============================================================================
// Money items (Dollar bills, Bitcoin tokens) ARE the currency — they must
// never appear as buyable/sellable in the trader.  They spawn as loot only.

static bool is_currency_item(const LootItem *item) {
    if (!item || !item->classname[0]) return false;
    // All CJ187-MoreMoney denominations: Dollar, Euro, Ruble
    return (util_str_contains_ci(item->classname, "Money_Dollar") ||
            util_str_contains_ci(item->classname, "Money-Dollar") ||
            util_str_contains_ci(item->classname, "Money_Euro") ||
            util_str_contains_ci(item->classname, "Money-Euro") ||
            util_str_contains_ci(item->classname, "Money_Ruble") ||
            util_str_contains_ci(item->classname, "Money-Ruble") ||
            util_str_contains_ci(item->classname, "Money_Bitcoin") ||
            util_str_contains_ci(item->classname, "Money-Bitcoin") ||
            util_str_contains_ci(item->classname, "HeirloomToken"));
}

// ============================================================================
// CURRENCY ASSIGNMENT
// ============================================================================
// Standard currency: CJ187-Money-Dollars-Only (USD) for all tiers
// Heirloom Tokens:   Tier 5 and items tagged Heirloom
// Black Market BTC:  Tier 9-10 (bought & sold for Bitcoin)
// Admin BTC:         Tier 11 (sell-only for Bitcoin)
// Zombies:           Not in store at all
// Currency items:    NOT in store (they are the medium of exchange, not goods)

static void assign_store_flags(LootItem *item) {
    // ── Currency items are NOT tradeable ── they ARE the medium of exchange.
    // Mark them as unbuyable/unsellable so they never appear in the trader.
    if (is_currency_item(item)) {
        item->buy_price  = -1;
        item->sell_price = -1;
        item->black_market = false;
        item->admin_only   = false;
        item->stock_override  = 0;
        item->restock_override = 0;
        item->currency[0] = '\0'; // no trade currency — IS the currency
        return;
    }

    bool is_heirloom = (util_strcasecmp(item->trader_cat, "Heirloom") == 0);
    
    // Vehicle categories stay in their shops with USD, even at Tier 9
    bool is_vehicle_cat = (util_strcasecmp(item->trader_cat, "Cars") == 0 ||
                           util_strcasecmp(item->trader_cat, "Trucks") == 0 ||
                           util_strcasecmp(item->trader_cat, "Armoured Vehicles") == 0 ||
                           util_strcasecmp(item->trader_cat, "Flight") == 0 ||
                           util_strcasecmp(item->trader_cat, "Boats") == 0 ||
                           util_strcasecmp(item->trader_cat, "Vehicle Parts") == 0);
    
    // Black market: items whose tier is flagged black_market in the loot policy
    // (T11 by default). Bought AND sold at the Black Market in Bitcoin.
    item->black_market = (!is_heirloom && !is_vehicle_cat &&
                          lp_tier_black_market(item->assigned_tier));
    // Contraband / no-trade tiers are excluded from every shop
    // (see item_belongs_to_shop), so nothing is admin sell-only anymore.
    item->admin_only   = false;
    item->stock_override = 0;
    item->restock_override = 0;
    
    // Currency assignment — USD (CJ187-Money-Dollars-Only) is the standard
    if (is_heirloom) {
        strncpy(item->currency, "HeirloomToken", 63);
    } else if (item->black_market || item->admin_only) {
        strncpy(item->currency, "CJ187-Money-Bitcoin", 63);
    } else {
        strncpy(item->currency, "CJ187-Money-Dollars-Only", 63);
    }
    
    // Price assignments: SELL = SELL_RATIO of BUY (hardcore: 15%)
    if (item->admin_only) {
        item->buy_price = -1;  // Not buyable
        item->sell_price = item->calculated_price;
        item->stock_override = 1;
        item->restock_override = 7200;
    } else {
        item->buy_price = item->calculated_price;
        item->sell_price = (int)(item->calculated_price * SELL_RATIO);
        if (item->sell_price < 1 && item->calculated_price > 0) item->sell_price = 1;
    }
    
    // Heirloom stock: limited, slow restock
    if (is_heirloom) {
        item->stock_override = 3;
        item->restock_override = 3600;
    }
    
    // Black market: limited stock, medium restock
    if (item->black_market && !item->admin_only) {
        item->stock_override = 5;
        item->restock_override = 1800;
    }
    
    // Vehicles: limited stock, slow restock (big purchases, not candy)
    if (is_vehicle_cat && !is_heirloom) {
        if (util_strcasecmp(item->trader_cat, "Vehicle Parts") != 0) {
            // Whole vehicles: very limited
            item->stock_override = 2;
            item->restock_override = 3600;
        } else {
            // Vehicle parts: more available
            item->stock_override = 10;
            item->restock_override = 900;
        }
    }
}

// ============================================================================
// ECONOMY BALANCING: Adjust nominal/lifetime/restock by tier
// ============================================================================
// This ensures the loot pools are properly scaled — not just prices.
// Higher tiers = rarer spawns, longer persistence, slower restocking.

static void balance_economy_values(LootItem *item) {
    if (loot_is_zombie(item) || loot_is_animal(item)) return; // Handled by loot_manager
    if (item->deleted) return;
    
    // ── CURRENCY ITEMS: denomination-based spawn counts ──
    // Higher denominations are rarer.  All values stay in the 1-100 range.
    // Tiers are assigned in loot_get_mod_tier().
    if (is_currency_item(item)) {
        // Denomination-based spawn scaling for all currency types.
        // Pattern: extract the numeric suffix and scale by face value.
        // Dollar/Euro/Ruble all follow the same rarity curve.
        const char *cn = item->classname;
        if (util_str_contains_ci(cn, "5000") || util_str_contains_ci(cn, "2000")) {
            item->nominal = 3;   item->min = 1;   // Ultra-rare mega bills
        } else if (util_str_contains_ci(cn, "1000") || util_str_contains_ci(cn, "500") ||
                   util_str_contains_ci(cn, "Dollar100") || util_str_contains_ci(cn, "Euro200")) {
            item->nominal = 5;   item->min = 2;   // Very rare high bills
        } else if (util_str_contains_ci(cn, "Dollar50") || util_str_contains_ci(cn, "Euro100") ||
                   util_str_contains_ci(cn, "Ruble200")) {
            item->nominal = 8;   item->min = 3;
        } else if (util_str_contains_ci(cn, "Dollar20") || util_str_contains_ci(cn, "Euro50") ||
                   util_str_contains_ci(cn, "Ruble100")) {
            item->nominal = 15;  item->min = 5;
        } else if (util_str_contains_ci(cn, "Dollar10") || util_str_contains_ci(cn, "Euro20") ||
                   util_str_contains_ci(cn, "Ruble50")) {
            item->nominal = 25;  item->min = 10;
        } else if (util_str_contains_ci(cn, "Dollar5") || util_str_contains_ci(cn, "Euro10") ||
                   util_str_contains_ci(cn, "Ruble10")) {
            item->nominal = 40;  item->min = 15;
        } else if (util_str_contains_ci(cn, "Dollar2") || util_str_contains_ci(cn, "Euro5") ||
                   util_str_contains_ci(cn, "Ruble5")) {
            item->nominal = 60;  item->min = 25;
        } else if (util_str_contains_ci(cn, "Dollar1") || util_str_contains_ci(cn, "Euro2") ||
                   util_str_contains_ci(cn, "Euro1") || util_str_contains_ci(cn, "Ruble2") ||
                   util_str_contains_ci(cn, "Ruble1")) {
            item->nominal = 80;  item->min = 35;  // Most common
        } else {
            // Bitcoin / HeirloomToken / any other currency
            item->nominal = 3;   item->min = 1;
        }
        item->lifetime = 14400;  // 4 hours
        item->restock = 600;     // 10 min restock
        item->modified = true;
        return; // Skip generic balancing
    }

    int tier = item->assigned_tier;
    if (tier <= 0) tier = 1;
    
    // --- NOMINAL SCALING ---
    // Sets maximum spawned instances on the map.
    // T1: abundant, T11: near-zero
    // "Lots of loot": targets bumped for an abundant-but-hardcore economy. This is
    // a CAP (nominal only reduced if it exceeds target*3), so higher = more loot kept.
    static const int nominal_targets[] = {
        /*T0*/ 60, /*T1*/ 60, /*T2*/ 40, /*T3*/ 25, /*T4*/ 16,
        /*T5*/ 8,  /*T6*/ 12, /*T7*/ 8,  /*T8*/ 5,  /*T9*/ 3,
        /*T10*/ 2, /*T11*/ 1, /*T12*/ 1, /*T13*/ 1, /*T14*/ 1, /*T15*/ 1, /*T16*/ 1
    };
    int nom_target = (tier >= 0 && tier <= 16) ? nominal_targets[tier] : 1;
    
    // Only reduce nominal if it's way above target (don't increase low-set values)
    // Exception: Heirloom items are ALWAYS rare
    bool is_heirloom = (util_strcasecmp(item->trader_cat, "Heirloom") == 0);
    if (is_heirloom) {
        item->nominal = 2; // Ultra rare
        item->min = 1;
    } else if (item->nominal > nom_target * 3) {
        // Item has way too many spawns for its tier — cap it
        item->nominal = nom_target * 2;
        item->modified = true;
    }
    // Ensure min is reasonable relative to nominal
    if (item->nominal > 0 && item->min <= 0) {
        item->min = (int)(item->nominal * 0.5);
        if (item->min < 1) item->min = 1;
    }

    // No-spawn tiers (Black Market, Contraband, or any user no-spawn tier): force
    // nominal 0 so they never world-spawn (obtainable only via trader / banned).
    if (!lp_tier_spawns(tier)) {
        item->nominal = 0;
        item->min = 0;
        item->modified = true;
    }
    
    // --- LIFETIME SCALING ---
    // How long items persist on the ground before despawning.
    // T1: short (encourages looting), high tiers: long (reward finding them)
    static const int lifetime_targets[] = {
        /*T0*/ 3600, /*T1*/ 3600, /*T2*/ 5400, /*T3*/ 7200, /*T4*/ 10800,
        /*T5*/ 14400, /*T6*/ 14400, /*T7*/ 21600, /*T8*/ 28800, /*T9*/ 43200,
        /*T10*/ 86400, /*T11*/ 86400
    };
    int life_target = (tier <= 11) ? lifetime_targets[tier] : 7200;
    if (item->lifetime < life_target) {
        item->lifetime = life_target;
        item->modified = true;
    }
    
    // --- RESTOCK SCALING ---
    // How often the CE can respawn this item (seconds).
    // T1: fast respawn, high tiers: slow (maintain rarity)
    static const int restock_targets[] = {
        /*T0*/ 0, /*T1*/ 0, /*T2*/ 300, /*T3*/ 600, /*T4*/ 900,
        /*T5*/ 1800, /*T6*/ 1200, /*T7*/ 1800, /*T8*/ 2400, /*T9*/ 3600,
        /*T10*/ 7200, /*T11*/ 14400
    };
    int restock_target = (tier <= 11) ? restock_targets[tier] : 600;
    if (item->restock < restock_target) {
        item->restock = restock_target;
        item->modified = true;
    }
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

void auditor_generate_store_data(AuditorContext *ctx) {
    if (!ctx) return;
    util_log(SEVERITY_INFO, "Economy Engine: Pricing %d items with category-tiered formula...", ctx->item_count);
    
    int priced = 0, heirloom_count = 0, bm_count = 0;
    
    for (int i = 0; i < ctx->item_count; i++) {
        LootItem *item = &ctx->items[i];
        if (item->deleted) continue;
        if (loot_is_zombie(item) || loot_is_animal(item)) continue;
        
        determine_category(item);
        item->calculated_price = calculate_price(item);
        assign_store_flags(item);
        balance_economy_values(item);
        
        priced++;
        if (util_strcasecmp(item->trader_cat, "Heirloom") == 0) heirloom_count++;
        if (item->black_market) bm_count++;
    }
    
    util_log(SEVERITY_INFO, "Economy Engine: Priced %d items. Heirloom: %d, Black Market: %d", priced, heirloom_count, bm_count);
}
