#include "loot_policy.h"
#include "auditor.h" /* util_log — soft config errors must not abort */

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
// key_label is used only for soft error messages (may be NULL).
static void lp_apply_tier_flag_list(const char *list, int which, const char *key_label) {
    if (!list || !list[0]) return;
    char toks[LP_MAX_TIERS][LP_MAX_NAME_LEN];
    int n = lp_split(list, toks, LP_MAX_TIERS);
    const char *kl = key_label ? key_label : "tier flag list";
    for (int i = 0; i < n; i++) {
        /* Require at least one digit — reject pure garbage tokens. */
        const char *p = toks[i];
        int has_digit = 0;
        if (*p == '+' || *p == '-') p++;
        for (; *p; p++) {
            if (isdigit((unsigned char)*p)) { has_digit = 1; break; }
            if (!isspace((unsigned char)*p)) break;
        }
        int t = atoi(toks[i]);
        if (!has_digit || t < 1 || t > g_loot_policy.tier_count) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: %s has invalid tier entry '%s' (valid 1..%d) — skipped",
                     kl, toks[i], g_loot_policy.tier_count);
            continue;
        }
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
        "Coast Scav", "Town Survivor", "Constable", "Outdoorsman", "Insurgent",
        "Infantry", "Spec Ops", "Operator", "Elite", "Mythic",
        "Black Market", "Contraband"
    };
    /* Soft defaults match config/loot_policy.ini distribution hints */
    static const int nom[]  = { 45, 28, 18, 14, 12, 8, 5, 3, 2, 1, 0, 0 };
    static const int mins[] = { 28, 16, 10,  8,  6, 4, 2, 1, 1, 0, 0, 0 };
    static const int life[] = { 3600, 5400, 7200, 9000, 10800, 14400, 18000, 21600, 28800, 36000, 86400, 86400 };
    static const int rest[] = { 0, 300, 600, 600, 900, 1800, 1800, 3600, 3600, 7200, 0, 0 };
    int n = (int)(sizeof(names) / sizeof(names[0])); // 12

    g_loot_policy.tier_count = n;
    for (int i = 0; i < n; i++) {
        strncpy(g_loot_policy.tiers[i].name, names[i], LP_MAX_TIER_NAME - 1);
        g_loot_policy.tiers[i].spawns       = true;
        g_loot_policy.tiers[i].tradeable    = true;
        g_loot_policy.tiers[i].black_market = false;
        g_loot_policy.tiers[i].nominal_target  = nom[i];
        g_loot_policy.tiers[i].min_target      = mins[i];
        g_loot_policy.tiers[i].lifetime_target = life[i];
        g_loot_policy.tiers[i].restock_target  = rest[i];
    }

    // T11 "Black Market" — no world spawn; purchasable at BM in Bitcoin.
    g_loot_policy.tiers[10].spawns       = false;
    g_loot_policy.tiers[10].black_market = true;
    // T12 "Contraband" — no spawn, no trade (blacklist bucket).
    g_loot_policy.tiers[11].spawns    = false;
    g_loot_policy.tiers[11].tradeable = false;

    g_loot_policy.contraband_tier          = 12;
    g_loot_policy.bitcoin_spawn_min_tier   = 4;
    g_loot_policy.bitcoin_spawn_max_tier   = 10;
    g_loot_policy.blacklist_count          = 0;
    g_loot_policy.has_distribution_targets = true;
    g_loot_policy.loaded                   = true;
}

