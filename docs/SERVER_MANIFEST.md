---

### PHASE 1: TECHNICAL REFRESHER (Packing & Unpacking)

As your Lead Dev, I use **PBO Manager** (3rd party tool) for unpacking because it’s faster than the official tools, but I will give you the official **DayZ Tools** method for packing to ensure key signatures are correct.

**A. How to UNPACK (View/Edit Files):**
*The "Surgeon's Scalpel"*
1.  Open **DayZ Tools** from Steam.
2.  Select **"DS Utils"** (DayZ Server Utilities).
3.  Click **"Extract PBO"** (The folder icon).
4.  Select your target file (e.g., `SNAFU_Weapons.pbo`).
5.  Select the Output folder. Click **Extract**.
    *   *Dev Note:* If you just want to peek inside, use the context menu tool **PBO Manager**. Right-click a PBO -> "Extract to...". It saves time.

**B. How to PACK (Finalize/Build):**
*The "Foundry Press"*
1.  Open **DayZ Tools** -> **Addon Builder**.
2.  **Source Directory:** Point this to your mod folder (the one containing `config.cpp` and `model.cfg`).
3.  **Destination Directory:** Where you want the `.pbo` to go (usually a "Packed" folder).
4.  **Options:**
    *   Check "Binarize".
    *   **Path to Project Drive:** Usually `P:\` (If you have your Mount set up).
5.  **Signatures (CRITICAL):**
    *   Point "Path to private key" to your `MentalIllness.biprivatekey`.
6.  Click **PACK**.

---

### PHASE 2: THE UPDATED MANIFEST (Version 1.2)

I have updated Ben's file.
**Changes made:**
1.  **Swapped** VPPAdminTools for **COT** (Matches your loadout).
2.  **Added** **MMG** to the Approved Mod Stack.
3.  **Added** **Stelliferrum Forge Server Pack** (Audio/Visuals).
4.  **Integrated MMG** into the Economy Tiers (Tier 7 & 9) to prevent players from getting heavy armor too early.

**Forward this file to Ben immediately.**

***

# SERVER_MANIFEST_v1.2.md

---

# PROJECT INDEX: STELLIFERRUM FORGE
**Mission Profile:** Hardcore Survival // Sci-Fi Horror // High-Stakes PVP
**Map:** Chernarus Plus (Default)
**Current Phase:** Phase 1 (Foundation & Economy Design)

---

## 1. THE CORE DIRECTIVES
*   **The Vibe:** You are not a hero; you are prey. Atmospheric horror mixed with military simulation.
*   **The Mod Rule:** Quality over Quantity. If it causes lag, it is cut.
*   **The Damage Model:** "Soft" Realistic. No bullet sponges.

---

## 2. THE MOD STACK (Active & Approved)
*Current status of active modifications.*

**A. CORE FRAMEWORKS & ADMIN:**
*   **CF** (Community Framework) - *Dependency.*
*   **Community-Online-Tools (COT)** - *Admin Management.*
*   **Stelliferrum Forge Server Pack** - *Custom Audio/Visuals/Assets.*

**B. GAMEPLAY & MECHANICS:**
*   **Mental Illness Overhaul** - *Sanity System (Hallucinations/Shakes).*
*   **Stamina:** Custom "Tactical" Settings (configured in `cfggameplay.json`).

**C. WEAPONRY & GEAR:**
*   **SNAFU Weapons** - *Main Firepower.*
*   **MMG (Mighty's Military Gear)** - *High-End Armor & Storage.*
    *   *Dev Note:* MMG Storage must be restricted to keep base FPS high.
*   **Vanilla Weapons** - *Low/Mid Tier.*

**D. AI / PVE (Pending Implementation):**
*   **Predator/Alien Mod** - *Boss Event.*
*   **DayZ-Creatures** - *Potential Dependency.*

---

## 3. THE 11-TIER ECONOMY LADDER
*All items must be assigned a Tier before being added to `types.xml`.*

**zone: CIVILIAN (The Struggle)**
*   **Tier 1 (Scavenger):** Coast. Wrenches, pipes, ruined clothes, plums.
*   **Tier 2 (Survivor):** Inland Towns. IJ-70, Pump Shotgun, Wool Coats, Canned Tuna.
*   **Tier 3 (Constable):** Police Stations. Scorpion, Glock, Stab Vest, Handcuffs.
*   **Tier 4 (Outdoorsman):** Summer Camps/Hunting Stands. Blaze, Tundra, Hunting Scope, Chlorine.

**zone: MILITARY (The Escalation)**
*   **Tier 5 (Insurgent):** Mil Checkpoints. Baby AK (74u), SKS, Mosin, Vanilla Camos.
*   **Tier 6 (Infantry):** VMC/NWAF (South). M16, AK-74, M79 Thumper.
*   **Tier 7 (Spec-Ops):** Tisy/Tri-Kresta. AKM, **MMG Tactical Vests**, NVGs (Green).

**zone: END-GAME (The Prize)**
*   **Tier 8 (Operator):** Heli Crashes Only. LAR, VSD, AUG AX, **MMG Helmets (Striker)**.
*   **Tier 9 (Black Market):** Keycard Rooms/Bunkers. .50 Cal (Barrett), Cheytac, C4, **MMG Heavy Plate Carriers**.
*   **Tier 10 (Mythic):** **THE PREDATOR DROP.** Alien Tech, Thermal Vision, Minigun. *Non-restocking until wipe or loss.*

**zone: ADMIN (Out-of-Economy)**
*   **Tier 11 (Admin):** Admin-only gear/tools. **No CLE spawns. Manual grants only.**

---

## 3A. STORE ECONOMY & PRICING RULES
*Store entries must reference Tier and Rank, and follow the currency rules below.*

**A. Currency Layers**
*   **Standard Market (Tiers 1-7):** Main currency (TBD).
*   **Black Market (Tiers 8-11):** **Separate currency** (placeholder: `BlackMarketToken`).
*   **Rule:** If currency is undefined, agents must attempt to locate a currency in trader configs first; if none exists, use the placeholder and flag it.

**B. Pricing Logic**
*   **Base Price:** Determined by Tier (higher tier = higher base cost).
*   **Rank Modifier:** Prices scale by item rank/rarity within its tier.
*   **Volatility:** Black Market prices can vary within a band to simulate scarcity.

**C. Admin Tier Controls**
*   **Tier 11 (Admin):** Sell-only (admins can sell, not buy). Not purchasable.
*   **Availability:** Extremely rare. Target **1 item globally** with **2-hour respawn** in event or boss pools only (e.g., Predator).
*   **No CLE spawns** outside approved event loot pools.

---

## 4. CUSTOM MECHANICS & SCRIPTS
**A. The "Sanity" Loop**
*   **Drain Triggers:** Cannibalism, Killing Players, Darkness, Low Health.
*   **Effects:** Auditory/Visual hallucinations (managed by Mental Illness Mod).
*   **Recovery:** Fire, Social interaction, Comfort items.

**B. The "Predator" Event**
*   **Timer:** Spawns once every 86,400 seconds (24 hours).
*   **Location:** Designated Hunting Ground (TBD).
*   **Reward:** 1x Tier 10 Item.

---

## 5. FILE DIRECTORY (Who edits what?)
*   **Loot Tables:** `mpmissions/db/types.xml` (The Master List).
*   **Mod Configs:** `SC/` or `Profile/` folder (MMG/SNAFU specific settings).
*   **Event Timers:** `db/events.xml` (Predator spawn time).
*   **Custom Logic:** `mpmissions/init.c` (Wipe rules, Loadouts).
*   **Game Rules:** `cfggameplay.json` (Stamina, hit reg).

---

## 6. CURRENT ACTION ITEMS (To-Do)
*   [ ] **Dev A (Mental):** Pack/Unpack "Stelliferrum Forge" and verify audio hooks.
*   [ ] **Dev B (Ben):** Integrate MMG items into `types.xml` but **EXCLUDE** them from spawning in Tier 1-5 zones.
*   [ ] **Joint:** Define the exact list of guns from SNAFU that make the cut.
*   [ ] **AI Task:** Write the Python script to balance the `types.xml` lifetimes.

***

### **Instruction for the Team:**
When you or your partner ask Gemini to write code, **paste the relevant section of this Index first**.

*Example:* "Gemini, I need to add the MMG Striker Helmet. Refer to the '11-Tier Economy Ladder' in the Index. This belongs in Tier 8. Please write the XML entry."