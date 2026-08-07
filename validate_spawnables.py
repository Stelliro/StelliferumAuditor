#!/usr/bin/env python3
"""
Stelliferum Spawnable Type Validator
=====================================
Validates cfgspawnabletypes.xml for:
1. Empty type blocks (no attachments/cargo)
2. No-cargo items that incorrectly have cargo
3. Specific-only items with wrong cargo contents
4. Oversized items spawning in undersized containers
5. Type-mismatched cargo (e.g. cloth in gas mask filter slot)

Uses merged_types.xml for category/size data and .phoenix/item_capacity.json for rules.
"""
import xml.etree.ElementTree as ET
import json
import os
import re
from collections import defaultdict

# ─── Paths ───────────────────────────────────────────────────────────────────
BASE = os.path.dirname(os.path.abspath(__file__))
OUTPUT = os.path.join(BASE, "build", "bin", "Release", "output")
SPAWNABLE_PATH = os.path.join(OUTPUT, "cfgspawnabletypes.xml")
TYPES_PATH = os.path.join(OUTPUT, "merged_types.xml")
CAPACITY_PATH = os.path.join(BASE, ".phoenix", "item_capacity.json")
REPORT_PATH = os.path.join(OUTPUT, "spawnable_validation_report.txt")

# ─── DayZ Item Size Database (slots WxH) ────────────────────────────────────
# Items that take 2x3=6 or more slots generally can't fit in small containers
# This is a curated reference of known large items
LARGE_ITEMS = {
    # Rifles & large weapons (typically 1x8 to 2x5 = 8-10 slots)
    "weapon", "gun", "rifle", "shotgun", "launcher",
}

# Items known to be very large (>= 6 slots inventory footprint)
KNOWN_LARGE_CLASSNAMES = {
    # Rifles (1x8+)
    "Mosin9130", "SVD", "M70Tundra", "CZ527", "CZ550", "Winchester70", "Repeater",
    "AK101", "AK74", "AKM", "AKS74U", "AK74_Green", "AK74_Black",
    "AK101_Black", "AK101_Green", "AKM_Black", "AKM_Green",
    "M4A1", "M16A2", "FAL", "SKS", "VSS", "VSD", "Vaiga",
    "Saiga",
    "ACR_Gun", "AA12_Gun", "SNAFU_AK12", "SNAFU_AK15",
    # Shotguns
    "Izh18", "Izh43", "MP133Shotgun", "Saiga",
    # Large tools (pickaxe, shovel, etc.)
    "Pickaxe", "Shovel", "FarmingHoe", "Sledgehammer",
    "Crowbar", "BaseballBat", "PipeWrench", "Sword",
    # Backpacks (can't go in cargo of most things)
    "TortillaBag", "AliceBag", "MountainBag", "HuntingBag",
    "CoyoteBag", "AssaultBag", "DryBag",
    # Storage (can't go in cargo)
    "Barrel", "SeaChest", "WoodenCrate",
    "LargeTent", "MediumTent", "CarTent", "PartyTent",
    # Vehicle parts
    "CarWheel", "CarDoor", "CarHood", "TruckBattery",
}

# Items that are small (1x1 to 2x2 = 1-4 slots)
SMALL_ITEMS_PATTERNS = [
    "Ammo_", "_Ammo", "Mag_", "_Mag",
    "Bandage", "Rag", "Tourniquet", "Morphine", "Epinephrine",
    "Saline", "Tablet", "Vitamin", "Codeine", "Tetracycline",
    "Battery", "Matchbox", "Compass", "Lockpick",
    "GasMask_Filter", "WaterPurificationTablets",
    "Nail", "Bolt", "Hook", "BoneHook",
    "Sharpening", "SewingKit", "LeatherSewingKit",
    "PurificationTablets", "CharcoalTablets", "PainkillerTablets",
    "DisinfectantAlcohol", "IodineTincture",
    "Money_", "BottleCap",
]

