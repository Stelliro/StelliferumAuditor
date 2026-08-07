# SYSTEM OVERVIEW - Complete Reference

## What is Stelliferum Auditor?

A comprehensive DayZ server economy management system with two components:

1. **C++ GUI Auditor** - Visual tool for parsing, auditing, and exporting DayZ economy files
2. **Python Economy Cortex** - Autonomous AI-driven economy optimizer (8-step workflow)

---

## Component 1: C++ Auditor GUI

### What It Does
- Loads `types.xml` files (vanilla + mods)
- Validates item economy settings
- Detects issues (duplicates, orphans, incorrect zones)
- Exports data to various formats
- Provides interactive UI for manual fixes

### Building

```cmd
# Windows (easiest)
build.bat

# Or PowerShell
.\build.ps1

# Or manual
mkdir build && cd build && cmake .. && cmake --build . --config Release
```

### Running
```cmd
build\bin\StelliferumAuditor.exe
```

### Features
- Automatic pre-upload restore point (`backups/last_upload/restore_manifest.txt`)
- Two UI modes: Developer (detailed) and Ben (simplified)

### Output
```

### Restore Last Upload Snapshot
```cmd
build\bin\StelliferumAuditor.exe --restore-last-upload
```
output/
├── items.csv                    ← Use this for economy optimizer
├── audit_report.txt
├── merged_types.xml
└── ...
```

**Key Output**: `items.csv` is fed to the Python economy optimizer

---

## Component 2: Python Economy Cortex (NEW!)

### What It Does

Autonomously optimizes DayZ economy using an 8-step workflow:

```
1. Load Configuration          → Initialize server profile
2. Parse Inventory Data        → Read exported CSV
3. Analyze Distribution        → Statistical metrics per tier
4. Identify Economy Gaps       → Find imbalances
5. Generate Recommendations   → LLM (OLLAMA) reasoning
6. Validate Changes            → Safety gates & rules
7. Apply Modifications         → Update nominal values
8. Export Results              → Records & audit trail
```

### Architecture

```
Vanilla Items (Immutable Baseline)
         ↑
         │
    Distribution Analysis
    (Statistical Baseline)
         ↑
         │
    Mod Items (Autonomous)
         ↑
         │
    OLLAMA LLM
    (AI Reasoning)
         ↑
         │
    Validation Layer
    (Safety Rules)
         ↑
         │
    Final Balanced Inventory
```

### Key Features

✓ **Hybrid Design**: Vanilla baseline + AI-driven mod optimization
✓ **Full Autonomy**: No human interaction needed for recommendations
✓ **Transparent**: Complete reasoning for every decision
✓ **Safe**: Multi-layer validation before applying changes
✓ **Auditable**: Full change history with timestamps
✓ **Reversible**: Complete before/after data captured

### How to Use

```bash
# Step 1: Export from C++ Auditor
# File → Export → CSV (creates output/items.csv)

# Step 2: Start OLLAMA (background service)
ollama serve

# Step 3: Run the optimizer
python3 economy_balancer.py

# Step 4: Review results
cat output/balance_report_*.txt
```

### Configuration

Edit `config/server_profile.json`:

```json
{
  "server_name": "Stelliferum Forge",
  "playstyle": "Hardcore Survival",
  "economy_mode": "balanced",              // Mode: balanced|hardcore|grindy|casual
  "economy_multiplier": 1.0,               // Global loot multiplier
  "military_loot_scarcity": "high",        // Military gear rarity
  "autonomous_mode": true,                 // Enable AI
  "ai_decision_confidence_threshold": 0.75 // Min AI confidence
}
```

### Output Files

After running `economy_balancer.py`:

```
output/
├── items.csv                          (original)
├── balanced_inventory_20260212_*.csv  (MODIFIED - use this!)
├── recommendations_20260212_*.json    (AI decisions)
├── balance_report_20260212_*.txt      (human summary)
└── economy_balancer.log               (debug log)

history/
└── changes_log_20260212_*.json        (audit trail)
```

