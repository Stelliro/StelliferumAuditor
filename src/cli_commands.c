/**
 * Simple headless command surface + config recipes.
 *
 * Console verbs (no -- flags required):
 *   help | list [path] | pull [recipe|all] | pipeline | push | restore | run <recipe>
 *
 * Optional recipes: config/commands.ini  (see commands.ini.example)
 */

#include "cli_commands.h"
#include "auditor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CMD_INI_PATH     "config/commands.ini"
#define CMD_MAX_RECIPES  64
#define CMD_MAX_NAME     64
#define CMD_MAX_VAL      512
#define CMD_MAX_STEPS    16

typedef struct {
    char remote_root[256];
    char remote_types[256];
    char remote_spawnable[256];
    char remote_events[256];
    char remote_globals[256];
    char remote_trader[256];
    char remote_sfl[256];
    char local_root[256];
} CmdPaths;

typedef struct {
    char name[CMD_MAX_NAME];
    char action[32];       /* download|upload|list|pipeline|push|restore|steps */
    char remote[CMD_MAX_VAL];
    char local[CMD_MAX_VAL];
    char steps[CMD_MAX_VAL]; /* comma list for action=steps */
} CmdRecipe;

static CmdRecipe g_recipes[CMD_MAX_RECIPES];
static int g_recipe_count = 0;
static int g_recipes_loaded = 0;

/* ------------------------------------------------------------------------- */

static void paths_load(CmdPaths *p) {
    memset(p, 0, sizeof(*p));
    strncpy(p->remote_root, "/", sizeof(p->remote_root) - 1);
    strncpy(p->local_root, "downloaded_mods", sizeof(p->local_root) - 1);
    strncpy(p->remote_types, "mpmissions/dayzOffline.chernarusplus/db/types.xml",
            sizeof(p->remote_types) - 1);
    strncpy(p->remote_spawnable, "mpmissions/dayzOffline.chernarusplus/cfgspawnabletypes.xml",
            sizeof(p->remote_spawnable) - 1);
    strncpy(p->remote_events, "mpmissions/dayzOffline.chernarusplus/db/events.xml",
            sizeof(p->remote_events) - 1);
    strncpy(p->remote_globals, "mpmissions/dayzOffline.chernarusplus/db/globals.xml",
            sizeof(p->remote_globals) - 1);
    strncpy(p->remote_trader, "profiles/Trader/TraderConfig.txt", sizeof(p->remote_trader) - 1);
    strncpy(p->remote_sfl, "SearchForLoot/SearchForLoot.json", sizeof(p->remote_sfl) - 1);

    /* Soft config validation (missing file / REMOTE_ROOT / LOCAL_ROOT) — never abort. */
    util_soft_validate_server_paths("config/server_paths.ini");

    {
        bool has_remote = util_read_ini_value("config/server_paths.ini", "REMOTE_ROOT",
                                              p->remote_root, sizeof(p->remote_root));
        bool has_local = util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT",
                                             p->local_root, sizeof(p->local_root));
        if (has_remote) {
            util_trim(p->remote_root);
            if (!p->remote_root[0])
                strncpy(p->remote_root, "/", sizeof(p->remote_root) - 1);
        }
        if (has_local) {
            util_trim(p->local_root);
            if (!p->local_root[0])
                strncpy(p->local_root, "downloaded_mods", sizeof(p->local_root) - 1);
        } else {
            strncpy(p->local_root, "downloaded_mods", sizeof(p->local_root) - 1);
        }
    }
    util_read_ini_value("config/server_paths.ini", "REMOTE_TYPES", p->remote_types, sizeof(p->remote_types));
    util_read_ini_value("config/server_paths.ini", "REMOTE_SPAWNABLE", p->remote_spawnable, sizeof(p->remote_spawnable));
    util_read_ini_value("config/server_paths.ini", "REMOTE_EVENTS", p->remote_events, sizeof(p->remote_events));
    util_read_ini_value("config/server_paths.ini", "REMOTE_GLOBALS", p->remote_globals, sizeof(p->remote_globals));
    util_read_ini_value("config/server_paths.ini", "REMOTE_TRADER", p->remote_trader, sizeof(p->remote_trader));
    util_read_ini_value("config/server_paths.ini", "REMOTE_SFL", p->remote_sfl, sizeof(p->remote_sfl));
}

