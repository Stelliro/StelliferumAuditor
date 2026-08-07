// ============================================================================
// Stelliferum Infected — ZombieBase Override
// ============================================================================
// Core modded class that wires everything together:
//
//  SERVER side
//    1. DeferredInit(): classify tier from classname
//    2. Scale max health for each hitzone
//    3. Sync m_StelliTier to all clients
//
//  CLIENT side
//    1. OnVariablesSynchronized(): receive tier
//    2. Create / update eye glow light
//
// Zombie attack damage bonus is handled in SurvivorBase_Modded.c.
// ============================================================================

modded class ZombieBase
{
    // ---- Synced variable — server writes, clients read ----
    protected int m_StelliTier = 0;

    // ---- Client-only: glow light handle ----
    protected ref StelliInfectedEyeGlow m_EyeGlow;

    // ================================================================
    //  Constructor — register the synced variable
    // ================================================================
    void ZombieBase()
    {
        RegisterNetSyncVariableInt("m_StelliTier", 0, 7);
    }

    // ================================================================
    //  Server: classify tier & apply health on first spawn
    // ================================================================
    override void DeferredInit()
    {
        super.DeferredInit();

        if (GetGame().IsServer())
        {
            m_StelliTier = StelliInfectedTierClassifier.GetTier(GetType());

            if (m_StelliTier > 0)
            {
                ApplyTierHealth();
            }

            SetSynchDirty();   // push tier to all clients
        }
    }

    // ================================================================
    //  Client: receive tier sync → update visuals
    // ================================================================
    override void OnVariablesSynchronized()
    {
        super.OnVariablesSynchronized();

        if (m_StelliTier > 0)
        {
            UpdateEyeGlow();
        }
    }

    // ================================================================
    //  Health scaling — multiply every hitzone by the tier factor
    // ================================================================
    protected void ApplyTierHealth()
    {
        float mult = StelliInfectedTierConfig.GetHealthMultiplier(m_StelliTier);
        if (mult <= 1.0)
            return;   // T1 uses vanilla health

        // Global (overall) health
        float baseHP = GetMaxHealth("", "Health");
        if (baseHP > 0)
            SetHealth("", "Health", baseHP * mult);

        // Head hitzone — still needs to be "headshotable" but sturdier
        float headHP = GetMaxHealth("Head", "Health");
        if (headHP > 0)
            SetHealth("Head", "Health", headHP * mult);

        // Torso
        float torsoHP = GetMaxHealth("Torso", "Health");
        if (torsoHP > 0)
            SetHealth("Torso", "Health", torsoHP * mult);
    }

    // ================================================================
    //  Eye glow — client-side point light on the head bone
    // ================================================================
    protected void UpdateEyeGlow()
    {
        if (!GetGame().IsClient() && !GetGame().IsMultiplayer())
            return;

        // Destroy previous glow if the tier changed mid-life (unlikely but safe)
        if (m_EyeGlow)
        {
            m_EyeGlow.Destroy();
            m_EyeGlow = null;
        }

        m_EyeGlow = StelliInfectedEyeGlow.CreateForZombie(m_StelliTier, this);
    }

    // ================================================================
    //  Cleanup on death / despawn
    // ================================================================
    override void EEDelete(EntityAI parent)
    {
        if (m_EyeGlow)
        {
            m_EyeGlow.Destroy();
            m_EyeGlow = null;
        }

        super.EEDelete(parent);
    }

    // ================================================================
    //  Public API — used by SurvivorBase_Modded for damage bonus
    // ================================================================
    int GetStelliTier()
    {
        return m_StelliTier;
    }

    float GetTierBonusMeleeDamage()
    {
        return StelliInfectedTierConfig.GetBonusMeleeDamage(m_StelliTier);
    }

    float GetTierBonusShockDamage()
    {
        return StelliInfectedTierConfig.GetBonusShockDamage(m_StelliTier);
    }
};