// ---------------------------------------------------------------------------
// load / save
// ---------------------------------------------------------------------------
bool loot_policy_load(const char *path) {
    loot_policy_init_defaults();

    if (!path || !path[0]) {
        util_log(SEVERITY_ERROR,
                 "loot_policy: no path provided — keeping built-in 12-tier defaults");
        return false; /* defaults remain in effect; non-fatal */
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Soft fail: missing/unreadable file must not crash or abort startup. */
        util_log(SEVERITY_ERROR,
                 "loot_policy: missing or unreadable '%s' — keeping built-in 12-tier defaults",
                 path);
        return false;
    }

    // Accumulate raw values first so key order in the file doesn't matter.
    char *v_names = (char *)calloc(1, 8192);
    char *v_black = (char *)calloc(1, 65536);
    char *line    = (char *)malloc(65536);
    char v_nospawn[512] = {0}, v_notrade[512] = {0}, v_bm[512] = {0};
    char v_nominal[512] = {0}, v_min[512] = {0}, v_life[512] = {0}, v_restock[512] = {0};
    char v_contra_raw[64] = {0}, v_btcmin_raw[64] = {0}, v_btcmax_raw[64] = {0};
    int  v_contra = -1, v_btcmin = -1, v_btcmax = -1;
    bool saw_names = false;
    bool saw_nospawn = false, saw_notrade = false, saw_bm = false;
    bool saw_contra = false, saw_btcmin = false, saw_btcmax = false;
    bool saw_nominal = false, saw_min = false, saw_life = false, saw_restock = false;

    if (!v_names || !v_black || !line) {
        util_log(SEVERITY_ERROR,
                 "loot_policy: out of memory while loading '%s' — keeping built-in defaults",
                 path);
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

        if      (lp_ieq(key, "TIER_NAMES"))        { saw_names = true; strncpy(v_names, val, 8191); }
        else if (lp_ieq(key, "NOSPAWN_TIERS"))     { saw_nospawn = true; strncpy(v_nospawn, val, sizeof(v_nospawn) - 1); }
        else if (lp_ieq(key, "NOTRADE_TIERS"))     { saw_notrade = true; strncpy(v_notrade, val, sizeof(v_notrade) - 1); }
        else if (lp_ieq(key, "BLACKMARKET_TIERS")) { saw_bm = true; strncpy(v_bm, val, sizeof(v_bm) - 1); }
        else if (lp_ieq(key, "CONTRABAND_TIER")) {
            saw_contra = true;
            strncpy(v_contra_raw, val, sizeof(v_contra_raw) - 1);
            v_contra = val[0] ? atoi(val) : -1;
        }
        else if (lp_ieq(key, "BITCOIN_SPAWN_MIN")) {
            saw_btcmin = true;
            strncpy(v_btcmin_raw, val, sizeof(v_btcmin_raw) - 1);
            v_btcmin = val[0] ? atoi(val) : -1;
        }
        else if (lp_ieq(key, "BITCOIN_SPAWN_MAX")) {
            saw_btcmax = true;
            strncpy(v_btcmax_raw, val, sizeof(v_btcmax_raw) - 1);
            v_btcmax = val[0] ? atoi(val) : -1;
        }
        else if (lp_ieq(key, "NOMINAL_TARGETS"))   { saw_nominal = true; strncpy(v_nominal, val, sizeof(v_nominal) - 1); }
        else if (lp_ieq(key, "MIN_TARGETS"))       { saw_min = true; strncpy(v_min, val, sizeof(v_min) - 1); }
        else if (lp_ieq(key, "LIFETIME_TARGETS"))  { saw_life = true; strncpy(v_life, val, sizeof(v_life) - 1); }
        else if (lp_ieq(key, "RESTOCK_TARGETS"))   { saw_restock = true; strncpy(v_restock, val, sizeof(v_restock) - 1); }
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
    if (saw_names && !v_names[0]) {
        util_log(SEVERITY_ERROR,
                 "loot_policy: TIER_NAMES is empty in '%s' — keeping default tier names",
                 path);
    } else if (v_names[0]) {
        char nametoks[LP_MAX_TIERS][LP_MAX_NAME_LEN];
        int cnt = lp_split(v_names, nametoks, LP_MAX_TIERS);
        if (cnt <= 0) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: TIER_NAMES produced no valid names in '%s' — keeping defaults",
                     path);
        } else {
            LpTier old[LP_MAX_TIERS];
            int old_n = g_loot_policy.tier_count;
            memcpy(old, g_loot_policy.tiers, sizeof(old));
            memset(g_loot_policy.tiers, 0, sizeof(g_loot_policy.tiers));
            for (int i = 0; i < cnt; i++) {
                strncpy(g_loot_policy.tiers[i].name, nametoks[i], LP_MAX_TIER_NAME - 1);
                g_loot_policy.tiers[i].spawns       = true;
                g_loot_policy.tiers[i].tradeable    = true;
                g_loot_policy.tiers[i].black_market = false;
                /* Keep prior distribution targets when renaming same-index tiers */
                if (i < old_n) {
                    g_loot_policy.tiers[i].nominal_target  = old[i].nominal_target;
                    g_loot_policy.tiers[i].min_target      = old[i].min_target;
                    g_loot_policy.tiers[i].lifetime_target = old[i].lifetime_target;
                    g_loot_policy.tiers[i].restock_target  = old[i].restock_target;
                } else {
                    g_loot_policy.tiers[i].nominal_target  = -1;
                    g_loot_policy.tiers[i].min_target      = -1;
                    g_loot_policy.tiers[i].lifetime_target = -1;
                    g_loot_policy.tiers[i].restock_target  = -1;
                }
            }
            g_loot_policy.tier_count = cnt;
        }
    }

    if (saw_nospawn && !v_nospawn[0])
        util_log(SEVERITY_ERROR, "loot_policy: NOSPAWN_TIERS is empty in '%s' — flag list ignored", path);
    if (saw_notrade && !v_notrade[0])
        util_log(SEVERITY_ERROR, "loot_policy: NOTRADE_TIERS is empty in '%s' — flag list ignored", path);
    if (saw_bm && !v_bm[0])
        util_log(SEVERITY_ERROR, "loot_policy: BLACKMARKET_TIERS is empty in '%s' — flag list ignored", path);

    lp_apply_tier_flag_list(v_nospawn, 0, "NOSPAWN_TIERS");
    lp_apply_tier_flag_list(v_notrade, 1, "NOTRADE_TIERS");
    lp_apply_tier_flag_list(v_bm,      2, "BLACKMARKET_TIERS");

    if (saw_contra) {
        if (!v_contra_raw[0]) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: CONTRABAND_TIER is empty in '%s' — keeping default %d",
                     path, g_loot_policy.contraband_tier);
        } else if (v_contra < 1 || v_contra > g_loot_policy.tier_count) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: CONTRABAND_TIER=%s out of range 1..%d — keeping default %d",
                     v_contra_raw, g_loot_policy.tier_count, g_loot_policy.contraband_tier);
        } else {
            g_loot_policy.contraband_tier = v_contra;
        }
    }
    if (saw_btcmin) {
        if (!v_btcmin_raw[0]) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: BITCOIN_SPAWN_MIN is empty in '%s' — keeping default %d",
                     path, g_loot_policy.bitcoin_spawn_min_tier);
        } else if (v_btcmin < 0 || v_btcmin > g_loot_policy.tier_count) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: BITCOIN_SPAWN_MIN=%s invalid (0..%d, 0=off) — keeping default %d",
                     v_btcmin_raw, g_loot_policy.tier_count, g_loot_policy.bitcoin_spawn_min_tier);
        } else {
            g_loot_policy.bitcoin_spawn_min_tier = v_btcmin;
        }
    }
    if (saw_btcmax) {
        if (!v_btcmax_raw[0]) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: BITCOIN_SPAWN_MAX is empty in '%s' — keeping default %d",
                     path, g_loot_policy.bitcoin_spawn_max_tier);
        } else if (v_btcmax < 0 || v_btcmax > g_loot_policy.tier_count) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: BITCOIN_SPAWN_MAX=%s invalid (0..%d, 0=off) — keeping default %d",
                     v_btcmax_raw, g_loot_policy.tier_count, g_loot_policy.bitcoin_spawn_max_tier);
        } else {
            g_loot_policy.bitcoin_spawn_max_tier = v_btcmax;
        }
    }
    if (g_loot_policy.bitcoin_spawn_min_tier > 0 && g_loot_policy.bitcoin_spawn_max_tier > 0 &&
        g_loot_policy.bitcoin_spawn_min_tier > g_loot_policy.bitcoin_spawn_max_tier) {
        util_log(SEVERITY_ERROR,
                 "loot_policy: BITCOIN_SPAWN_MIN (%d) > BITCOIN_SPAWN_MAX (%d) — range left as-is (may disable spawn)",
                 g_loot_policy.bitcoin_spawn_min_tier, g_loot_policy.bitcoin_spawn_max_tier);
    }

    /* Distribution target lists (optional) */
    {
        char toks[LP_MAX_TIERS][LP_MAX_NAME_LEN];
        int n, i;
        if (saw_nominal && !v_nominal[0]) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: NOMINAL_TARGETS is empty in '%s' — distribution list ignored",
                     path);
        } else if (v_nominal[0]) {
            n = lp_split(v_nominal, toks, LP_MAX_TIERS);
            if (n <= 0) {
                util_log(SEVERITY_ERROR,
                         "loot_policy: NOMINAL_TARGETS malformed in '%s' — ignored", path);
            } else {
                for (i = 0; i < n && i < g_loot_policy.tier_count; i++)
                    g_loot_policy.tiers[i].nominal_target = atoi(toks[i]);
                g_loot_policy.has_distribution_targets = true;
            }
        }
        if (saw_min && !v_min[0]) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: MIN_TARGETS is empty in '%s' — distribution list ignored",
                     path);
        } else if (v_min[0]) {
            n = lp_split(v_min, toks, LP_MAX_TIERS);
            if (n <= 0) {
                util_log(SEVERITY_ERROR,
                         "loot_policy: MIN_TARGETS malformed in '%s' — ignored", path);
            } else {
                for (i = 0; i < n && i < g_loot_policy.tier_count; i++)
                    g_loot_policy.tiers[i].min_target = atoi(toks[i]);
                g_loot_policy.has_distribution_targets = true;
            }
        }
        if (saw_life && !v_life[0]) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: LIFETIME_TARGETS is empty in '%s' — distribution list ignored",
                     path);
        } else if (v_life[0]) {
            n = lp_split(v_life, toks, LP_MAX_TIERS);
            if (n <= 0) {
                util_log(SEVERITY_ERROR,
                         "loot_policy: LIFETIME_TARGETS malformed in '%s' — ignored", path);
            } else {
                for (i = 0; i < n && i < g_loot_policy.tier_count; i++)
                    g_loot_policy.tiers[i].lifetime_target = atoi(toks[i]);
                g_loot_policy.has_distribution_targets = true;
            }
        }
        if (saw_restock && !v_restock[0]) {
            util_log(SEVERITY_ERROR,
                     "loot_policy: RESTOCK_TARGETS is empty in '%s' — distribution list ignored",
                     path);
        } else if (v_restock[0]) {
            n = lp_split(v_restock, toks, LP_MAX_TIERS);
            if (n <= 0) {
                util_log(SEVERITY_ERROR,
                         "loot_policy: RESTOCK_TARGETS malformed in '%s' — ignored", path);
            } else {
                for (i = 0; i < n && i < g_loot_policy.tier_count; i++)
                    g_loot_policy.tiers[i].restock_target = atoi(toks[i]);
                g_loot_policy.has_distribution_targets = true;
            }
        }
    }

    if (v_black[0])
        g_loot_policy.blacklist_count =
            lp_split(v_black, g_loot_policy.blacklist, LP_MAX_BLACKLIST);

    free(v_names);
    free(v_black);
    g_loot_policy.loaded = true;
    return true;
}