# ─── No-Cargo Items/Patterns (from Phoenix config + gap_filler.c) ───────────
NO_CARGO_PATTERNS = [
    "Balaclava", "SkiMask", "Bandana", "SurgicalMask", "MouthRag", "FaceCover",
    "Gloves", "Armband", "Glasses", "Goggles", "Eyepatch", "Headband", "Headtorch",
    "Watch", "Ring",
]
NO_CARGO_BOOT_PATTERNS = [
    "Boots", "Shoes", "Sneakers", "JoggingShoes", "Wellies", "HikingBoots",
    "MilitaryBoots", "WorkingBoots", "CombatBoots",
]
NO_CARGO_HAT_PATTERNS = [
    "BaseballCap", "Beret", "Beanie", "Ushanka", "Boonie", "ZSh3PilotHelmet",
    "Wig", "NBCHood", "SantasHat", "WitchHat", "PumpkinHelmet", "DirtBikeHelmet",
]

# Items that cannot appear in ANY cargo (too large, base building, vehicles)
NEVER_IN_CARGO_PATTERNS = [
    "Barrel", "SeaChest", "WoodenCrate", "Tent", "Shelter", "Canopy",
    "Fence", "Watchtower", "CombinationLock",
    "CarWheel", "Tire", "CarDoor", "CarHood", "SparkPlug",
    "HeadlightH7", "CarRadiator", "TruckBattery", "CarBattery",
    "CivSedanDoors", "CivSedanWheel", "CivSedanHood", "CivSedanTrunk",
    "OffroadDoors", "OffroadWheel", "OffroadHood", "OffroadTrunk",
    "HatchbackDoors", "HatchbackHood", "HatchbackTrunk",
    "Sedan02Doors", "Sedan02Hood", "Sedan02Trunk", "Sedan02Wheel",
    "Bus_Wheel", "Truck_01_Wheel", "Truck_01_Door",
    "GunnerDoors", "GunnerWheel", "GunnerHood",
]

# Classification for container size (what kind of container this is)
LARGE_CONTAINER_PATTERNS = [
    "Mountain", "Alice", "Coyote", "Tortilla", "FieldPack", "AssaultPack",
    "Drybag", "HuntingPack", "Barrel", "SeaChest", "WoodenCrate",
    "Tent", "Shelter", "Canopy", "FirstAidKit", "MedicalCase",
    "Protector", "AmmoBox",
    # Modded large bags
    "assault_pack", "mmps_bag", "supplybag", "mmps_Pack",
    "MassCoyote", "MassWinter", "ShelterStick",
]
MEDIUM_CONTAINER_PATTERNS = [
    "Vest", "PlateCarrier", "ChestRig", "Smersh",
    "Jacket", "Coat", "Hoodie", "Gorka", "Parka", "Windbreaker",
    "Firefighter", "Ghillie",
    "Pants", "Jeans", "Trousers", "BDU", "Cargopants",
    "Helmet", "Mich", "BallisticHelm", "TacticalHelm",
    "Holster", "Pouch",
]
SMALL_CONTAINER_PATTERNS = [
    "Shirt", "TShirt", "Armband",
    "Hat", "Cap", "Beret", "Beanie",
]

# Specific-only items: items that should ONLY have very specific cargo
# (loaded from Phoenix config, supplemented by known DayZ vanilla rules)
SPECIFIC_ONLY_RULES = {
    "AirborneMask": ["GasMask_Filter"],
    "GP5GasMask": ["GasMask_Filter"],
    "GasMask": ["GasMask_Filter"],
    "FilteringBottle": ["WaterPurificationTablets"],
}

# Items that are clearly weapon attachments / parts - should NOT be in clothing cargo
ATTACHMENT_ONLY_ITEMS = [
    "Optic", "Bttstck", "Hndgrd", "Suppressor", "Compensator",
    "Bayonet", "Bipod", "Rail", "Wrap", "Silencer",
    "Light",  # weapon lights (TLRLight, UniversalLight) — but Flashlight is OK
]


def matches_any(classname, patterns):
    """Check if classname contains any pattern (case-insensitive)."""
    cl = classname.lower()
    return any(p.lower() in cl for p in patterns)


