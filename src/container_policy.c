#include "container_policy.h"
#include "auditor.h" /* util_log — soft config errors must not abort */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Global policy — populated by container_policy_load() from util_init_context. */
ContainerPolicy g_container_policy = {0};

/* ---------------------------------------------------------------------------
 * small helpers (module depends on libc + util_log only)
 * --------------------------------------------------------------------------- */

static void cp_trim(char *s) {
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static bool cp_ieq(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* Parse comma-separated ints into dst[0..max-1]. Unset slots kept as-is when
 * token is empty or "-1". Returns number of tokens seen. */
static int cp_parse_int_list(const char *src, int *dst, int max) {
    if (!src || !dst || max <= 0) return 0;
    int n = 0;
    const char *p = src;
    while (*p && n < max) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        /* trim token */
        while (start < p && isspace((unsigned char)*start)) start++;
        const char *end = p;
        while (end > start && isspace((unsigned char)end[-1])) end--;
        if (end > start) {
            char buf[32];
            int len = (int)(end - start);
            if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
            memcpy(buf, start, (size_t)len);
            buf[len] = '\0';
            if (!(buf[0] == '-' && buf[1] == '1' && buf[2] == '\0')) {
                /* only digits / optional leading + or - */
                char *ep = NULL;
                long v = strtol(buf, &ep, 10);
                if (ep && ep != buf && *ep == '\0') {
                    dst[n] = (int)v;
                }
                /* garbage token: leave dst[n] unchanged */
            }
            /* else -1: leave default */
        }
        n++;
        if (*p == ',') p++;
    }
    return n;
}

/* Find or create a mod override slot by id (case-insensitive). */
static CpModOverride *cp_mod_slot(const char *mod_id) {
    if (!mod_id || !mod_id[0]) return NULL;
    for (int i = 0; i < g_container_policy.mod_count; i++) {
        if (cp_ieq(g_container_policy.mods[i].mod_id, mod_id))
            return &g_container_policy.mods[i];
    }
    if (g_container_policy.mod_count >= CP_MAX_MOD_OVERRIDES) {
        util_log(SEVERITY_WARNING,
                 "container_policy: mod override capacity (%d) full — ignored '%s'",
                 CP_MAX_MOD_OVERRIDES, mod_id);
        return NULL;
    }
    CpModOverride *m = &g_container_policy.mods[g_container_policy.mod_count++];
    memset(m, 0, sizeof(*m));
    strncpy(m->mod_id, mod_id, CP_MAX_MOD_ID - 1);
    m->mod_id[CP_MAX_MOD_ID - 1] = '\0';
    m->cargo_max = -1;
    m->attachment_max = -1;
    m->items_max = -1;
    m->tier_min = 0;
    return m;
}

/* Parse MOD_OVERRIDE.<mod_id>.<FIELD>=value */
static void cp_handle_mod_override_key(const char *key, const char *val) {
    /* key already matched prefix case-insensitively by caller */
    const char *rest = key + (int)strlen("MOD_OVERRIDE.");
    if (!*rest) return;

    char mod_id[CP_MAX_MOD_ID];
    const char *dot = strrchr(rest, '.');
    if (!dot || dot == rest || !dot[1]) return;

    size_t id_len = (size_t)(dot - rest);
    if (id_len >= sizeof(mod_id)) id_len = sizeof(mod_id) - 1;
    memcpy(mod_id, rest, id_len);
    mod_id[id_len] = '\0';

    const char *field = dot + 1;
    CpModOverride *m = cp_mod_slot(mod_id);
    if (!m) return;

    if (!val || !val[0]) return;
    int v = atoi(val);

    if (cp_ieq(field, "CARGO_MAX")) {
        m->cargo_max = v;
    } else if (cp_ieq(field, "ATTACHMENT_MAX")) {
        m->attachment_max = v;
    } else if (cp_ieq(field, "ITEMS_MAX")) {
        m->items_max = v;
    } else if (cp_ieq(field, "TIER_MIN")) {
        m->tier_min = v;
    }
    /* unknown field: ignore for forward-compat */
}

/* ---------------------------------------------------------------------------
 * defaults — progressive 12-tier map (matches loot_policy tier count)
 * --------------------------------------------------------------------------- */
void container_policy_init_defaults(void) {
    memset(&g_container_policy, 0, sizeof(g_container_policy));

    /* Soft progressive caps: denser/simple early; roomier high tiers; BM/Contra 0 */
    static const int cargo[] = { 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 0, 0 };
    static const int attach[] = { 2, 2,  3,  3,  4,  4,  5,  6,  6,  8, 0, 0 };
    static const int items[]  = { 8, 10, 12, 14, 16, 18, 20, 22, 24, 28, 0, 0 };
    int n = 12;

    g_container_policy.tier_count = n;
    for (int i = 0; i < n; i++) {
        g_container_policy.cargo_max_by_tier[i] = cargo[i];
        g_container_policy.attachment_max_by_tier[i] = attach[i];
        g_container_policy.items_max_by_tier[i] = items[i];
    }
    for (int i = n; i < CP_MAX_TIERS; i++) {
        g_container_policy.cargo_max_by_tier[i] = -1;
        g_container_policy.attachment_max_by_tier[i] = -1;
        g_container_policy.items_max_by_tier[i] = -1;
    }

    g_container_policy.default_cargo_max = 12;
    g_container_policy.default_attachment_max = 4;
    g_container_policy.default_items_max = 20;
    g_container_policy.enable_tier_scaling = true;
    g_container_policy.mod_count = 0;
    g_container_policy.loaded = true;
    g_container_policy.from_file = false;
}

/* ---------------------------------------------------------------------------
 * load
 * --------------------------------------------------------------------------- */
bool container_policy_load(const char *path) {
    container_policy_init_defaults();

    if (!path || !path[0]) {
        util_log(SEVERITY_WARNING,
                 "container_policy: no path provided — keeping built-in defaults");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Soft fail: optional first-cut file; do not abort pipeline. */
        util_log(SEVERITY_WARNING,
                 "container_policy: missing or unreadable '%s' — keeping built-in defaults",
                 path);
        return false;
    }

    char line[2048];
    int overrides = 0;

    while (fgets(line, sizeof(line), f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char key[192];
        strncpy(key, line, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
        cp_trim(key);
        char *val = eq + 1;
        cp_trim(val);
        if (!key[0]) continue;

        if (cp_ieq(key, "DEFAULT_CARGO_MAX")) {
            if (val[0]) g_container_policy.default_cargo_max = atoi(val);
        } else if (cp_ieq(key, "DEFAULT_ATTACHMENT_MAX")) {
            if (val[0]) g_container_policy.default_attachment_max = atoi(val);
        } else if (cp_ieq(key, "DEFAULT_ITEMS_MAX")) {
            if (val[0]) g_container_policy.default_items_max = atoi(val);
        } else if (cp_ieq(key, "ENABLE_TIER_SCALING")) {
            if (val[0]) {
                g_container_policy.enable_tier_scaling =
                    !(val[0] == '0' && val[1] == '\0') &&
                    !cp_ieq(val, "false") && !cp_ieq(val, "no") && !cp_ieq(val, "off");
            }
        } else if (cp_ieq(key, "CARGO_MAX_BY_TIER")) {
            int seen = cp_parse_int_list(val, g_container_policy.cargo_max_by_tier, CP_MAX_TIERS);
            if (seen > g_container_policy.tier_count) g_container_policy.tier_count = seen;
        } else if (cp_ieq(key, "ATTACHMENT_MAX_BY_TIER")) {
            int seen = cp_parse_int_list(val, g_container_policy.attachment_max_by_tier, CP_MAX_TIERS);
            if (seen > g_container_policy.tier_count) g_container_policy.tier_count = seen;
        } else if (cp_ieq(key, "ITEMS_MAX_BY_TIER")) {
            int seen = cp_parse_int_list(val, g_container_policy.items_max_by_tier, CP_MAX_TIERS);
            if (seen > g_container_policy.tier_count) g_container_policy.tier_count = seen;
        } else {
            /* MOD_OVERRIDE.<id>.FIELD — prefix match case-insensitive */
            const char *pfx = "MOD_OVERRIDE.";
            size_t plen = strlen(pfx);
            bool match = true;
            for (size_t i = 0; i < plen; i++) {
                char a = (char)tolower((unsigned char)key[i]);
                char b = (char)tolower((unsigned char)pfx[i]);
                if (a != b) { match = false; break; }
            }
            if (match && key[plen]) {
                cp_handle_mod_override_key(key, val);
                overrides++;
            }
        }
    }
    fclose(f);

    if (g_container_policy.tier_count < 1)
        g_container_policy.tier_count = 12;
    if (g_container_policy.tier_count > CP_MAX_TIERS)
        g_container_policy.tier_count = CP_MAX_TIERS;

    g_container_policy.loaded = true;
    g_container_policy.from_file = true;

    util_log(SEVERITY_INFO,
             "container_policy: loaded '%s' (tiers=%d mods=%d scaling=%s) — apply/export TODO",
             path,
             g_container_policy.tier_count,
             g_container_policy.mod_count,
             g_container_policy.enable_tier_scaling ? "on" : "off");
    (void)overrides;
    return true;
}

/* ---------------------------------------------------------------------------
 * queries
 * --------------------------------------------------------------------------- */

static int cp_tier_value(const int *arr, int tier, int fallback) {
    if (!arr || tier < 1) return fallback;
    int idx = tier - 1;
    if (idx >= CP_MAX_TIERS) return fallback;
    if (idx >= g_container_policy.tier_count) {
        /* beyond known list: still allow if array slot was set */
    }
    int v = arr[idx];
    if (v < 0) return fallback;
    return v;
}

int cp_cargo_max_for_tier(int tier) {
    return cp_tier_value(g_container_policy.cargo_max_by_tier, tier,
                         g_container_policy.default_cargo_max);
}

int cp_attachment_max_for_tier(int tier) {
    return cp_tier_value(g_container_policy.attachment_max_by_tier, tier,
                         g_container_policy.default_attachment_max);
}

int cp_items_max_for_tier(int tier) {
    return cp_tier_value(g_container_policy.items_max_by_tier, tier,
                         g_container_policy.default_items_max);
}

const CpModOverride *cp_find_mod(const char *mod_id) {
    if (!mod_id || !mod_id[0]) return NULL;
    for (int i = 0; i < g_container_policy.mod_count; i++) {
        if (cp_ieq(g_container_policy.mods[i].mod_id, mod_id))
            return &g_container_policy.mods[i];
    }
    return NULL;
}

/* TODO: container_policy_apply_cargo — walk items / spawnables and write
 * cargo/attachment caps from g_container_policy. Not implemented this ship. */
