// ============================================================================
// Stelliferum Infected — DayZ Mod Configuration
// ============================================================================
// Tiered zombie difficulty system with glowing eyes, health scaling,
// and bonus melee damage.  Shared mod (client + server).
//
// Build:  Pack this folder into StelliferumInfected.pbo using Addon Builder
//         or pboProject, then place in @StelliferumInfected/addons/.
// ============================================================================

class CfgPatches
{
    class StelliferumInfected
    {
        units[] = {};
        weapons[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {"DZ_Data", "DZ_Scripts_4_World"};
    };
};

class CfgMods
{
    class StelliferumInfected
    {
        dir = "@StelliferumInfected";
        picture = "";
        action = "";
        hideName = 1;
        hidePicture = 1;
        name = "Stelliferum Infected";
        credits = "Stelliferum Forge";
        author = "Stelliferum";
        authorID = "";
        version = "1.0";
        extra = 0;
        type = "mod";

        dependencies[] = {"World Module"};

        class defs
        {
            class worldScriptModule
            {
                value = "";
                files[] = {"@StelliferumInfected/Addons/StelliferumInfected/scripts/4_World"};
            };
        };
    };
};
