#!/usr/bin/env python3
"""
PHOENIX ITEM SCANNER — AI-Driven Item Capacity Discovery
=========================================================
Uses Qwen 2.5 (via Ollama) and DayZ Wiki to research modded and vanilla items,
determining their actual cargo/attachment capabilities.

Pipeline:
  1. Load items.csv (exported by auditor C code)
  2. Filter to clothing/equipment candidates
  3. For each candidate, fetch DayZ Wiki page via Fandom API
  4. Feed wiki text + item context to Qwen 2.5 for classification
  5. Validate AI responses against confidence thresholds
  6. Merge results into .phoenix/item_capacity.json
  7. Log everything for traceability

Integration:
  - Called by gap_filler.c via system("python phoenix_item_scanner.py")
  - Reads:  output/items.csv
  - Writes: .phoenix/item_capacity.json
  - Cache:  .phoenix/wiki_cache/ (7-day TTL)

Architecture follows economy_balancer.py patterns (Ollama JSON mode,
validation gates, timestamped outputs).
"""

import csv
import json
import os
import sys
import re
import hashlib
import logging
import time
from datetime import datetime, timedelta
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Any
import requests

# ============================================================================
# CONFIGURATION
# ============================================================================

PHOENIX_DIR = ".phoenix"
CAPACITY_FILE = os.path.join(PHOENIX_DIR, "item_capacity.json")
WIKI_CACHE_DIR = os.path.join(PHOENIX_DIR, "wiki_cache")
ITEMS_CSV = os.path.join("output", "items.csv")
OUTPUT_DIR = "output"
LOG_DIR = OUTPUT_DIR

OLLAMA_URL = os.getenv("OLLAMA_URL", "http://localhost:11434/api/generate")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "qwen2.5")
OLLAMA_TIMEOUT = 60  # Higher timeout for research queries
OLLAMA_HEALTH_TIMEOUT = 5

WIKI_API_URL = "https://dayz.fandom.com/api.php"
WIKI_CACHE_TTL_DAYS = 7

# Confidence threshold — AI responses below this are discarded
CONFIDENCE_THRESHOLD = 0.70

# Maximum items to process per run (safety limit)
MAX_ITEMS_PER_RUN = 200

# Categories of items to scan (from items.csv Category column)
TARGET_CATEGORIES = {"clothes", "containers"}

# ============================================================================
# LOGGING
# ============================================================================

os.makedirs(OUTPUT_DIR, exist_ok=True)
os.makedirs(PHOENIX_DIR, exist_ok=True)
os.makedirs(WIKI_CACHE_DIR, exist_ok=True)

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(name)s: %(message)s',
    handlers=[
        logging.FileHandler(os.path.join(LOG_DIR, "phoenix_scanner.log")),
        logging.StreamHandler(sys.stdout)
    ]
)
logger = logging.getLogger("PHOENIX")


# ============================================================================
# STEP 1: LOAD ITEMS
# ============================================================================

class ItemRecord:
    """Lightweight item record from items.csv."""
    def __init__(self, row: Dict):
        self.classname = row.get('Classname', '').strip()
        self.source_file = row.get('SourceFile', '').strip()
        self.category = row.get('Category', '').strip().lower()
        self.tier = int(row.get('Tier', 4))
        self.nominal = int(row.get('Nominal', 10))

    @property
    def is_vanilla(self) -> bool:
        return 'types.xml' == self.source_file or self.source_file == ''

    @property
    def mod_name(self) -> str:
        """Extract mod name from source file (e.g. 'SNAFU_types.xml' -> 'SNAFU')."""
        if self.is_vanilla:
            return 'vanilla'
        parts = self.source_file.split('_')
        return parts[0] if parts else 'unknown'


def load_items(csv_path: str) -> List[ItemRecord]:
    """Load items from auditor CSV export."""
    if not os.path.exists(csv_path):
        logger.error(f"Items CSV not found: {csv_path}")
        logger.info("  Run the auditor to export items.csv first.")
        return []

    items = []
    with open(csv_path, 'r', encoding='utf-8-sig') as f:
        reader = csv.DictReader(f)
        for row in reader:
            items.append(ItemRecord(row))

    logger.info(f"Loaded {len(items)} items from {csv_path}")
    return items


