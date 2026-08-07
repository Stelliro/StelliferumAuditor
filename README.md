# Stelliferum Auditor

**Desktop economy toolkit for DayZ servers** — pull mission/mod XMLs over SFTP, audit and merge loot (`types.xml` and friends), apply tier policy, export trader/SFL configs, and push a clean economy back to the host.

Built for **Stelliferrum Forge** (*“You are not a hero; you are prey.”*).

---

## Features

- **SFTP / FTP download** via portable WinSCP — smart sync, local edits preserved
- **Base snapshot** — first pull copies originals into `base/` (never mutated by the app)
- **Multi-file economy load** — vanilla + every mod `types.xml` in one pass
- **XML classification** — economy, spawnable, territory, globals, events, trader/config
- **11-tier loot policy** — scavenger coast through mythic / collectible tiers
- **Conflict merge** — duplicate classnames resolved (mod-aware)
- **Gap fill** — missing usage, tier, min, lifetime filled from policy
- **Swarm pipeline** — index → sort → parse → audit → stitch → export
- **Exports** — merged `types.xml`, spawnables, economy core, trader shops, SearchForLoot, CSV, audit reports, mod manifest
- **Upload restore point** — last-upload snapshot + headless restore CLI
- **Optional Python cortex** — `economy_balancer.py` for offline distribution analysis

## Example item catalog

A checked-in snapshot of a full economy export, rendered as Markdown grids:

**[examples/items/README.md](examples/items/README.md)** — summary tables by tier & category, plus full grids under:

| Section | Path |
| --- | --- |
| Index + preview grid | [examples/items/README.md](examples/items/README.md) |
| By tier | [examples/items/by-tier/](examples/items/by-tier/) |
| By category | [examples/items/by-category/](examples/items/by-category/) |

---

## Quick start

### Requirements

| Tool | Notes |
| --- | --- |
| Windows 10/11 | Primary target |
| Visual Studio 2022 | C++ desktop workload |
| CMake 3.16+ | On `PATH` |
| Git | Fetches raylib / ImGui / rlImGui |
| WinSCP Portable | See [libs/README.md](libs/README.md) |
| Python 3.10+ | Optional: offline economy cortex / helpers |

### Build

```cmd
build.bat
```

Or:

```powershell
.\build.ps1
```

### Run

```cmd
build\bin\Release\StelliferumAuditor.exe
```

### Restore last upload (headless)

```cmd
build\bin\Release\StelliferumAuditor.exe --restore-last-upload
```

### Typical workflow

1. Copy `config/ftp.ini.example` → `config/ftp.ini` and set host/user/pass  
2. Adjust `config/server_paths.ini` (`REMOTE_ROOT`, mission paths, shop mod)  
3. **Download server files** → lands in `downloaded_mods/`  
4. **Load scanned data** → parse economy XMLs  
5. **Run audit** / resolve conflicts / apply tier policy  
6. **Export** → `output/` (`types.xml`, `items.csv`, shops, reports, …)  
7. Upload when ready; use restore if a push goes wrong  

---

## Directory layout

```text
StelliferumAuditor/
├── src/                      # C/C++ application sources
├── include/                  # Public headers (auditor, loot, web lookup, …)
├── config/                   # Policy + path templates (no secrets in git)
│   ├── loot_policy.ini       # live tier / blacklist policy (source of truth)
│   ├── server_paths.ini.example
│   ├── ftp.ini.example       # copy → ftp.ini (gitignored)
│   └── …                     # workshop IDs, SFL template, legacy JSON helpers
├── examples/
│   └── items/                # Example Markdown item grids (export snapshot)
├── docs/                     # Build / economy / system notes
├── libs/                     # WinSCP portable (local; see libs/README.md)
├── mods/                     # Optional local mod sources (binaries ignored)
├── base/                     # Unmodified server snapshot (local, gitignored)
├── downloaded_mods/          # Live sync tree (local, gitignored)
├── output/                   # Export products (local, gitignored)
├── backups/                  # Upload restore points (local, gitignored)
├── build.bat / build.ps1
├── CMakeLists.txt
├── economy_balancer.py       # Optional offline economy cortex
├── phoenix_item_scanner.py
└── README.md
```

