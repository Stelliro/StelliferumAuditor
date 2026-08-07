# Autonomous Economy Cortex - 8-Step Balance System

## Overview

The **Stelliferum Autonomous Economy Cortex** is a sophisticated hybrid system that combines:
- Statistical distribution analysis
- LLM (OLLAMA) decision-making
- Validation and safety rules
- Complete audit trails
- Reversible modifications

It processes DayZ economy data through 8 autonomous steps to achieve balanced, consistent loot distributions across vanilla items and mods.

---

## Architecture: Hybrid Human-In-The-Loop + Autonomous AI

```
┌─────────────────────────────────────────────────────────────┐
│  HUMAN DECISIONS → VANILLA ITEMS ← IMMUTABLE BASELINE       │
│                                                               │
│  AI REASONING → MOD ITEMS ← FULLY AUTONOMOUS                │
│                                                               │
│  VALIDATION LAYER → ALL CHANGES ← RULE-BASED GATING        │
└─────────────────────────────────────────────────────────────┘
```

### Design Principles

1. **Vanilla Preservation**: Vanilla DayZ items serve as immutable baseline for distribution metrics
2. **Mod Autonomy**: Mod items undergo full autonomous optimization
3. **Transparency**: Every change logged with AI reasoning
4. **Safety**: Strict validation before any modifications
5. **Reversibility**: Complete change history for rollback

---

## The 8-Step Process

### STEP 1: Load Configuration

**Purpose**: Initialize server profile and workspace

**What it does:**
- Loads or creates `config/server_profile.json`
- Initializes output directories
- Sets up logging infrastructure

**Profile Structure:**
```json
{
  "version": "2.0",
  "server_name": "Stelliferum Forge",
  "playstyle": "Hardcore Survival",
  "economy_mode": "balanced|hardcore|grindy|casual",
  "economy_multiplier": 1.0,
  "military_loot_scarcity": "high|medium|low",
  "autonomous_mode": true,
  "ai_decision_confidence_threshold": 0.75
}
```

**Key Settings:**
- `economy_mode`: Overall tone
  - `balanced`: Mix of challenges and rewards
  - `hardcore`: Scarce resources, difficult survival
  - `grindy`: Grind-heavy, limited RNG
  - `casual`: Abundant loot, easier gameplay
- `autonomous_mode`: Enable/disable AI recommendations
- `ai_decision_confidence_threshold`: Minimum confidence (0.0-1.0) for applying changes

---

### STEP 2: Parse Inventory Data

**Purpose**: Load and normalize item inventory

**What it does:**
- Reads `output/items.csv` (exported from C++ auditor)
- Converts rows to `InventoryItem` objects
- Classifies items by source (vanilla vs mod)
- Computes item metadata (tier, category, weapon status)

**Input Format (CSV):**
```csv
Classname,Category,Tier,Nominal,Lifetime,SourceFile,ModSource
M4A1,Weapon,1,25,3600,mod_file.xml,@Morty'sWeapons
Makarov,Weapon,3,15,5400,types.xml,Vanilla
```

**Output Summary:**
```
✓ Parsed 5000 items
  Distribution: 3000 vanilla, 2000 mod items
  Tier distribution: {1: 450, 2: 800, 3: 1200, 4: 1800, 5: 750}
```

---

### STEP 3: Analyze Distribution Metrics

**Purpose**: Compute statistical baselines and distribution patterns

**What it does:**
- Calculates per-tier statistics (mean, median, std dev, range)
- Analyzes per-category patterns
- Computes weapon distribution curves
- Identifies distribution anomalies

**Analysis Output:**
```
DISTRIBUTION ANALYSIS:
  Total Items: 5000
  Tier Stats:
    T1: 450 items, Mean=20.5, Median=20
    T2: 800 items, Mean=18.3, Median=18
    T3: 1200 items, Mean=15.2, Median=15
    T4: 1800 items, Mean=12.1, Median=12
    T5: 750 items, Mean=8.4, Median=8
  Weapons: 1200 total
```

**Tier Interpretation:**
- **Tier 1**: Rarest (military high-end)
- **Tier 2**: Rare (endgame gear)
- **Tier 3**: Moderate (standard weapons)
- **Tier 4**: Common (basic supplies)
- **Tier 5**: Abundant (trash/consumables)

---

### STEP 4: Identify Economy Gaps

**Purpose**: Detect structural imbalances and missing items

**What it does:**
- Checks tier representation (minimum 5 items per tier)
- Analyzes weapon distribution balance
- Identifies category-level anomalies
- Flags items that deviate from expected distribution

**Gap Types:**
- `tier_underrepresented`: Tier has fewer than 5 items
- `weapon_distribution_imbalance`: High-tier weapons > 2x low-tier weapons
- `category_density_anomaly`: Category distribution unusual for tier
- `outlier_nominal_values`: Nominal value > 2σ from mean

