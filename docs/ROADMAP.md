# Roadmap / backlog

Tracked product work beyond the 1.0.0 baseline. Prefer GitHub Issues for discussion; this file is the short index.

| Status | Item | Issue |
| --- | --- | --- |
| **Todo** | **Mod containers & mod-function loot params by tier** — Detect containers (and similar mod storage functions) from different mods; configure per-mod loot parameters (nominal/min/restock/lifetime, cargo/spawnable rules, usage/values) that can differ by economy tier; apply via audit/export. | [#1](https://github.com/Stelliro/StelliferumAuditor/issues/1) |

## Detail: mod containers / mod-function loot

**Problem.** Mods add different container and storage types (crates, bags, lockers, vehicle cargo, custom boxes). Global tier policy alone is not enough when each mod “function” needs its own loot rules per tier.

**Direction.**

1. Register mod container / storage classnames (by mod + function).
2. Define **loot parameter profiles** per mod function (and overrides per tier).
3. Drive `types.xml` + `cfgspawnabletypes` (cargo/attachments) from those profiles on export.
4. Expose config + UI so parameters are editable without mass hand-editing XMLs.

**Acceptance (summary).** List/tag mod containers; set parameters per mod function; vary by tier; audit+export honor them; document 1–2 real mod examples.
