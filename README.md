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

Work planned after the **1.0.0** baseline. Issues hold full write-ups; this table is the living checklist.

Product features and engineering debt are listed separately. Several engineering items came from an external review (WinSCP fragility, Python friction, platform lock-in, config validation) and were **confirmed against the current C/C++ backend**—with extra findings from code inspection.

### Product

| Status | Item | Issue |
| --- | --- | --- |
| **Todo** | **Mod containers & mod-function loot params by tier** — Detect containers/storage from different mods; per-mod loot parameters that can differ by economy tier; apply via audit/export. | [#1](https://github.com/Stelliro/StelliferumAuditor/issues/1) |
| **Todo** | **UI overhaul** — Navigation, large item/issue tables (filter/search), swarm + FTP status, safer empty states / pre-upload guards, 1080p/1440p polish. | [#2](https://github.com/Stelliro/StelliferumAuditor/issues/2) |
| **Todo** | **Better loot & shop configuration** — First-class loot policy editing plus stronger shop/trader setup (stock, pricing, categories, currencies, previews/diffs). | [#3](https://github.com/Stelliro/StelliferumAuditor/issues/3) |
| **Todo** | **Standalone FTP tool** — Lightweight download/upload/sync without the full Auditor; shared credentials/paths, dry-run, clear errors. Best after native transfer (#5). | [#4](https://github.com/Stelliro/StelliferumAuditor/issues/4) |
| **Todo** | **Expansion Market export parity** — UI offers Expansion Market but writer currently falls back to TraderPlus. | [#10](https://github.com/Stelliro/StelliferumAuditor/issues/10) |

### Engineering & reliability

| Status | Item | Issue |
| --- | --- | --- |
| **Todo** | **Native SFTP/FTP stack** — Replace WinSCP subprocess + PE patch + log scraping with libcurl/libssh2 (or similar). Structured errors, cancel/progress, no opaque child hangs. | [#5](https://github.com/Stelliro/StelliferumAuditor/issues/5) |
| **Todo** | **Zero-dependency AI helpers** — Stop requiring admins to install Python/Ollama for cortex/Phoenix (`system("python …")`); bundle, reimplement, or graceful offline fallback. | [#6](https://github.com/Stelliro/StelliferumAuditor/issues/6) |
| **Todo** | **Cross-platform builds** — Linux/macOS once transfers are native and Win32-only paths are abstracted. | [#7](https://github.com/Stelliro/StelliferumAuditor/issues/7) |
| **Todo** | **Config validation & dead loaders** — Schema-checked INI/JSON with ImGui errors; wire or remove `tier_rules.json` / `known_items.json` stubs; real AI-balancer JSON reimport. | [#8](https://github.com/Stelliro/StelliferumAuditor/issues/8) |
| **Todo** | **Transfer security** — Drop trust-any `-hostkey=*`; no password on process argv; scrub `.TEMP` scripts/logs. | [#9](https://github.com/Stelliro/StelliferumAuditor/issues/9) |
| **Todo** | **Reliability** — Automated tests + CI; soft `MAX_ITEMS`/batch limits with UI; thread safety for pipeline/FTP vs UI; split monolithic `ui.cpp`. | [#11](https://github.com/Stelliro/StelliferumAuditor/issues/11) |

### Review notes (confirmed in code)

1. **WinSCP is a weak link (agree + more).** Transfers go through generated scripts and `CreateProcess` on an embedded WinSCP binary (`ftp_manager.c`). The app also **patches the PE subsystem** at runtime to invent `WinSCP.com` (`dependency_manager.c`). Connection quality and error handling depend on log tailing. Native libcurl/libssh2 is the right long-term fix (#5).

2. **Python cortex friction (agree + more).** Optional helpers are launched with bare `system("python …")` from C. They further expect **Ollama/Qwen** and `requests`—far more than “Python on PATH.” Core 1.0.0 should stay double-click; AI extras need packaging or offline paths (#6).

3. **Platform lock-in (agree).** CMake/raylib/ImGui are portable, but WinSCP embedding, Win32 threads, and Windows-first scripts pin the product. Native transfers unlock Linux/macOS (#5 → #7).

4. **Config validation (agree, nuance).** There is **no nlohmann/json** in-tree—JSON is hand-rolled string scanning in several modules. `loot_policy.ini` loads with soft defaults; `known_items.json` is a **stub loader** (`return true`); balancer reimport is a **stub**. Users need red UI errors, not silent wrong economies (#8).

5. **Extra findings from the backend**
   - **Transfer security:** SFTP uses `-hostkey=*`; credentials are passed as WinSCP `/parameter` (visible in process listings); `.TEMP/winscp*.log` may retain sensitive detail (#9).
   - **Incomplete shop path:** Expansion Market export not implemented; silent-ish fallback to TraderPlus (#10).
   - **Hard capacity / buffers:** `MAX_ITEMS` can skip remaining classnames; some WinSCP batch scripts are fixed-size buffers that truncate when overfull (#11).
   - **Concurrency:** Background download/pipeline/upload threads share auditor state with minimal locking (#11).
   - **No automated tests/CI** in the repo today (#11).
   - **UI mass:** `ui.cpp` is ~2.6k lines—hard to evolve safely alongside #2.

### Suggested order

1. **#9** quick wins on host keys / credential handling (even before full rewrite)  
2. **#5** native transfer (unblocks #4 standalone FTP and #7 cross-platform)  
3. **#8** config honesty + validation (prevents bad economies)  
4. **#3 / #1 / #10** loot-shop-container product work on a solid base  
5. **#2 / #11** UI overhaul + modularization + tests  
6. **#6** package or demote Python/AI extras  
7. **#7** publish non-Windows builds when #5 is done  

---


## License

Internal tool for **Stelliferrum Forge**. All rights reserved unless otherwise noted by the project owner.

---

**Stelliferrum Forge Dev Team**
