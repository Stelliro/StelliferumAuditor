// ============================================================================
// Stelliferum Infected — Survivor Damage Scaling
// ============================================================================
// Modded SurvivorBase that applies BONUS damage when a tiered zombie
// hits a player.  Runs server-side only (damage is authoritative there).
//
// Flow:
//   1. Vanilla melee damage is applied normally (super.EEHitBy)
//   2. We check if the attacker was a ZombieBase with tier > 1
//   3. Add flat bonus HP damage  + shock damage from the tier config
//
// Tier 1 zombies deal vanilla damage only — no bonus.
// ============================================================================

modded class SurvivorBase
{
    override void EEHitBy(TotalDamageResult damageResult, int damageType,
                          EntityAI source, int component, string dmgZone,
                          string ammo, vector modelPos, float speedCoef)
    {
        // Let vanilla (and other mods) apply base damage first.
        super.EEHitBy(damageResult, damageType, source, component,
                       dmgZone, ammo, modelPos, speedCoef);

        // Only apply bonus on the server.
        if (!GetGame().IsServer())
            return;

        // Was the attacker a zombie?
        if (!source || !source.IsInherited(ZombieBase))
            return;

        ZombieBase zombie = ZombieBase.Cast(source);
        if (!zombie)
            return;

        int tier = zombie.GetStelliTier();
        if (tier <= 1)
            return;   // T1 = vanilla damage, no bonus

        // ---- Bonus HP damage ----
        float bonusDmg = zombie.GetTierBonusMeleeDamage();
        if (bonusDmg > 0)
        {
            if (dmgZone != "")
                DecreaseHealth(dmgZone, "Health", bonusDmg);
            else
                DecreaseHealth("", "Health", bonusDmg);
        }

        // ---- Bonus shock damage (stagger / unconscious) ----
        float bonusShock = zombie.GetTierBonusShockDamage();
        if (bonusShock > 0)
        {
            // Shock is stored as positive; decreasing it pushes toward KO.
            AddHealth("", "Shock", -bonusShock);
        }
    }
};