def is_no_cargo_item(classname, no_cargo_explicit):
    """Check if an item should have NO general cargo."""
    if classname in no_cargo_explicit:
        return True
    return (matches_any(classname, NO_CARGO_PATTERNS) or
            matches_any(classname, NO_CARGO_BOOT_PATTERNS) or
            matches_any(classname, NO_CARGO_HAT_PATTERNS))


def is_never_in_cargo(classname):
    """Check if an item should NEVER appear inside cargo."""
    return matches_any(classname, NEVER_IN_CARGO_PATTERNS)


def classify_container_size(classname):
    """Classify a container as LARGE/MEDIUM/SMALL."""
    if matches_any(classname, LARGE_CONTAINER_PATTERNS):
        return "LARGE"
    if matches_any(classname, MEDIUM_CONTAINER_PATTERNS):
        return "MEDIUM"
    return "SMALL"


def is_weapon(classname, categories):
    """Check if an item is a weapon based on types.xml category or name."""
    cl = classname.lower()
    # Ammo, magazines, optics, attachments, vehicles, and clothing are NOT weapons
    if any(skip in cl for skip in ["ammo_", "ammobox", "ammopile",
                                    "mag_", "_mag", "optic", "bttstck", "hndgrd",
                                    "suppressor", "compensator", "bayonet", "bipod",
                                    "sling", "grip", "wrap", "light", "nvgoggles",
                                    "nvgheadstrap", "drum",
                                    "truck", "sedan", "offroad", "hatchback", "bus_",
                                    "jltv", "door", "hood", "trunk", "wheel",
                                    "ghillie", "sheath", "holster", "pouch"]):
        return False
    cat = categories.get(classname, "")
    if cat == "weapons":
        return True
    weapon_keywords = ["_gun", "rifle", "shotgun", "pistol", "launcher",
                       "mosin", "svd", "tundra", "cz527", "cz550", "winchester",
                       "m4a1", "m16", "fal_", "sks_",
                       "vss_", "vsd_", "vaiga", "saiga_",
                       "mp5k", "ump45", "bizon", "skorpion", "mp133",
                       "izh18", "izh43", "repeater", "longhorn", "deagle",
                       "magnum", "fnx45", "glock", "cz75", "makarov", "cr75",
                       "colt1911", "engraved1911", "derringer", "flaregun",
                       "crossbow", "b95", "cz61", "asval",
                       "tf_m4a1", "tf_akmn", "tf_aks74u", "tf_adar",
                       "tf_m14_", "tf_mk47", "tf_scar", "tf_sr25", "tf_svch",
                       "tf_vpo", "tf_m79", "tf_fiveseven", "tf_mp443",
                       "tf_makarovpb",
                       "snafu_ak"]
    return any(w in cl for w in weapon_keywords)


def is_large_item(classname, categories):
    """Check if an item is too large for small/medium containers."""
    cl = classname.lower()
    # Ammo, magazines, optics, slings, grips, NVGs are NOT large
    if any(skip in cl for skip in ["ammo_", "ammobox", "ammopile",
                                    "mag_", "_mag", "optic", "sling", "grip",
                                    "nvgoggles", "nvgheadstrap", "bttstck", "hndgrd",
                                    "suppressor", "compensator", "bayonet", "bipod",
                                    "drum", "wrap", "filter", "tablet"]):
        return False
    if classname in KNOWN_LARGE_CLASSNAMES:
        return True
    # Rifles and large weapons
    if is_weapon(classname, categories):
        # Small weapons (pistols, SMGs, derringers) are OK in cargo
        small_weapon_keywords = ["pistol", "deagle", "magnum", "fnx45", "glock",
                                  "cz75", "makarov", "cr75", "derringer", "longhorn",
                                  "flaregun", "skorpion", "mp5k", "ump45", "bizon",
                                  "cz61", "colt1911", "engraved1911", "tiss",
                                  "tf_fiveseven", "tf_mp443", "tf_makarovpb",
                                  "p1"]
        if any(p in cl for p in small_weapon_keywords):
            return False
        return True  # All other weapons are large
    return False