**Example Output:**
```
✓ Identified 3 economy gaps
  - tier_underrepresented (severity: high)
  - weapon_distribution_imbalance (severity: high)
  - category_density_anomaly (severity: medium)
```

---

### STEP 5: Generate LLM Recommendations

**Purpose**: Use OLLAMA to generate AI-reasoned modifications

**What it does:**
- Filters candidate items (mods + outliers)
- Queries OLLAMA for each item
- Generates recommendations with confidence scores
- Captures AI reasoning for audit trail

**LLM Prompt Context:**
```
CONTEXT:
- Server Playstyle: Hardcore Survival
- Economy Mode: balanced
- Item: M4A1 (Category: Weapon)
- Source: mod
- Current Tier: 1
- Current Nominal: 25
- Tier Baseline: Mean=20.5, Median=20, StdDev=2.1

TASK: Analyze if nominal fits distribution.
OUTPUT: {"recommendation": "ADJUST|ACCEPT|REPRIORITIZE", ...}
```

**OLLAMA Model Selection:**
- `llama3` (default): Good balance of speed/reasoning
- `mistral`: Fast, lightweight
- `neural-chat`: Specialized for technical reasoning

**Environment Setup:**
```bash
# Install OLLAMA
curl https://ollama.ai/install.sh | sh

# Download model
ollama pull llama3

# Run service
ollama serve
```

**Recommendation Output:**
```json
{
  "recommendation": "ADJUST",
  "new_nominal": 18,
  "new_tier": null,
  "confidence": 0.87,
  "reasoning": "M4A1 nominal 25 is above tier 1 median of 20. For hardcore server, 18 is more appropriate."
}
```

---

### STEP 6: Validate Changes Against Rules

**Purpose**: Apply safety gates before modifying data

**What it does:**
- Checks confidence thresholds
- Validates nominal bounds (1-500)
- Verifies tier validity (1-5)
- Separates accepted vs rejected recommendations

**Validation Rules:**
1. **Confidence**: Must meet `ai_decision_confidence_threshold` (default 0.75)
2. **Nominal Bounds**: 1 ≤ nominal ≤ 500
3. **Tier Range**: 1 ≤ tier ≤ 5
4. **Source Protection**: Vanilla items skipped (unless explicitly in config)

**Output:**
```
✓ Validated: 42 | Rejected: 8
  ✗ M4A1: Low confidence (0.62)
  ✗ Cough_Syrup: Nominal out of bounds: 650
  ✗ PantsMiltacs_Camo: Invalid tier: 6
```

---

### STEP 7: Apply Modifications

**Purpose**: Execute validated changes to inventory

**What it does:**
- Iterates through validated recommendations
- Updates item nominal/tier values in-memory
- Builds comprehensive change log
- Maintains before/after audit trail

**Change Log Structure:**
```json
{
  "timestamp": "2026-02-12T10:30:45.123456",
  "total_changes": 42,
  "modifications": [
    {
      "item": "M4A1",
      "change_type": "nominal",
      "original_nominal": 25,
      "new_nominal": 18,
      "reasoning": "Adjusted for hardcore economy balance"
    },
    ...
  ]
}
```

---

### STEP 8: Export Results & Reports

**Purpose**: Generate output artifacts and documentation

**What it does:**
- Exports modified inventory to timestamped CSV
- Saves all recommendations as JSON
- Archives change log to history directory
- Generates balance report in text format

**Output Files:**

1. **Balanced Inventory**
   ```
   output/balanced_inventory_20260212_103045.csv
   ```
   CSV with updated nominal/tier values

2. **Recommendations Journal**
   ```
   output/recommendations_20260212_103045.json
   ```
   Complete reasoning for all AI decisions

3. **Change History**
   ```
   history/changes_log_20260212_103045.json
   ```
   Audit trail with timestamps and full diffs

4. **Balance Report**
   ```
   output/balance_report_20260212_103045.txt
   ```
   Human-readable summary of changes

**Directory Structure After Run:**
```
output/
├── items.csv                           (original inventory)
├── balanced_inventory_20260212_*.csv   (modified)
├── recommendations_20260212_*.json     (AI decisions)
├── balance_report_20260212_*.txt       (summary)
└── economy_balancer.log                (debug log)

history/
└── changes_log_20260212_*.json         (audit trail)
```

---

## Usage Guide

### Quick Start

```bash
# 1. Export inventory from C++ auditor
#    File → Export → CSV

# 2. Ensure OLLAMA is running
ollama serve

# 3. Run economy balancer
python3 economy_balancer.py

# 4. Review results in output/ and history/
```

### Configuration Customization