---

## Complete Workflow

```
                    ┌─────────────────────┐
                    │  Raw types.xml      │
                    │  + Mod XMLs         │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  C++ Auditor GUI    │
                    │  - Parse XML        │
                    │  - Detect issues    │
                    │  - Export items.csv │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  items.csv          │
                    │  (Inventory Data)   │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  Economy Cortex     │
                    │  8-Step Process     │
                    │  - Analyze          │
                    │  - Recommend (AI)   │
                    │  - Validate         │
                    │  - Apply            │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  balanced_inventory │
                    │  _*.csv             │
                    │  (Optimized)        │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  Import back into   │
                    │  C++ Auditor        │
                    │  Export final       │
                    │  types.xml          │
                    └──────────┬──────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │  Deploy to Server   │
                    │  Copy types.xml     │
                    │  Restart DayZ       │
                    └─────────────────────┘
```

---

## File Reference

### Build Scripts

| File | Platform | Usage |
|------|----------|-------|
| `build.bat` | Windows batch | `build.bat` or `build.bat debug` |
| `build.ps1` | Windows PowerShell | `.\build.ps1` or `.\build.ps1 -Config Debug` |

### Documentation

| File | Purpose |
|------|---------|
| `README.md` | Main project information |
| `BUILD.md` | Detailed build instructions |
| `ECONOMY_CORTEX.md` | 8-step economy system guide |
| `QUICK_START.md` | Quick setup (original goals) |
| This file | System overview reference |

### Configuration

| File | Purpose |
|------|---------|
| `config/server_profile.json` | Economy mode & AI settings |
| `config/tier_rules.json` | Tier definitions |
| `config/known_items.json` | Pre-categorized items database |
| `config/ftp.ini` | FTP deployment settings |

### Source Code

| File | Purpose |
|------|---------|
| `src/main.c` | Application entry point |
| `src/ui.c` | Raylib GUI implementation |
| `src/parser.c` | XML parsing logic |
| `src/auditor.c` | Core audit engine |
| `src/writer.c` | Export formatting |
| `src/loot_manager.c` | Loot-specific logic |
| `economy_balancer.py` | AI economy optimizer |

### Execution Artifacts

| Directory | Contents |
|-----------|----------|
| `build/` | CMake build directory |
| `build/bin/` | Compiled executables |
| `output/` | Exported data & reports |
| `history/` | Change audit trail |

---

## Technology Stack

### C++ Auditor
- **Language**: C11
- **UI Framework**: Raylib 5.5 + raygui
- **Build System**: CMake 3.16+
- **Compiler**: MSVC (Windows), GCC/Clang (Linux/macOS)
- **Memory**: ~195MB (65,000 items × ~3KB each)

### Python Economy Cortex
- **Language**: Python 3.8+
- **External Service**: OLLAMA (LLM engine)
- **Models**: llama3 (default), mistral, neural-chat, etc.
- **Network**: HTTP POST to OLLAMA API
- **Dependencies**: requests library

---

## Tier System Explained

### The 5 Tiers

```
TIER 1: MILITARY (Rarest)
├─ Mean Nominal: ~20
├─ Examples: M4A1, VSS, high-tier military gear
└─ Spawn Rate: 2-4%

TIER 2: ENDGAME (Rare)
├─ Mean Nominal: ~18
├─ Examples: Exotic weapons, specialized gear
└─ Spawn Rate: 3-5%

TIER 3: MODERATE (Medium)
├─ Mean Nominal: ~15
├─ Examples: Standard weapons, tools
└─ Spawn Rate: 5-10%

TIER 4: COMMON (Common)
├─ Mean Nominal: ~12
├─ Examples: Basic supplies, ammunition
└─ Spawn Rate: 10-20%

TIER 5: ABUNDANT (Most)
├─ Mean Nominal: ~8
├─ Examples: Food, rags, trash items
└─ Spawn Rate: 20-40%
```

### Nominal Value