def filter_candidates(items: List[ItemRecord]) -> List[ItemRecord]:
    """Filter to items that need capacity research."""
    candidates = []
    for item in items:
        if not item.classname:
            continue
        # Only scan clothes and containers
        if item.category not in TARGET_CATEGORIES:
            continue
        # Skip items that are clearly not containers (ammo, mags, optics, etc.)
        cn_lower = item.classname.lower()
        skip_patterns = [
            'ammo_', '_ammo', 'mag_', 'optic', 'scope', 'suppressor',
            'bttstck', 'hndgrd', 'compensator', 'bayonet', 'rail_',
            'carwheel', 'sparkplug', 'carbattery', 'truckbattery',
        ]
        if any(pat in cn_lower for pat in skip_patterns):
            continue
        candidates.append(item)

    logger.info(f"Filtered to {len(candidates)} clothing/equipment candidates")
    return candidates


# ============================================================================
# STEP 2: WIKI FETCH WITH CACHE
# ============================================================================

def wiki_cache_path(item_name: str) -> str:
    """Get cache file path for a wiki page."""
    safe_name = re.sub(r'[^\w\-]', '_', item_name)
    return os.path.join(WIKI_CACHE_DIR, f"{safe_name}.txt")


def wiki_cache_valid(cache_path: str) -> bool:
    """Check if cached wiki page is still fresh."""
    if not os.path.exists(cache_path):
        return False
    mtime = datetime.fromtimestamp(os.path.getmtime(cache_path))
    return datetime.now() - mtime < timedelta(days=WIKI_CACHE_TTL_DAYS)


def fetch_wiki_page(item_name: str) -> Optional[str]:
    """Fetch DayZ Wiki page for an item. Uses cache if available.

    API: https://dayz.fandom.com/api.php?action=parse&page=NAME&format=json&prop=wikitext
    Returns: MediaWiki wikitext or None if page doesn't exist.
    """
    cache_path = wiki_cache_path(item_name)

    # Check cache first
    if wiki_cache_valid(cache_path):
        try:
            with open(cache_path, 'r', encoding='utf-8') as f:
                text = f.read()
            if text.strip():
                return text
        except (IOError, UnicodeDecodeError):
            pass

    # Fetch from wiki — convert spaces to underscores for URL
    page_name = item_name.replace(' ', '_')
    # Also try stripping common suffixes like _ColorBase
    search_names = [page_name]
    for suffix in ['_ColorBase', '_Base', '_Color']:
        if page_name.endswith(suffix):
            search_names.append(page_name[:-len(suffix)])

    wikitext = None
    for name in search_names:
        try:
            resp = requests.get(
                WIKI_API_URL,
                params={
                    'action': 'parse',
                    'page': name,
                    'format': 'json',
                    'prop': 'wikitext',
                },
                timeout=15
            )
            if resp.status_code == 200:
                data = resp.json()
                if 'parse' in data and 'wikitext' in data['parse']:
                    wikitext = data['parse']['wikitext'].get('*', '')
                    if wikitext:
                        break
        except (requests.RequestException, json.JSONDecodeError, KeyError) as e:
            logger.debug(f"Wiki fetch failed for '{name}': {e}")
            continue

    # Cache result (even empty — prevents re-fetching missing pages)
    if wikitext is not None:
        try:
            with open(cache_path, 'w', encoding='utf-8') as f:
                f.write(wikitext)
        except IOError:
            pass
        return wikitext if wikitext.strip() else None
    else:
        # Cache empty result
        try:
            with open(cache_path, 'w', encoding='utf-8') as f:
                f.write('')
        except IOError:
            pass
        return None


