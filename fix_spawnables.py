#!/usr/bin/env python3
"""
Stelliferum Spawnable Type Fixer
=================================
Cleans cfgspawnabletypes.xml output by:
1. Removing oversized items from small/medium container cargo
2. Removing type-mismatched cargo (e.g. non-filters in gas masks)
3. Removing items that should never appear in cargo (vehicle parts, etc.)
4. Removing cargo blocks from items that should have no cargo
5. Removing empty <cargo> blocks left after cleanup

Creates a backup before writing.
"""
import xml.etree.ElementTree as ET
import os
import shutil
import json
import re
from datetime import datetime

BASE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(BASE, "build", "bin", "Release", "output")
SPAWNABLE_PATH = os.path.join(OUTPUT, "cfgspawnabletypes.xml")
TYPES_PATH = os.path.join(OUTPUT, "merged_types.xml")
CAPACITY_PATH = os.path.join(BASE, ".phoenix", "item_capacity.json")

# ─── Item classification rules ───────────────────────────────────────────────

# Large tools that can't fit in ANY clothing cargo (6+ inventory slots)
LARGE_TOOLS = {
    "BarbedWire", "MetalWire", "Broom", "BaseballBat", "Crowbar",
    "Pickaxe", "Shovel", "Sledgehammer", "FarmingHoe", "PipeWrench",
    "Pipe", "GardenLime", "Fertilizer", "WoodenPlank", "LongWoodenStick",
    "FireExtinguisher", "PowerGenerator", "Spotlight", "BatteryCharger",
    "Sword", "Machete", "OrientalMachete",
}

# Vehicle parts that should never be in cargo
VEHICLE_PART_PATTERNS = [
    "CivSedan", "Offroad", "Hatchback", "Sedan02", "Bus_Wheel",
    "Truck_01_Wheel", "Truck_01_Door", "GunnerDoors", "GunnerWheel",
    "GunnerHood", "CarWheel", "CarDoor", "CarHood", "CarRadiator",
    "TruckBattery", "CarBattery", "SparkPlug", "HeadlightH7",
]

# Backpacks/bags that can't fit in small containers or pouches
BAG_PATTERNS = [
    "Backpack", "AliceBag", "MountainBag", "CoyoteBag", "TortillaBag",
    "HuntingBag", "AssaultBag", "DryBag", "DrysackBag", "FieldPack",
    "CanvasBag", "CourierBag", "ChildBag", "SchoolBag",
    "assault_pack", "mmps_bag", "supplybag", "mmps_Pack",
]

# Items that should have NO cargo at all (masks, glasses, etc.)
NO_CARGO_ITEMS = set()  # populated from Phoenix config

# Items with specific-only cargo
SPECIFIC_ONLY = {}  # populated from Phoenix config

# No-cargo patterns
NO_CARGO_PATTERNS = [
    "Balaclava", "SkiMask", "Bandana", "SurgicalMask", "MouthRag",
    "FaceCover", "facemask", "Gloves", "Armband", "Glasses", "Goggles",
    "Eyepatch", "EyeMask", "Headband", "Headtorch", "Watch", "Ring",
    "Gas_Mask", "GasMask", "HockeyMask", "SkullMask", "PaydayMask",
    "MimeMask", "NioshFace", "WeldingMask", "Plague_Mask",
]
NO_CARGO_BOOT_PATTERNS = [
    "Boots", "Shoes", "Sneakers", "JoggingShoes", "Wellies",
    "HikingBoots", "MilitaryBoots", "WorkingBoots", "CombatBoots",
]
NO_CARGO_HAT_PATTERNS = [
    "BaseballCap", "Beret", "Beanie", "Ushanka", "Boonie",
    "ZSh3PilotHelmet", "Wig", "NBCHood", "SantasHat", "WitchHat",
    "PumpkinHelmet", "DirtBikeHelmet",
]

# Container size classification
LARGE_CONTAINER_PATTERNS = [
    "Mountain", "Alice", "Coyote", "Tortilla", "FieldPack", "AssaultPack",
    "Drybag", "HuntingPack", "Barrel", "SeaChest", "WoodenCrate",
    "Tent", "Shelter", "Canopy", "FirstAidKit", "MedicalCase",
    "Protector", "AmmoBox", "AmmoCan",
]
MEDIUM_CONTAINER_PATTERNS = [
    "Vest", "PlateCarrier", "ChestRig", "Smersh",
    "Jacket", "Coat", "Hoodie", "Gorka", "Parka", "Windbreaker",
    "Firefighter", "Ghillie", "Pants", "Jeans", "Trousers", "BDU",
    "Cargopants", "Helmet", "Mich", "BallisticHelm", "TacticalHelm",
    "Holster", "Pouch",
]