void lp_apply_distribution(int tier, int *nominal, int *min_val, int *lifetime, int *restock) {
    LpTier *tr;
    if (tier < 1 || tier > g_loot_policy.tier_count) return;
    tr = &g_loot_policy.tiers[tier - 1];

    if (!tr->spawns) {
        if (nominal) *nominal = 0;
        if (min_val) *min_val = 0;
        if (lifetime && tr->lifetime_target > 0) *lifetime = tr->lifetime_target;
        if (restock && tr->restock_target >= 0) *restock = tr->restock_target;
        return;
    }

    if (!g_loot_policy.has_distribution_targets) return;

    /* Soft blend: pull toward target without erasing item-specific rarity fully.
     * new = (2*target + old) / 3  when old > 0; else use target. */
    if (nominal && tr->nominal_target >= 0) {
        if (*nominal <= 0) *nominal = tr->nominal_target;
        else *nominal = (2 * tr->nominal_target + *nominal) / 3;
    }
    if (min_val && tr->min_target >= 0) {
        if (*min_val <= 0) *min_val = tr->min_target;
        else *min_val = (2 * tr->min_target + *min_val) / 3;
        if (nominal && *min_val > *nominal) *min_val = (*nominal > 0) ? (*nominal * 2 / 3) : 0;
    }
    if (lifetime && tr->lifetime_target > 0) {
        if (*lifetime <= 0) *lifetime = tr->lifetime_target;
        else *lifetime = (2 * tr->lifetime_target + *lifetime) / 3;
    }
    if (restock && tr->restock_target >= 0) {
        *restock = tr->restock_target;
    }
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

    /* Preserve distribution soft-targets on save so UI/CLI round-trip does not drop them. */
    {
        bool write_dist = g_loot_policy.has_distribution_targets;
        if (!write_dist) {
            for (int i = 0; i < g_loot_policy.tier_count; i++) {
                if (g_loot_policy.tiers[i].nominal_target >= 0 ||
                    g_loot_policy.tiers[i].min_target >= 0 ||
                    g_loot_policy.tiers[i].lifetime_target >= 0 ||
                    g_loot_policy.tiers[i].restock_target >= 0) {
                    write_dist = true;
                    break;
                }
            }
        }
        if (write_dist) {
            fprintf(f, "# Distribution soft-targets (comma lists align with TIER_NAMES order).\n");
            fprintf(f, "NOMINAL_TARGETS=");
            for (int i = 0; i < g_loot_policy.tier_count; i++)
                fprintf(f, "%s%d", (i ? "," : ""),
                        g_loot_policy.tiers[i].nominal_target >= 0
                            ? g_loot_policy.tiers[i].nominal_target : 0);
            fprintf(f, "\n");
            fprintf(f, "MIN_TARGETS=");
            for (int i = 0; i < g_loot_policy.tier_count; i++)
                fprintf(f, "%s%d", (i ? "," : ""),
                        g_loot_policy.tiers[i].min_target >= 0
                            ? g_loot_policy.tiers[i].min_target : 0);
            fprintf(f, "\n");
            fprintf(f, "LIFETIME_TARGETS=");
            for (int i = 0; i < g_loot_policy.tier_count; i++)
                fprintf(f, "%s%d", (i ? "," : ""),
                        g_loot_policy.tiers[i].lifetime_target >= 0
                            ? g_loot_policy.tiers[i].lifetime_target : 0);
            fprintf(f, "\n");
            fprintf(f, "RESTOCK_TARGETS=");
            for (int i = 0; i < g_loot_policy.tier_count; i++)
                fprintf(f, "%s%d", (i ? "," : ""),
                        g_loot_policy.tiers[i].restock_target >= 0
                            ? g_loot_policy.tiers[i].restock_target : 0);
            fprintf(f, "\n\n");
        }
    }

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
            g_loot_policy.tiers[i].spawns           = old[i].spawns;
            g_loot_policy.tiers[i].tradeable        = old[i].tradeable;
            g_loot_policy.tiers[i].black_market     = old[i].black_market;
            /* Preserve distribution soft-targets when renaming same-index tiers */
            g_loot_policy.tiers[i].nominal_target   = old[i].nominal_target;
            g_loot_policy.tiers[i].min_target       = old[i].min_target;
            g_loot_policy.tiers[i].lifetime_target  = old[i].lifetime_target;
            g_loot_policy.tiers[i].restock_target   = old[i].restock_target;
        } else {
            g_loot_policy.tiers[i].spawns           = true;
            g_loot_policy.tiers[i].tradeable        = true;
            g_loot_policy.tiers[i].black_market     = false;
            g_loot_policy.tiers[i].nominal_target   = -1;
            g_loot_policy.tiers[i].min_target       = -1;
            g_loot_policy.tiers[i].lifetime_target  = -1;
            g_loot_policy.tiers[i].restock_target   = -1;
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
