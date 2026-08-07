#include "loot_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Global policy — populated by loot_policy_load() at startup and by the UI
// Save button. Read by loot_manager.c / auditor.c / writer.c during generation.
LootPolicy g_loot_policy = {0};

// ---------------------------------------------------------------------------
// small self-contained helpers (module depends only on libc)
// ---------------------------------------------------------------------------

static void lp_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static bool lp_ieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

// Split a comma-separated list into out[][LP_MAX_NAME_LEN]; trims each token,
// skips empties. Returns token count (<= max).
static int lp_split(const char *src, char out[][LP_MAX_NAME_LEN], int max) {
    int n = 0;
    const char *p = src ? src : "";
    while (*p && n < max) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        int len = (int)(p - start);
        while (len > 0 && isspace((unsigned char)start[len - 1])) len--;
        if (len > 0) {
            if (len >= LP_MAX_NAME_LEN) len = LP_MAX_NAME_LEN - 1;
            memcpy(out[n], start, (size_t)len);
            out[n][len] = '\0';
            n++;
        }
    }
    return n;
}

// Apply a flag to every tier listed in a comma-separated tier-number string.
// which: 0 = spawns=false, 1 = tradeable=false, 2 = black_market=true
static void lp_apply_tier_flag_list(const char *list, int which) {
    if (!list || !list[0]) return;
    char toks[LP_MAX_TIERS][LP_MAX_NAME_LEN];
    int n = lp_split(list, toks, LP_MAX_TIERS);
    for (int i = 0; i < n; i++) {
        int t = atoi(toks[i]);
        if (t < 1 || t > g_loot_policy.tier_count) continue;
        LpTier *tr = &g_loot_policy.tiers[t - 1];
        if      (which == 0) tr->spawns       = false;
        else if (which == 1) tr->tradeable    = false;
        else if (which == 2) tr->black_market = true;
    }
}

// ---------------------------------------------------------------------------
// defaults — the locked 12-tier map
// ---------------------------------------------------------------------------
void loot_policy_init_defaults(void) {
    memset(&g_loot_policy, 0, sizeof(g_loot_policy));

    static const char *names[] = {
        "Scavenger", "Survivor", "Constable", "Outdoorsman", "Insurgent",
        "Infantry", "Spec-Ops", "Operator", "Mythic", "Elite",
        "Black Market", "Contraband"
    };
    int n = (int)(sizeof(names) / sizeof(names[0])); // 12

    g_loot_policy.tier_count = n;
    for (int i = 0; i < n; i++) {
        strncpy(g_loot_policy.tiers[i].name, names[i], LP_MAX_TIER_NAME - 1);
        g_loot_policy.tiers[i].spawns       = true;
        g_loot_policy.tiers[i].tradeable    = true;
        g_loot_policy.tiers[i].black_market = false;
    }

    // T11 "Black Market" — no world spawn; purchasable at BM in Bitcoin.
    g_loot_policy.tiers[10].spawns       = false;
    g_loot_policy.tiers[10].black_market = true;
    // T12 "Contraband" — no spawn, no trade (blacklist bucket).
    g_loot_policy.tiers[11].spawns    = false;
    g_loot_policy.tiers[11].tradeable = false;

    g_loot_policy.contraband_tier        = 12;
    g_loot_policy.bitcoin_spawn_min_tier = 3;
    g_loot_policy.bitcoin_spawn_max_tier = 11;
    g_loot_policy.blacklist_count        = 0;
    g_loot_policy.loaded                 = true;
}