static void str_trim_inplace(char *s) {
    char *start, *end;
    if (!s) return;
    start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
}

/** Expand ${VAR} placeholders from CmdPaths into out. */
static void expand_vars(const CmdPaths *p, const char *in, char *out, size_t out_len) {
    size_t oi = 0;
    const char *s = in ? in : "";
    if (!out || out_len == 0) return;
    out[0] = '\0';

    while (*s && oi + 1 < out_len) {
        if (s[0] == '$' && s[1] == '{') {
            const char *end = strchr(s + 2, '}');
            char key[64];
            size_t klen;
            const char *val = NULL;
            if (!end) { out[oi++] = *s++; continue; }
            klen = (size_t)(end - (s + 2));
            if (klen >= sizeof(key)) klen = sizeof(key) - 1;
            memcpy(key, s + 2, klen);
            key[klen] = '\0';

            if (strcmp(key, "REMOTE_ROOT") == 0) val = p->remote_root;
            else if (strcmp(key, "LOCAL_ROOT") == 0) val = p->local_root;
            else if (strcmp(key, "REMOTE_TYPES") == 0) val = p->remote_types;
            else if (strcmp(key, "REMOTE_SPAWNABLE") == 0) val = p->remote_spawnable;
            else if (strcmp(key, "REMOTE_EVENTS") == 0) val = p->remote_events;
            else if (strcmp(key, "REMOTE_GLOBALS") == 0) val = p->remote_globals;
            else if (strcmp(key, "REMOTE_TRADER") == 0) val = p->remote_trader;
            else if (strcmp(key, "REMOTE_SFL") == 0) val = p->remote_sfl;

            if (val) {
                size_t vlen = strlen(val);
                if (oi + vlen >= out_len) vlen = out_len - oi - 1;
                memcpy(out + oi, val, vlen);
                oi += vlen;
                out[oi] = '\0';
            }
            s = end + 1;
        } else {
            out[oi++] = *s++;
            out[oi] = '\0';
        }
    }

    /* Collapse accidental double slashes (keep leading // if any — rare). */
    {
        char *w = out;
        char *r = out;
        while (*r) {
            *w++ = *r;
            if (*r == '/' && r[1] == '/') {
                while (r[1] == '/') r++;
            }
            r++;
        }
        *w = '\0';
    }
}

static void join_remote(char *out, size_t out_len, const char *root, const char *rel) {
    size_t rlen;
    if (!out || out_len == 0) return;
    if (!root) root = "";
    if (!rel) rel = "";
    while (*rel == '/') rel++;
    rlen = strlen(root);
    if (rlen > 0 && root[rlen - 1] == '/')
        snprintf(out, out_len, "%s%s", root, rel);
    else if (rlen > 0)
        snprintf(out, out_len, "%s/%s", root, rel);
    else
        snprintf(out, out_len, "/%s", rel);
}

/* ------------------------------------------------------------------------- */
/* Recipe INI loader                                                          */
/* ------------------------------------------------------------------------- */

static void recipes_load(void) {
    FILE *f;
    char line[1024];
    CmdRecipe *cur = NULL;

    g_recipe_count = 0;
    g_recipes_loaded = 1;
    memset(g_recipes, 0, sizeof(g_recipes));

    f = fopen(CMD_INI_PATH, "r");
    if (!f) return; /* optional */

    while (fgets(line, sizeof(line), f)) {
        char *hash;
        size_t len;
        str_trim_inplace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        /* strip inline comments */
        hash = strchr(line, '#');
        if (hash) { *hash = '\0'; str_trim_inplace(line); }
        if (line[0] == '\0') continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end || g_recipe_count >= CMD_MAX_RECIPES) continue;
            *end = '\0';
            cur = &g_recipes[g_recipe_count++];
            memset(cur, 0, sizeof(*cur));
            strncpy(cur->name, line + 1, CMD_MAX_NAME - 1);
            str_trim_inplace(cur->name);
            continue;
        }

        if (!cur) continue;
        {
            char *eq = strchr(line, '=');
            char key[64];
            char *val;
            if (!eq) continue;
            *eq = '\0';
            strncpy(key, line, sizeof(key) - 1);
            key[sizeof(key) - 1] = '\0';
            str_trim_inplace(key);
            val = eq + 1;
            str_trim_inplace(val);

            if (util_strcasecmp(key, "action") == 0 || util_strcasecmp(key, "cmd") == 0)
                strncpy(cur->action, val, sizeof(cur->action) - 1);
            else if (util_strcasecmp(key, "remote") == 0)
                strncpy(cur->remote, val, sizeof(cur->remote) - 1);
            else if (util_strcasecmp(key, "local") == 0)
                strncpy(cur->local, val, sizeof(cur->local) - 1);
            else if (util_strcasecmp(key, "steps") == 0)
                strncpy(cur->steps, val, sizeof(cur->steps) - 1);
        }
    }
    fclose(f);
    util_log(SEVERITY_INFO, "CLI: loaded %d recipe(s) from %s", g_recipe_count, CMD_INI_PATH);
}