The core economy parameter - spawn availability percentage.

- **Vanilla items**: Use as immutable baseline
- **Mod items**: Subject to AI optimization
- **Range**: 1 (rarest) to 500 (most common)
- **Higher = More frequently spawned**

---

## Running the System

### Prerequisites

```bash
# Required
✓ CMake 3.16+ (for C++ build)
✓ C11 compiler (Visual Studio, GCC, or Clang)
✓ Python 3.8+
✓ Git (for dependencies)

# For AI Optimization
✓ OLLAMA (https://ollama.ai)
✓ Model like llama3 (ollama pull llama3)
```

### Step-by-Step Execution

```bash
# 1. BUILD THE AUDITOR
cd /path/to/StelliferumAuditor
build.bat                      # Windows
# OR
mkdir build && cd build && cmake .. && make -j$(nproc)  # Linux/macOS

# 2. RUN THE AUDITOR
build\bin\StelliferumAuditor.exe                        # Windows
# OR
./build/bin/StelliferumAuditor                          # Linux/macOS

# In GUI:
#   File → Load → Select types.xml files
#   File → Export → CSV

# 3. START OLLAMA (background)
ollama serve

# In another terminal:
# 4. RUN ECONOMY CORTEX
python3 economy_balancer.py

# 5. CHECK RESULTS
cat output/balance_report_*.txt
```

---

## Common Tasks

### Change Economy Mode

Edit `config/server_profile.json`:

```json
// Hardcore (scarce resources)
{"economy_mode": "hardcore", "economy_multiplier": 0.5}

// Casual (abundant)
{"economy_mode": "casual", "economy_multiplier": 2.0}

// Grindy (time-intensive)
{"economy_mode": "grindy", "economy_multiplier": 0.3}
```

### Disable Autonomous Mode

```json
{"autonomous_mode": false}
```

Then recommendations generated but not applied.

### Review AI Decisions

```bash
# Generated JSON with reasoning for each item
cat output/recommendations_*.json | jq .

# Or import into spreadsheet
output/balanced_inventory_*.csv
```

### Rollback Changes

```bash
# Check what changed
cat history/changes_log_*.json | jq '.modifications[]'

# Manually revert nominal values using original CSV
cp output/items.csv <rollback>
```

---

## Troubleshooting Guide

| Issue | Solution |
|-------|----------|
| Cannot find CMake | Install from cmake.org, add to PATH |
| Visual Studio not found | Install VS 2022 with C++ workload |
| OLLAMA connection failed | Run `ollama serve` in terminal |
| items.csv not found | Export from C++ auditor first |
| Low AI confidence | Lower threshold in server_profile.json |
| JSON parse error | Check OLLAMA model compatibility |

---

## Documentation Hierarchy

```
This File
├─ High-level overview
├─ Architecture explanation
└─ Quick reference

├─ BUILD.md
│  └─ Detailed build procedures & troubleshooting
│
├─ ECONOMY_CORTEX.md
│  └─ Complete 8-step system documentation
│
└─ README.md
   └─ Original project information
```

---

## Performance Characteristics

| Operation | Time | Notes |
|-----------|------|-------|
| Build (Release) | 30-60s | Depends on CPU cores |
| Parse 5,000 items | 200ms | XML parsing |
| Analyze distribution | 100ms | Statistics compute |
| AI recommendations (20 items) | ~100s | Network/OLLAMA latency |
| Apply modifications | <10ms | In-memory updates |
| Full 8-step process | ~150s | Including AI |

---

## Next Steps

1. **Build**: `build.bat`
2. **Run Auditor**: `build\bin\StelliferumAuditor.exe`
3. **Export Data**: File → Export → CSV
4. **Configure**: Edit `config/server_profile.json` for economy mode
5. **Optimize**: `python3 economy_balancer.py` (requires OLLAMA)
6. **Deploy**: Review results, import back into auditor, deploy to server

---

**Version**: 2.0
**Last Updated**: 2026-02-12
**Status**: Production Ready
