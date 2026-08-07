#ifndef CONTAINER_POLICY_H
#define CONTAINER_POLICY_H

// ============================================================================
// CONTAINER POLICY — mod-container defaults by tier (+ optional per-mod)
// ============================================================================
// First-cut schema + load hooks for roadmap #1. Runtime source of truth:
//   config/container_policy.ini
// Loaded from util_init_context; missing file soft-fails with defaults.
//
// Full cargo apply/export (types/spawnables/mod CE) is intentionally NOT
// implemented here — see TODO on container_policy_apply_cargo().
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

#define CP_MAX_TIERS          16
#define CP_MAX_MOD_OVERRIDES  64
#define CP_MAX_MOD_ID         96
#define CP_CONFIG_PATH        "config/container_policy.ini"

/* -1 means unset: query helpers fall back to default_* scalars. */
typedef struct {
    char mod_id[CP_MAX_MOD_ID];
    int  cargo_max;       /* -1 = unset */
    int  attachment_max;  /* -1 = unset */
    int  items_max;       /* -1 = unset */
    int  tier_min;        /* 0 = no minimum; else 1-based floor */
} CpModOverride;

typedef struct {
    int  tier_count; /* active slots in per-tier arrays (usually 12) */
    int  cargo_max_by_tier[CP_MAX_TIERS];
    int  attachment_max_by_tier[CP_MAX_TIERS];
    int  items_max_by_tier[CP_MAX_TIERS];

    int  default_cargo_max;
    int  default_attachment_max;
    int  default_items_max;
    bool enable_tier_scaling;

    CpModOverride mods[CP_MAX_MOD_OVERRIDES];
    int  mod_count;

    bool loaded;    /* struct is valid (defaults and/or file) */
    bool from_file; /* true if INI was opened and parsed */
} ContainerPolicy;

extern ContainerPolicy g_container_policy;

/* Seed g_container_policy with built-in progressive defaults (12 tiers). */
void container_policy_init_defaults(void);

/* Load from `path` (flat KEY=value INI). Starts from defaults, overrides with
 * file contents. Soft-fails: missing/unreadable logs a warning and keeps
 * defaults — never aborts. Returns false if file missing/unreadable. */
bool container_policy_load(const char *path);

/* --- queries (tier args are 1-based) --- */
int  cp_cargo_max_for_tier(int tier);
int  cp_attachment_max_for_tier(int tier);
int  cp_items_max_for_tier(int tier);
const CpModOverride *cp_find_mod(const char *mod_id);

/* TODO (out of scope this ship): apply cargo/attachment caps into economy
 * outputs / spawnable types / mod container XML. Load + queries only for now. */
/* void container_policy_apply_cargo(void); */

#ifdef __cplusplus
}
#endif

#endif /* CONTAINER_POLICY_H */
