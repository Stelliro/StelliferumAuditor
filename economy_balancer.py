#!/usr/bin/env python3
"""
STELLIFERUM AUTONOMOUS ECONOMY BALANCER
=======================================================
8-Step Hybrid AI-Driven Economy Management System

This system autonomously:
1. Loads Configuration (Vanilla + Mods)
2. Parses Inventory Data
3. Analyzes Distribution Metrics
4. Identifies Economy Gaps
5. Generates LLM Recommendations
6. Validates Changes Against Rules
7. Applies Modifications
8. Exports Results & Reports

Architecture: Hybrid Human-In-The-Loop + Autonomous AI
- Vanilla items serve as immutable baseline
- Mod items subject to autonomous optimization
- LLM (OLLAMA) provides reasoning for all changes
- All changes logged and reversible

v3.0 — Tier-rules-aware, full 11-tier support, statistical fallback,
        lifetime/restock/min analysis, economy_mode multipliers,
        tier overload + empty category detection, CSV column fix.
"""

import csv
import json
import os
import sys
import statistics
import logging
import hashlib
import shutil
import xml.etree.ElementTree as ET
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple, Optional
from enum import Enum
import requests

# ============================================================================
# CONFIGURATION & CONSTANTS
# ============================================================================

CONFIG_DIR = "config"
OUTPUT_DIR = "output"
DATA_DIR = "data"
HISTORY_DIR = "history"

OLLAMA_URL = os.getenv("OLLAMA_URL", "http://localhost:11434/api/generate")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "llama3")
OLLAMA_TIMEOUT = 30

# Maximum items to submit to LLM per run (0 = unlimited statistical-only)
MAX_LLM_ITEMS = int(os.getenv("BALANCER_LLM_LIMIT", "60"))

# TierLevel enum matching server tier_rules.json (T1-T11)
class TierLevel(Enum):
    TIER_1_SCAVENGER = 1    # Coastal civilian basics
    TIER_2_SURVIVOR = 2     # Village/Town civilian gear
    TIER_3_CONSTABLE = 3    # Police/Town mid-tier
    TIER_4_OUTDOORSMAN = 4  # Hunting/Farm specialist
    TIER_5_INSURGENT = 5    # Low-end military
    TIER_6_INFANTRY = 6     # Standard military
    TIER_7_SPECOPS = 7      # Spec-Ops rare military
    TIER_8_OPERATOR = 8     # Endgame operator
    TIER_9_BLACKMARKET = 9  # Black market / store-only
    TIER_10_MYTHIC = 10     # Mythic rarity
    TIER_11_ADMIN = 11      # Admin-only

# Zone classification for tier grouping
class TierZone(Enum):
    CIVILIAN = "CIVILIAN"
    MILITARY = "MILITARY"
    ENDGAME = "ENDGAME"
    ADMIN = "ADMIN"

class ItemSource(Enum):
    VANILLA = "types.xml"
    MOD = "mod"
    GENERATED = "generated"

class ChangeCategory(Enum):
    NOMINAL_ADJUSTMENT = "nominal"
    LIFETIME_ADJUSTMENT = "lifetime"
    RESTOCK_ADJUSTMENT = "restock"
    RARITY_ADJUSTMENT = "rarity"
    USAGE_MODIFICATION = "usage"
    SPAWNING_CHANGES = "spawning"
    BALANCING = "balancing"
    TIER_REASSIGNMENT = "tier"

# Economy mode multipliers — scale nominal targets
ECONOMY_MODE_MULTIPLIERS = {
    "casual":   1.5,   # 50% more loot
    "balanced": 1.0,   # Baseline
    "hardcore": 0.65,  # 35% less loot
    "grindy":   0.80,  # 20% less loot, slower restock
}

# Restock mode multipliers — scale restock timers
RESTOCK_MODE_MULTIPLIERS = {
    "casual":   0.6,   # Faster restock
    "balanced": 1.0,
    "hardcore": 1.4,   # Slower restock
    "grindy":   1.8,   # Much slower restock
}

# Tier overload threshold — flag if any tier has > this % of all items
TIER_OVERLOAD_THRESHOLD = 0.30  # 30%