// ---------------------------------------------------------------------------
// load / save
// ---------------------------------------------------------------------------
bool loot_policy_load(const char *path) {
    loot_policy_init_defaults();

    FILE *f = fopen(path, "rb");
    if (!f) return false; // keep defaults in effect

    // Accumulate raw values first so key order in the file doesn't matter.
    char *v_names = (char *)calloc(1, 8192);
    char *v_black = (char *)calloc(1, 65536);
    char *line    = (char *)malloc(65536);
    char v_nospawn[512] = {0}, v_notrade[512] = {0}, v_bm[512] = {0};
    int  v_contra = -1, v_btcmin = -1, v_btcmax = -1;

    if (!v_names || !v_black || !line) {
        free(v_names); free(v_black); free(line); fclose(f);
        return false;
    }

    while (fgets(line, 65536, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char key[64];
        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        lp_trim(key);
        char *val = eq + 1;
        lp_trim(val);

        if      (lp_ieq(key, "TIER_NAMES"))        strncpy(v_names, val, 8191);
        else if (lp_ieq(key, "NOSPAWN_TIERS"))     strncpy(v_nospawn, val, sizeof(v_nospawn) - 1);
        else if (lp_ieq(key, "NOTRADE_TIERS"))     strncpy(v_notrade, val, sizeof(v_notrade) - 1);
        else if (lp_ieq(key, "BLACKMARKET_TIERS")) strncpy(v_bm, val, sizeof(v_bm) - 1);
        else if (lp_ieq(key, "CONTRABAND_TIER"))   v_contra = atoi(val);
        else if (lp_ieq(key, "BITCOIN_SPAWN_MIN")) v_btcmin = atoi(val);
        else if (lp_ieq(key, "BITCOIN_SPAWN_MAX")) v_btcmax = atoi(val);
        else if (lp_ieq(key, "BLACKLIST")) {
            if (val[0]) {
                size_t used = strlen(v_black);
                if (used && used < 65534) { v_black[used] = ','; v_black[used + 1] = '\0'; used++; }
                if (used < 65535) strncat(v_black, val, 65535 - used);
            }
        }
    }
    free(line);
    fclose(f);

    // Apply names FIRST (resets the tier table), then flag lists, then scalars.
    if (v_names[0]) {
        char nametoks[LP_MAX_TIERS][LP_MAX_NAME_LEN];
        int cnt = lp_split(v_names, nametoks, LP_MAX_TIERS);
        if (cnt > 0) {
            memset(g_loot_policy.tiers, 0, sizeof(g_loot_policy.tiers));
            for (int i = 0; i < cnt; i++) {
                strncpy(g_loot_policy.tiers[i].name, nametoks[i], LP_MAX_TIER_NAME - 1);
                g_loot_policy.tiers[i].spawns       = true;
                g_loot_policy.tiers[i].tradeable    = true;
                g_loot_policy.tiers[i].black_market = false;
            }
            g_loot_policy.tier_count = cnt;
        }
    }

    lp_apply_tier_flag_list(v_nospawn, 0);
    lp_apply_tier_flag_list(v_notrade, 1);
    lp_apply_tier_flag_list(v_bm,      2);

    if (v_contra >= 0) g_loot_policy.contraband_tier        = v_contra;
    if (v_btcmin >= 0) g_loot_policy.bitcoin_spawn_min_tier = v_btcmin;
    if (v_btcmax >= 0) g_loot_policy.bitcoin_spawn_max_tier = v_btcmax;

    if (v_black[0])
        g_loot_policy.blacklist_count =
            lp_split(v_black, g_loot_policy.blacklist, LP_MAX_BLACKLIST);

    free(v_names);
    free(v_black);
    g_loot_policy.loaded = true;
    return true;
}

static void lp_write_tier_number_list(FILE *f, int which) {
    int first = 1;
    for (int i = 0; i < g_loot_policy.tier_count; i++) {
        bool hit = (which == 0) ? !g_loot_policy.tiers[i].spawns
                 : (which == 1) ? !g_loot_policy.tiers[i].tradeable
                 :                 g_loot_policy.tiers[i].black_market;
        if (hit) { fprintf(f, "%s%d", first ? "" : ",", i + 1); first = 0; }
    }
}

bool loot_policy_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    fprintf(f, "# Stelliferum Loot Policy - the real config the generator reads.\n");
    fprintf(f, "# Edit here or via the in-app \"Loot Policy\" tab.\n");
    fprintf(f, "#\n");
    fprintf(f, "# TIER_NAMES: comma-separated. Number of names = number of tiers.\n");
    fprintf(f, "TIER_NAMES=");
    for (int i = 0; i < g_loot_policy.tier_count; i++)
        fprintf(f, "%s%s", (i ? "," : ""), g_loot_policy.tiers[i].name);
    fprintf(f, "\n\n");

    fprintf(f, "# Tiers that do NOT world-spawn (nominal 0).\n");
    fprintf(f, "NOSPAWN_TIERS=");   lp_write_tier_number_list(f, 0); fprintf(f, "\n");
    fprintf(f, "# Tiers excluded from ALL traders (banned/contraband).\n");
    fprintf(f, "NOTRADE_TIERS=");   lp_write_tier_number_list(f, 1); fprintf(f, "\n");
    fprintf(f, "# Tiers sold at the Black Market in Bitcoin.\n");
    fprintf(f, "BLACKMARKET_TIERS="); lp_write_tier_number_list(f, 2); fprintf(f, "\n\n");

    fprintf(f, "# Tier that blacklisted items are forced into.\n");
    fprintf(f, "CONTRABAND_TIER=%d\n\n", g_loot_policy.contraband_tier);

    fprintf(f, "# Bitcoin world-spawn scaling (rare at MIN -> fairly common at MAX; 0 = off).\n");
    fprintf(f, "BITCOIN_SPAWN_MIN=%d\n",   g_loot_policy.bitcoin_spawn_min_tier);
    fprintf(f, "BITCOIN_SPAWN_MAX=%d\n\n", g_loot_policy.bitcoin_spawn_max_tier);

    fprintf(f, "# Never-spawn blacklist (comma-separated classnames).\n");
    fprintf(f, "BLACKLIST=");
    for (int i = 0; i < g_loot_policy.blacklist_count; i++)
        fprintf(f, "%s%s", (i ? "," : ""), g_loot_policy.blacklist[i]);
    fprintf(f, "\n");

    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// setters (used by the in-app Loot Policy editor)
// ---------------------------------------------------------------------------
void loot_policy_set_tiers_csv(const char *csv) {
    char names[LP_MAX_TIERS][LP_MAX_NAME_LEN];
    int cnt = lp_split(csv, names, LP_MAX_TIERS);
    if (cnt <= 0) return;

    LpTier old[LP_MAX_TIERS];
    memcpy(old, g_loot_policy.tiers, sizeof(old));
    int old_count = g_loot_policy.tier_count;

    memset(g_loot_policy.tiers, 0, sizeof(g_loot_policy.tiers));
    for (int i = 0; i < cnt; i++) {
        strncpy(g_loot_policy.tiers[i].name, names[i], LP_MAX_TIER_NAME - 1);
        if (i < old_count) {
            g_loot_policy.tiers[i].spawns       = old[i].spawns;
            g_loot_policy.tiers[i].tradeable    = old[i].tradeable;
            g_loot_policy.tiers[i].black_market = old[i].black_market;
        } else {
            g_loot_policy.tiers[i].spawns       = true;
            g_loot_policy.tiers[i].tradeable    = true;
            g_loot_policy.tiers[i].black_market = false;
        }
    }
    g_loot_policy.tier_count = cnt;
}

void loot_policy_set_blacklist_csv(const char *csv) {
    g_loot_policy.blacklist_count = lp_split(csv, g_loot_policy.blacklist, LP_MAX_BLACKLIST);
}

// ---------------------------------------------------------------------------
// queries (1-based tier indices)
// ---------------------------------------------------------------------------
int lp_tier_count(void) {
    return g_loot_policy.loaded ? g_loot_policy.tier_count : 12;
}

const char *lp_tier_name(int tier) {
    if (tier >= 1 && tier <= g_loot_policy.tier_count)
        return g_loot_policy.tiers[tier - 1].name;
    return "";
}

bool lp_tier_spawns(int tier) {
    if (tier >= 1 && tier <= g_loot_policy.tier_count)
        return g_loot_policy.tiers[tier - 1].spawns;
    return true; // unknown tiers default to spawning
}

bool lp_tier_tradeable(int tier) {
    if (tier >= 1 && tier <= g_loot_policy.tier_count)
        return g_loot_policy.tiers[tier - 1].tradeable;
    return true;
}

bool lp_tier_black_market(int tier) {
    if (tier >= 1 && tier <= g_loot_policy.tier_count)
        return g_loot_policy.tiers[tier - 1].black_market;
    return false;
}

int lp_black_market_tier(void) {
    for (int i = 0; i < g_loot_policy.tier_count; i++)
        if (g_loot_policy.tiers[i].black_market) return i + 1;
    return 0;
}

int lp_contraband_tier(void) {
    return g_loot_policy.contraband_tier;
}

bool lp_is_blacklisted(const char *classname) {
    if (!classname || !classname[0]) return false;
    for (int i = 0; i < g_loot_policy.blacklist_count; i++)
        if (lp_ieq(g_loot_policy.blacklist[i], classname)) return true;
    return false;
}