# Tactical containers that CAN hold weapon attachments
TACTICAL_CONTAINER_PATTERNS = [
    "Vest", "PlateCarrier", "ChestRig", "Smersh", "Carrier", "Rig",
    "Backpack", "AliceBag", "MountainBag", "CoyoteBag", "HuntingBag",
    "AssaultBag", "FieldPack", "DryBag", "Tent", "SeaChest",
    "WoodenCrate", "Barrel", "AmmoBox", "AmmoCan",
    "Ratnik", "Molle", "6sh112",
]

# Known medical items (allowed in medical containers)
MEDICAL_ITEM_PATTERNS = [
    "Bandage", "Morphine", "Epinephrine", "Saline", "Tourniquet",
    "Tetracycline", "Codeine", "Charcoal", "Vitamins", "Vitamin",
    "Syringe", "BloodBag", "BloodTest", "Thermometer", "Splint",
    "Disinfectant", "IodineTincture", "PainKiller", "Painkiller",
    "AntiChemInjector", "StartKit", "Rag", "SewingKit",
    "PurificationTablets",
]

# Known ammo/mag items (allowed in ammo containers)
AMMO_ITEM_PATTERNS = [
    "Ammo", "Mag_", "_Mag", "Drum", "rdMag", "Stanag", "Clip_", "_Clip",
]

# Weapon attachment patterns
WEAPON_ATTACHMENT_PATTERNS = [
    "Bttstck", "Hndgrd", "Bayonet", "Bipod", "_Suppressor",
    "Compensator", "RifleSling", "PistolGrip", "PBS1",
]


def matches_any(cn, patterns):
    cl = cn.lower()
    return any(p.lower() in cl for p in patterns)


def is_no_cargo(cn):
    if cn in NO_CARGO_ITEMS:
        return True
    return (matches_any(cn, NO_CARGO_PATTERNS) or
            matches_any(cn, NO_CARGO_BOOT_PATTERNS) or
            matches_any(cn, NO_CARGO_HAT_PATTERNS))


def container_size(cn):
    if matches_any(cn, LARGE_CONTAINER_PATTERNS):
        return "LARGE"
    if matches_any(cn, MEDIUM_CONTAINER_PATTERNS):
        return "MEDIUM"
    return "SMALL"


def is_large_tool(item_name):
    return item_name in LARGE_TOOLS


def is_vehicle_part(item_name):
    return matches_any(item_name, VEHICLE_PART_PATTERNS)


def is_bag(item_name):
    return matches_any(item_name, BAG_PATTERNS)


def is_large_weapon(item_name, categories):
    cat = categories.get(item_name, "")
    cl = item_name.lower()
    # Skip mags, optics, attachments
    if any(skip in cl for skip in ["mag_", "_mag", "optic", "bttstck", "hndgrd",
                                    "suppressor", "compensator", "bayonet", "bipod",
                                    "sling", "grip", "drum", "ammo"]):
        return False
    if cat == "weapons":
        # Small weapons are OK
        small = ["pistol", "glock", "cz75", "fnx", "deagle", "mlock",
                 "magnum", "derringer", "p1", "mk2", "flare", "skorpion", "mp5k"]
        if any(s in cl for s in small):
            return False
        return True
    # Weapon-like classnames
    if any(w in cl for w in ["_gun", "rifle", "shotgun", "launcher"]):
        if not any(skip in cl for skip in ["mag_", "_mag", "optic", "sling", "bttstck", "hndgrd"]):
            return True
    return False


def is_gas_mask(cn):
    cl = cn.lower()
    return ("gasmask" in cl or "gas_mask" in cl or "airbornemask" in cl or "gp5" in cl)


