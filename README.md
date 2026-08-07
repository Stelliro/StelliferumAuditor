# Stelliferum Auditor

**Desktop economy toolkit for DayZ servers** — pull mission/mod XMLs over SFTP, audit and merge loot (`types.xml` and friends), apply tier policy, export trader/SFL configs, and push a clean economy back to the host.

Built for **Stelliferrum Forge** (*“You are not a hero; you are prey.”*).

---

## Features

- **Native SFTP / FTP** via **libcurl** (libssh2 for SFTP) — preferred on all platforms; smart sync, cancel, progress
- **WinSCP optional** — Windows-only legacy fallback if native is unavailable; **not required on Linux/macOS**
- **Standalone transfer CLI** — same binary: `--ftp-list` / `--ftp-download` / `--ftp-upload` (no GUI)
- **Transfer security** — credentials only from `config/ftp.ini` (never on argv); SFTP host-key / known_hosts on the native path
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
| Windows 10/11 | Primary desktop target |
| Linux / macOS | Transfer + CLI supported via native libcurl (see [docs/BUILD.md](docs/BUILD.md)) |
| Visual Studio 2022 | C++ desktop workload (Windows) |
| CMake 3.16+ | On `PATH` |
| Git | Fetches raylib / ImGui / rlImGui / libcurl+libssh2 (default) |
| OpenSSL dev headers | Linux when building curl via FetchContent (`libssl-dev` / `openssl-devel`) |
| WinSCP Portable | **Optional Windows fallback only** — see [libs/README.md](libs/README.md); **not used on Linux** |
| Python 3.10+ | Optional: offline economy cortex / helpers |

### Build

```cmd
build.bat
```

Or:

```powershell
.\build.ps1
```

Linux / macOS (native transfer only — no WinSCP):

```bash
mkdir -p build && cd build
cmake .. -G "Unix Makefiles"
cmake --build . --config Release --parallel
```

See [docs/BUILD.md](docs/BUILD.md) for `STELLI_USE_LIBCURL` / `STELLI_FETCH_LIBCURL` and platform tables.

### Run

```cmd
build\bin\Release\StelliferumAuditor.exe
```

```bash
./build/bin/StelliferumAuditor
```

### Simple headless commands (console + config)

Short verbs for Alpine / scripts / ops (quiet by default — no focus-stealing windows):

```text
StelliferumAuditor help
StelliferumAuditor list [path]
StelliferumAuditor pull                 # recursive REMOTE_ROOT -> LOCAL_ROOT
StelliferumAuditor pipeline             # local sort/parse/audit/export
StelliferumAuditor push                 # upload standard output/ pack
StelliferumAuditor restore              # newest backups/*/restore_manifest.txt
StelliferumAuditor run <recipe>         # named recipe from config/commands.ini
```

Optional recipes: copy `config/commands.ini.example` → `config/commands.ini`. Use `${REMOTE_ROOT}`, `${REMOTE_TYPES}`, etc. from `server_paths.ini`. Pipeline rules stay in `config/loot_policy.ini`.

Long flags still work for advanced scripting:

```text
StelliferumAuditor --ftp-list [path] [--remote <path>] [--dry-run]
StelliferumAuditor --ftp-download [--remote <path>] [--local <path>] [--dry-run]
StelliferumAuditor --ftp-upload   [--remote <path>] [--local <path>] [--dry-run]
StelliferumAuditor --ftp-push-economy
StelliferumAuditor --regen | --headless | --restore-last-upload
```

| Flag | Meaning |
| --- | --- |
| `--ftp-list [path]` | List remote directory (optional path; default `REMOTE_ROOT`) |
| `--ftp-download` | Download remote → local (file or recursive directory) |
| `--ftp-upload` | Upload local → remote (file or directory) |
| `--remote <path>` | Remote path (default: `REMOTE_ROOT` from `server_paths.ini`) |
| `--local <path>` | Local path (default: `LOCAL_ROOT` from `server_paths.ini`) |
| `--dry-run` | Print planned action; no network transfer |
| `--help` / `-h` / `--ftp-help` | Show transfer CLI help |

Examples (Windows):

