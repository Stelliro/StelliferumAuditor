// ============================================================================
// Stelliferum Infected — Tier Classifier
// ============================================================================
// Maps zombie classnames to tiers 1-7.  This mirrors the auditor's
// loot_get_zombie_tier() logic (loot_manager.c) so that the CE spawn
// tier and the runtime difficulty tier always agree.
//
// If you add new zombie classname patterns to the auditor, update this
// file to match.
// ============================================================================

class StelliInfectedTierClassifier
{
    // Returns 1-7 based on classname, or 1 as fallback.
    static int GetTier(string className)
    {
        string lower = className;
        lower.ToLower();

        // ---- Tier 7: Raid Boss ----
        if (lower.Contains("raidboss") || lower.Contains("raid_boss"))
            return 7;

        // ---- Tier 6: Boss ----
        if (lower.Contains("boss_") || lower.Contains("_boss"))
        {
            // Exclude mini-bosses — those are Tier 5.
            if (!lower.Contains("miniboss") && !lower.Contains("mini_boss"))
                return 6;
        }

        // ---- Tier 5: Mini-Boss / Super Zombie ----
        if (lower.Contains("miniboss") || lower.Contains("mini_boss"))
            return 5;
        if (lower.Contains("super") || lower.Contains("elite"))
            return 5;

        // ---- SNAFU zombies: T4-T5 ----
        if (lower.Contains("snafu"))
        {
            if (lower.Contains("heavy") || lower.Contains("elite"))
                return 5;
            return 4;
        }

        // ---- GoreZ zombies: T3-T5 ----
        if (lower.Contains("gorez") || lower.Contains("gore"))
        {
            if (lower.Contains("heavy") || lower.Contains("big"))
                return 5;
            return 3;
        }

        // ---- Heavy variants: T4 ----
        if (lower.Contains("heavy"))
            return 4;

        // ---- Military variants: T3 ----
        if (lower.Contains("soldier") || lower.Contains("military"))
            return 3;
        if (lower.Contains("soldiernormal") || lower.Contains("patrolnormal"))
            return 3;

        // ---- Police / Guard: T2 ----
        if (lower.Contains("police") || lower.Contains("guard"))
            return 2;

        // ---- Industrial / City: T2 ----
        if (lower.Contains("citizen") || lower.Contains("city"))
            return 2;
        if (lower.Contains("industrial"))
            return 2;

        // ---- Coastal / Village / Farm: T1 ----
        if (lower.Contains("village") || lower.Contains("farmer"))
            return 1;
        if (lower.Contains("fisherman") || lower.Contains("thin"))
            return 1;

        // ---- Default: civilian baseline ----
        return 1;
    }
};