Runtime folders (`base/`, `downloaded_mods/`, `output/`, `build/`, …) are **gitignored** so the repo stays source-only.

---

## Configuration

### `config/ftp.ini` (local only)

```ini
HOST=your.server.hostname
PORT=22
USER=your_username
PASS=your_password
```

### `config/server_paths.ini`

```ini
REMOTE_ROOT=/path/on/host/
LOCAL_ROOT=downloaded_mods
DOWNLOAD_ALL_XML=1
DOWNLOAD_CONFIG_FILES=1
RESCAN_DAYS=7
SHOP_MOD=drjones
```

### `config/loot_policy.ini`

**Source of truth** for tier names, spawn/trade/black-market flags, contraband tier, Bitcoin spawn range, and classname blacklist. Edited via the in-app Loot Policy tab or by hand.

> Note: `config/tier_rules.json` and `config/known_items.json` are present for historical/docs purposes; the C runtime currently loads **`loot_policy.ini`**. Wiring or removing the dead JSON loaders is tracked under roadmap [#8](https://github.com/Stelliro/StelliferumAuditor/issues/8).

---

## Economy tiers

| Tier | Name | Zone | Role |
| ---: | --- | --- | --- |
| 1 | Scavenger | Civilian | Coast / junk density |
| 2 | Survivor | Civilian | Inland towns |
| 3 | Constable | Civilian | Police / civic |
| 4 | Outdoorsman | Civilian | Hunting / rural |
| 5 | Insurgent | Military | Checkpoints |
| 6 | Infantry | Military | Major bases |
| 7 | Spec-Ops | Military | High-tier military |
| 8 | Operator | End-Game | Heli / rare |
| 9 | Black Market | End-Game | Bunker / illicit |
| 10 | Mythic | End-Game | Ultra-rare |
| 11 | Collectible / Craftable | Special | Caps, craft-only, zero-spawn |

See the [example catalog](examples/items/README.md) for a full classname grid from a real export.

---

## Key outputs (`output/`)

| File / folder | Purpose |
| --- | --- |
| `types.xml` | Merged economy types |
| `cfgspawnabletypes.xml` | Merged spawnable attachments/cargo |
| `items.csv` | Spreadsheet of classnames + tier stats (feeds the catalog script) |
| `mod_manifest.md` | Mod → file inventory |
| `shops/` | Per-shop trader files |
| `TraderConfig.txt` | Dr. Jones-style trader config |
| `SearchForLoot.json` | Search For Loot Improved config |
| `audit_report*.txt` | Human-readable audit |

---

## Dependencies

| Library | Role |
| --- | --- |
| [raylib 5.5](https://www.raylib.com/) | Window / input (CMake FetchContent) |
| [Dear ImGui](https://github.com/ocornut/imgui) | Desktop UI |
| [rlImGui](https://github.com/raylib-extras/rlImGui) | raylib backend for ImGui |
| [WinSCP](https://winscp.net/) | SFTP/FTP scripting (portable binary, local) |

More build detail: [docs/BUILD.md](docs/BUILD.md). System design: [docs/SYSTEM_OVERVIEW.md](docs/SYSTEM_OVERVIEW.md).

---

## Optional Python helpers

| Script | Purpose |
| --- | --- |
| `economy_balancer.py` | Offline economy analysis / cortex |
| `phoenix_item_scanner.py` | Item capacity / Phoenix scan helpers |
| `fix_spawnables.py` / `validate_spawnables.py` | Spawnable maintenance utilities |

---

## Roadmap

Work planned after **1.0.0**. Ordered by **implementation priority** (dependencies + risk first, features after the foundation can hold them). Issue numbers are GitHub links, not priority numbers.

| Priority | Status | Item | Issue | Why here |
| ---: | --- | --- | --- | --- |
| 1 | **Todo** | **Transfer security** — Host-key pinning (drop `-hostkey=*`), no password on process argv, scrub `.TEMP` scripts/logs. | [#9](https://github.com/Stelliro/StelliferumAuditor/issues/9) | Smallest high-impact safety fix; can land while WinSCP still exists. |
| 2 | **Todo** | **Native SFTP/FTP stack** — Replace WinSCP subprocess + PE patch + log scraping with libcurl/libssh2. Structured errors, cancel, progress. | [#5](https://github.com/Stelliro/StelliferumAuditor/issues/5) | Removes the weakest runtime dependency; unblocks standalone FTP and non-Windows. |
| 3 | **Todo** | **Config validation & dead loaders** — Schema-checked configs + ImGui errors; wire or delete stub `tier_rules`/`known_items`; real AI JSON reimport. | [#8](https://github.com/Stelliro/StelliferumAuditor/issues/8) | Stops silent wrong economies before we invest in richer policy UI. |
| 4 | **Todo** | **Reliability baseline** — Tests + CI; soft capacity limits; thread safety for pipeline/FTP vs UI; start splitting `ui.cpp`. | [#11](https://github.com/Stelliro/StelliferumAuditor/issues/11) | Safety net before large product/UI refactors. |
| 5 | **Todo** | **Better loot & shop configuration** — First-class policy editing; shop stock/pricing/categories/currencies; previews/diffs. | [#3](https://github.com/Stelliro/StelliferumAuditor/issues/3) | Core product value once config and export paths are trustworthy. |
| 6 | **Todo** | **Mod containers & mod-function loot by tier** — Per-mod container/storage parameters that vary by tier; audit/export integration. | [#1](https://github.com/Stelliro/StelliferumAuditor/issues/1) | Builds on solid loot policy (#3) instead of another parallel override system. |
| 7 | **Todo** | **Expansion Market export parity** — Real Expansion JSON export; no silent TraderPlus fallback. | [#10](https://github.com/Stelliro/StelliferumAuditor/issues/10) | Natural extension of shop config work (#3). |
| 8 | **Todo** | **UI overhaul** — Navigation, filterable tables, pipeline/FTP status, safer guards, layout polish. | [#2](https://github.com/Stelliro/StelliferumAuditor/issues/2) | Redesign after data/transfer behavior is stable so UI does not encode WinSCP quirks. |
| 9 | **Todo** | **Standalone FTP tool** — Lightweight CLI/UI for sync without the full Auditor. | [#4](https://github.com/Stelliro/StelliferumAuditor/issues/4) | Reuses the native transfer API from #5; thin product on top of #2 patterns. |
| 10 | **Todo** | **Zero-dependency AI helpers** — Bundle, reimplement, or demote Python/Ollama cortex & Phoenix so core stays double-click. | [#6](https://github.com/Stelliro/StelliferumAuditor/issues/6) | Optional power features; must not block daily economy workflows. |
| 11 | **Todo** | **Cross-platform builds** — Linux/macOS after native transfers and OS abstractions. | [#7](https://github.com/Stelliro/StelliferumAuditor/issues/7) | Payoff of #5/#11; publish when Windows path is solid, not before. |

### Why this order

Foundation first: **secure transfers → native transfers → honest config → tests/threads**. Product next: **loot/shops → containers → Expansion**. Experience last: **UI polish → standalone FTP**. Optional/expansion last: **AI packaging → non-Windows**.

### Background (review + code audit)

External review flagged WinSCP fragility, Python/env friction, Windows lock-in, and weak config validation. Backend inspection confirmed those and added: PE subsystem patching for WinSCP, credentials on argv + trust-any host keys, stub loaders (`known_items`, balancer JSON import), incomplete Expansion export, hard `MAX_ITEMS`/batch buffers, lightly synchronized background threads, no CI tests, and a ~2.6k-line `ui.cpp`.

---

## License

Internal tool for **Stelliferrum Forge**. All rights reserved unless otherwise noted by the project owner.

---

**Stelliferrum Forge Dev Team**
