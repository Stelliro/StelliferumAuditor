# Implementation Process (Multi-Track)

**Ship:** multi-track-ship / roadmap-loot  
**Plan path:** `docs/process-roadmap-loot-ship.md`  
**Brief:** `docs/ROADMAP_SHIP_BRIEF.md`  
**Pipeline mode:** `both` (after_each + batch)  
**QA dir:** `qa/roadmap-loot`  
**Created:** 2026-08-07  
**Auto-approve plan:** yes  

Agents execute tracks in the order listed under **Track order** unless two tracks have fully non-overlapping owner files and no shared acceptance dependency. Every track starts as **Status: pending**. Update status in this file as work progresses (`pending` → `in_progress` → `done` / `blocked`). Do not rewrite foundation surfaces; extend them. Keep public CLI (`sfa` verbs: help/list/pull/pipeline/push/restore/run) working throughout.

---

## Goal

Ship remaining roadmap priorities that are feasible now:

1. **Loot tier rename + better distribution** per `docs/ROADMAP_SHIP_BRIEF.md` and `config/loot_policy.ini` (progressive fantasy Coast Scav…Mythic/Black Market/Contraband; policy-driven nominal/min/lifetime/restock).
2. **Config validation** for `loot_policy` / `server_paths` load errors (soft, non-fatal).
3. **Mod-container policy first cut** (config schema + load hooks; full cargo apply later).
4. **Mark Linux/Alpine headless complete** in README (native libcurl path; no WinSCP required).
5. Run **sfa pipeline** → **native FTP push** → **pull key files** and verify sizes under `qa/roadmap-loot`.

**Ingest summary:** Finish loot tier rename + distribution wiring (INI already has Coast Scav…Mythic and `NOMINAL_*` hints; C load/apply and store balance still incomplete or partially wired); soft config validation for loot_policy/server_paths; mod-container policy first cut (schema+load stub); mark Linux/Alpine headless complete in README; then sfa pipeline → native FTP push → pull/verify sizes. Skip full UI overhaul, Expansion Market full export, and AI packaging. Keep public CLI verbs intact.

---

## Foundation (done — do not redo)

Already shipped and must not be rewritten:

1. **Native libcurl FTP/SFTP stack** with host-key security (roadmap **#5 / #9**). Execution record: `docs/process-native-ftp-ship.md`. Brief: `docs/NATIVE_FTP_SHIP_BRIEF.md`.
2. **Standalone transfer CLI and sfa verbs:** `help` / `list` / `pull` / `pipeline` / `push` / `restore` / `run` (and equivalent binary `--ftp-*` / `--regen` / `--headless` surfaces). Do not rename, remove, or re-spec the public verb surface.
3. **`config/loot_policy.ini` is runtime source of truth** for tier policy. `config/tier_rules.json` is historical only; do not wire it as SoT this ship.
4. **12-tier flag model:** `NOSPAWN` / `NOTRADE` / `BLACKMARKET` / `CONTRABAND` (+ blacklist → contraband tier).
5. **Soft `MAX_ITEMS` parser warning** already exists (do not re-architect capacity).
6. **Partial in-tree progress (verify, complete gaps, do not redesign):**
   - `config/loot_policy.ini` — `TIER_NAMES` already renamed to progressive fantasy; `NOMINAL_TARGETS` / `MIN_TARGETS` / `LIFETIME_TARGETS` / `RESTOCK_TARGETS` hints drafted.
   - `include/loot_policy.h` — `LpTier` distribution fields + `lp_apply_distribution` declared; `lp_black_market_tier()` present.
   - `src/loot_policy.c` — C defaults updated to Coast Scav…Contraband; load path may already parse distribution lists and implement soft-blend apply; **verify end-to-end** and close remaining holes (save round-trip, callers, store_generator hard tables, hardcoded BM tier index).
   - `src/auditor.c` may already call `lp_apply_distribution` in one path; **still** has hardcoded `assigned_tier != 9` BM whitelist logic that breaks under Elite/Mythic order.
   - `src/store_generator.c` **still uses hardcoded** `nominal_targets[]` / `lifetime_targets[]` (and related) in `balance_economy_values` — must prefer policy.

Do **not** reopen: native-ftp process, WinSCP hard-dependency, or rewrite of `ftp_*` public ABI.

---

## Excluded

Do **not** implement in this ship:

- Full UI overhaul (roadmap **#2**)
- Expansion Market full export (roadmap **#10**)
- Zero-dep AI packaging (roadmap **#6**)
- Full mod-container cargo apply/export (beyond schema + load stub)
- Full config schema for dead `tier_rules.json` / `known_items` loaders (only soft validation for loot_policy + server_paths)
- Full desktop packaging / published Linux GUI builds
- Re-open native FTP/SFTP foundation (**#5 / #9 / #4** CLI)
- Rewrite of sfa public verb surface
- Treating `tier_rules.json` or `examples/items` catalog as runtime SoT
- Non-destructive requirement to overhaul UI Loot Policy tab (`ui.cpp`) — prefer non-destructive save for new keys; document if UI desync remains

---

## Shared QA pipeline

### Settings

| Key | Value |
|-----|--------|
| `qa_dir` | `qa/roadmap-loot` |
| `pipeline_mode` | `both` |
| Modes | `after_each` (per-track checks) + `batch` (full ship gate) |

Create `qa/roadmap-loot/` at first QA run. Store numbered logs, pipeline captures, push/pull size evidence, and final checklist under that tree (mirror `qa/native-ftp/` style). Prefer relative paths from repo root. Keep public CLI verbs working throughout.

### Artifact paths (canonical)

| # | Artifact | Path |
|---|----------|------|
| 01 | Release build log | `qa/roadmap-loot/01-release-build.log` |
| 02 | Tier / policy dump or grep evidence | `qa/roadmap-loot/02-tier-policy-evidence.txt` |
| 03 | sfa pipeline (or --regen) log | `qa/roadmap-loot/03-sfa-pipeline.log` |
| 04 | Distribution / items.csv / types sample | `qa/roadmap-loot/04-distribution-sanity.txt` |
| 05 | Config validation soft-error smoke | `qa/roadmap-loot/05-config-validation-smoke.txt` |
| 06 | Container policy load evidence | `qa/roadmap-loot/06-container-policy.txt` |
| 07 | README / roadmap status scan | `qa/roadmap-loot/07-readme-scan.txt` |
| 08 | sfa push / FTP push log | `qa/roadmap-loot/08-sfa-push.log` |
| 09 | Pull-back + size/checksum verify | `qa/roadmap-loot/09-pull-verify.txt` |
| 10 | Public sfa verbs still work | `qa/roadmap-loot/10-sfa-verbs-smoke.txt` |
| — | Batch final checklist | `qa/roadmap-loot/batch-acceptance.md` |
| — | Track notes / blockers | `qa/roadmap-loot/track-notes.md` |

Optional (cheap if easy): light smoke for `MAX_ITEMS` / missing config path under same QA dir.

### P1–P8 shared steps

Execute these as applicable in **after_each** (subset) and always as **batch** gate before Done criteria.

1. **P1 — Release build**  
   Configure/build Release if sources changed. Capture → `01-release-build.log`. Exit 0 required before pipeline claims.

2. **P2 — Policy / tier evidence**  
   Confirm `TIER_NAMES` and C defaults are progressive fantasy; BM=11 NOSPAWN, Contraband=12 NOTRADE; no remaining hard `tier == 9` BM assumptions where policy should apply. Capture greps/snippets → `02-tier-policy-evidence.txt`.

3. **P3 — sfa pipeline**  
   From project root with `config/` present: `sfa pipeline` (or binary `--regen`). Capture full log → `03-sfa-pipeline.log`. Inspect `output/types.xml`, `output/items.csv`, reports for new tier labels and distribution sanity → `04-distribution-sanity.txt`.

4. **P4 — Config validation smoke**  
   Soft-fail paths: missing/unreadable `loot_policy.ini` logs error and keeps defaults; missing/empty critical `server_paths` keys log soft errors without fatal abort. Capture → `05-config-validation-smoke.txt`.

5. **P5 — Container policy stub**  
   Schema file present; load wired with soft fail; pipeline still runs. Capture → `06-container-policy.txt`.

6. **P6 — README consistency**  
   Economy tiers table matches 12-tier policy; roadmap #7 notes Alpine/Linux headless CLI + transfer complete; do not claim UI/Expansion/AI done. Capture scan → `07-readme-scan.txt`.

7. **P7 — Native FTP push + pull verify**  
   `sfa push` or `--ftp-push-economy` via native FTP; pull `types.xml` (or key file); compare local vs remote/pulled size (and checksum if available). Capture → `08-sfa-push.log`, `09-pull-verify.txt`. **Needs live `config/ftp.ini` credentials and network** — dry-run cannot fully accept size match; if network blocked, mark track blocked with evidence, not fake PASS.

8. **P8 — Public CLI + batch gate**  
   `sfa help` / `list` / `pull` / `pipeline` / `push` / `restore` / `run` surface still documented and invokable. Fill `batch-acceptance.md` with pass/fail per acceptance bullets. Capture verb smoke → `10-sfa-verbs-smoke.txt`.

### When to run

| Mode | When | What |
|------|------|------|
| `after_each` | End of each track | Track-specific **Pipeline checks** + any P-steps listed there; append notes to `track-notes.md` |
| `batch` | After all tracks (or when ship claims done) | Full P1–P8; write `batch-acceptance.md` |

### Hotspot sequencing

- **`src/loot_policy.c` / `include/loot_policy.h` / `config/loot_policy.ini`** — shared by **loot-tier-rename**, **loot-distribution**, **config-validation**. Run **rename → distribution → validation** (or single owner for all three). Do not parallel-edit.
- **`src/store_generator.c` + `src/auditor.c`** — hardcoded tier indices/tables; fix under rename + distribution before pipeline-push-verify.
- **`README.md`** — contested by rename table updates and linux-headless status. Serialize: prefer **readme-linux-headless** after rename table content is known, or single owner for all README edits in one pass.
- **`CMakeLists.txt`** — only for **mod-container-policy** adding `container_policy.c`; avoid unrelated cmake churn.
- **`pipeline-push-verify`** last; depends on distribution + rename acceptance in pipeline outputs.
- **UI (`ui.cpp`)** — out of scope for overhaul; if save path drops new distribution keys, prefer non-destructive save extension in loot_policy_save or document residual desync in track notes (do not full UI rewrite).

---

## Tracks

Execute in the order below unless two tracks are proven non-overlapping (owner files + acceptance independent). **Default is sequential** for this ship because of shared `loot_policy` and README hotspots.

### Track loot-tier-rename: Loot tier rename + policy name consistency

- **Status:** done
- **Result:** Progressive fantasy names confirmed (INI + C defaults); BM=11 NOSPAWN/BLACKMARKET, Contraband=12 NOTRADE; `auditor_enforce_black_market_spawns` uses `lp_black_market_tier()`; store BM category uses `lp_tier_black_market()` (Elite/Mythic no longer mis-tagged); loot_manager BM fallbacks → 11; SFL zero-nominal skip policy-aware; writer audit labels via `lp_tier_name` for all policy tiers; README Economy table = 12-tier; sfa verbs untouched. Release build OK. Evidence: `qa/roadmap-loot/01-release-build.log`, `02-tier-policy-evidence.txt`.
- **Owner files:**
  - `config/loot_policy.ini`
  - `src/loot_policy.c`
  - `include/loot_policy.h`
  - `src/auditor.c`
  - `src/loot_manager.c`
  - `src/store_generator.c`
  - `README.md` (Economy tiers table only if not deferred to readme-linux-headless — coordinate)
- **Actions:**
  1. Confirm `TIER_NAMES` and C `loot_policy_init_defaults` names match progressive fantasy:  
     Coast Scav, Town Survivor, Constable, Outdoorsman, Insurgent, Infantry, Spec Ops, Operator, Elite, Mythic, Black Market, Contraband.
  2. Confirm flags: BM = tier **11** NOSPAWN + BLACKMARKET; Contraband = tier **12** NOTRADE (+ NOSPAWN as policy requires); `CONTRABAND_TIER=12`.
  3. Replace hardcoded BM spawn logic that assumes `assigned_tier == 9` (or `!= 9`) with `lp_black_market_tier()` (notably `auditor_enforce_black_market_spawns` in `src/auditor.c`).
  4. Grep store/loot paths for stale labels and hard tier indices that break Elite/Mythic order; fix critical paths only (no drive-by refactors).
  5. Prefer `lp_tier_name` for pipeline labels where applicable.
  6. Update README Economy tiers table to 12-tier policy (or leave a precise handoff note for **readme-linux-headless** if serializing README to that track — only one track must finish the table).
  7. Do **not** change sfa public verbs.
- **Acceptance:**
  - `TIER_NAMES` and C defaults are progressive fantasy (Coast Scav…Elite, Mythic, Black Market, Contraband).
  - BM=11 NOSPAWN, Contraband=12 NOTRADE; BM spawn logic uses `lp_black_market_tier()`.
  - README Economy tiers table matches 12-tier policy (before or as part of README track, but ship-final must match).
  - Pipeline labels use `lp_tier_name` where applicable.
  - sfa verbs unchanged.
- **Pipeline checks (after_each):**
  - P1 if C sources changed.
  - P2 greps for hard `assigned_tier == 9` / `!= 9` BM logic and default name list.
  - Artifacts: partial `02-tier-policy-evidence.txt`.

---

### Track loot-distribution: Tier distribution via policy targets + store balance

- **Status:** done
- **Result:** NOMINAL/MIN/LIFETIME/RESTOCK load from `loot_policy.ini`; `lp_apply_distribution` soft-blend + NOSPAWN zero; `balance_economy_values` prefers policy targets (hard tables fallback only); currency/heirloom special-cases kept; BM/Contraband nominal 0 via apply/`lp_tier_spawns`; `loot_policy_save` writes distribution lists; `set_tiers_csv` preserves targets. Release build OK. Evidence: `qa/roadmap-loot/01-release-build.log`, `04-distribution-sanity.txt`.
- **QA (2026-08-07 light):** **fail** (pre-fix) — store hard tables preferred; save omitted targets. Superseded by track Result + re-run.
- **QA (2026-08-07 light re-run):** **pass** — store prefers `has_distribution_targets` + `lp_apply_distribution`; save writes NOMINAL/MIN/LIFETIME/RESTOCK; hard tables fallback only. Evidence: `qa/roadmap-loot/04-distribution-sanity.txt`.
- **Owner files:**
  - `config/loot_policy.ini`
  - `include/loot_policy.h`
  - `src/loot_policy.c`
  - `src/store_generator.c`
  - `src/auditor.c`
- **Actions:**
  1. Ensure `NOMINAL_TARGETS` / `MIN_TARGETS` / `LIFETIME_TARGETS` / `RESTOCK_TARGETS` load from INI into `LpTier` fields and set `has_distribution_targets`.
  2. Ensure `lp_apply_distribution` is fully implemented (soft blend toward targets; NOSPAWN forces nominal/min 0).
  3. Wire apply from `balance_economy_values` (or equivalent primary balance path) so store_generator **stops preferring** static hardcoded balance tables when policy targets exist; keep currency special-case if needed but BM/Contraband nominal 0 via `lp_tier_spawns` / apply.
  4. Confirm auditor path that already calls `lp_apply_distribution` remains coherent with store path (no double-apply that zeros then re-inflates incorrectly).
  5. **Save round-trip:** `loot_policy_save` must preserve distribution target lists when present (today save may omit them — fix so UI/CLI save does not drop hints). Prefer non-destructive: write lists when `has_distribution_targets` or any target ≥ 0.
  6. Sanity: lower tiers denser/shorter life; Operator/Elite/Mythic sparse/long life; BM/Contraband nominal 0.
  7. When applying `TIER_NAMES`, do not drop distribution targets (preserve/reload lists after name apply — partial preserve logic may already exist; verify).
- **Acceptance:**
  - Distribution target keys load from `loot_policy.ini`.
  - `lp_apply_distribution` implemented and used from balance path(s).
  - Density/lifetime curve matches goals; BM/Contraband nominal 0.
  - Save round-trip preserves hints when present.
- **Pipeline checks (after_each):**
  - P1 if sources changed.
  - P3 light or full if build ready: sample `items.csv` / types for tier density trend → start `04-distribution-sanity.txt`.
  - Note any residual hardcoded tables still used for non-policy paths in `track-notes.md`.

---

### Track config-validation: Config validation for loot_policy and server_paths

- **Status:** done
- **Result:** Soft non-fatal validation: `loot_policy_load` logs `SEVERITY_ERROR` on missing/unreadable path, OOM, empty critical keys (TIER_NAMES/flag lists/scalars), and invalid flag tokens; keeps defaults/partial apply. `util_soft_validate_server_paths` logs missing file / REMOTE_ROOT / LOCAL_ROOT; wired from `util_init_context`, `main` `load_server_paths`, CLI `paths_load`. No abort on bad config. Release build OK. Evidence: `qa/roadmap-loot/05-config-validation-smoke.txt`.
- **QA (2026-08-07 light):** **fail** — missing `loot_policy.ini` keeps defaults but does not log `SEVERITY_ERROR`; `load_server_paths` does not soft-error missing REMOTE_ROOT/LOCAL_ROOT. Evidence: `qa/roadmap-loot/05-config-validation-smoke.txt`.
- **QA (2026-08-07 light re-run):** **fail** — same gaps; `loot_policy_load` still `return false` with no log; `load_server_paths` INFO-only when root present.
- **QA (2026-08-07 implement):** **pass** — missing loot_policy `--regen` exit 0 + ERROR log; empty TIER_NAMES/bad NOSPAWN/empty CONTRABAND log errors; missing server_paths `--regen` ERROR + exit 0; sfa help exit 0.
- **Owner files:**
  - `src/loot_policy.c`
  - `src/util.c`
  - `src/main.c`
  - `src/cli_commands.c`
  - `include/loot_policy.h` (only if additive helpers needed)
- **Actions:**
  1. Missing/unreadable `loot_policy.ini`: log `SEVERITY_ERROR` (or equivalent clear error) and keep defaults **without crash**.
  2. Empty/malformed critical keys (e.g. empty `TIER_NAMES` when key present, nonsense flag lists): surface clear log errors; keep safe defaults / partial apply without abort.
  3. `server_paths.ini` missing file or missing `REMOTE_ROOT` / `LOCAL_ROOT` (and other critical keys on CLI/GUI/headless load paths): log soft errors; no fatal abort on bad config alone.
  4. Prefer centralizing soft checks near existing `loot_policy_load` / `load_server_paths` / `util_init_context` / CLI path loaders — do not invent a second config framework.
  5. Do **not** implement full JSON schema for dead `tier_rules.json` / `known_items` loaders this ship.
- **Acceptance:**
  - Missing/unreadable loot_policy logs error and keeps defaults without crash.
  - Empty/malformed critical keys surface clear log errors.
  - server_paths missing REMOTE_ROOT/LOCAL_ROOT (or file) logs soft errors on load paths used by CLI/GUI/headless.
  - No fatal abort on bad config.
- **Pipeline checks (after_each):**
  - P4: optional rename-aside smoke (temp missing file or documented dry test) → `05-config-validation-smoke.txt`.
  - Confirm sfa still starts/help works (P8 light).

---

### Track mod-container-policy: Mod-container policy first cut (schema + load hooks)

- **Status:** done
- **Result:** Documented `config/container_policy.ini` (tier CARGO/ATTACHMENT/ITEMS lists + DEFAULT_* + ENABLE_TIER_SCALING + optional MOD_OVERRIDE.*); `include/container_policy.h` + `src/container_policy.c` load stub populates `g_container_policy`; soft-fail WARN on missing file keeps defaults; wired from `util_init_context`; CMake SOURCES/HEADERS; README #1 Partial (schema+load only). Full apply/export TODO. Release build OK; `--regen` exit 0 loads policy. Evidence: `qa/roadmap-loot/06-container-policy.txt`, `01-release-build.log`.
- **QA (2026-08-07 light):** **fail** — `config/container_policy.ini`, `include/container_policy.h`, `src/container_policy.c` absent; not wired in util/CMake. Evidence: `qa/roadmap-loot/06-container-policy.txt`.
- **QA (2026-08-07 light re-run):** **fail** — still absent.
- **QA (2026-08-07 light post config-validation):** **fail** — still absent (0 CMake/util hits). Evidence: `qa/roadmap-loot/06-container-policy.txt`.
- **QA (2026-08-07 implement):** **pass** — schema+load+wire; missing file WARN + exit 0; loaded INFO on regen; apply still TODO.
- **Owner files:**
  - `config/container_policy.ini` (new schema file)
  - `include/container_policy.h` (new)
  - `src/container_policy.c` (new)
  - `src/util.c` (wire load from `util_init_context` or pipeline init)
  - `CMakeLists.txt` (add source only)
  - `README.md` (short note on schema — coordinate with readme track)
- **Actions:**
  1. Documented INI schema for mod container defaults by tier and/or mod (comment header + example keys; e.g. sections or flat keys for default cargo/attachment caps by tier).
  2. Implement load stub that populates a global/struct; soft-fail if file missing (log warning/error, continue).
  3. Wire from `util_init_context` or pipeline init so load runs in normal CLI/pipeline flow.
  4. Full cargo apply/export may remain TODO (comment clearly).
  5. Add `container_policy.c` to CMake `SOURCES` only — no unrelated cmake churn.
  6. `sfa pipeline` still runs after wiring.
- **Acceptance:**
  - Documented schema file exists.
  - Load stub populates global/struct; soft fail on missing file.
  - Wired from init path; full apply may be TODO.
  - sfa pipeline still runs.
- **Pipeline checks (after_each):**
  - P1, P5 → `06-container-policy.txt`.
  - Pipeline smoke if cheap (or defer full P3 to later tracks).

---

### Track readme-linux-headless: Mark Linux/Alpine headless CLI complete in README

- **Status:** done
- **Result:** README #7 Partial: Alpine/Linux **headless CLI + transfer path complete** (native libcurl, no WinSCP); full desktop packaging / published Linux builds still open. Requirements + progress notes aligned. Loot/config bullets Partial for #3 (tier rename+distribution), #8 (soft loot_policy/server_paths validation only), #1 (container schema+load stub). 12-tier Economy table present. No UI (#2) / Expansion (#10) / AI (#6) over-claim. Evidence: `qa/roadmap-loot/07-readme-scan.txt`.
- **Owner files:**
  - `README.md`
- **Actions:**
  1. Roadmap **#7** (cross-platform): note Alpine/Linux **headless CLI + transfer path complete** (native libcurl, no WinSCP); full desktop packaging / published Linux builds remain open if true.
  2. Features / Quick start consistent with native transfer and sfa usage.
  3. Economy tiers table = 12-tier progressive fantasy if not already finalized by **loot-tier-rename**.
  4. Update loot/config roadmap bullets for this ship:
     - Partial progress on **#3** (tier rename + distribution) / **#8** (soft validation for loot_policy/server_paths only) / **#1** (schema+load stub) as actually delivered — do not claim full UI, Expansion Market, or AI packaging.
  5. Do not claim UI overhaul, Expansion Market export, or AI packaging complete.
- **Acceptance:**
  - #7 notes Alpine/Linux headless CLI + transfer complete; packaging remains open if true.
  - Features/Quick start consistent.
  - Loot/config roadmap bullets updated for this ship without over-claiming.
- **Pipeline checks (after_each):**
  - P6 → `07-readme-scan.txt`.

---

### Track pipeline-push-verify: Pipeline, native FTP push, pull-back size verify

- **Status:** done
- **Result:** `--regen` exit 0 (~16.5s) with progressive tier labels (Coast Scav…Black Market/Contraband) in audit; native FTP `push` exit 0 (9/9 libcurl); pull types.xml size 2331636 + SHA256 match. Public verbs help/list/--help exit 0. No code changes. Evidence: `qa/roadmap-loot/03-sfa-pipeline.log`, `08-sfa-push.log`, `09-pull-verify.txt`, `10-sfa-verbs-smoke.txt`, `batch-acceptance.md`.
- **Owner files:**
  - `config/commands.ini` (only if recipe tweaks needed; prefer no churn)
  - `src/cli_commands.c` (only if push/pipeline bugs block acceptance; prefer no verb surface change)
  - `qa/roadmap-loot/` (artifacts)
- **Actions:**
  1. Ensure Release binary available (P1).
  2. From project root: `sfa pipeline` (or `--regen`); capture `03-sfa-pipeline.log`.
  3. Inspect `output/types.xml`, `output/items.csv`, reports for new tier labels and distribution sanity → `04-distribution-sanity.txt`.
  4. `sfa push` or `--ftp-push-economy` via **native FTP** → `08-sfa-push.log`.
  5. `sfa run types` or pull types (key file); compare local vs remote/pulled **size** (checksum if available) → `09-pull-verify.txt`.
  6. Optional light smoke for MAX_ITEMS / config path if cheap.
  7. Write `qa/roadmap-loot/batch-acceptance.md` checklist.
  8. Confirm public sfa verbs still work → `10-sfa-verbs-smoke.txt`.
  9. Do not change public verb names or help contract unless fixing a true regression.
- **Acceptance:**
  - Pipeline succeeds with new tier labels in outputs.
  - Push succeeds via native FTP.
  - Pull types (or key file) size/checksum matches upload.
  - Artifacts under `qa/roadmap-loot`.
  - Public sfa verbs still work.
- **Pipeline checks (after_each):**
  - Full P1–P8 as batch gate when this track completes the ship.
  - Live credentials required for P7 size match; document BLOCKED with logs if network/auth unavailable (do not invent PASS).

---

## Diagnose/Fix policy

1. **Reproduce with evidence** under `qa/roadmap-loot/` (log + command + exit code) before large code changes.
2. **Prefer policy-driven fixes** (`lp_*` queries, INI targets) over new hardcoded tier tables.
3. **No foundation rewrites:** native FTP, sfa verb surface, WinSCP optional fallback, MAX_ITEMS warning design stay.
4. **Hotspot ownership:** serialize `loot_policy.*` / `loot_policy.ini`; serialize README; CMake only for container source add.
5. **Soft fail over hard abort** for config: log clearly, keep defaults, continue when safe.
6. **If live FTP cannot run:** mark **pipeline-push-verify** / P7 blocked with evidence; complete offline tracks and document residual risk — do not fake size match.
7. **UI desync:** if Loot Policy tab save drops distribution keys, fix `loot_policy_save` first; only minimal UI glue if required for non-destructive round-trip — no UI overhaul.
8. **Stale JSON/examples:** do not treat as SoT; optional note in README already exists for tier_rules/known_items.
9. **Regressions:** if sfa help/list/pull/pipeline/push/restore/run break, fix immediately before continuing other tracks.
10. Update this file’s track **Status** and **Verification log** as work completes.

---

## Done criteria

Ship is done when **all** of the following hold:

- [x] All tracks **Status: done** (or explicitly **blocked** only for live-network P7 with documented evidence and owner decision)
- [x] Tier names progressive fantasy; BM/Contraband flags correct; no hard tier==9 BM whitelist
- [x] Distribution targets load + apply; store balance prefers policy; BM/Contraband nominal 0
- [x] Soft config validation for loot_policy + server_paths without crash/abort
- [x] Container policy schema + load stub wired; pipeline still runs
- [x] README: 12-tier table + #7 Alpine/Linux headless complete; no over-claims on UI/Expansion/AI
- [x] `sfa pipeline` succeeds; native FTP push succeeds; pull size/checksum verified under `qa/roadmap-loot`
- [x] Public sfa verbs intact (`10-sfa-verbs-smoke.txt`)
- [x] `qa/roadmap-loot/batch-acceptance.md` filled with pass/fail evidence
- [x] This process file statuses and verification log updated

---

## Verification log

| When | What | Result | Artifacts / notes |
|------|------|--------|-------------------|
| 2026-08-07 | Plan written (auto-approved) | pending execution | `docs/process-roadmap-loot-ship.md`; brief `docs/ROADMAP_SHIP_BRIEF.md` |
| | | | Pre-ship code notes: INI + defaults already renamed; distribution load/apply partially present in `loot_policy.c`; `auditor.c` still hardcodes `assigned_tier != 9`; `store_generator.c` still has hardcoded balance tables; `loot_policy_save` may omit distribution keys; README still shows old 11-tier table |
| 2026-08-07 | Track loot-tier-rename | done | BM spawn/category logic policy-driven; README 12-tier; no hard `assigned_tier==9` BM; build OK. `qa/roadmap-loot/01-release-build.log`, `02-tier-policy-evidence.txt`, `track-notes.md` |
| 2026-08-07 | Shared QA light P1–P8 | **fail closed** | Build exit 0; pipeline exit 0; push exit 0 native FTP 9/9; pull types size+SHA256 match; sfa help exit 0. Fail: loot-distribution (store hard tables + save omit targets), config-validation (no SEVERITY_ERROR soft paths), mod-container-policy (missing files). Pass tracks: loot-tier-rename, readme-linux-headless, pipeline-push-verify. Artifacts under `qa/roadmap-loot/` incl. `batch-acceptance.md`. |
| 2026-08-07 | Track loot-distribution | done | store balance prefers `lp_apply_distribution`; save writes NOMINAL/MIN/LIFETIME/RESTOCK; set_tiers preserves targets; build OK. `qa/roadmap-loot/01-release-build.log`, `04-distribution-sanity.txt`, `track-notes.md` |
| 2026-08-07 | Shared QA light re-run P1–P8 | **fail closed** | Build exit 0; ctest exit 0 (no tests); `--regen` exit 0 (39/39); push exit 0 native FTP 9/9; pull types size 2331636 + SHA256 match. Pass: loot-tier-rename, loot-distribution (re-verified store policy + save), readme-linux-headless, pipeline-push-verify. Fail: config-validation (no SEVERITY_ERROR soft paths), mod-container-policy (files absent). Artifacts under `qa/roadmap-loot/` incl. `batch-acceptance.md`. |
| 2026-08-07 | Track config-validation | done | Soft SEVERITY_ERROR for missing/malformed loot_policy + server_paths roots; defaults kept; no fatal abort; Release build OK. `qa/roadmap-loot/05-config-validation-smoke.txt`, `track-notes.md`. |
| 2026-08-07 | Shared QA light P1–P8 (post config-validation) | **fail closed** | Build exit 0; ctest exit 0 (no tests); `--regen` exit 0 (39 ok/0 failed ~17s); push exit 0 native FTP 9/9; pull types size 2331636 + SHA256 match; config smoke ACCEPT_RUNTIME=True (missing loot_policy ERROR, missing server_paths ERROR, malformed keys). Pass: loot-tier-rename, loot-distribution, config-validation, readme-linux-headless, pipeline-push-verify. Fail: mod-container-policy (schema/load absent). Artifacts under `qa/roadmap-loot/` incl. `batch-acceptance.md`. |
| 2026-08-07 | Track mod-container-policy | done | Schema INI + load stub + util_init_context wire + CMake; soft-fail missing file WARN; `--regen` loads policy exit 0; apply/export TODO. Evidence: `qa/roadmap-loot/06-container-policy.txt`, `01-release-build.log`. |
| 2026-08-07 | Shared QA light P1–P8 (post mod-container-policy / full ship) | **pass** | Build exit 0; ctest exit 0 (no tests); exclusive `--regen` exit 0 (39 ok/0 failed; container_policy loaded; audit Coast Scav…Contraband); config smoke ACCEPT_RUNTIME=True (A missing loot_policy ERROR, B malformed keys, C missing server_paths ERROR, D missing container WARN); push exit 0 native FTP 9/9; pull types size 2331636 + SHA256 match; README #7 + 12-tier OK. All tracks ok. Artifacts under `qa/roadmap-loot/` incl. `batch-acceptance.md`, `track-notes.md`. |
| 2026-08-07 | Track readme-linux-headless (wording finalize) | done | README #7 = headless CLI + transfer complete (native libcurl, no WinSCP); packaging open; #3/#8/#1 Partial ship notes; no UI/Expansion/AI claim. P6 rescan → `07-readme-scan.txt`. |
| 2026-08-07 | Shared QA light P1–P8 (re-run / ship gate) | **pass** | Build exit 0; ctest exit 0 (no tests); `--regen` exit 0 (39 ok/0 failed ~17s; container_policy loaded; audit Coast Scav…Contraband); config smoke ACCEPT_RUNTIME=True (A/B/C/D); push exit 0 native FTP 9/9; pull types size 2331636 + SHA256 match; help/list/--help exit 0; README #7 + 12-tier OK. All six tracks ok. Artifacts under `qa/roadmap-loot/` incl. `batch-acceptance.md`, `track-notes.md`. |
| 2026-08-07 | Track pipeline-push-verify (fresh re-run) | done / **pass** | `--regen` exit 0 ~16.5s progressive labels; `push` exit 0 native FTP 9/9; pull types size 2331636 + SHA256 FF0F5368… match; help/list/--help exit 0. Artifacts refreshed: `03-sfa-pipeline.log`, `04-distribution-sanity.txt`, `08-sfa-push.log`, `09-pull-verify.txt`, `10-sfa-verbs-smoke.txt`, `batch-acceptance.md`. No source changes. |
| 2026-08-07 | Shared QA light P1–P8 (pipeline-push-verify after_each / ship gate re-run) | **pass** | Build exit 0; ctest exit 0 (no tests); exclusive `--regen` exit 0 (~16.6s, 39 ok/0 failed; container_policy loaded; Coast Scav…Contraband; BM 706 + Contraband 5 zero-nominal); config smoke ACCEPT_RUNTIME=True (A missing loot_policy ERROR, B malformed keys, C missing server_paths ERROR, D missing container WARN); push exit 0 native FTP 9/9; pull types size 2331636 + SHA256 FF0F5368… match; help/list/--help exit 0; README #7 + 12-tier OK. All six tracks ok. Artifacts under `qa/roadmap-loot/` incl. `batch-acceptance.md`, `track-notes.md`. |
| 2026-08-07 | Shared QA **FULL batch** P1–P8 (multi-track-ship gate) | **pass** | Build exit 0; ctest exit 0 (no tests); `--regen` exit 0 (~16.8s, 39 ok/0 failed; Coast Scav…Contraband); HARD_TIER9_BM_HITS=0 LP_BM=6; store prefers policy; T11 706/706 + T12 5/5 zero-nominal; config smoke ACCEPT_RUNTIME=True (A ERROR exit 0, B empty TIER_NAMES/CONTRABAND ERROR exit 0, C missing server_paths ERROR help+regen exit 0, D missing container WARN exit 0); push exit 0 native FTP 9/9; pull types size 2331636 + SHA256 FF0F5368… match; help/list/--help/sfa.cmd help exit 0; README #7 + 12-tier OK. All six tracks ok. Artifacts: `qa/roadmap-loot/` 01–10, `batch-acceptance.md`, `track-notes.md`, `pipeline_report.md`, `pipeline_report.json`, `pulled/types.xml`. |

### Batch acceptance checklist (copy into `qa/roadmap-loot/batch-acceptance.md` at batch)

- [x] Release build succeeds after code tracks
- [x] TIER_NAMES / C defaults: Coast Scav…Mythic, Black Market, Contraband
- [x] BM=11 NOSPAWN, Contraband=12 NOTRADE; `lp_black_market_tier()` used for BM spawn logic
- [x] NOMINAL/MIN/LIFETIME/RESTOCK targets load and apply; store path policy-driven
- [x] BM/Contraband nominal 0 in pipeline outputs (spot-check)
- [x] Missing/bad loot_policy and server_paths soft-error without crash
- [x] `config/container_policy.ini` schema + load stub wired
- [x] README #7 Alpine/Linux headless complete; 12-tier table; no UI/Expansion/AI over-claim
- [x] `sfa pipeline` OK with new tier labels
- [x] Native FTP `sfa push` OK
- [x] Pull types (or key file) size/checksum match
- [x] Public sfa verbs help/list/pull/pipeline/push/restore/run still work
- [x] Plan file maintained under `docs/process-roadmap-loot-ship.md`

---

## Track order (machine-readable)

```
loot-tier-rename
loot-distribution
config-validation
mod-container-policy
readme-linux-headless
pipeline-push-verify
```

**pipeline_mode:** `both`  
**qa_dir:** `qa/roadmap-loot`  
**plan_path:** `docs/process-roadmap-loot-ship.md`  
**goal:** Ship remaining roadmap items: loot tier rename + distribution, soft config validation, mod-container policy stub, README Linux/Alpine headless complete, then sfa pipeline → native FTP push → pull/verify under qa/roadmap-loot  
**notes:** Auto-approved. Sequential ownership on loot_policy and README. Foundation native-ftp + sfa verbs frozen. Partial code already has renamed INI/defaults and partial distribution load/apply; finish wiring, remove hard tier-9 BM, replace store hard tables, save round-trip, validation, container stub, README, live push/verify. Skip UI overhaul, Expansion Market export, AI packaging.