static const CmdRecipe *recipe_find(const char *name) {
    int i;
    if (!name || !*name) return NULL;
    if (!g_recipes_loaded) recipes_load();
    for (i = 0; i < g_recipe_count; i++) {
        if (util_strcasecmp(g_recipes[i].name, name) == 0)
            return &g_recipes[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Core actions                                                               */
/* ------------------------------------------------------------------------- */

static int load_creds(char *host, size_t host_len, int *port, char *user, size_t user_len,
                      char *pass, size_t pass_len) {
    char port_buf[16] = {0};
    if (!util_read_ini_value("config/ftp.ini", "HOST", host, host_len)) return 0;
    if (!util_read_ini_value("config/ftp.ini", "PORT", port_buf, sizeof(port_buf))) return 0;
    if (!util_read_ini_value("config/ftp.ini", "USER", user, user_len)) return 0;
    if (!util_read_ini_value("config/ftp.ini", "PASS", pass, pass_len)) return 0;
    *port = atoi(port_buf);
    return 1;
}

static int action_list(const CmdPaths *paths, const char *remote_opt) {
    char host[128] = {0}, user[128] = {0}, pass[128] = {0};
    int port = 0;
    char remote[MAX_PATH_LEN];
    RemoteFileBrowser *browser;
    int e;

    if (!load_creds(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "list: missing config/ftp.ini");
        return 1;
    }
#ifdef STELLI_USE_LIBCURL
    if (ftp_native_backend_available())
        ftp_native_reload_security_config();
#endif
    if (remote_opt && remote_opt[0])
        strncpy(remote, remote_opt, sizeof(remote) - 1);
    else
        strncpy(remote, paths->remote_root, sizeof(remote) - 1);
    remote[sizeof(remote) - 1] = '\0';

    browser = (RemoteFileBrowser *)malloc(sizeof(RemoteFileBrowser));
    if (!browser) return 1;
    memset(browser, 0, sizeof(*browser));
    util_log(SEVERITY_INFO, "list: %s", remote);
    if (!ftp_list_directory(host, port, user, pass, remote, browser)) {
        util_log(SEVERITY_ERROR, "list failed: %s",
                 browser->error[0] ? browser->error : "unknown");
        free(browser);
        return 1;
    }
    printf("Remote: %s  (%d entries)\n",
           browser->current_path[0] ? browser->current_path : remote, browser->count);
    for (e = 0; e < browser->count; e++) {
        const RemoteFileEntry *ent = &browser->entries[e];
        printf("%s  %12lld  %s\n",
               ent->is_directory ? "dir " : "file",
               (long long)ent->size, ent->name);
    }
    free(browser);
    return 0;
}

static int action_download(const char *remote, const char *local) {
    char host[128] = {0}, user[128] = {0}, pass[128] = {0};
    int port = 0;
    if (!remote || !remote[0] || !local || !local[0]) {
        util_log(SEVERITY_ERROR, "download: need remote and local paths");
        return 1;
    }
    if (!load_creds(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "download: missing config/ftp.ini");
        return 1;
    }
#ifdef STELLI_USE_LIBCURL
    if (ftp_native_backend_available())
        ftp_native_reload_security_config();
#endif
    util_log(SEVERITY_INFO, "download: %s -> %s", remote, local);
    /* Heuristic: trailing slash or no extension => recursive dir */
    {
        size_t rlen = strlen(remote);
        int as_file = 1;
        if (rlen > 0 && (remote[rlen - 1] == '/' || remote[rlen - 1] == '\\'))
            as_file = 0;
        else {
            const char *base = strrchr(remote, '/');
            if (!base) base = remote; else base++;
            if (!strchr(base, '.')) as_file = 0;
        }
        if (as_file) {
            char parent[MAX_PATH_LEN];
            char *slash;
            strncpy(parent, local, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            slash = strrchr(parent, '/');
#ifdef _WIN32
            {
                char *b = strrchr(parent, '\\');
                if (b && (!slash || b > slash)) slash = b;
            }
#endif
            if (slash && slash != parent) {
                *slash = '\0';
                util_ensure_directory(parent);
            }
            if (!ftp_download_file(host, port, user, pass, remote, local)) {
                util_log(SEVERITY_ERROR, "download file failed");
                return 1;
            }
        } else {
            util_ensure_directory(local);
            if (!ftp_download_recursive(host, port, user, pass, remote, local)) {
                util_log(SEVERITY_ERROR, "download recursive failed");
                return 1;
            }
        }
    }
    util_log(SEVERITY_INFO, "download complete");
    return 0;
}

static int action_upload(const char *local, const char *remote) {
    char host[128] = {0}, user[128] = {0}, pass[128] = {0};
    int port = 0;
    if (!remote || !remote[0] || !local || !local[0]) {
        util_log(SEVERITY_ERROR, "upload: need remote and local paths");
        return 1;
    }
    if (!load_creds(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "upload: missing config/ftp.ini");
        return 1;
    }
#ifdef STELLI_USE_LIBCURL
    if (ftp_native_backend_available())
        ftp_native_reload_security_config();
#endif
    util_log(SEVERITY_INFO, "upload: %s -> %s", local, remote);
    if (!util_file_exists(local)) {
        util_log(SEVERITY_ERROR, "upload: local path missing: %s", local);
        return 1;
    }
    if (!ftp_upload_file(host, port, user, pass, local, remote)) {
        util_log(SEVERITY_ERROR, "upload failed");
        return 1;
    }
    util_log(SEVERITY_INFO, "upload complete");
    return 0;
}

static int action_pipeline(void) {
    /* Re-enter via main-style regen: allocate context and run swarm */
    AuditorContext *ctx = (AuditorContext *)malloc(sizeof(AuditorContext));
    CmdPaths paths;
    int rc;
    if (!ctx) {
        util_log(SEVERITY_CRITICAL, "pipeline: out of memory");
        return 1;
    }
    util_init_context(ctx);
    paths_load(&paths);
    util_ensure_directory(paths.local_root);
    util_ensure_directory("output");
    util_log(SEVERITY_INFO, "pipeline: LOCAL REGEN from %s", paths.local_root);
    classname_map_init(ctx);
    if (!ctx->file_cache.loaded) file_cache_load(&ctx->file_cache);
    file_cache_prune(&ctx->file_cache);
    ctx->file_cache.count = 0;
    util_backup_and_clean_output();
    swarm_plan(ctx);
    swarm_execute(ctx);
    file_cache_save(&ctx->file_cache);
    util_log(SEVERITY_INFO, "pipeline: complete (%d issues)", ctx->issue_count);
    rc = 0;
    if (ctx->original_items) free(ctx->original_items);
    free(ctx);
    return rc;
}

/* Forward: push-economy is in main.c — call via re-exec argv style helper.
 * We duplicate a thin wrapper by calling system is bad. Instead declare
 * external symbols we need from auditor public FTP API and build push list
 * similar to main's run_ftp_push_economy — keep simple: shell out is not used.
 * For modularity, re-invoke built-in by mapping to existing process entry is hard.
 * We'll implement a compact push using ftp_upload_batch for key files. */

static int action_push(const CmdPaths *paths) {
    char host[128] = {0}, user[128] = {0}, pass[128] = {0};
    int port = 0;
    char mission[512];
    char mission_rel[256];
    char *db;
    char full[MAX_PATH_LEN];
#define PMAX 64
    char locals[PMAX][MAX_PATH_LEN];
    char remotes[PMAX][MAX_PATH_LEN];
    const char *lp[PMAX], *rp[PMAX];
    int n = 0, ok = 0, fail = 0, i;

    if (!load_creds(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "push: missing config/ftp.ini");
        return 1;
    }
#ifdef STELLI_USE_LIBCURL
    if (ftp_native_backend_available())
        ftp_native_reload_security_config();
#endif

    strncpy(mission_rel, paths->remote_types, sizeof(mission_rel) - 1);
    mission_rel[sizeof(mission_rel) - 1] = '\0';
    db = strstr(mission_rel, "/db/");
    if (db) *db = '\0';
    join_remote(mission, sizeof(mission), paths->remote_root, mission_rel);
    {
        size_t m = strlen(mission);
        if (m > 1 && mission[m - 1] == '/') mission[m - 1] = '\0';
    }

#define ADD(loc, rem) do { \
    if (n < PMAX && util_file_exists(loc)) { \
        strncpy(locals[n], loc, MAX_PATH_LEN - 1); locals[n][MAX_PATH_LEN-1]=0; \
        strncpy(remotes[n], rem, MAX_PATH_LEN - 1); remotes[n][MAX_PATH_LEN-1]=0; \
        n++; \
    } \
} while (0)

    join_remote(full, sizeof(full), paths->remote_root, paths->remote_types);
    ADD("output/types.xml", full);
    snprintf(full, sizeof(full), "%s/cfgspawnabletypes.xml", mission);
    ADD("output/cfgspawnabletypes.xml", full);
    snprintf(full, sizeof(full), "%s/cfgeconomycore.xml", mission);
    ADD("output/cfgeconomycore.xml", full);
    snprintf(full, sizeof(full), "%s/cfglimitsdefinitionuser.xml", mission);
    ADD("output/cfglimitsdefinitionuser.xml", full);
    snprintf(full, sizeof(full), "%s/cfgrandompresets.xml", mission);
    ADD("output/cfgrandompresets.xml", full);
    join_remote(full, sizeof(full), paths->remote_root, paths->remote_trader);
    ADD("output/TraderConfig.txt", full);
    join_remote(full, sizeof(full), paths->remote_root, paths->remote_sfl);
    ADD("output/SearchForLoot.json", full);
    snprintf(full, sizeof(full), "%s/db/types_infected.xml", mission);
    ADD("output/zombie_tiers/types_infected.xml", full);
    snprintf(full, sizeof(full), "%s/db/types_wildlife.xml", mission);
    ADD("output/zombie_tiers/types_wildlife.xml", full);
#undef ADD

    if (n == 0) {
        util_log(SEVERITY_ERROR, "push: no output files found — run 'pipeline' first");
        return 1;
    }
    for (i = 0; i < n; i++) { lp[i] = locals[i]; rp[i] = remotes[i]; }
    util_log(SEVERITY_INFO, "push: uploading %d core file(s) in one batch", n);
    if (!ftp_upload_batch(host, port, user, pass, lp, rp, n, &ok, &fail)) {
        util_log(SEVERITY_ERROR, "push failed (%d ok, %d fail)", ok, fail);
        return 1;
    }
    util_log(SEVERITY_INFO, "push complete: %d ok, %d fail", ok, fail);
    return fail > 0 ? 1 : 0;
#undef PMAX
}

static int action_restore(void) {
    /* Match main --restore-last-upload entry */
    char host[128] = {0}, user[128] = {0}, pass[128] = {0};
    int port = 0;
    RestorePointInfo points[MAX_RESTORE_POINTS];
    int count;

    if (!load_creds(host, sizeof(host), &port, user, sizeof(user), pass, sizeof(pass))) {
        util_log(SEVERITY_ERROR, "restore: missing config/ftp.ini");
        return 1;
    }
#ifdef STELLI_USE_LIBCURL
    if (ftp_native_backend_available())
        ftp_native_reload_security_config();
#endif
    count = ftp_list_restore_points(points, MAX_RESTORE_POINTS);
    if (count <= 0) {
        util_log(SEVERITY_ERROR, "restore: no restore points in backups/");
        return 1;
    }
    util_log(SEVERITY_INFO, "restore: using %s (%s)", points[0].label, points[0].manifest);
    if (!ftp_restore_from_manifest(host, port, user, pass, points[0].manifest)) {
        util_log(SEVERITY_ERROR, "restore failed");
        return 1;
    }
    util_log(SEVERITY_INFO, "restore complete");
    return 0;
}

static int action_pull_all(const CmdPaths *paths) {
    char remote[MAX_PATH_LEN], local[MAX_PATH_LEN];
    join_remote(remote, sizeof(remote), paths->remote_root, "");
    strncpy(local, paths->local_root, sizeof(local) - 1);
    local[sizeof(local) - 1] = '\0';
    /* Ensure trailing slash semantics for recursive */
    {
        size_t r = strlen(remote);
        if (r > 0 && remote[r - 1] != '/') {
            if (r + 1 < sizeof(remote)) { remote[r] = '/'; remote[r + 1] = '\0'; }
        }
    }
    return action_download(remote, local);
}

/* ------------------------------------------------------------------------- */
/* Recipe / verb execution                                                    */
/* ------------------------------------------------------------------------- */

static int run_recipe_by_name(const CmdPaths *paths, const char *name, int depth);

static int run_steps(const CmdPaths *paths, const char *steps_csv, int depth) {
    char buf[CMD_MAX_VAL];
    char *tok, *save = NULL;
    int rc = 0;
    if (!steps_csv || !*steps_csv) return 1;
    strncpy(buf, steps_csv, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (tok = buf; ; ) {
        char *comma = strchr(tok, ',');
        if (comma) *comma = '\0';
        str_trim_inplace(tok);
        if (*tok) {
            util_log(SEVERITY_INFO, "steps: -> %s", tok);
            rc = run_recipe_by_name(paths, tok, depth + 1);
            if (rc != 0) return rc;
        }
        if (!comma) break;
        tok = comma + 1;
    }
    (void)save;
    return 0;
}

static int run_recipe_by_name(const CmdPaths *paths, const char *name, int depth) {
    const CmdRecipe *r;
    char remote[CMD_MAX_VAL], local[CMD_MAX_VAL];

    if (depth > 8) {
        util_log(SEVERITY_ERROR, "recipe recursion too deep at '%s'", name);
        return 1;
    }
    if (!name || !*name) return 1;

    /* Built-in verbs */
    if (util_strcasecmp(name, "help") == 0) { cli_print_simple_help(); return 0; }
    if (util_strcasecmp(name, "list") == 0) return action_list(paths, NULL);
    if (util_strcasecmp(name, "pull") == 0) return action_pull_all(paths);
    if (util_strcasecmp(name, "pipeline") == 0 || util_strcasecmp(name, "regen") == 0)
        return action_pipeline();
    if (util_strcasecmp(name, "push") == 0) return action_push(paths);
    if (util_strcasecmp(name, "restore") == 0) return action_restore();

    r = recipe_find(name);
    if (!r) {
        util_log(SEVERITY_ERROR, "unknown command/recipe: '%s' (try 'help')", name);
        return 1;
    }

    expand_vars(paths, r->remote, remote, sizeof(remote));
    expand_vars(paths, r->local, local, sizeof(local));

    if (util_strcasecmp(r->action, "download") == 0 || util_strcasecmp(r->action, "pull") == 0)
        return action_download(remote, local);
    if (util_strcasecmp(r->action, "upload") == 0)
        return action_upload(local, remote);
    if (util_strcasecmp(r->action, "list") == 0)
        return action_list(paths, remote[0] ? remote : NULL);
    if (util_strcasecmp(r->action, "pipeline") == 0 || util_strcasecmp(r->action, "regen") == 0)
        return action_pipeline();
    if (util_strcasecmp(r->action, "push") == 0)
        return action_push(paths);
    if (util_strcasecmp(r->action, "restore") == 0)
        return action_restore();
    if (util_strcasecmp(r->action, "steps") == 0 || util_strcasecmp(r->action, "sequence") == 0)
        return run_steps(paths, r->steps, depth);

    util_log(SEVERITY_ERROR, "recipe '%s': unknown action '%s'", r->name, r->action);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void cli_print_simple_help(void) {
    int i;
    if (!g_recipes_loaded) recipes_load();

    printf(
        "SFA — Stelliferum Auditor (simple terminal commands)\n"
        "\n"
        "Launch:\n"
        "  sfa                     Interactive terminal (type commands)\n"
        "  sfa help | pull | push  One-shot command\n"
        "  sfa shell               Same as interactive terminal\n"
        "\n"
        "  (sfa.cmd / sfa finds a prebuilt StelliferumAuditor binary — no rebuild required)\n"
        "\n"
        "Built-in verbs:\n"
        "  help                 Show this help\n"
        "  list [path]          List remote FTP directory (default REMOTE_ROOT)\n"
        "  pull                 Download REMOTE_ROOT -> LOCAL_ROOT (recursive)\n"
        "  pipeline | regen     Run economy pipeline on local files only\n"
        "  push                 Upload standard output/ pack to server\n"
        "  restore              Recall newest backups/*/restore_manifest.txt\n"
        "  run <recipe>         Run a named recipe from config/commands.ini\n"
        "  shell | console      Interactive command loop\n"
        "  exit | quit          Leave interactive shell\n"
        "\n"
        "Config (edit rules / paths without rebuilding):\n"
        "  config/ftp.ini            HOST PORT USER PASS\n"
        "  config/server_paths.ini   REMOTE_* LOCAL_ROOT paths\n"
        "  config/commands.ini       optional recipes (see commands.ini.example)\n"
        "  config/loot_policy.ini    tier / blacklist rules for pipeline\n"
        "\n"
        "Examples:\n"
        "  sfa\n"
        "  sfa> help\n"
        "  sfa> pull\n"
        "  sfa> pipeline\n"
        "  sfa> push\n"
        "  sfa run cycle\n"
        "\n"
    );

    if (g_recipe_count > 0) {
        printf("Recipes in config/commands.ini:\n");
        for (i = 0; i < g_recipe_count; i++) {
            printf("  %-16s  action=%s\n", g_recipes[i].name,
                   g_recipes[i].action[0] ? g_recipes[i].action : "?");
        }
        printf("\n");
    } else {
        printf("No config/commands.ini yet — copy config/commands.ini.example to add recipes.\n\n");
    }
}

/* Split a line into argv-style tokens (max 16). Returns argc. */
static int split_line_argv(char *line, char **argv_out, int max_argv) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_argv) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        argv_out[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}

int cli_run_shell(void) {
    CmdPaths paths;
    char line[1024];

    /* Visible console for interactive use (attach parent or show one). */
    util_setup_console();
    paths_load(&paths);
    recipes_load();

    printf("\n");
    printf("SFA terminal — type a command, or 'help'. Type 'exit' to quit.\n");
    printf("Working directory should be the project/release folder (config/ next to you).\n\n");

    for (;;) {
        char *argv_local[16];
        int argc_local;
        int rc;

        printf("sfa> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        str_trim_inplace(line);
        if (line[0] == '\0') continue;

        if (util_strcasecmp(line, "exit") == 0 || util_strcasecmp(line, "quit") == 0 ||
            util_strcasecmp(line, "q") == 0) {
            printf("bye\n");
            break;
        }

        argc_local = split_line_argv(line, argv_local, 16);
        if (argc_local <= 0) continue;

        /* Fake argv[0] = sfa for run_recipe paths that expect argc/argv shape */
        {
            char *fake_argv[17];
            int i;
            fake_argv[0] = (char *)"sfa";
            for (i = 0; i < argc_local; i++)
                fake_argv[i + 1] = argv_local[i];
            /* Reuse one-shot runner without re-init console */
            if (util_strcasecmp(argv_local[0], "help") == 0) {
                cli_print_simple_help();
                continue;
            }
            if (util_strcasecmp(argv_local[0], "list") == 0) {
                const char *path = (argc_local >= 2) ? argv_local[1] : NULL;
                char expanded[MAX_PATH_LEN];
                if (path) {
                    expand_vars(&paths, path, expanded, sizeof(expanded));
                    rc = action_list(&paths, expanded);
                } else {
                    rc = action_list(&paths, NULL);
                }
            } else if (util_strcasecmp(argv_local[0], "pull") == 0) {
                if (argc_local >= 2 && util_strcasecmp(argv_local[1], "all") != 0)
                    rc = run_recipe_by_name(&paths, argv_local[1], 0);
                else
                    rc = action_pull_all(&paths);
            } else if (util_strcasecmp(argv_local[0], "pipeline") == 0 ||
                       util_strcasecmp(argv_local[0], "regen") == 0) {
                rc = action_pipeline();
            } else if (util_strcasecmp(argv_local[0], "push") == 0) {
                rc = action_push(&paths);
            } else if (util_strcasecmp(argv_local[0], "restore") == 0) {
                rc = action_restore();
            } else if (util_strcasecmp(argv_local[0], "run") == 0) {
                if (argc_local < 2) {
                    printf("usage: run <recipe>\n");
                    rc = 1;
                } else {
                    rc = run_recipe_by_name(&paths, argv_local[1], 0);
                }
            } else if (util_strcasecmp(argv_local[0], "shell") == 0 ||
                       util_strcasecmp(argv_local[0], "console") == 0) {
                printf("(already in shell)\n");
                rc = 0;
            } else {
                rc = run_recipe_by_name(&paths, argv_local[0], 0);
            }
            if (rc != 0)
                printf("[exit code %d]\n", rc);
            (void)fake_argv;
        }
    }
    return 0;
}

int cli_is_simple_command(int argc, char **argv) {
    const char *v;
    if (argc < 2 || !argv[1]) return 0;
    v = argv[1];
    /* Not a long flag */
    if (v[0] == '-' && v[1] == '-') return 0;
    if (v[0] == '-' && v[1] != '\0') return 0;

    if (util_strcasecmp(v, "help") == 0) return 1;
    if (util_strcasecmp(v, "list") == 0) return 1;
    if (util_strcasecmp(v, "pull") == 0) return 1;
    if (util_strcasecmp(v, "pipeline") == 0) return 1;
    if (util_strcasecmp(v, "regen") == 0) return 1;
    if (util_strcasecmp(v, "push") == 0) return 1;
    if (util_strcasecmp(v, "restore") == 0) return 1;
    if (util_strcasecmp(v, "run") == 0) return 1;
    if (util_strcasecmp(v, "shell") == 0) return 1;
    if (util_strcasecmp(v, "console") == 0) return 1;
    if (util_strcasecmp(v, "repl") == 0) return 1;
    /* bare recipe name */
    if (!g_recipes_loaded) recipes_load();
    if (recipe_find(v)) return 1;
    return 0;
}

int cli_run_simple_command(int argc, char **argv) {
    CmdPaths paths;
    const char *verb;
    int quiet = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0)
            quiet = 0;
        if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0)
            quiet = 1;
    }

    if (argc < 2) {
        return cli_run_shell();
    }
    verb = argv[1];

    /* Interactive terminal — always want a real console */
    if (util_strcasecmp(verb, "shell") == 0 ||
        util_strcasecmp(verb, "console") == 0 ||
        util_strcasecmp(verb, "repl") == 0) {
        return cli_run_shell();
    }

    /* help needs a real console so text is visible when launched via sfa.cmd */
    if (util_strcasecmp(verb, "help") == 0) {
        util_setup_console();
        paths_load(&paths);
        recipes_load();
        cli_print_simple_help();
        return 0;
    }

    if (quiet) util_setup_console_quiet();
    else util_setup_console();

    paths_load(&paths);
    recipes_load();
    if (util_strcasecmp(verb, "list") == 0) {
        const char *path = (argc >= 3 && argv[2][0] != '-') ? argv[2] : NULL;
        char expanded[MAX_PATH_LEN];
        if (path) {
            expand_vars(&paths, path, expanded, sizeof(expanded));
            return action_list(&paths, expanded);
        }
        return action_list(&paths, NULL);
    }
    if (util_strcasecmp(verb, "pull") == 0) {
        /* pull [recipe|all] */
        if (argc >= 3 && argv[2][0] != '-' && util_strcasecmp(argv[2], "all") != 0)
            return run_recipe_by_name(&paths, argv[2], 0);
        return action_pull_all(&paths);
    }
    if (util_strcasecmp(verb, "pipeline") == 0 || util_strcasecmp(verb, "regen") == 0)
        return action_pipeline();
    if (util_strcasecmp(verb, "push") == 0)
        return action_push(&paths);
    if (util_strcasecmp(verb, "restore") == 0)
        return action_restore();
    if (util_strcasecmp(verb, "run") == 0) {
        if (argc < 3) {
            util_log(SEVERITY_ERROR, "run: need a recipe name (see 'help')");
            return 1;
        }
        return run_recipe_by_name(&paths, argv[2], 0);
    }

    /* Bare recipe name as command */
    return run_recipe_by_name(&paths, verb, 0);
}