```cmd
build\bin\Release\StelliferumAuditor.exe --ftp-list --dry-run
build\bin\Release\StelliferumAuditor.exe --ftp-download --remote /mission --local downloaded_mods --dry-run
build\bin\Release\StelliferumAuditor.exe --ftp-upload --local output --remote /mission/output
```

Port heuristic (same as GUI): **22 / 2222 / 8827 → SFTP**; other ports → FTP. Backend prefers **native libcurl**; on Windows, WinSCP is optional fallback only. **Linux does not require WinSCP** for core transfer.

> **Note:** Bare `--local` (no `--ftp-*`) still means local regen (no FTP). With a transfer flag, `--local <path>` sets the local transfer path.

### Other CLI modes

```cmd
build\bin\Release\StelliferumAuditor.exe --headless
build\bin\Release\StelliferumAuditor.exe --regen
build\bin\Release\StelliferumAuditor.exe --restore-last-upload
```

| Mode | Purpose |
| --- | --- |
| `--headless` | Full download → pipeline → upload |
| `--regen` / bare `--local` | Local pipeline only (no FTP) |
| `--restore-last-upload` | Restore newest pre-upload snapshot |

### Typical workflow

1. Copy `config/ftp.ini.example` → `config/ftp.ini` and set host/user/pass (and host-key policy if using SFTP)  
2. Adjust `config/server_paths.ini` (`REMOTE_ROOT`, mission paths, shop mod)  
3. **Download server files** (GUI or `--ftp-download`) → lands in `downloaded_mods/`  
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
├── libs/                     # Optional WinSCP portable (Windows fallback; see libs/README.md)
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

Copy from [`config/ftp.ini.example`](config/ftp.ini.example). **Never commit real passwords.** Used by the GUI, `--headless`, `--restore-last-upload`, and the `--ftp-*` CLI.

```ini
HOST=your.server.hostname
PORT=22
USER=your_username
PASS=your_password

# SFTP host-key verification (native libcurl path; ports 22/2222/8827)
KNOWN_HOSTS=config/known_hosts
# HOST_KEY_PIN=SHA256:xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
HOST_KEY_POLICY=pin
```

| Key | Purpose |
| --- | --- |
| `HOST` / `PORT` / `USER` / `PASS` | Server credentials — **read from this file only**; never pass passwords on the process command line |
| `KNOWN_HOSTS` | OpenSSH-style known_hosts file (default `config/known_hosts`) |
| `HOST_KEY_PIN` | Optional SHA256 fingerprint pin of the server host key |
| `HOST_KEY_POLICY` | `pin` (default: first-connect append to known_hosts), `fail` (strict), or `trust` (**insecure**, debug only) |

**Security expectations (native path):**

