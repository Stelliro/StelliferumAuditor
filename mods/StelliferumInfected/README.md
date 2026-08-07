# Stelliferum Infected — Tiered Zombie Difficulty Mod

Server-side + client-side DayZ mod that scales zombie **health**, **melee damage**, and gives them **glowing eyes** coloured by their difficulty tier.

## Tier Table

| Tier | Eyes | Health | Bonus Melee | Bonus Shock | Where They Spawn |
|------|--------|--------|-------------|-------------|------------------|
| T1 | **Green** | 1x (100 HP) | +0 | +0 | Coast, villages |
| T2 | **Yellow** | 1.5x (150 HP) | +5 | +5 | Industrial, farms, towns |
| T3 | **Red** | 2.5x (250 HP) | +12 | +15 | Military checkpoints |
| T4 | **Purple** | 4x (400 HP) | +20 | +25 | NWAF, Tisy, deep military |
| T5 | **Gold** | 8x (800 HP) | +35 | +50 | Tier 4 areas (rare: 0-1 on map) |
| T6 | **White** | 12x | +50 | +70 | Boss (future/event) |
| T7 | **Orange** | 20x | +75 | +100 | Raid Boss (future/event) |

T5 "Super Zombies" are the key risk factor: massive HP, devastating attacks, but extremely rare — at most 1 on the entire server at a time, spawning only in Tier 4 military areas.

## How It Works

1. **Server** spawns a zombie → `DeferredInit()` reads its classname
2. `StelliInfectedTierClassifier` maps the classname to a tier (1–7)
3. Health is multiplied on all hitzones (global, head, torso)
4. `m_StelliTier` syncs to all clients via `RegisterNetSyncVariableInt`
5. **Client** receives the sync → creates a coloured `PointLightBase` on the zombie's head
6. When the zombie hits a player, `SurvivorBase_Modded` applies bonus flat damage + shock

## File Structure

```
mods/StelliferumInfected/
├── config.cpp                                  # CfgPatches + CfgMods
└── scripts/
    └── 4_World/
        └── StelliferumInfected/
            ├── InfectedTierConfig.c            # All tuning constants (edit this to rebalance)
            ├── InfectedTierClassifier.c        # Classname → tier mapping
            ├── InfectedEyeGlow.c               # PointLightBase subclass per tier colour
            ├── ZombieBase_Modded.c             # modded ZombieBase (health + sync + glow)
            └── SurvivorBase_Modded.c           # modded SurvivorBase (bonus damage on hit)
```

## Building & Installing

### Prerequisites
- **DayZ Tools** (Steam → Tools → DayZ Tools)
- **Addon Builder** or **pboProject**

### Steps

1. **Pack the PBO**
   - Open Addon Builder (from DayZ Tools)
   - Source directory: `mods/StelliferumInfected/`
   - Destination: `@StelliferumInfected/addons/`
   - Set prefix to `StelliferumInfected`
   - Click "Pack"

2. **Install on server** — place PBO into the standalone `@StelliferumInfected` mod:
   ```
   @StelliferumInfected/
   ├── Addons/
   │   └── StelliferumInfected.pbo
   └── Keys/
       └── StelliferumInfected.bikey    (generate with DSSignFile from DayZ Tools)
   ```

3. **Server startup** — add as a separate `-mod=` entry:
   `-mod=@Stelliferrum Forge Server Pack;@StelliferumInfected`

4. **Client side** — players need the mod too (for eye glow rendering). Distribute via Steam Workshop or manual install.

## Tuning

All balance values live in **`InfectedTierConfig.c`**. Edit the `switch` statements to adjust:
- `GetHealthMultiplier()` — zombie HP scaling
- `GetBonusMeleeDamage()` — flat HP damage added per hit
- `GetBonusShockDamage()` — knockout potential per hit
- `GetEyeColor()` — RGB glow colours
- `GetEyeGlowRadius()` / `GetEyeGlowBrightness()` — visibility at distance

After editing, re-pack the PBO and restart the server.

## Integration with the Auditor

The **Stelliferum Auditor** manages zombie spawn rates via types.xml:
- T1–T4: Normal spawn rates scaled by tier
- **T5**: nominal=1, min=0 → at most 1 super zombie on the map at a time
- T6–T7: nominal=2/1, min=0 → event-level bosses

The auditor's `loot_get_zombie_tier()` classname logic mirrors `InfectedTierClassifier.GetTier()` — keep them in sync when adding new zombie classname patterns.

## Adding New Zombie Types

1. Add the classname pattern to `InfectedTierClassifier.c` (EnforceScript)
2. Add the same pattern to `loot_get_zombie_tier()` in `src/loot_manager.c` (auditor C code)
3. Re-pack PBO and rebuild auditor
