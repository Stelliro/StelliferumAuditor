# Roadmap ship brief — remaining work + loot overhaul

## Goal
Advance open roadmap items that are shippable now, with emphasis on:
- **#3 Better loot & shop configuration** (tier naming + distribution)
- **#8 Config validation** (loot_policy / server_paths load errors surface clearly)
- **#1 Mod containers** (first cut: config schema + tier-aware container defaults in policy)
- **#7 Cross-platform** mark Alpine/Linux headless CLI complete
- **#11** light reliability: soft MAX_ITEMS warning already exists; add simple smoke script

Then: update loot tier names + distribution, run pipeline, **push** to server, **pull** back and verify.

## Out of scope this ship
- Full UI overhaul (#2)
- Expansion Market full export (#10) — unless trivial stub warning only
- Zero-dep AI packaging (#6)

## Loot tier rename (target)
Replace awkward order Mythic/Elite with progressive fantasy that matches zones:

| # | New name | Zone role | Spawn? | Trade |
|---|----------|-----------|--------|-------|
| 1 | Coast Scav | Civilian coast junk | yes | yes |
| 2 | Town Survivor | Inland towns | yes | yes |
| 3 | Constable | Police/civic | yes | yes |
| 4 | Outdoorsman | Hunting/rural | yes | yes |
| 5 | Insurgent | Soft military | yes | yes |
| 6 | Infantry | Bases | yes | yes |
| 7 | Spec Ops | High military | yes | yes |
| 8 | Operator | End-game field | yes | yes |
| 9 | Elite | Near-mythic | yes | yes |
| 10 | Mythic | Ultra rare | yes | yes |
| 11 | Black Market | No world spawn; BTC shop | no | black market |
| 12 | Contraband | Banned | no | no |

## Distribution goals
- Lower tiers: higher nominal/min, shorter lifetime
- Mid military: sparse nominal, longer lifetime
- Operator/Elite/Mythic: very low nominal, long lifetime
- Black Market / Contraband: nominal 0 (already NOSPAWN)
- Extend loot_policy.ini with optional per-tier nominal hints if code supports; otherwise document and adjust loot_manager heuristics / known multipliers

## Acceptance
- [ ] loot_policy.ini has clearer TIER_NAMES
- [ ] pipeline (--regen or sfa pipeline) produces items with new tier labels where applicable
- [ ] config validation: missing/broken loot_policy fails soft with log error (no crash)
- [ ] container policy file or section for mod containers (schema + load stub OK if full apply later)
- [ ] sfa push / --ftp-push-economy succeeds
- [ ] pull types.xml (or key file) and checksum/size match upload
- [ ] README roadmap statuses updated

## Commands (use sfa / quiet)
```
sfa pipeline
sfa push
sfa run types   # or pull types then compare
```