def extract_wiki_capacity_hints(wikitext: str) -> Dict[str, Any]:
    """Extract capacity-related info from wikitext.

    Looks for patterns like:
      | capacity = 12 (3×4)
      | slots = Gas Mask Filter
      | size = 3x4
      | cargo = Yes/No
    """
    hints: Dict[str, Any] = {}

    # Capacity line (e.g., "| capacity = 30 (6×5)" or "| capacity = Gas Mask Filter")
    cap_match = re.search(r'\|\s*capacity\s*=\s*(.+?)(?:\n|\|)', wikitext, re.IGNORECASE)
    if cap_match:
        hints['capacity_raw'] = cap_match.group(1).strip()

    # Size line (e.g., "| size = 3x4 (12 Slots)")
    size_match = re.search(r'\|\s*size\s*=\s*(.+?)(?:\n|\|)', wikitext, re.IGNORECASE)
    if size_match:
        hints['size_raw'] = size_match.group(1).strip()

    # Slot count from infobox
    slots_match = re.search(r'(\d+)\s*(?:slots?|Slots?)', wikitext)
    if slots_match:
        hints['slot_count'] = int(slots_match.group(1))

    # Dimension pattern like "3×4" or "3x4"
    dim_match = re.search(r'(\d+)\s*[×xX]\s*(\d+)', wikitext)
    if dim_match:
        hints['dimensions'] = f"{dim_match.group(1)}x{dim_match.group(2)}"
        hints['computed_slots'] = int(dim_match.group(1)) * int(dim_match.group(2))

    # Specific item references in capacity (like "Gas Mask Filter")
    if 'capacity_raw' in hints:
        cap_text = hints['capacity_raw']
        # If capacity is a specific item name (not a number/dimension)
        if not re.match(r'^\d', cap_text) and not re.match(r'^No|^None|^N/A', cap_text, re.IGNORECASE):
            hints['specific_items_raw'] = cap_text

    # "Attachments" section
    attach_match = re.search(r'\|\s*attachments?\s*=\s*(.+?)(?:\n\||\n\n)', wikitext, re.IGNORECASE | re.DOTALL)
    if attach_match:
        hints['attachments_raw'] = attach_match.group(1).strip()

    # Look for "Cannot store" / "No inventory" indicators
    if re.search(r'cannot\s+store|no\s+inventory|no\s+cargo|no\s+storage', wikitext, re.IGNORECASE):
        hints['no_cargo_indicator'] = True

    return hints


# ============================================================================
# STEP 3: OLLAMA QUERY — QWEN 2.5
# ============================================================================

def check_ollama() -> bool:
    """Check if Ollama is running and the model is available."""
    try:
        base_url = OLLAMA_URL.rsplit('/', 1)[0]
        resp = requests.get(f"{base_url}/api/tags", timeout=OLLAMA_HEALTH_TIMEOUT)
        if resp.status_code != 200:
            return False
        # Check if our model is available
        models = resp.json().get('models', [])
        model_names = [m.get('name', '').split(':')[0] for m in models]
        if OLLAMA_MODEL.split(':')[0] not in model_names:
            logger.warning(f"Model '{OLLAMA_MODEL}' not found. Available: {model_names}")
            logger.info(f"  Pull it with: ollama pull {OLLAMA_MODEL}")
            return False
        return True
    except (requests.RequestException, json.JSONDecodeError):
        return False