```bash
# Edit config/server_profile.json
{
  "economy_mode": "hardcore",         # Less loot overall
  "military_loot_scarcity": "high",   # Very rare military gear
  "economy_multiplier": 0.5,          # 50% of default drop rates
  "ai_decision_confidence_threshold": 0.85  # Strict AI decisions
}
```

### Different Economy Types

**Balanced Mode** (default)
```json
{
  "economy_mode": "balanced",
  "economy_multiplier": 1.0,
  "military_loot_scarcity": "medium"
}
```

**Hardcore Mode** (survival-focused)
```json
{
  "economy_mode": "hardcore",
  "economy_multiplier": 0.5,
  "military_loot_scarcity": "high"
}
```

**Grindy Mode** (time-intensive)
```json
{
  "economy_mode": "grindy",
  "economy_multiplier": 0.3,
  "military_loot_scarcity": "very_high"
}
```

**Casual Mode** (relaxed)
```json
{
  "economy_mode": "casual",
  "economy_multiplier": 2.0,
  "military_loot_scarcity": "low"
}
```

### Disabling Autonomous Mode

For manual review only (no modifications applied):

```json
{
  "autonomous_mode": false
}
```

Then recommendations are generated but not applied. Review `output/recommendations_*.json` manually.

---

## Advanced Features

### Confidence Threshold Tuning

```json
{
  "ai_decision_confidence_threshold": 0.9  # Very strict
}
```

- Higher values (0.9): Only high-confidence recommendations accepted
- Lower values (0.5): More permissive, captures edge cases
- Recommended: 0.75 (default)

### Custom OLLAMA Models

```bash
# Use a different model
export OLLAMA_MODEL=mistral
python3 economy_balancer.py

# Or pull specialized models
ollama pull neural-chat
export OLLAMA_MODEL=neural-chat
python3 economy_balancer.py
```

### Batch Processing

Process multiple server profiles:

```bash
# Create different profiles
config/hardcore_profile.json
config/casual_profile.json

# Then modify script to loop through profiles
# (See CORTEX_BATCH_MODE in future versions)
```

---

## Troubleshooting

### OLLAMA Connection Error
```
✗ Cannot reach OLLAMA at http://localhost:11434/api/generate
```

**Solution:**
```bash
# Start OLLAMA service
ollama serve

# In another terminal
python3 economy_balancer.py
```

### JSON Parse Error
```
✗ JSON parse error for M4A1
```

**Cause**: OLLAMA returned invalid JSON

**Solution:**
- Check OLLAMA model: `ollama list`
- Ensure model supports JSON output format
- Try: `ollama pull llama3:latest`

### Missing items.csv
```
✗ Inventory file not found: output/items.csv
```

**Solution:**
1. Run C++ auditor: `build/bin/StelliferumAuditor.exe`
2. Load types.xml files
3. Export to CSV: File → Export → Inventory CSV
4. Then run economy_balancer.py

### Low Confidence Recommendations
```
✓ Validated: 5 | Rejected: 37
  ✗ Item1: Low confidence (0.62)
```

**Solution:**
- Lower threshold: `"ai_decision_confidence_threshold": 0.65`
- Review OLLAMA model performance
- Increase context clarity in tier stats

---

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Parse 5,000 items | ~200ms |
| Analyze distribution | ~100ms |
| Identify gaps | ~50ms |
| Generate 50 recommendations | ~150s (network/OLLAMA dependent) |
| Validate recommendations | ~10ms |
| Apply modifications | ~5ms |
| Export results | ~200ms |
| **Total (50 items)** | ~160s |

---

## Safety & Rollback

### Complete Audit Trail

Every run generates timestamped artifacts:
```
history/changes_log_20260212_103045.json
```

Contains:
- Exact changes made
- Timestamp of each modification
- AI reasoning for each decision
- Original and new values

### Manual Rollback

```bash
# Reset to original inventory
cp output/items.csv output/items_backup.csv

# Or restore from previous run
# (Manually revert item nominals based on change log)
```

### Dry-Run Mode

Planned feature (future versions):
```json
{
  "dry_run_mode": true  // Generate recommendations without applying
}
```

---

## Integration with C++ Auditor

```
C++ Auditor (UI)
      ↓
      Export: items.csv
      ↓
Python Economy Cortex
      ↓
      Import modified CSV
      ↓
C++ Auditor (Visualization)
      ↓
      Apply to types.xml
```

## Future Enhancements

Planned features:
- [ ] Batch profile processing
- [ ] Dry-run mode
- [ ] Interactive confidence adjustment
- [ ] Mod-specific balancing rules
- [ ] Real-time OLLAMA integration
- [ ] Multi-tier strategy matrices
- [ ] Economy impact forecasting
- [ ] Automated A/B testing framework

---

## License & Attribution

Part of the **Stelliferum Auditor** project.
Hybrid AI system combining statistical analysis with LLM reasoning.