# ============================================================================
# SETUP LOGGING
# ============================================================================

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s',
    handlers=[
        logging.FileHandler(f"{OUTPUT_DIR}/economy_balancer.log"),
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger("CORTEX_ECONOMY")

# ============================================================================
# TIER RULES LOADER
# ============================================================================

_tier_rules: Dict = {}

def load_tier_rules() -> Dict:
    """Load tier_rules.json and return the tiers dict keyed by int tier."""
    global _tier_rules
    path = f"{CONFIG_DIR}/tier_rules.json"
    if not os.path.exists(path):
        logger.warning(f"tier_rules.json not found at {path} — using built-in defaults")
        return {}
    with open(path, 'r', encoding='utf-8') as f:
        raw = json.load(f)
    tiers = {}
    for k, v in raw.get("tiers", {}).items():
        tiers[int(k)] = v
    _tier_rules = tiers
    logger.info(f"  Loaded {len(tiers)} tier definitions from tier_rules.json")
    return tiers

def get_tier_rule(tier: int) -> Dict:
    """Return the rule dict for a given tier, or sensible defaults."""
    if tier in _tier_rules:
        return _tier_rules[tier]
    return {
        "name": f"Tier{tier}",
        "zone": "CIVILIAN" if tier <= 4 else ("MILITARY" if tier <= 7 else "ENDGAME"),
        "nominal_min": 0,
        "nominal_max": 100,
        "lifetime_min": 3600,
        "lifetime_max": 86400,
        "allowed_usages": [],
        "forbidden_categories": [],
    }

# ============================================================================
# STEP 1: LOAD CONFIGURATION
# ============================================================================

def create_default_config() -> Dict:
    """Create default server profile if missing."""
    return {
        "version": "3.0",
        "server_name": "Stelliferum Forge",
        "playstyle": "Hardcore Survival",
        "economy_mode": "hardcore",  # balanced, hardcore, grindy, casual
        "economy_multiplier": 1.0,
        "military_loot_scarcity": "high",
        "mod_integration_strategy": "blend",
        "loot_distribution_model": "logarithmic",
        "autonomous_mode": True,
        "ai_decision_confidence_threshold": 0.75,
        "llm_item_limit": MAX_LLM_ITEMS,
        "changelog_enabled": True,
        "notes": "Autonomous economy management with LLM guidance — v3.0 tier-rules-aware"
    }

def load_config() -> Dict:
    """Load or create server configuration profile."""
    logger.info("=" * 70)
    logger.info("STEP 1: LOAD CONFIGURATION")
    logger.info("=" * 70)
    
    os.makedirs(CONFIG_DIR, exist_ok=True)
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    os.makedirs(HISTORY_DIR, exist_ok=True)
    
    profile_path = f"{CONFIG_DIR}/server_profile.json"
    
    if not os.path.exists(profile_path):
        logger.info("No profile found. Creating default...")
        profile = create_default_config()
        with open(profile_path, 'w') as f:
            json.dump(profile, f, indent=4)
        logger.info(f"✓ Default profile created: {profile_path}")
    else:
        with open(profile_path, 'r') as f:
            profile = json.load(f)
        logger.info(f"✓ Loaded profile: {profile['server_name']}")
    
    # Load tier rules (must happen early — used by analysis and validation)
    load_tier_rules()
    
    mode = profile.get('economy_mode', 'balanced')
    nom_mult = ECONOMY_MODE_MULTIPLIERS.get(mode, 1.0)
    restock_mult = RESTOCK_MODE_MULTIPLIERS.get(mode, 1.0)
    logger.info(f"  Mode: {mode} | Economy Multiplier: {profile.get('economy_multiplier', 1.0)}")
    logger.info(f"  Nominal scale: {nom_mult:.2f}x | Restock scale: {restock_mult:.2f}x")
    logger.info(f"  Autonomous: {'Yes' if profile.get('autonomous_mode') else 'No'}")
    return profile

# ============================================================================
# STEP 2: PARSE INVENTORY DATA
# ============================================================================

class InventoryItem:
    """Represents a single inventory item with full economy metadata."""
    def __init__(self, data: Dict, source: ItemSource):
        self.classname = data.get('Classname', 'unknown')
        self.category = data.get('Category', '').strip()
        self.tier = int(data.get('Tier', 4))
        self.nominal = int(data.get('Nominal', 10))
        self.lifetime = int(data.get('Lifetime', 3600))
        self.min = int(data.get('Min', 0))
        self.restock = int(data.get('Restock', 0))
        self.source = source
        self.source_file = data.get('SourceFile', '')
        self.mod_name = self._extract_mod_name(self.source_file)
        self.is_weapon = 'weapon' in self.category.lower() or '_gun' in self.classname.lower()
        self.is_disabled = (self.nominal == 0)

    @staticmethod
    def _extract_mod_name(source_file: str) -> str:
        """Extract mod name from source filename (e.g., '@SNAFU_Weapons__SNAFU_types.xml' -> 'SNAFU_Weapons')."""
        if not source_file:
            return 'N/A'
        if source_file.startswith('@'):
            parts = source_file.split('__', 1)
            return parts[0].lstrip('@')
        if source_file in ('types.xml', 'vanilla__types.xml'):
            return 'Vanilla'
        return source_file.replace('.xml', '')

    @property
    def tier_rule(self) -> Dict:
        return get_tier_rule(self.tier)

    @property
    def tier_zone(self) -> str:
        return self.tier_rule.get('zone', 'CIVILIAN')

    def to_dict(self) -> Dict:
        return {
            'classname': self.classname,
            'category': self.category,
            'tier': self.tier,
            'nominal': self.nominal,
            'min': self.min,
            'lifetime': self.lifetime,
            'restock': self.restock,
            'source': self.source.value,
            'mod_name': self.mod_name,
            'is_weapon': self.is_weapon,
            'is_disabled': self.is_disabled,
        }

def parse_inventory_data(csv_path: str) -> List[InventoryItem]:
    """Parse inventory CSV exported by the C++ auditor."""
    logger.info("=" * 70)
    logger.info("STEP 2: PARSE INVENTORY DATA")
    logger.info("=" * 70)
    
    if not os.path.exists(csv_path):
        logger.error(f"✗ Inventory file not found: {csv_path}")
        logger.info("  Run the C++ auditor to export inventory first.")
        return []
    
    items = []
    with open(csv_path, 'r', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        for row in reader:
            sf = row.get('SourceFile', '')
            source = ItemSource.VANILLA if sf in ('types.xml', 'vanilla__types.xml') else ItemSource.MOD
            items.append(InventoryItem(row, source))
    
    # Separate active items from disabled (nominal=0)
    active = [i for i in items if not i.is_disabled]
    disabled = [i for i in items if i.is_disabled]

    logger.info(f"✓ Parsed {len(items)} items ({len(active)} active, {len(disabled)} disabled/store-only)")
    
    vanilla_count = sum(1 for i in items if i.source == ItemSource.VANILLA)
    mod_count = sum(1 for i in items if i.source == ItemSource.MOD)
    logger.info(f"  Source split: {vanilla_count} vanilla, {mod_count} mod items")
    
    no_cat = sum(1 for i in items if not i.category)
    if no_cat:
        logger.warning(f"  ⚠ {no_cat} items have no category — pricing may be inaccurate")
    
    tier_dist = {}
    for item in items:
        tier_dist[item.tier] = tier_dist.get(item.tier, 0) + 1
    logger.info(f"  Tier distribution: {dict(sorted(tier_dist.items()))}")
    
    return items

# ============================================================================
# STEP 3: ANALYZE DISTRIBUTION METRICS
# ============================================================================

class DistributionAnalysis:
    """Statistical analysis of item distribution across all 11 tiers."""
    
    def __init__(self, items: List[InventoryItem], profile: Dict):
        self.items = items
        self.active_items = [i for i in items if not i.is_disabled]
        self.disabled_items = [i for i in items if i.is_disabled]
        self.profile = profile
        self.tier_stats = {}
        self.category_stats = {}
        self.weapon_distribution = None
        self.lifetime_stats = {}
        self.restock_stats = {}
        self.zone_stats = {}
        self.analyze()
    
    def analyze(self):
        """Compute statistical metrics across all 11 tiers."""
        # --- Tier analysis (T1-T11, active items only) ---
        for tier in range(1, 12):
            tier_items = [i for i in self.active_items if i.tier == tier]
            if tier_items:
                nominals = [i.nominal for i in tier_items]
                lifetimes = [i.lifetime for i in tier_items]
                restocks = [i.restock for i in tier_items]
                mins = [i.min for i in tier_items]
                rule = get_tier_rule(tier)
                self.tier_stats[tier] = {
                    'count': len(tier_items),
                    'pct': len(tier_items) / max(1, len(self.active_items)),
                    'name': rule.get('name', f'T{tier}'),
                    'zone': rule.get('zone', 'UNKNOWN'),
                    'nominal_mean': statistics.mean(nominals),
                    'nominal_median': statistics.median(nominals),
                    'nominal_stdev': statistics.stdev(nominals) if len(nominals) > 1 else 0,
                    'nominal_min': min(nominals),
                    'nominal_max': max(nominals),
                    'nominal_rule_min': rule.get('nominal_min', 0),
                    'nominal_rule_max': rule.get('nominal_max', 100),
                    'lifetime_mean': statistics.mean(lifetimes),
                    'lifetime_median': statistics.median(lifetimes),
                    'lifetime_rule_min': rule.get('lifetime_min', 3600),
                    'lifetime_rule_max': rule.get('lifetime_max', 86400),
                    'restock_mean': statistics.mean(restocks),
                    'min_mean': statistics.mean(mins),
                    'weapons': sum(1 for i in tier_items if i.is_weapon),
                    'no_category': sum(1 for i in tier_items if not i.category),
                    'vanilla': sum(1 for i in tier_items if i.source == ItemSource.VANILLA),
                    'mod': sum(1 for i in tier_items if i.source == ItemSource.MOD),
                }
        
        # --- Category analysis ---
        categories = set(i.category for i in self.active_items)
        for cat in categories:
            cat_items = [i for i in self.active_items if i.category == cat]
            nominals = [i.nominal for i in cat_items]
            self.category_stats[cat if cat else '(empty)'] = {
                'count': len(cat_items),
                'nominal_mean': statistics.mean(nominals),
                'nominal_median': statistics.median(nominals),
                'weapons': sum(1 for i in cat_items if i.is_weapon),
                'tier_spread': sorted(set(i.tier for i in cat_items)),
            }
        
        # --- Zone aggregation ---
        for zone in ('CIVILIAN', 'MILITARY', 'ENDGAME', 'ADMIN'):
            zone_items = [i for i in self.active_items if get_tier_rule(i.tier).get('zone') == zone]
            if zone_items:
                self.zone_stats[zone] = {
                    'count': len(zone_items),
                    'pct': len(zone_items) / max(1, len(self.active_items)),
                    'avg_nominal': statistics.mean([i.nominal for i in zone_items]),
                }
        
        # --- Weapon distribution (all 11 tiers) ---
        weapons = [i for i in self.active_items if i.is_weapon]
        if weapons:
            self.weapon_distribution = {
                'total': len(weapons),
                'by_tier': {t: len([w for w in weapons if w.tier == t]) for t in range(1, 12)},
                'avg_nominal': statistics.mean([w.nominal for w in weapons]),
                'civilian': sum(1 for w in weapons if w.tier <= 4),
                'military': sum(1 for w in weapons if 5 <= w.tier <= 7),
                'endgame': sum(1 for w in weapons if w.tier >= 8),
            }
    
    def report(self) -> str:
        """Generate analysis report."""
        report = "DISTRIBUTION ANALYSIS:\n"
        report += f"  Total Items: {len(self.items)} ({len(self.active_items)} active, {len(self.disabled_items)} disabled)\n\n"
        report += f"  {'Tier':<6} {'Name':<14} {'Zone':<10} {'Count':>6} {'%':>6} {'NomMean':>8} {'NomMed':>7} {'Rules':>12} {'Weapons':>8} {'NoCat':>6}\n"
        report += f"  {'-'*5:<6} {'-'*13:<14} {'-'*9:<10} {'-'*5:>6} {'-'*5:>6} {'-'*7:>8} {'-'*6:>7} {'-'*11:>12} {'-'*7:>8} {'-'*5:>6}\n"
        for tier in range(1, 12):
            s = self.tier_stats.get(tier, {})
            if s:
                report += (f"  T{tier:<4} {s['name']:<14} {s['zone']:<10} {s['count']:>6} "
                          f"{s['pct']*100:>5.1f}% {s['nominal_mean']:>8.1f} {s['nominal_median']:>7.0f} "
                          f"{s['nominal_rule_min']:>4}-{s['nominal_rule_max']:<6} {s['weapons']:>8} {s['no_category']:>6}\n")
            else:
                rule = get_tier_rule(tier)
                report += f"  T{tier:<4} {rule.get('name','?'):<14} {rule.get('zone','?'):<10} {'0':>6} {'0.0%':>6}\n"
        
        report += f"\n  Zone Summary:\n"
        for zone, zs in sorted(self.zone_stats.items()):
            report += f"    {zone:<10}: {zs['count']:>5} items ({zs['pct']*100:.1f}%), avg nominal {zs['avg_nominal']:.1f}\n"
        
        if self.weapon_distribution:
            wd = self.weapon_distribution
            report += f"\n  Weapons: {wd['total']} total (Civilian:{wd['civilian']} Military:{wd['military']} Endgame:{wd['endgame']})\n"
        return report

def analyze_distribution(items: List[InventoryItem], profile: Dict) -> DistributionAnalysis:
    """Analyze economy distribution across all 11 tiers."""
    logger.info("=" * 70)
    logger.info("STEP 3: ANALYZE DISTRIBUTION METRICS")
    logger.info("=" * 70)
    
    analysis = DistributionAnalysis(items, profile)
    logger.info(analysis.report())
    return analysis

# ============================================================================
# STEP 4: IDENTIFY ECONOMY GAPS
# ============================================================================

def identify_gaps(items: List[InventoryItem], analysis: DistributionAnalysis,
                  profile: Dict) -> List[Dict]:
    """Identify gaps and imbalances in economy structure using tier_rules.json."""
    logger.info("=" * 70)
    logger.info("STEP 4: IDENTIFY ECONOMY GAPS")
    logger.info("=" * 70)
    
    gaps = []
    active_items = [i for i in items if not i.is_disabled]
    total_active = len(active_items)
    
    # --- 1. Empty / underrepresented tiers (T1-T11) ---
    for tier in range(1, 12):
        count = analysis.tier_stats.get(tier, {}).get('count', 0)
        if count == 0:
            rule = get_tier_rule(tier)
            # Tier 8 is commonly empty — operator gap
            gaps.append({
                'type': 'tier_empty',
                'tier': tier,
                'tier_name': rule.get('name', f'T{tier}'),
                'severity': 'high' if tier <= 8 else 'low',
                'detail': f"Tier {tier} ({rule.get('name')}) has 0 items — no loot spawns for this tier",
            })
        elif count < 5:
            gaps.append({
                'type': 'tier_underrepresented',
                'tier': tier,
                'current_count': count,
                'severity': 'medium',
                'detail': f"Tier {tier} has only {count} items — very thin loot pool",
            })
    
    # --- 2. Tier overload (any tier > TIER_OVERLOAD_THRESHOLD of all active items) ---
    for tier, stats in analysis.tier_stats.items():
        pct = stats.get('pct', 0)
        if pct > TIER_OVERLOAD_THRESHOLD and stats['count'] > 20:
            gaps.append({
                'type': 'tier_overload',
                'tier': tier,
                'tier_name': stats['name'],
                'pct': pct,
                'count': stats['count'],
                'severity': 'high',
                'detail': f"Tier {tier} ({stats['name']}) has {stats['count']} items ({pct*100:.1f}%) "
                          f"— overloaded vs {TIER_OVERLOAD_THRESHOLD*100:.0f}% threshold",
            })
    
    # --- 3. Items with nominals outside tier_rules.json bounds ---
    nom_violations = []
    for item in active_items:
        rule = get_tier_rule(item.tier)
        rmin = rule.get('nominal_min', 0)
        rmax = rule.get('nominal_max', 100)
        if rmax > 0 and item.nominal > rmax * 3:
            nom_violations.append(item)
    if nom_violations:
        gaps.append({
            'type': 'nominal_outliers',
            'count': len(nom_violations),
            'severity': 'medium',
            'examples': [f"{i.classname} (T{i.tier}: nom={i.nominal}, max={get_tier_rule(i.tier).get('nominal_max',100)})"
                        for i in nom_violations[:8]],
            'detail': f"{len(nom_violations)} items have nominals far above their tier's configured max",
        })
    
    # --- 4. Items with lifetime outside tier bounds ---
    life_violations = []
    for item in active_items:
        rule = get_tier_rule(item.tier)
        lmin = rule.get('lifetime_min', 1800)
        lmax = rule.get('lifetime_max', 86400)
        if item.lifetime < lmin or item.lifetime > lmax * 2:
            life_violations.append(item)
    if life_violations:
        gaps.append({
            'type': 'lifetime_outliers',
            'count': len(life_violations),
            'severity': 'medium',
            'examples': [f"{i.classname} (T{i.tier}: life={i.lifetime}s, range={get_tier_rule(i.tier).get('lifetime_min',0)}-{get_tier_rule(i.tier).get('lifetime_max',0)})"
                        for i in life_violations[:8]],
            'detail': f"{len(life_violations)} items have lifetimes outside their tier's configured range",
        })
    
    # --- 5. Empty category items ---
    empty_cat = [i for i in active_items if not i.category]
    if empty_cat:
        gaps.append({
            'type': 'missing_category',
            'count': len(empty_cat),
            'severity': 'medium',
            'tier_breakdown': {t: sum(1 for i in empty_cat if i.tier == t) for t in range(1, 12)
                              if any(i.tier == t for i in empty_cat)},
            'detail': f"{len(empty_cat)} active items have no category — store pricing will use fallback",
        })
    
    # --- 6. Weapon distribution imbalance (across zones) ---
    if analysis.weapon_distribution:
        wd = analysis.weapon_distribution
        civ = wd['civilian']
        mil = wd['military']
        end = wd['endgame']
        total_weapons = wd['total']
        
        # Endgame weapons shouldn't be > 40% of all weapons
        if total_weapons > 10:
            endgame_pct = end / total_weapons
            if endgame_pct > 0.40:
                gaps.append({
                    'type': 'weapon_endgame_heavy',
                    'endgame_pct': endgame_pct,
                    'endgame_count': end,
                    'severity': 'high',
                    'detail': f"Endgame weapons ({end}) are {endgame_pct*100:.0f}% of total — too many rare guns",
                })
            # Military should have at least some representation
            if mil < total_weapons * 0.10:
                gaps.append({
                    'type': 'weapon_military_underrepresented',
                    'military_count': mil,
                    'severity': 'medium',
                    'detail': f"Only {mil} military-tier weapons ({mil/total_weapons*100:.0f}%) — thin mid-game",
                })
    
    # --- 7. Min value sanity (min should be 30-70% of nominal) ---
    bad_mins = [i for i in active_items if i.nominal > 0 and (i.min <= 0 or i.min > i.nominal)]
    if len(bad_mins) > 20:
        gaps.append({
            'type': 'min_value_issues',
            'count': len(bad_mins),
            'severity': 'low',
            'detail': f"{len(bad_mins)} items have min=0 or min>nominal — CE may not restock them properly",
        })
    
    # --- 8. Restock consistency (items with very high restock for low tier) ---
    slow_restock_low_tier = [i for i in active_items if i.tier <= 3 and i.restock > 1800]
    if slow_restock_low_tier:
        gaps.append({
            'type': 'restock_too_slow',
            'count': len(slow_restock_low_tier),
            'severity': 'low',
            'detail': f"{len(slow_restock_low_tier)} low-tier items have restock >1800s — may feel scarce",
        })
    
    logger.info(f"✓ Identified {len(gaps)} economy gaps")
    for gap in gaps:
        logger.info(f"  [{gap.get('severity','?').upper():>6}] {gap['type']}: {gap.get('detail', '')[:90]}")
    
    return gaps

# ============================================================================
# STEP 5: GENERATE LLM RECOMMENDATIONS (with statistical fallback)
# ============================================================================

def _statistical_recommendation(item: InventoryItem, analysis: DistributionAnalysis,
                                 profile: Dict) -> Optional[Dict]:
    """Pure-statistics fallback when OLLAMA is unavailable or for bulk processing."""
    tier_stats = analysis.tier_stats.get(item.tier, {})
    if not tier_stats:
        return None
    
    rule = get_tier_rule(item.tier)
    mode = profile.get('economy_mode', 'balanced')
    nom_mult = ECONOMY_MODE_MULTIPLIERS.get(mode, 1.0)
    restock_mult = RESTOCK_MODE_MULTIPLIERS.get(mode, 1.0)
    
    rec = {
        'recommendation': 'ACCEPT',
        'new_nominal': None,
        'new_lifetime': None,
        'new_restock': None,
        'new_tier': None,
        'confidence': 0.80,
        'reasoning': '',
    }
    changes = []
    
    # -- Nominal check against tier rules --
    nom_max = int(rule.get('nominal_max', 100) * nom_mult)
    nom_min = int(rule.get('nominal_min', 0) * nom_mult)
    median = tier_stats.get('nominal_median', 10)
    stdev = tier_stats.get('nominal_stdev', 5)
    
    # Clamp extreme outliers to 2x the tier rule max
    if nom_max > 0 and item.nominal > nom_max * 2:
        target = int(nom_max * 1.5)
        rec['new_nominal'] = target
        rec['recommendation'] = 'ADJUST'
        changes.append(f"Nominal {item.nominal}->{target}: exceeds tier {item.tier} max ({nom_max})")
    # Raise items below tier floor (except disabled items)
    elif nom_min > 0 and item.nominal < nom_min and item.nominal > 0:
        rec['new_nominal'] = nom_min
        rec['recommendation'] = 'ADJUST'
        changes.append(f"Nominal {item.nominal}->{nom_min}: below tier {item.tier} floor ({nom_min})")
    # Items > 2 standard deviations from tier median
    elif stdev > 0 and abs(item.nominal - median) > stdev * 2.5:
        # Pull toward median (25% correction)
        target = int(median + (item.nominal - median) * 0.75)
        target = max(nom_min, min(nom_max, target))
        if target != item.nominal:
            rec['new_nominal'] = target
            rec['recommendation'] = 'ADJUST'
            changes.append(f"Nominal {item.nominal}->{target}: outlier from T{item.tier} median ({median:.0f})")
    
    # -- Lifetime check against tier rules --
    life_min = rule.get('lifetime_min', 3600)
    life_max = rule.get('lifetime_max', 86400)
    if item.lifetime < life_min:
        rec['new_lifetime'] = life_min
        rec['recommendation'] = 'ADJUST'
        changes.append(f"Lifetime {item.lifetime}->{life_min}: below T{item.tier} floor")
    elif item.lifetime > life_max * 2:
        target = int(life_max * 1.5)
        rec['new_lifetime'] = target
        rec['recommendation'] = 'ADJUST'
        changes.append(f"Lifetime {item.lifetime}->{target}: exceeds T{item.tier} ceiling")
    
    # -- Restock check (scale by mode) --
    if item.tier <= 3 and item.restock > int(1200 * restock_mult):
        target = int(600 * restock_mult)
        rec['new_restock'] = target
        rec['recommendation'] = 'ADJUST'
        changes.append(f"Restock {item.restock}->{target}: too slow for low tier")
    
    if not changes:
        return None  # Item is fine
    
    rec['reasoning'] = '; '.join(changes)
    return rec


def query_llama_autonomously(item: InventoryItem, analysis: DistributionAnalysis, 
                              profile: Dict, context: str = "") -> Optional[Dict]:
    """Query OLLAMA LLM for autonomous economy decisions (tier-rules-aware)."""
    
    tier_stats = analysis.tier_stats.get(item.tier, {})
    rule = get_tier_rule(item.tier)
    mode = profile.get('economy_mode', 'balanced')
    
    prompt = f"""You are an expert DayZ Economy Designer AI for the "{profile.get('server_name', 'Stelliferum Forge')}" server.

CONTEXT:
- Server Playstyle: {profile.get('playstyle', 'Hardcore Survival')}
- Economy Mode: {mode}
- Item: {item.classname} (Category: {item.category or '(none)'})
- Source: {item.source.value} / Mod: {item.mod_name}
- Current Tier: {item.tier} ({rule.get('name', '?')}, {rule.get('zone', '?')} zone)
- Current Nominal: {item.nominal} | Min: {item.min} | Lifetime: {item.lifetime}s | Restock: {item.restock}s

TIER RULES (from server config):
- Tier {item.tier} "{rule.get('name')}": nominal range {rule.get('nominal_min',0)}-{rule.get('nominal_max',100)}, lifetime {rule.get('lifetime_min',3600)}-{rule.get('lifetime_max',86400)}s
- Allowed usages: {rule.get('allowed_usages', [])}

BASELINE DATA FOR TIER {item.tier}:
- Mean Nominal: {tier_stats.get('nominal_mean', 10):.1f}
- Median: {tier_stats.get('nominal_median', 10)}
- Std Dev: {tier_stats.get('nominal_stdev', 0):.2f}
- Actual Range: {tier_stats.get('nominal_min', 1)} - {tier_stats.get('nominal_max', 100)}
- Avg Lifetime: {tier_stats.get('lifetime_mean', 7200):.0f}s

INSTRUCTIONS:
1. Check if nominal fits the tier's configured range and distribution
2. Check if lifetime is appropriate for the tier
3. Consider economy mode: {mode} ({'scarce resources' if mode == 'hardcore' else 'balanced distribution' if mode == 'balanced' else 'abundant loot' if mode == 'casual' else 'slow grind'})
4. For weapons: rarity should increase with tier
5. Output ONLY valid JSON

RESPOND WITH JSON ONLY:
{{
    "recommendation": "ACCEPT|ADJUST|REPRIORITIZE",
    "new_nominal": <int or null>,
    "new_lifetime": <int or null>,
    "new_restock": <int or null>,
    "new_tier": <int 1-11 or null>,
    "confidence": <0.0-1.0>,
    "reasoning": "Brief explanation"
}}"""

    try:
        response = requests.post(
            OLLAMA_URL,
            json={
                "model": OLLAMA_MODEL,
                "prompt": prompt,
                "stream": False,
                "format": "json"
            },
            timeout=OLLAMA_TIMEOUT
        )
        
        if response.status_code == 200:
            result_text = response.json().get('response', '').strip()
            # Clean markdown if present
            if '```json' in result_text:
                result_text = result_text.split('```json')[1].split('```')[0].strip()
            elif '```' in result_text:
                result_text = result_text.split('```')[1].split('```')[0].strip()
            
            return json.loads(result_text)
    except requests.exceptions.Timeout:
        logger.warning(f"  OLLAMA timeout for {item.classname}")
    except json.JSONDecodeError as e:
        logger.warning(f"  JSON parse error for {item.classname}: {e}")
    except Exception as e:
        logger.warning(f"  Error querying OLLAMA: {e}")
    
    return None

def generate_recommendations(items: List[InventoryItem], analysis: DistributionAnalysis, 
                            profile: Dict) -> List[Dict]:
    """Generate recommendations: LLM for top candidates, statistics for bulk."""
    logger.info("=" * 70)
    logger.info("STEP 5: GENERATE LLM RECOMMENDATIONS")
    logger.info("=" * 70)
    
    recommendations = []
    active_items = [i for i in items if not i.is_disabled and i.source == ItemSource.MOD]
    
    # Also include vanilla outliers (> 2.5σ from tier median)
    vanilla_outliers = []
    for i in items:
        if i.is_disabled or i.source != ItemSource.VANILLA:
            continue
        ts = analysis.tier_stats.get(i.tier, {})
        med = ts.get('nominal_median', 10)
        sd = ts.get('nominal_stdev', 5)
        if sd > 0 and abs(i.nominal - med) > sd * 2.5:
            vanilla_outliers.append(i)
    
    candidate_items = active_items + vanilla_outliers
    logger.info(f"  Candidates: {len(active_items)} mod items + {len(vanilla_outliers)} vanilla outliers")
    
    # --- PHASE A: Statistical bulk analysis (always runs) ---
    stat_recs = 0
    for item in candidate_items:
        rec = _statistical_recommendation(item, analysis, profile)
        if rec:
            rec['item'] = item.classname
            rec['original_nominal'] = item.nominal
            rec['original_lifetime'] = item.lifetime
            rec['original_restock'] = item.restock
            rec['method'] = 'statistical'
            recommendations.append(rec)
            stat_recs += 1
    logger.info(f"  ✓ Statistical pass: {stat_recs} adjustments from {len(candidate_items)} candidates")
    
    # --- PHASE B: LLM deep analysis (top outliers only) ---
    llm_limit = profile.get('llm_item_limit', MAX_LLM_ITEMS)
    if llm_limit <= 0:
        logger.info("  LLM analysis disabled (llm_item_limit=0)")
        return recommendations
    
    # Check OLLAMA availability
    ollama_ok = False
    try:
        requests.get(f"{OLLAMA_URL.rsplit('/', 1)[0]}/tags", timeout=5)
        ollama_ok = True
        logger.info(f"  ✓ Connected to OLLAMA at {OLLAMA_URL}")
    except Exception:
        logger.warning(f"  ⚠ Cannot reach OLLAMA — using statistical-only mode")
        return recommendations
    
    # Pick worst outliers for LLM review (sorted by deviation magnitude)
    def deviation_score(item):
        ts = analysis.tier_stats.get(item.tier, {})
        med = ts.get('nominal_median', 10)
        sd = max(ts.get('nominal_stdev', 1), 1)
        return abs(item.nominal - med) / sd
    
    llm_candidates = sorted(candidate_items, key=deviation_score, reverse=True)[:llm_limit]
    logger.info(f"  Submitting top {len(llm_candidates)} outliers to LLM...")
    
    llm_recs = 0
    for idx, item in enumerate(llm_candidates, 1):
        logger.info(f"  [{idx}/{len(llm_candidates)}] Thinking about {item.classname} (T{item.tier}, nom={item.nominal})...")
        
        rec = query_llama_autonomously(item, analysis, profile)
        if rec and rec.get('recommendation') != 'ACCEPT':
            rec['item'] = item.classname
            rec['original_nominal'] = item.nominal
            rec['original_lifetime'] = item.lifetime
            rec['original_restock'] = item.restock
            rec['method'] = 'llm'
            # LLM recs override statistical ones for the same item
            recommendations = [r for r in recommendations if r.get('item') != item.classname]
            recommendations.append(rec)
            llm_recs += 1
            logger.info(f"       → {rec['recommendation']} (Confidence: {rec.get('confidence', 0):.2f})")
    
    logger.info(f"  ✓ LLM pass: {llm_recs} adjustments from {len(llm_candidates)} reviews")
    logger.info(f"✓ Total recommendations: {len(recommendations)}")
    return recommendations

# ============================================================================
# STEP 6: VALIDATE CHANGES AGAINST RULES (tier-rules-aware)
# ============================================================================

def validate_recommendations(recommendations: List[Dict], items: List[InventoryItem], 
                            profile: Dict) -> Tuple[List[Dict], List[Dict]]:
    """Validate recommendations against tier_rules.json bounds and server config."""
    logger.info("=" * 70)
    logger.info("STEP 6: VALIDATE CHANGES AGAINST RULES")
    logger.info("=" * 70)
    
    validated = []
    rejected = []
    confidence_threshold = profile.get('ai_decision_confidence_threshold', 0.75)
    mode = profile.get('economy_mode', 'balanced')
    nom_mult = ECONOMY_MODE_MULTIPLIERS.get(mode, 1.0)
    
    for rec in recommendations:
        errors = []
        
        # -- Confidence check --
        if rec.get('confidence', 0) < confidence_threshold:
            errors.append(f"Low confidence ({rec.get('confidence', 0):.2f} < {confidence_threshold})")
        
        # -- Tier bounds (1-11) --
        new_tier = rec.get('new_tier')
        if new_tier is not None:
            if new_tier < 1 or new_tier > 11:
                errors.append(f"Invalid tier: {new_tier} (must be 1-11)")
            # Don't allow autonomous tier change to Admin (T11)
            if new_tier == 11 and rec.get('method') != 'manual':
                errors.append("Cannot auto-promote to Admin tier (T11)")
        
        # -- Nominal validation against tier rules --
        effective_tier = new_tier if new_tier else None
        if effective_tier is None:
            # Look up original item's tier
            matching = [i for i in items if i.classname == rec.get('item')]
            effective_tier = matching[0].tier if matching else 4
        
        rule = get_tier_rule(effective_tier)
        new_nom = rec.get('new_nominal')
        
        if new_nom is not None:
            rmax = int(rule.get('nominal_max', 100) * nom_mult)
            rmin = int(rule.get('nominal_min', 0) * nom_mult)
            # Allow up to 3x the tier max (soft cap — the C engine does further clamping)
            if rmax > 0 and new_nom > rmax * 3:
                errors.append(f"Nominal {new_nom} exceeds 3x tier {effective_tier} max ({rmax})")
            if new_nom < 0:
                errors.append(f"Negative nominal: {new_nom}")
        
        # -- Lifetime validation --
        new_life = rec.get('new_lifetime')
        if new_life is not None:
            if new_life < 600:
                errors.append(f"Lifetime too short: {new_life}s (min 600)")
            if new_life > 604800:  # 7 days max
                errors.append(f"Lifetime too long: {new_life}s (max 604800)")
        
        # -- Restock validation --
        new_restock = rec.get('new_restock')
        if new_restock is not None:
            if new_restock < 0:
                errors.append(f"Negative restock: {new_restock}")
            if new_restock > 86400:  # 24h max
                errors.append(f"Restock too slow: {new_restock}s (max 86400)")
        
        if errors:
            rec['rejection_reason'] = "; ".join(errors)
            rejected.append(rec)
        else:
            validated.append(rec)
    
    logger.info(f"✓ Validated: {len(validated)} | Rejected: {len(rejected)}")
    for r in rejected[:10]:
        logger.info(f"  ✗ {r.get('item', '?')}: {r['rejection_reason']}")
    
    return validated, rejected

# ============================================================================
# STEP 7: APPLY MODIFICATIONS (nominal + lifetime + restock + tier)
# ============================================================================

def apply_modifications(validated: List[Dict], items: List[InventoryItem]) -> Dict:
    """Apply validated changes to inventory items."""
    logger.info("=" * 70)
    logger.info("STEP 7: APPLY MODIFICATIONS")
    logger.info("=" * 70)
    
    changes_log = {
        'timestamp': datetime.now().isoformat(),
        'total_changes': 0,
        'nominal_changes': 0,
        'lifetime_changes': 0,
        'restock_changes': 0,
        'tier_changes': 0,
        'modifications': []
    }
    
    for rec in validated:
        item_name = rec.get('item', '')
        items_to_update = [i for i in items if i.classname == item_name]
        
        for item in items_to_update:
            change = {
                'item': item_name,
                'method': rec.get('method', 'unknown'),
                'reasoning': rec.get('reasoning', 'No reasoning provided'),
                'changes': {},
            }
            
            if rec.get('new_nominal') is not None and rec['new_nominal'] != item.nominal:
                change['changes']['nominal'] = {'from': item.nominal, 'to': rec['new_nominal']}
                item.nominal = rec['new_nominal']
                changes_log['nominal_changes'] += 1
            
            if rec.get('new_lifetime') is not None and rec['new_lifetime'] != item.lifetime:
                change['changes']['lifetime'] = {'from': item.lifetime, 'to': rec['new_lifetime']}
                item.lifetime = rec['new_lifetime']
                changes_log['lifetime_changes'] += 1
            
            if rec.get('new_restock') is not None and rec['new_restock'] != item.restock:
                change['changes']['restock'] = {'from': item.restock, 'to': rec['new_restock']}
                item.restock = rec['new_restock']
                changes_log['restock_changes'] += 1
            
            if rec.get('new_tier') is not None and rec['new_tier'] != item.tier:
                change['changes']['tier'] = {'from': item.tier, 'to': rec['new_tier']}
                item.tier = rec['new_tier']
                changes_log['tier_changes'] += 1
            
            # Fix min if nominal changed and min is now invalid
            if item.nominal > 0 and (item.min <= 0 or item.min > item.nominal):
                old_min = item.min
                item.min = max(1, int(item.nominal * 0.5))
                change['changes']['min'] = {'from': old_min, 'to': item.min}
            
            if change['changes']:
                changes_log['modifications'].append(change)
                changes_log['total_changes'] += 1
    
    logger.info(f"✓ Applied {changes_log['total_changes']} modifications:")
    logger.info(f"    Nominal: {changes_log['nominal_changes']}, Lifetime: {changes_log['lifetime_changes']}, "
                f"Restock: {changes_log['restock_changes']}, Tier: {changes_log['tier_changes']}")
    return changes_log

# ============================================================================
# STEP 8: EXPORT RESULTS & REPORTS
# ============================================================================

def export_results(items: List[InventoryItem], recommendations: List[Dict], 
                  changes_log: Dict, analysis: DistributionAnalysis, profile: Dict,
                  gaps: List[Dict] = None):
    """Export all results and reports."""
    logger.info("=" * 70)
    logger.info("STEP 8: EXPORT RESULTS & REPORTS")
    logger.info("=" * 70)
    
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    
    # Export modified inventory CSV (same columns as input for round-trip)
    csv_path = f"{OUTPUT_DIR}/balanced_inventory_{timestamp}.csv"
    with open(csv_path, 'w', newline='', encoding='utf-8') as f:
        writer = csv.DictWriter(f, fieldnames=['Classname', 'SourceFile', 'Tier', 'Category',
                                               'Nominal', 'Lifetime', 'Min', 'Restock'])
        writer.writeheader()
        for item in items:
            writer.writerow({
                'Classname': item.classname,
                'SourceFile': item.source_file,
                'Tier': item.tier,
                'Category': item.category,
                'Nominal': item.nominal,
                'Lifetime': item.lifetime,
                'Min': item.min,
                'Restock': item.restock,
            })
    logger.info(f"✓ Exported inventory: {csv_path}")
    
    # Export recommendations as JSON (serializable)
    rec_path = f"{OUTPUT_DIR}/recommendations_{timestamp}.json"
    safe_recs = []
    for r in recommendations:
        sr = {k: v for k, v in r.items()}
        safe_recs.append(sr)
    with open(rec_path, 'w') as f:
        json.dump(safe_recs, f, indent=2, default=str)
    logger.info(f"✓ Exported recommendations: {rec_path}")
    
    # Export changes log
    os.makedirs(HISTORY_DIR, exist_ok=True)
    log_path = f"{HISTORY_DIR}/changes_log_{timestamp}.json"
    with open(log_path, 'w') as f:
        json.dump(changes_log, f, indent=2, default=str)
    logger.info(f"✓ Exported changes log: {log_path}")
    
    # Generate detailed summary report
    report_path = f"{OUTPUT_DIR}/balance_report_{timestamp}.txt"
    with open(report_path, 'w') as f:
        f.write("STELLIFERUM ECONOMY BALANCE REPORT v3.0\n")
        f.write("=" * 70 + "\n\n")
        f.write(f"Timestamp: {datetime.now().isoformat()}\n")
        f.write(f"Server: {profile.get('server_name', '?')}\n")
        f.write(f"Mode: {profile.get('economy_mode', '?')}\n")
        f.write(f"Playstyle: {profile.get('playstyle', '?')}\n\n")
        
        f.write(analysis.report())
        
        if gaps:
            f.write(f"\nECONOMY GAPS IDENTIFIED: {len(gaps)}\n")
            f.write("-" * 50 + "\n")
            for gap in gaps:
                f.write(f"  [{gap.get('severity','?').upper():>6}] {gap['type']}\n")
                f.write(f"           {gap.get('detail', '')}\n")
        
        f.write(f"\nRECOMMENDATIONS: {len(recommendations)} total\n")
        stat_count = sum(1 for r in recommendations if r.get('method') == 'statistical')
        llm_count = sum(1 for r in recommendations if r.get('method') == 'llm')
        f.write(f"  Statistical: {stat_count} | LLM: {llm_count}\n")
        
        f.write(f"\nMODIFICATIONS APPLIED: {changes_log['total_changes']}\n")
        f.write(f"  Nominal: {changes_log.get('nominal_changes', 0)}\n")
        f.write(f"  Lifetime: {changes_log.get('lifetime_changes', 0)}\n")
        f.write(f"  Restock: {changes_log.get('restock_changes', 0)}\n")
        f.write(f"  Tier: {changes_log.get('tier_changes', 0)}\n")
        
        # Top 20 biggest changes
        if changes_log.get('modifications'):
            f.write(f"\nTOP CHANGES:\n")
            sorted_mods = sorted(changes_log['modifications'],
                               key=lambda m: sum(abs(v.get('to',0)-v.get('from',0)) for v in m.get('changes',{}).values()),
                               reverse=True)
            for mod in sorted_mods[:20]:
                f.write(f"  {mod['item']}: ")
                parts = []
                for field, vals in mod.get('changes', {}).items():
                    parts.append(f"{field} {vals.get('from','?')}->{vals.get('to','?')}")
                f.write(", ".join(parts))
                f.write(f"  [{mod.get('method', '?')}]\n")
    
    logger.info(f"✓ Exported report: {report_path}")
    
    logger.info("=" * 70)
    logger.info("AUTONOMOUS ECONOMY BALANCING COMPLETE")
    logger.info("=" * 70)

# ============================================================================
# MAIN ORCHESTRATION
# ============================================================================

def main():
    """Execute 8-step autonomous economy balancing workflow."""
    logger.info("╔" + "=" * 68 + "╗")
    logger.info("║  STELLIFERUM AUTONOMOUS ECONOMY CORTEX v3.0                      ║")
    logger.info("║  8-Step Hybrid AI-Driven Economy Management System                ║")
    logger.info("║  Tier-rules-aware | 11-tier | Statistical fallback                ║")
    logger.info("╚" + "=" * 68 + "╝\n")
    
    try:
        # STEP 1
        profile = load_config()
        
        # STEP 2
        items = parse_inventory_data(f"{OUTPUT_DIR}/items.csv")
        if not items:
            logger.error("No items loaded. Exiting.")
            return
        
        # STEP 3
        analysis = analyze_distribution(items, profile)
        
        # STEP 4
        gaps = identify_gaps(items, analysis, profile)
        
        # STEP 5
        if profile.get('autonomous_mode'):
            recommendations = generate_recommendations(items, analysis, profile)
        else:
            recommendations = []
            logger.info("Autonomous mode disabled. Skipping recommendations.")
        
        # STEP 6
        validated, rejected = validate_recommendations(recommendations, items, profile)
        
        # STEP 7
        changes_log = apply_modifications(validated, items)
        
        # STEP 8
        export_results(items, recommendations, changes_log, analysis, profile, gaps)
        
        logger.info("\n✓ SUCCESS - Economy balancing complete")
        logger.info(f"  {len(items)} items analyzed | {len(gaps)} gaps found | "
                    f"{changes_log['total_changes']} modifications applied")
        
    except Exception as e:
        logger.exception(f"Fatal error: {e}")
        sys.exit(1)

if __name__ == "__main__":
    main()
