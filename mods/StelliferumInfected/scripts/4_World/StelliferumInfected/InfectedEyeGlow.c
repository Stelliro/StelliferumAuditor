// ============================================================================
// Stelliferum Infected — Eye Glow System
// ============================================================================
// Coloured point-light attached to the zombie's head bone.
//
// SERVER sets m_StelliTier and syncs it.
// CLIENT receives the sync and creates a local PointLightBase with the
// correct colour / radius / brightness for that tier.
//
//   T1 Green  |  T2 Yellow  |  T3 Red
//   T4 Purple |  T5 Gold    |  T6 White  |  T7 Orange
// ============================================================================

class StelliInfectedEyeGlow extends PointLightBase
{
    // ---- Constructor: safe defaults ----
    void StelliInfectedEyeGlow()
    {
        SetVisibleDuringDaylight(true);
        SetFlareVisible(false);
        SetCastShadow(false);
        SetFadeOutTime(0.5);
    }

    // ---- Apply tier-specific colours and intensity ----
    void ConfigureForTier(int tier)
    {
        float r, g, b;
        StelliInfectedTierConfig.GetEyeColor(tier, r, g, b);

        SetRadiusTo(StelliInfectedTierConfig.GetEyeGlowRadius(tier));
        SetBrightnessTo(StelliInfectedTierConfig.GetEyeGlowBrightness(tier));
        SetDiffuseColor(r, g, b);
        SetAmbientColor(r * 0.3, g * 0.3, b * 0.3);
    }

    // ---- Factory: create glow and attach to zombie head ----
    static StelliInfectedEyeGlow CreateForZombie(int tier, DayZInfected zombie)
    {
        if (!zombie || tier < 1 || tier > 7)
            return null;

        // Try to attach at the head bone; fall back to a sensible offset.
        vector headOffset = "0 1.6 0";   // approximate eye height
        int headIdx = zombie.GetBoneIndexByName("Head");
        if (headIdx != -1)
        {
            headOffset = zombie.GetBonePositionLS(headIdx);
            headOffset[1] = headOffset[1] + 0.05;   // nudge up to eye level
        }

        StelliInfectedEyeGlow glow = StelliInfectedEyeGlow.Cast(
            ScriptedLightBase.CreateLight(StelliInfectedEyeGlow, headOffset, zombie)
        );

        if (glow)
        {
            glow.ConfigureForTier(tier);
            glow.AttachOnObject(zombie, headOffset);
        }

        return glow;
    }
};
