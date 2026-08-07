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
│   ├── server_paths.ini
│   ├── tier_rules.json
│   ├── known_items.json
│   ├── loot_policy.ini
│   └── ftp.ini.example       # copy → ftp.ini (gitignored)
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

### `config/tier_rules.json`

Per-tier nominal/lifetime bounds and allowed usages. The Auditor validates items against these rules during audit.

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

Work planned after the **1.0.0** baseline. Issues hold the full write-ups; this table is the living checklist.

| Status | Item | Issue |
| --- | --- | --- |
| **Todo** | **Mod containers & mod-function loot params by tier** — Detect containers/storage from different mods; set per-mod loot parameters (nominal/min/restock/lifetime, cargo/spawnables, usage/values) that can differ by economy tier; apply via audit/export. | [#1](https://github.com/Stelliro/StelliferumAuditor/issues/1) |
| **Todo** | **UI overhaul** — Cleaner daily workflow UI: navigation, large item/issue tables (filter/search), swarm + FTP status, safer empty states / pre-upload guards, layout polish for 1080p/1440p. | [#2](https://github.com/Stelliro/StelliferumAuditor/issues/2) |
| **Todo** | **Better loot & shop configuration** — First-class loot policy editing (tiers, categories, mod overrides, audit explanations) plus stronger shop/trader setup (stock, pricing, categories, currencies, previews/diffs) across supported trader formats. | [#3](https://github.com/Stelliro/StelliferumAuditor/issues/3) |

### Notes on the above

**Mod containers / mod functions.** Global tier policy is not enough when mods add crates, bags, lockers, vehicle cargo, and custom boxes—each “function” may need its own rules per tier, then written into `types.xml` and spawnables.

**UI.** Scale and clarity for multi-thousand classname economies: find items, understand pipeline state, and export/upload without digging.

**Loot & shops.** Economy and market should be configured in-app (or via clear config) with previews, not only post-hoc hand edits of exported shop files.

---

## License

Internal tool for **Stelliferrum Forge**. All rights reserved unless otherwise noted by the project owner.

---

**Stelliferrum Forge Dev Team**