def should_remove_from_cargo(container_name, item_name, categories, csize):
    """Returns (should_remove, reason) tuple."""
    # Vehicle parts: ALWAYS wrong
    if is_vehicle_part(item_name):
        return True, "vehicle_part"

    # Gas masks: only GasMask_Filter allowed
    if is_gas_mask(container_name):
        if "filter" not in item_name.lower():
            return True, "gas_mask_wrong_cargo"

    # Specific-only containers
    if container_name in SPECIFIC_ONLY:
        allowed = [a.lower() for a in SPECIFIC_ONLY[container_name]]
        if item_name.lower() not in allowed and not any(a in item_name.lower() for a in allowed):
            return True, "specific_only_violation"

    # Medical containers: ONLY medical items allowed
    cl = container_name.lower()
    is_medical = any(m.lower() in cl for m in ["FirstAidKit", "MedicalCase", "Medkit", "medbag"])
    if is_medical:
        if not matches_any(item_name, MEDICAL_ITEM_PATTERNS):
            return True, "non_medical_in_medkit"

    # Ammo containers: ONLY ammo/mags allowed (plain AmmoBox/AmmoCan, not AmmoBox_556_20Rnd)
    is_ammo_cont = ("ammobox" in cl or "ammocan" in cl) and "_" not in container_name.split("AmmoBox")[-1].split("AmmoCan")[-1][:1]
    # Simpler: just check if it's exactly "AmmoBox" or "AmmoCan" (no additional suffix like _556)
    is_ammo_cont = container_name in ("AmmoBox", "AmmoCan")
    if is_ammo_cont:
        if not matches_any(item_name, AMMO_ITEM_PATTERNS):
            return True, "non_ammo_in_ammobox"

    # Weapon attachments in non-tactical clothing
    il = item_name.lower()
    is_weapon_att = any(p.lower() in il for p in WEAPON_ATTACHMENT_PATTERNS)
    if is_weapon_att:
        if not matches_any(container_name, TACTICAL_CONTAINER_PATTERNS):
            return True, "weapon_att_in_clothing"

    # Size-based: small containers
    if csize == "SMALL":
        if is_large_tool(item_name) or is_large_weapon(item_name, categories) or is_bag(item_name):
            return True, "too_large_for_small"

    # Size-based: medium containers
    if csize == "MEDIUM":
        if is_large_tool(item_name) or is_large_weapon(item_name, categories) or is_bag(item_name):
            return True, "too_large_for_medium"

    # Holster/pouch: no bags, no drum mags, no large weapons, no rifles, no flags, no food, no bark
    if matches_any(container_name, ["Holster", "Pouch"]):
        if is_bag(item_name):
            return True, "bag_in_pouch"
        # Small bags that also shouldn't be in pouches
        if any(b in il for b in ["duffelbag", "furimprovisedbag", "avsbag",
                                   "camelbakbag", "mapbag", "afakmedpouch"]):
            return True, "bag_in_pouch"
        # Drum mags are too large for pouches
        if "drum" in il:
            return True, "drum_in_pouch"
        # Rifles and large weapons can't go in pouches
        if is_large_weapon(item_name, categories):
            return True, "weapon_in_pouch"
        # Weapon attachments (buttstock, handguard, suppressor, compensator) too big for pouches
        if any(att in il for att in ["bttstck", "hndgrd", "riflesling",
                                      "suppressor", "compensator", "bayonet",
                                      "pistolgrip", "pbs1"]):
            return True, "attachment_in_pouch"
        # Flags shouldn't be in pouches
        if il.startswith("flag_"):
            return True, "flag_in_pouch"
        # Food/steaks/blood bags don't belong in holsters/pouches
        if any(food in il for food in ["steakmeat", "bloodbagfull", "bark_",
                                         "bakedbeanscan", "cannedtuna"]):
            return True, "food_in_pouch"

    return False, ""