def is_small_item(classname):
    """Check if an item is small (fits in any container)."""
    return matches_any(classname, SMALL_ITEMS_PATTERNS)


def is_wrong_type_for_container(container_name, cargo_item, categories):
    """
    Check if cargo_item is the wrong type for container_name.
    Returns (is_wrong, reason) tuple.
    """
    cn_lower = container_name.lower()
    ci_lower = cargo_item.lower()

    # === SPECIFIC-ONLY CONTAINERS ===
    for spec_container, allowed in SPECIFIC_ONLY_RULES.items():
        if spec_container.lower() in cn_lower or cn_lower in spec_container.lower():
            allowed_lower = [a.lower() for a in allowed]
            if ci_lower not in allowed_lower and not any(a in ci_lower for a in allowed_lower):
                return True, f"{container_name} can only hold {', '.join(allowed)}, not {cargo_item}"

    # === GAS MASKS (even variants) ===
    if "gasmask" in cn_lower or "gas_mask" in cn_lower or "airbornemask" in cn_lower or "gp5" in cn_lower or "pmk" in cn_lower:
        if "filter" not in ci_lower and "gasmask_filter" not in ci_lower:
            return True, f"Gas mask {container_name} can only hold GasMask_Filter, not {cargo_item}"

    # === AMMO BOXES should primarily hold ammo, not random items ===
    if "ammobox" in cn_lower or "ammocan" in cn_lower:
        if not any(pat.lower() in ci_lower for pat in ["ammo", "mag_", "_mag", "grenade", "mine"]):
            return True, f"Ammo container {container_name} should hold ammo/mags, not {cargo_item}"

    # === MEDICAL CONTAINERS ===
    if any(med in cn_lower for med in ["firstaidkit", "medicalcase", "medkit", "medbag"]):
        med_items = ["bandage", "morphine", "epinephrine", "saline", "tourniquet",
                     "tetracycline", "codeine", "charcoal", "vitamin", "syringe",
                     "bloodbag", "bloodtest", "thermometer", "splint", "disinfect",
                     "iodine", "painkiller", "anticheminjector", "rag",
                     "purification", "startkit"]
        if not any(m in ci_lower for m in med_items):
            return True, f"Medical container {container_name} should hold medical items, not {cargo_item}"

    # === HOLSTER / POUCHES - no large items ===
    if "holster" in cn_lower or "pouch" in cn_lower:
        # Magazines ARE allowed in pouches (that's their purpose)
        if "mag_" in ci_lower or "_mag" in ci_lower:
            pass  # Mags are fine in pouches
        elif any(big in ci_lower for big in ["rifle", "barrel", "tent", "backpack"]):
            return True, f"Holster/pouch {container_name} can't fit {cargo_item}"
        elif any(bag in ci_lower for bag in ["duffelbag", "furimprovisedbag", "avsbag",
                                              "camelbakbag", "mapbag", "afakmedpouch",
                                              "assault_pack", "mmps_bag", "supplybag"]):
            return True, f"Holster/pouch {container_name} can't fit bag {cargo_item}"
        elif any(att in ci_lower for att in ["bttstck", "hndgrd", "suppressor",
                                              "compensator", "bayonet", "riflesling",
                                              "pistolgrip", "pbs1"]):
            return True, f"Holster/pouch {container_name} can't fit attachment {cargo_item}"
        elif ci_lower.startswith("flag_"):
            return True, f"Holster/pouch {container_name} can't fit {cargo_item}"
        elif any(food in ci_lower for food in ["steakmeat", "bloodbagfull", "bark_",
                                                "bakedbeanscan"]):
            return True, f"Holster/pouch {container_name} can't fit food {cargo_item}"
        elif "drum" in ci_lower:
            return True, f"Holster/pouch {container_name} can't fit drum mag {cargo_item}"

    # === WEAPON in clothing cargo (not vests/packs) - weapons shouldn't spawn in shirts etc ===
    if is_weapon(cargo_item, categories) and not any(
        p.lower() in cn_lower for p in ["vest", "plate", "chest", "smersh",
                                         "pack", "bag", "barrel", "crate",
                                         "tent", "shelter", "case", "box",
                                         "coyote", "tortilla", "mountain",
                                         "alice", "hunting", "field"]):
        # Check if it's a large weapon (not pistol)
        if is_large_item(cargo_item, categories):
            return True, f"Large weapon {cargo_item} can't fit in {container_name} cargo"

    # === Large item in small container ===
    container_size = classify_container_size(container_name)
    if container_size == "SMALL" and is_large_item(cargo_item, categories):
        return True, f"Large item {cargo_item} in SMALL container {container_name}"

    # === Items that should NEVER be in cargo ===
    if is_never_in_cargo(cargo_item):
        return True, f"{cargo_item} should never appear in any cargo"

    # === Weapon attachments in non-weapon cargo (optics in pants, etc.) ===
    # These are attachments, not general items
    if not is_weapon(container_name, categories):
        att_keywords = ["Bttstck", "Hndgrd", "Suppressor", "Compensator", "Bayonet", "Bipod"]
        if any(att.lower() in ci_lower for att in att_keywords):
            # These are weapon-specific attachments, they shouldn't be in clothing cargo
            # Unless the container is a weapon case or similar
            if not any(w in cn_lower for w in ["case", "box", "crate", "barrel", "pack", "bag", "vest",
                                                       "plate", "chest", "smersh", "carrier", "rig",
                                                       "ratnik", "molle", "sea", "tent", "6sh112"]):
                return True, f"Weapon attachment {cargo_item} shouldn't be in {container_name} cargo"

    return False, ""