- Passwords are applied only via in-process libcurl options — **not** CreateProcess/argv and **not** plaintext temp scripts.
- SFTP verifies the host key via known_hosts and/or pin; mismatches are rejected (unless `trust`).
- Optional **WinSCP** fallback on Windows is **legacy/weaker** (scripted session; prefer native).

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
| [libcurl](https://curl.se/libcurl/) + [libssh2](https://www.libssh2.org/) | **Native** FTP + SFTP (default `STELLI_USE_LIBCURL`; FetchContent or system) |
| [WinSCP](https://winscp.net/) | **Optional Windows fallback only** — portable binary under `libs/winscp/`; not required on Linux/macOS |

More build detail: [docs/BUILD.md](docs/BUILD.md). Ship brief: [docs/NATIVE_FTP_SHIP_BRIEF.md](docs/NATIVE_FTP_SHIP_BRIEF.md). System design: [docs/SYSTEM_OVERVIEW.md](docs/SYSTEM_OVERVIEW.md).

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
| 1 | **Done** (native path) | **Transfer security** — Host-key / known_hosts + optional pin; no password on process argv; no plaintext temp scripts on native path. WinSCP fallback remains legacy/weaker. | [#9](https://github.com/Stelliro/StelliferumAuditor/issues/9) | Shipped with native libcurl path (`KNOWN_HOSTS` / `HOST_KEY_PIN` / `HOST_KEY_POLICY` in `ftp.ini`). |
| 2 | **Done** | **Native SFTP/FTP stack** — libcurl + libssh2 preferred; structured cancel/progress; WinSCP optional Windows fallback only. | [#5](https://github.com/Stelliro/StelliferumAuditor/issues/5) | Core transfer no longer depends on WinSCP subprocess for the success path. |
| 3 | **Todo** | **Config validation & dead loaders** — Schema-checked configs + ImGui errors; wire or delete stub `tier_rules`/`known_items`; real AI JSON reimport. | [#8](https://github.com/Stelliro/StelliferumAuditor/issues/8) | Stops silent wrong economies before we invest in richer policy UI. |
| 4 | **Todo** | **Reliability baseline** — Tests + CI; soft capacity limits; thread safety for pipeline/FTP vs UI; start splitting `ui.cpp`. | [#11](https://github.com/Stelliro/StelliferumAuditor/issues/11) | Safety net before large product/UI refactors. |
| 5 | **Todo** | **Better loot & shop configuration** — First-class policy editing; shop stock/pricing/categories/currencies; previews/diffs. | [#3](https://github.com/Stelliro/StelliferumAuditor/issues/3) | Core product value once config and export paths are trustworthy. |
| 6 | **Todo** | **Mod containers & mod-function loot by tier** — Per-mod container/storage parameters that vary by tier; audit/export integration. | [#1](https://github.com/Stelliro/StelliferumAuditor/issues/1) | Builds on solid loot policy (#3) instead of another parallel override system. |
| 7 | **Todo** | **Expansion Market export parity** — Real Expansion JSON export; no silent TraderPlus fallback. | [#10](https://github.com/Stelliro/StelliferumAuditor/issues/10) | Natural extension of shop config work (#3). |
| 8 | **Todo** | **UI overhaul** — Navigation, filterable tables, pipeline/FTP status, safer guards, layout polish. | [#2](https://github.com/Stelliro/StelliferumAuditor/issues/2) | Redesign after data/transfer behavior is stable so UI does not encode WinSCP quirks. |
| 9 | **Done** (CLI mode) | **Standalone FTP tool** — Integrated CLI on the same binary (`--ftp-list` / `--ftp-download` / `--ftp-upload`); dedicated lightweight UI still open if desired. | [#4](https://github.com/Stelliro/StelliferumAuditor/issues/4) | Reuses native transfer API from #5; ops can sync without the full GUI pipeline. |
| 10 | **Todo** | **Zero-dependency AI helpers** — Bundle, reimplement, or demote Python/Ollama cortex & Phoenix so core stays double-click. | [#6](https://github.com/Stelliro/StelliferumAuditor/issues/6) | Optional power features; must not block daily economy workflows. |
| 11 | **In progress** | **Cross-platform builds** — Linux/macOS transfer path without WinSCP; full desktop packaging / published builds still open. | [#7](https://github.com/Stelliro/StelliferumAuditor/issues/7) | Native transfers + CMake Unix generators land structure; broader OS polish follows. |

### Progress note (native-ftp ship)

Roadmap **#9**, **#5**, **#4** (as CLI mode), and structural **#7** (Linux without WinSCP for core transfer) landed in the native FTP/SFTP ship. Execution record: [docs/process-native-ftp-ship.md](docs/process-native-ftp-ship.md). Brief: [docs/NATIVE_FTP_SHIP_BRIEF.md](docs/NATIVE_FTP_SHIP_BRIEF.md).

### Why this order

Foundation first: **secure transfers → native transfers → honest config → tests/threads**. Product next: **loot/shops → containers → Expansion**. Experience: **UI polish** (standalone FTP CLI already shipped). Optional/expansion last: **AI packaging → full non-Windows product builds**.

### Background (review + code audit)

External review flagged WinSCP fragility, Python/env friction, Windows lock-in, and weak config validation. Backend inspection confirmed those and added: PE subsystem patching for WinSCP, credentials on argv + trust-any host keys (legacy WinSCP path), stub loaders (`known_items`, balancer JSON import), incomplete Expansion export, hard `MAX_ITEMS`/batch buffers, lightly synchronized background threads, no CI tests, and a ~2.6k-line `ui.cpp`. The **native path** addresses transfer security and the WinSCP hard-dependency for core sync; remaining items stay on the roadmap above.

---

## License

Internal tool for **Stelliferrum Forge**. All rights reserved unless otherwise noted by the project owner.

---

**Stelliferrum Forge Dev Team**
