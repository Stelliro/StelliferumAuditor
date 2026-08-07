// ============================================================================
// Stelliferum Infected — Tier Configuration
// ============================================================================
// Central tuning constants for every tier.  Edit ONLY this file to rebalance
// zombie difficulty — all other scripts read from here.
//
// Tier 1: Green Eyes   — Civilian zones (coast, villages)
// Tier 2: Yellow Eyes  — Industrial / farm zones
// Tier 3: Red Eyes     — Military checkpoints
// Tier 4: Purple Eyes  — Deep military (NWAF, Tisy)
// Tier 5: Gold Eyes    — Super Zombie  (rare boss, 1-2 per Tier 4 area)
// Tier 6: White Eyes   — Boss          (event-level, future use)
// Tier 7: Orange Eyes  — Raid Boss     (event-level, future use)
// ============================================================================

class StelliInfectedTierConfig
{
    // -----------------------------------------------------------------
    // Health multiplier (relative to the vanilla base HP of the zombie).
    // Vanilla civilian ≈ 100 HP, military ≈ 120 HP depending on type.
    // -----------------------------------------------------------------
    static float GetHealthMultiplier(int tier)
    {
        switch (tier)
        {
            case 1: return 1.0;    // ~100 HP  — vanilla baseline
            case 2: return 1.5;    // ~150 HP  — slightly tougher
            case 3: return 2.5;    // ~250 HP  — military grade
            case 4: return 4.0;    // ~400 HP  — endgame bruisers
            case 5: return 8.0;    // ~800 HP  — super zombie (mini-boss)
            case 6: return 12.0;   // ~1200 HP — boss
            case 7: return 20.0;   // ~2000 HP — raid boss
        }
        return 1.0;
    }

    // -----------------------------------------------------------------
    // Bonus FLAT damage added to every zombie melee hit (on top of
    // vanilla hit which does roughly 10-20 HP).
    // -----------------------------------------------------------------
    static float GetBonusMeleeDamage(int tier)
    {
        switch (tier)
        {
            case 1: return 0.0;    // Vanilla damage only
            case 2: return 5.0;    // Slightly harder hits
            case 3: return 12.0;   // Noticeable pain
            case 4: return 20.0;   // Dangerous — 3-4 hits kills geared player
            case 5: return 35.0;   // Devastating — 2-3 hits lethal
            case 6: return 50.0;   // Boss — near one-shot
            case 7: return 75.0;   // Raid boss — instant threat
        }
        return 0.0;
    }

    // -----------------------------------------------------------------
    // Bonus SHOCK damage per hit (stagger / knockout potential).
    // Vanilla shock threshold is ~100 before unconscious.
    // -----------------------------------------------------------------
    static float GetBonusShockDamage(int tier)
    {
        switch (tier)
        {
            case 1: return 0.0;
            case 2: return 5.0;
            case 3: return 15.0;
            case 4: return 25.0;
            case 5: return 50.0;   // Can knock you unconscious
            case 6: return 70.0;
            case 7: return 100.0;  // Instant KO
        }
        return 0.0;
    }

    // -----------------------------------------------------------------
    // Eye glow colour  (RGB, each component 0.0 – 1.0)
    // -----------------------------------------------------------------
    static void GetEyeColor(int tier, out float r, out float g, out float b)
    {
        switch (tier)
        {
            case 1: r = 0.0;  g = 1.0;  b = 0.0;  return;  // Green
            case 2: r = 1.0;  g = 1.0;  b = 0.0;  return;  // Yellow
            case 3: r = 1.0;  g = 0.0;  b = 0.0;  return;  // Red
            case 4: r = 0.6;  g = 0.0;  b = 1.0;  return;  // Purple
            case 5: r = 1.0;  g = 0.85; b = 0.0;  return;  // Gold
            case 6: r = 1.0;  g = 1.0;  b = 1.0;  return;  // White
            case 7: r = 1.0;  g = 0.4;  b = 0.0;  return;  // Orange
        }
        r = 0.5;  g = 0.5;  b = 0.5;  // fallback: dim grey
    }

    // -----------------------------------------------------------------
    // Eye glow radius  (world units — bigger = more visible at range)
    // -----------------------------------------------------------------
    static float GetEyeGlowRadius(int tier)
    {
        switch (tier)
        {
            case 1: return 0.3;
            case 2: return 0.4;
            case 3: return 0.5;
            case 4: return 0.7;
            case 5: return 1.2;    // Super zombie — obvious from a distance
            case 6: return 1.5;
            case 7: return 2.0;
        }
        return 0.3;
    }

    // -----------------------------------------------------------------
    // Eye glow brightness  (intensity multiplier)
    // -----------------------------------------------------------------
    static float GetEyeGlowBrightness(int tier)
    {
        switch (tier)
        {
            case 1: return 0.5;
            case 2: return 0.7;
            case 3: return 1.0;
            case 4: return 1.5;
            case 5: return 3.0;    // Unmistakable at night
            case 6: return 4.0;
            case 7: return 5.0;
        }
        return 0.5;
    }
};