def main():
    # ─── Load Phoenix capacity config ─────────────────────────────────
    global NO_CARGO_ITEMS, SPECIFIC_ONLY
    if os.path.exists(CAPACITY_PATH):
        with open(CAPACITY_PATH, "r") as f:
            cap = json.load(f)
        rules = cap.get("rules", {})
        NO_CARGO_ITEMS = set(rules.get("no_cargo", {}).get("items", []))
        spec_items = rules.get("specific_only", {}).get("items", {})
        for item_name, item_data in spec_items.items():
            SPECIFIC_ONLY[item_name] = item_data.get("cargo", [])

    # ─── Load categories from merged_types.xml ────────────────────────
    categories = {}
    if os.path.exists(TYPES_PATH):
        tree = ET.parse(TYPES_PATH)
        for typ in tree.getroot().findall("type"):
            name = typ.get("name", "")
            cat_el = typ.find("category")
            if cat_el is not None:
                categories[name] = cat_el.get("name", "")

    # ─── Parse cfgspawnabletypes.xml ──────────────────────────────────
    if not os.path.exists(SPAWNABLE_PATH):
        print(f"ERROR: {SPAWNABLE_PATH} not found!")
        return

    # Backup
    backup_path = SPAWNABLE_PATH + f".backup_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    shutil.copy2(SPAWNABLE_PATH, backup_path)
    print(f"Backup created: {backup_path}")

    # We need to do line-level XML editing to preserve formatting
    # (ElementTree would reformat the whole file)
    with open(SPAWNABLE_PATH, "r", encoding="utf-8") as f:
        content = f.read()

    # Parse with ElementTree for analysis, then rebuild
    tree = ET.parse(SPAWNABLE_PATH)
    root = tree.getroot()

    stats = {
        "cargo_items_removed": 0,
        "cargo_blocks_removed": 0,
        "full_cargo_removed": 0,  # entire cargo section removed (no-cargo items)
        "types_fixed": set(),
        "reasons": {},
    }

    types_to_rebuild = []

    for typ in root.findall("type"):
        name = typ.get("name", "")
        if not name:
            continue

        modified = False
        csize = container_size(name)

        # ── Check 1: Remove all cargo from no-cargo items ──
        if is_no_cargo(name) and name not in SPECIFIC_ONLY:
            cargo_blocks = typ.findall("cargo")
            if cargo_blocks:
                for cb in cargo_blocks:
                    typ.remove(cb)
                modified = True
                stats["full_cargo_removed"] += 1
                stats["types_fixed"].add(name)
                stats["reasons"]["no_cargo"] = stats["reasons"].get("no_cargo", 0) + 1

        # ── Check 2: Fix specific-only containers ──
        if name in SPECIFIC_ONLY:
            allowed = [a.lower() for a in SPECIFIC_ONLY[name]]
            for cb in list(typ.findall("cargo")):
                items_to_remove = []
                for item in cb.findall("item"):
                    item_name = item.get("name", "")
                    if item_name.lower() not in allowed and not any(
                            a in item_name.lower() for a in allowed):
                        items_to_remove.append(item)
                for item in items_to_remove:
                    cb.remove(item)
                    modified = True
                    stats["cargo_items_removed"] += 1
                    stats["types_fixed"].add(name)
                    stats["reasons"]["specific_only"] = stats["reasons"].get("specific_only", 0) + 1
                # If cargo block is now empty, remove it
                if len(cb.findall("item")) == 0:
                    typ.remove(cb)
                    stats["cargo_blocks_removed"] += 1

        # ── Check 3: Remove invalid items from cargo ──
        for cb in list(typ.findall("cargo")):
            items_to_remove = []
            for item in cb.findall("item"):
                item_name = item.get("name", "")
                should_remove, reason = should_remove_from_cargo(
                    name, item_name, categories, csize)
                if should_remove:
                    items_to_remove.append(item)
                    stats["cargo_items_removed"] += 1
                    stats["reasons"][reason] = stats["reasons"].get(reason, 0) + 1

            for item in items_to_remove:
                cb.remove(item)
                modified = True
                stats["types_fixed"].add(name)

            # Remove empty cargo blocks
            if len(cb.findall("item")) == 0:
                typ.remove(cb)
                stats["cargo_blocks_removed"] += 1

        if modified:
            types_to_rebuild.append(name)

    # ─── Write fixed XML ──────────────────────────────────────────────
    # Use custom serialization to match the original formatting style
    lines = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>']
    lines.append("<!-- Generated by StelliferumAuditor — Merged Spawnable Types -->")
    lines.append("<!-- Post-processed by spawnable fixer: removed invalid cargo items -->")
    lines.append("<spawnabletypes>")

    for typ in root.findall("type"):
        name = typ.get("name", "")
        cargo_blocks = typ.findall("cargo")
        attach_blocks = typ.findall("attachments")

        lines.append(f'    <type name="{name}">')

        for ab in attach_blocks:
            chance = ab.get("chance", "0.50")
            lines.append(f'    <attachments chance="{chance}">')
            for item in ab.findall("item"):
                iname = item.get("name", "")
                ichance = item.get("chance", "0.50")
                ihealth = item.get("health", "0.20,0.85")
                lines.append(f'        <item name="{iname}" chance="{ichance}" health="{ihealth}" />')
            lines.append("    </attachments>")

        for cb in cargo_blocks:
            chance = cb.get("chance", "0.30")
            items = cb.findall("item")
            if not items:
                continue
            lines.append(f'    <cargo chance="{chance}">')
            for item in items:
                iname = item.get("name", "")
                ichance = item.get("chance", "0.03")
                ihealth = item.get("health", "0.04,0.33")
                lines.append(f'        <item name="{iname}" chance="{ichance}" health="{ihealth}" />')
            lines.append("    </cargo>")

        lines.append("</type>")

    lines.append("</spawnabletypes>")

    with open(SPAWNABLE_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    # ─── Report ───────────────────────────────────────────────────────
    print(f"\n{'=' * 60}")
    print(" SPAWNABLE FIX SUMMARY")
    print(f"{'=' * 60}")
    print(f"Types modified:          {len(stats['types_fixed'])}")
    print(f"Cargo items removed:     {stats['cargo_items_removed']}")
    print(f"Cargo blocks removed:    {stats['cargo_blocks_removed']}")
    print(f"Full cargo removed:      {stats['full_cargo_removed']} (no-cargo items)")
    print(f"\nRemoval reasons:")
    for reason, count in sorted(stats["reasons"].items(), key=lambda x: -x[1]):
        print(f"  {reason:30s} {count:>5d}")
    print(f"\nFixed file written to: {SPAWNABLE_PATH}")
    print(f"Backup at: {backup_path}")


if __name__ == "__main__":
    main()