def main():
    issues = {
        "CRITICAL": [],   # Game-breaking: wrong items in specific-only containers
        "HIGH": [],       # Serious: oversized items in small containers
        "MEDIUM": [],     # Notable: type mismatches (ammo in medical, etc.)
        "LOW": [],        # Minor: empty blocks, minor affinity violations
        "INFO": [],       # Informational
    }

    # ─── Load Phoenix capacity config ────────────────────────────────────
    no_cargo_explicit = set()
    specific_only = {}

    if os.path.exists(CAPACITY_PATH):
        with open(CAPACITY_PATH, "r") as f:
            cap = json.load(f)
        rules = cap.get("rules", {})
        no_cargo_explicit = set(rules.get("no_cargo", {}).get("items", []))
        spec_items = rules.get("specific_only", {}).get("items", {})
        for item_name, item_data in spec_items.items():
            specific_only[item_name] = item_data.get("cargo", [])
            SPECIFIC_ONLY_RULES[item_name] = item_data.get("cargo", [])
        issues["INFO"].append(f"Loaded Phoenix config: {len(no_cargo_explicit)} no-cargo items, "
                              f"{len(specific_only)} specific-only items")
    else:
        issues["INFO"].append(f"WARNING: Phoenix config not found at {CAPACITY_PATH}")

    # ─── Parse merged_types.xml for categories ───────────────────────────
    categories = {}
    item_exists = set()

    if os.path.exists(TYPES_PATH):
        tree = ET.parse(TYPES_PATH)
        root = tree.getroot()
        for typ in root.findall("type"):
            name = typ.get("name", "")
            if not name:
                continue
            item_exists.add(name)
            cat_el = typ.find("category")
            if cat_el is not None:
                categories[name] = cat_el.get("name", "")

        issues["INFO"].append(f"Loaded {len(item_exists)} items from merged_types.xml, "
                              f"{len(categories)} with categories")
    else:
        issues["INFO"].append(f"WARNING: merged_types.xml not found at {TYPES_PATH}")

    # ─── Parse cfgspawnabletypes.xml ─────────────────────────────────────
    if not os.path.exists(SPAWNABLE_PATH):
        print(f"ERROR: {SPAWNABLE_PATH} not found!")
        return

    tree = ET.parse(SPAWNABLE_PATH)
    root = tree.getroot()

    total_types = 0
    empty_types = 0
    types_with_cargo = 0
    types_with_attachments = 0

    for typ in root.findall("type"):
        name = typ.get("name", "")
        if not name:
            continue
        total_types += 1

        cargo_blocks = typ.findall("cargo")
        attachment_blocks = typ.findall("attachments")

        has_cargo = len(cargo_blocks) > 0
        has_attachments = len(attachment_blocks) > 0

        if has_cargo:
            types_with_cargo += 1
        if has_attachments:
            types_with_attachments += 1

        # ── CHECK 1: Empty type blocks ──
        if not has_cargo and not has_attachments:
            empty_types += 1
            # Only flag if item is known to exist and should have something
            if is_weapon(name, categories):
                issues["MEDIUM"].append(
                    f"EMPTY BLOCK: Weapon '{name}' has no attachments or cargo defined")
            else:
                issues["LOW"].append(
                    f"EMPTY BLOCK: '{name}' has no attachments or cargo defined")

        # ── CHECK 2: No-cargo items with cargo blocks ──
        if has_cargo and is_no_cargo_item(name, no_cargo_explicit):
            # Check if it's a specific-only item (those are allowed specific cargo)
            if name not in specific_only:
                cargo_items_list = []
                for cb in cargo_blocks:
                    for item in cb.findall("item"):
                        cargo_items_list.append(item.get("name", ""))
                issues["HIGH"].append(
                    f"NO-CARGO VIOLATION: '{name}' should have NO cargo but has "
                    f"{len(cargo_items_list)} cargo items: {', '.join(cargo_items_list[:5])}"
                    f"{'...' if len(cargo_items_list) > 5 else ''}")

        # ── CHECK 3: Specific-only items with wrong cargo ──
        if name in specific_only and has_cargo:
            allowed = [a.lower() for a in specific_only[name]]
            for cb in cargo_blocks:
                for item in cb.findall("item"):
                    item_name = item.get("name", "")
                    if item_name.lower() not in allowed and not any(
                            a in item_name.lower() for a in allowed):
                        issues["CRITICAL"].append(
                            f"SPECIFIC-ONLY VIOLATION: '{name}' cargo contains "
                            f"'{item_name}' but only allows: {', '.join(specific_only[name])}")

        # ── CHECK 4: Validate each cargo item ──
        if has_cargo:
            for cb in cargo_blocks:
                for item in cb.findall("item"):
                    item_name = item.get("name", "")
                    if not item_name:
                        continue

                    # Items that should never be in cargo
                    if is_never_in_cargo(item_name):
                        issues["CRITICAL"].append(
                            f"NEVER-IN-CARGO: '{item_name}' found in cargo of '{name}'")

                    # Oversized items in containers
                    container_size = classify_container_size(name)
                    if is_large_item(item_name, categories):
                        if container_size == "SMALL":
                            issues["HIGH"].append(
                                f"SIZE MISMATCH: Large item '{item_name}' in SMALL "
                                f"container '{name}'")
                        elif container_size == "MEDIUM":
                            issues["MEDIUM"].append(
                                f"SIZE MISMATCH: Large item '{item_name}' in MEDIUM "
                                f"container '{name}'")

                    # Type mismatch checks
                    is_wrong, reason = is_wrong_type_for_container(
                        name, item_name, categories)
                    if is_wrong:
                        # Determine severity
                        if "gas mask" in reason.lower() or "specific" in reason.lower():
                            issues["CRITICAL"].append(f"TYPE MISMATCH: {reason}")
                        elif "can't fit" in reason.lower() or "never" in reason.lower():
                            issues["HIGH"].append(f"TYPE MISMATCH: {reason}")
                        else:
                            issues["MEDIUM"].append(f"TYPE MISMATCH: {reason}")

                    # Check if cargo item exists in types.xml
                    if item_exists and item_name not in item_exists:
                        issues["MEDIUM"].append(
                            f"MISSING ITEM: '{item_name}' referenced in cargo of "
                            f"'{name}' but not found in merged_types.xml")

        # ── CHECK 5: Validate each attachment item ──
        if has_attachments:
            for ab in attachment_blocks:
                for item in ab.findall("item"):
                    item_name = item.get("name", "")
                    if not item_name:
                        continue
                    # Check attachment exists
                    if item_exists and item_name not in item_exists:
                        issues["MEDIUM"].append(
                            f"MISSING ATTACHMENT: '{item_name}' referenced as "
                            f"attachment on '{name}' but not found in merged_types.xml")

    # ── CHECK 6: Scan for gas mask / filter issues across ALL entries ──
    gas_mask_names = ["AirborneMask", "GP5GasMask", "GasMask", "Gas_Mask", "PMK"]
    for typ in root.findall("type"):
        name = typ.get("name", "")
        cn_lower = name.lower()

        # Check all gas mask variants (color variants etc.)
        is_gas_mask = any(gm.lower() in cn_lower for gm in gas_mask_names)

        if is_gas_mask:
            for cb in typ.findall("cargo"):
                for item in cb.findall("item"):
                    item_name = item.get("name", "")
                    if "filter" not in item_name.lower():
                        issues["CRITICAL"].append(
                            f"GAS MASK VIOLATION: '{name}' has non-filter cargo item "
                            f"'{item_name}' — gas masks can ONLY hold GasMask_Filter")

    # ── CHECK 7: Look for items spawning inside items of wrong categories ──
    # E.g., BarbedWire in Leggings, Broom in Leggings
    problem_items_in_clothing = [
        "BarbedWire", "MetalWire", "WoodenPlank", "LongWoodenStick",
        "Broom", "Shovel", "Pickaxe", "Sledgehammer", "Crowbar",
        "FarmingHoe", "BaseballBat", "Pipe", "PipeWrench",
        "GardenLime", "Fertilizer",
    ]
    for typ in root.findall("type"):
        name = typ.get("name", "")
        cat = categories.get(name, "")
        if cat == "clothes":
            for cb in typ.findall("cargo"):
                for item in cb.findall("item"):
                    item_name = item.get("name", "")
                    if item_name in problem_items_in_clothing:
                        issues["HIGH"].append(
                            f"ABSURD CARGO: '{item_name}' (large/tool) spawning "
                            f"in clothing '{name}' cargo")

    # ─── Generate Report ─────────────────────────────────────────────────
    lines = []
    lines.append("=" * 72)
    lines.append(" STELLIFERUM SPAWNABLE TYPE VALIDATION REPORT")
    lines.append("=" * 72)
    lines.append("")
    lines.append(f"Total spawnable types:        {total_types}")
    lines.append(f"Types with cargo:             {types_with_cargo}")
    lines.append(f"Types with attachments:        {types_with_attachments}")
    lines.append(f"Empty type blocks:            {empty_types}")
    lines.append("")

    total_issues = sum(len(v) for v in issues.values() if v != issues["INFO"])
    lines.append(f"TOTAL ISSUES FOUND: {total_issues}")
    lines.append(f"  CRITICAL: {len(issues['CRITICAL'])}")
    lines.append(f"  HIGH:     {len(issues['HIGH'])}")
    lines.append(f"  MEDIUM:   {len(issues['MEDIUM'])}")
    lines.append(f"  LOW:      {len(issues['LOW'])}")
    lines.append("")

    for severity in ["CRITICAL", "HIGH", "MEDIUM", "LOW", "INFO"]:
        if issues[severity]:
            lines.append("-" * 72)
            lines.append(f" [{severity}] — {len(issues[severity])} issues")
            lines.append("-" * 72)
            # Deduplicate
            seen = set()
            for issue in sorted(issues[severity]):
                if issue not in seen:
                    seen.add(issue)
                    lines.append(f"  {issue}")
            lines.append("")

    report = "\n".join(lines)
    with open(REPORT_PATH, "w", encoding="utf-8") as f:
        f.write(report)

    print(report)
    print(f"\nReport saved to: {REPORT_PATH}")

    return issues


if __name__ == "__main__":
    main()