def query_item_capacity(item: ItemRecord, wiki_hints: Dict[str, Any],
                        wiki_text_snippet: str = "") -> Optional[Dict]:
    """Query Qwen 2.5 to classify an item's cargo/attachment capabilities.

    Returns a dict with:
      classification: "no_cargo" | "specific_only" | "has_cargo" | "modded_slots"
      confidence: 0.0 - 1.0
      reasoning: str
      cargo_items: list of specific item classnames (if specific_only)
      cargo_chance: float (if specific_only or has_cargo)
      cargo_slots: int (estimated)
    """
    # Build context from wiki hints
    wiki_context = "No wiki data available."
    if wiki_hints:
        parts = []
        if 'capacity_raw' in wiki_hints:
            parts.append(f"Capacity: {wiki_hints['capacity_raw']}")
        if 'size_raw' in wiki_hints:
            parts.append(f"Size: {wiki_hints['size_raw']}")
        if 'slot_count' in wiki_hints:
            parts.append(f"Slots: {wiki_hints['slot_count']}")
        if 'dimensions' in wiki_hints:
            parts.append(f"Dimensions: {wiki_hints['dimensions']}")
        if 'specific_items_raw' in wiki_hints:
            parts.append(f"Specific items: {wiki_hints['specific_items_raw']}")
        if 'attachments_raw' in wiki_hints:
            parts.append(f"Attachments: {wiki_hints['attachments_raw']}")
        if wiki_hints.get('no_cargo_indicator'):
            parts.append("Wiki indicates: NO cargo/storage")
        if parts:
            wiki_context = " | ".join(parts)

    # Truncate wiki text for context window
    wiki_snippet = ""
    if wiki_text_snippet:
        wiki_snippet = wiki_text_snippet[:2000]

    prompt = f"""You are a DayZ item capacity classification expert. Analyze this item and determine its cargo/storage capabilities.

ITEM:
- Classname: {item.classname}
- Category: {item.category}
- Source: {"Vanilla" if item.is_vanilla else f"Mod ({item.mod_name})"}
- Tier: {item.tier}

WIKI DATA:
{wiki_context}

WIKI TEXT EXCERPT:
{wiki_snippet}

CLASSIFICATION RULES:
1. "no_cargo" — Item has NO general cargo inventory at all. Examples: gloves, boots, armbands, glasses, watches, simple hats (baseball caps, berets, beanies), face masks without filter slots.
2. "specific_only" — Item can ONLY hold specific items, not general cargo. Examples: gas masks (hold only Gas Mask Filter), NVG headstraps (hold only NVGoggles). List the specific item classnames.
3. "has_cargo" — Item has general cargo inventory (pockets, storage space). Examples: jackets, pants, vests, backpacks, helmets with visors. Estimate the slot count.
4. "modded_slots" — Modded item with EXTRA attachment slots or cargo beyond vanilla equivalent. Only use if wiki/context clearly indicates modded capacity.

IMPORTANT:
- DayZ boots and shoes NEVER have cargo inventory
- DayZ gloves NEVER have cargo inventory
- DayZ armbands NEVER have cargo inventory  
- DayZ glasses, goggles, eyewear NEVER have cargo inventory
- Simple hats (caps, berets, beanies) rarely have cargo — most have 0 slots
- Gas masks have a specific filter slot, NOT general cargo
- Jackets, coats, pants, vests DO have general cargo (pockets)
- Backpacks, bags, cases, tents have LARGE general cargo
- If wiki says "Capacity: X" where X is a number or dimension, that's general cargo
- If wiki says "Capacity: [item name]", that's specific-only cargo
- If no wiki data and the item is clothing of an unknown type, check the classname for hints

RESPOND WITH JSON ONLY (no markdown, no explanation outside JSON):
{{
    "classification": "no_cargo|specific_only|has_cargo|modded_slots",
    "confidence": 0.0,
    "reasoning": "Brief explanation",
    "cargo_items": ["ItemClassname1"],
    "cargo_chance": 0.20,
    "cargo_slots": 0
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

            result = json.loads(result_text)

            # Validate required fields
            if 'classification' not in result or 'confidence' not in result:
                logger.warning(f"  Missing required fields for {item.classname}")
                return None

            # Normalize classification
            valid_classes = {'no_cargo', 'specific_only', 'has_cargo', 'modded_slots'}
            if result['classification'] not in valid_classes:
                logger.warning(f"  Invalid classification '{result['classification']}' for {item.classname}")
                return None

            # Ensure numeric confidence
            result['confidence'] = float(result.get('confidence', 0))

            # Ensure lists
            if 'cargo_items' not in result:
                result['cargo_items'] = []
            if not isinstance(result['cargo_items'], list):
                result['cargo_items'] = [str(result['cargo_items'])]

            # Ensure numeric cargo_chance and cargo_slots
            result['cargo_chance'] = float(result.get('cargo_chance', 0.20))
            result['cargo_slots'] = int(result.get('cargo_slots', 0))

            return result

    except requests.exceptions.Timeout:
        logger.warning(f"  Ollama timeout for {item.classname}")
    except json.JSONDecodeError as e:
        logger.warning(f"  JSON parse error for {item.classname}: {e}")
    except Exception as e:
        logger.warning(f"  Error querying Ollama for {item.classname}: {e}")

    return None


# ============================================================================
# STEP 4: HEURISTIC FALLBACK
# ============================================================================

# If Ollama is unreachable, use pattern-based heuristics (mirrors is_no_cargo_clothing in C)
NO_CARGO_PATTERNS = [
    'gasmask', 'airbornemask', 'gp5',
    'balaclava', 'skimask', 'bandana', 'surgicalmask', 'mouthrag', 'facecover',
    'gloves', 'nbcgloves',
    'boots', 'shoes', 'sneakers', 'joggingshoes', 'nbcboots', 'wellies',
    'hikingboots', 'militaryboots', 'workingboots', 'combatboots',
    'armband',
    'glasses', 'goggles', 'aviatorglasses', 'sportglasses',
    'thinframesglasses', 'nvgoggles', 'eyepatch',
    'headband', 'nvgheadstrap', 'headtorch',
    'baseballcap', 'beret', 'beanie', 'ushanka', 'boonie',
    'zsh3pilothelmet', 'wig', 'nbchood',
    'santashat', 'witchhat', 'pumpkinhelmet', 'dirtbikehelmet',
    'watch', 'ring',
]

SPECIFIC_ONLY_ITEMS = {
    'AirborneMask': {'cargo': ['GasMask_Filter'], 'chance': 0.20},
    'GP5GasMask': {'cargo': ['GasMask_Filter'], 'chance': 0.20},
    'GasMask': {'cargo': ['GasMask_Filter'], 'chance': 0.20},
    'NVGHeadstrap': {'cargo': ['NVGoggles'], 'chance': 0.15},
}

HAS_CARGO_PATTERNS = [
    'jacket', 'coat', 'hoodie', 'gorka', 'parka', 'windbreaker', 'firefighter', 'ghillie',
    'pants', 'jeans', 'trousers', 'bdu', 'cargopants',
    'vest', 'platecarrier', 'chestrig', 'smersh',
    'bag', 'pack', 'barrel', 'crate', 'tent', 'case', 'pouch', 'holster',
    'belt', 'chest', 'drybag',
    'helmet', 'mich', 'ballistichelm', 'tacticalhelm',
]


def heuristic_classify(item: ItemRecord) -> Optional[Dict]:
    """Classify item using pattern matching when AI is unavailable."""
    cn_lower = item.classname.lower()

    # Check specific-only items first (exact match)
    if item.classname in SPECIFIC_ONLY_ITEMS:
        spec = SPECIFIC_ONLY_ITEMS[item.classname]
        return {
            'classification': 'specific_only',
            'confidence': 0.95,
            'reasoning': f'Hardcoded specific-only item (known DayZ behavior)',
            'cargo_items': spec['cargo'],
            'cargo_chance': spec['chance'],
            'cargo_slots': 0,
            'source': 'heuristic',
        }

    # Check no-cargo patterns
    for pat in NO_CARGO_PATTERNS:
        if pat in cn_lower:
            return {
                'classification': 'no_cargo',
                'confidence': 0.85,
                'reasoning': f'Matches no-cargo pattern: {pat}',
                'cargo_items': [],
                'cargo_chance': 0,
                'cargo_slots': 0,
                'source': 'heuristic',
            }

    # Check has-cargo patterns
    for pat in HAS_CARGO_PATTERNS:
        if pat in cn_lower:
            return {
                'classification': 'has_cargo',
                'confidence': 0.80,
                'reasoning': f'Matches has-cargo pattern: {pat}',
                'cargo_items': [],
                'cargo_chance': 0.50,
                'cargo_slots': 12,  # Default estimate
                'source': 'heuristic',
            }

    # Unknown — skip (let C code handle with defaults)
    return None


# ============================================================================
# STEP 5: LOAD & MERGE CAPACITY CONFIG
# ============================================================================

def load_capacity_config() -> Dict:
    """Load existing item_capacity.json or create a new one."""
    if os.path.exists(CAPACITY_FILE):
        try:
            with open(CAPACITY_FILE, 'r', encoding='utf-8') as f:
                return json.load(f)
        except (json.JSONDecodeError, IOError) as e:
            logger.warning(f"Failed to load {CAPACITY_FILE}: {e}")
            # Backup corrupt file
            backup = f"{CAPACITY_FILE}.bak.{int(time.time())}"
            try:
                os.rename(CAPACITY_FILE, backup)
                logger.info(f"Backed up corrupt config to {backup}")
            except IOError:
                pass

    # Create new config
    return {
        "version": 2,
        "description": "Item cargo/attachment capacity rules. Generated by Phoenix Item Scanner.",
        "updated_at": datetime.now().isoformat(),
        "updated_by": "phoenix_item_scanner",
        "rules": {
            "no_cargo": {
                "description": "Items with NO general cargo inventory.",
                "items": []
            },
            "specific_only": {
                "description": "Items that can ONLY hold specific items.",
                "items": {}
            },
            "modded_slots": {
                "description": "Modded items with EXTRA slots.",
                "items": {}
            },
            "has_cargo": {
                "description": "Items confirmed to have general cargo.",
                "items": []
            }
        },
        "classname_patterns": {
            "description": "Fallback classname patterns (case-insensitive).",
            "no_cargo_patterns": [],
            "no_cargo_boot_patterns": [],
            "no_cargo_hat_patterns": []
        }
    }


def merge_result(config: Dict, item: ItemRecord, result: Dict):
    """Merge a single AI/heuristic result into the capacity config."""
    rules = config.get('rules', {})
    cn = item.classname
    classification = result['classification']
    source = result.get('source', 'ai')
    confidence = result.get('confidence', 0)

    if classification == 'no_cargo':
        no_cargo_list = rules.get('no_cargo', {}).get('items', [])
        if cn not in no_cargo_list:
            no_cargo_list.append(cn)
        rules.setdefault('no_cargo', {})['items'] = no_cargo_list

        # Remove from other lists if present
        has_cargo_list = rules.get('has_cargo', {}).get('items', [])
        if cn in has_cargo_list:
            has_cargo_list.remove(cn)
        specific_items = rules.get('specific_only', {}).get('items', {})
        specific_items.pop(cn, None)
        modded_items = rules.get('modded_slots', {}).get('items', {})
        modded_items.pop(cn, None)

    elif classification == 'specific_only':
        specific_items = rules.setdefault('specific_only', {}).setdefault('items', {})
        specific_items[cn] = {
            'cargo': result.get('cargo_items', []),
            'cargo_chance': result.get('cargo_chance', 0.20),
            'source': f"{source} (confidence: {confidence:.2f})",
            'verified_by': source,
        }
        # Remove from no_cargo if present
        no_cargo_list = rules.get('no_cargo', {}).get('items', [])
        if cn in no_cargo_list:
            no_cargo_list.remove(cn)

    elif classification == 'has_cargo':
        has_cargo_list = rules.setdefault('has_cargo', {}).setdefault('items', [])
        if cn not in has_cargo_list:
            has_cargo_list.append(cn)
        # Remove from no_cargo if present
        no_cargo_list = rules.get('no_cargo', {}).get('items', [])
        if cn in no_cargo_list:
            no_cargo_list.remove(cn)

    elif classification == 'modded_slots':
        modded_items = rules.setdefault('modded_slots', {}).setdefault('items', {})
        modded_items[cn] = {
            'cargo_slots': result.get('cargo_slots', 0),
            'cargo_items': result.get('cargo_items', []),
            'source': f"{source} (confidence: {confidence:.2f})",
            'verified_by': source,
        }
        # Modded slots override no_cargo
        no_cargo_list = rules.get('no_cargo', {}).get('items', [])
        if cn in no_cargo_list:
            no_cargo_list.remove(cn)


def save_capacity_config(config: Dict):
    """Write updated config to .phoenix/item_capacity.json."""
    config['updated_at'] = datetime.now().isoformat()
    config['updated_by'] = 'phoenix_item_scanner'
    try:
        with open(CAPACITY_FILE, 'w', encoding='utf-8') as f:
            json.dump(config, f, indent=2, ensure_ascii=False)
        logger.info(f"Saved capacity config to {CAPACITY_FILE}")
    except IOError as e:
        logger.error(f"Failed to save {CAPACITY_FILE}: {e}")


# ============================================================================
# STEP 6: ORCHESTRATION
# ============================================================================

def build_already_classified(config: Dict) -> set:
    """Build set of classnames already in the config (skip re-scanning)."""
    classified = set()
    rules = config.get('rules', {})

    for cn in rules.get('no_cargo', {}).get('items', []):
        classified.add(cn)
    for cn in rules.get('specific_only', {}).get('items', {}).keys():
        classified.add(cn)
    for cn in rules.get('has_cargo', {}).get('items', []):
        classified.add(cn)
    for cn in rules.get('modded_slots', {}).get('items', {}).keys():
        classified.add(cn)

    return classified


def run_scanner():
    """Main Phoenix scanner pipeline."""
    logger.info("=" * 70)
    logger.info("PHOENIX ITEM SCANNER — AI-Driven Capacity Discovery")
    logger.info(f"  Model: {OLLAMA_MODEL} | Wiki: dayz.fandom.com")
    logger.info("=" * 70)

    # Step 1: Load items
    items = load_items(ITEMS_CSV)
    if not items:
        logger.error("No items loaded. Aborting.")
        return

    candidates = filter_candidates(items)
    if not candidates:
        logger.info("No candidates to scan.")
        return

    # Step 2: Load existing config
    config = load_capacity_config()
    already_classified = build_already_classified(config)
    logger.info(f"Already classified: {len(already_classified)} items")

    # Filter out already-classified items
    new_candidates = [c for c in candidates if c.classname not in already_classified]
    logger.info(f"New candidates to scan: {len(new_candidates)}")

    if not new_candidates:
        logger.info("All candidates already classified. Nothing to do.")
        return

    # Safety limit
    if len(new_candidates) > MAX_ITEMS_PER_RUN:
        logger.info(f"Capping at {MAX_ITEMS_PER_RUN} items per run (total: {len(new_candidates)})")
        new_candidates = new_candidates[:MAX_ITEMS_PER_RUN]

    # Step 3: Check Ollama
    ai_available = check_ollama()
    if ai_available:
        logger.info(f"AI available: {OLLAMA_MODEL} via Ollama")
    else:
        logger.warning(f"AI not available — using heuristic fallback only")
        logger.info(f"  Start Ollama: ollama serve")
        logger.info(f"  Pull model:   ollama pull {OLLAMA_MODEL}")

    # Step 4: Process candidates
    stats = {
        'total': len(new_candidates),
        'no_cargo': 0,
        'specific_only': 0,
        'has_cargo': 0,
        'modded_slots': 0,
        'skipped': 0,
        'ai_used': 0,
        'heuristic_used': 0,
        'wiki_fetched': 0,
        'wiki_cached': 0,
    }

    for idx, item in enumerate(new_candidates, 1):
        logger.info(f"[{idx}/{stats['total']}] Scanning: {item.classname} ({item.category})")

        result = None

        if ai_available:
            # Try wiki + AI path
            wiki_text = fetch_wiki_page(item.classname)
            if wiki_text:
                stats['wiki_fetched'] += 1
                wiki_hints = extract_wiki_capacity_hints(wiki_text)
                # Truncate wiki text for AI prompt
                wiki_snippet = wiki_text[:2000] if wiki_text else ""
            else:
                wiki_hints = {}
                wiki_snippet = ""
                # Check cache to distinguish fresh miss from cached miss
                cp = wiki_cache_path(item.classname)
                if os.path.exists(cp):
                    stats['wiki_cached'] += 1

            result = query_item_capacity(item, wiki_hints, wiki_snippet)

            if result:
                stats['ai_used'] += 1
                result['source'] = 'ai'

                # Validate confidence
                if result['confidence'] < CONFIDENCE_THRESHOLD:
                    logger.info(f"    Low confidence ({result['confidence']:.2f}) — supplementing with heuristic")
                    heuristic = heuristic_classify(item)
                    if heuristic:
                        # If heuristic and AI agree, boost confidence
                        if heuristic['classification'] == result['classification']:
                            result['confidence'] = max(result['confidence'], heuristic['confidence'])
                            result['source'] = 'ai+heuristic'
                        else:
                            # Prefer heuristic for known patterns
                            if heuristic['confidence'] > result['confidence']:
                                result = heuristic
                                logger.info(f"    Heuristic override: {result['classification']}")

        # Fallback to heuristic if AI unavailable or failed
        if result is None:
            result = heuristic_classify(item)
            if result:
                stats['heuristic_used'] += 1

        if result is None:
            logger.info(f"    Skipped (no classification)")
            stats['skipped'] += 1
            continue

        # Validate and merge
        classification = result['classification']
        confidence = result.get('confidence', 0)

        if confidence < CONFIDENCE_THRESHOLD and result.get('source') != 'heuristic':
            logger.info(f"    Rejected: confidence too low ({confidence:.2f})")
            stats['skipped'] += 1
            continue

        merge_result(config, item, result)
        stats[classification] += 1
        logger.info(f"    -> {classification} (confidence: {confidence:.2f}, source: {result.get('source', '?')})")

        # Rate limit wiki + AI requests
        if ai_available and stats['ai_used'] % 5 == 0:
            time.sleep(0.5)  # Brief pause every 5 AI queries

    # Step 5: Save updated config
    save_capacity_config(config)

    # Step 6: Summary report
    logger.info("=" * 70)
    logger.info("PHOENIX SCAN COMPLETE")
    logger.info("=" * 70)
    logger.info(f"  Scanned:       {stats['total']}")
    logger.info(f"  no_cargo:      {stats['no_cargo']}")
    logger.info(f"  specific_only: {stats['specific_only']}")
    logger.info(f"  has_cargo:     {stats['has_cargo']}")
    logger.info(f"  modded_slots:  {stats['modded_slots']}")
    logger.info(f"  skipped:       {stats['skipped']}")
    logger.info(f"  AI queries:    {stats['ai_used']}")
    logger.info(f"  Heuristic:     {stats['heuristic_used']}")
    logger.info(f"  Wiki fetches:  {stats['wiki_fetched']}")
    logger.info(f"  Wiki cached:   {stats['wiki_cached']}")

    # Export scan report
    report_path = os.path.join(OUTPUT_DIR, f"phoenix_scan_{datetime.now().strftime('%Y%m%d_%H%M%S')}.json")
    try:
        with open(report_path, 'w', encoding='utf-8') as f:
            json.dump({
                'timestamp': datetime.now().isoformat(),
                'model': OLLAMA_MODEL,
                'stats': stats,
                'config_path': CAPACITY_FILE,
            }, f, indent=2)
        logger.info(f"  Report: {report_path}")
    except IOError:
        pass

    # Count totals in config
    rules = config.get('rules', {})
    total_classified = (
        len(rules.get('no_cargo', {}).get('items', [])) +
        len(rules.get('specific_only', {}).get('items', {})) +
        len(rules.get('has_cargo', {}).get('items', [])) +
        len(rules.get('modded_slots', {}).get('items', {}))
    )
    logger.info(f"  Total classified items in config: {total_classified}")
    logger.info("=" * 70)


# ============================================================================
# MAIN
# ============================================================================

if __name__ == "__main__":
    try:
        run_scanner()
    except KeyboardInterrupt:
        logger.info("\nInterrupted by user.")
        sys.exit(0)
    except Exception as e:
        logger.exception(f"Fatal error: {e}")
        sys.exit(1)
