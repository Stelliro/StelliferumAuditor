/**
 * STELLIFERUM SWARM PIPELINE
 * --------------------------
 * Agent-based processing system. Each "agent" is a processing function that
 * handles a specific stage of the economy audit pipeline. Agents communicate
 * through the shared AuditorContext (the "relay").
 *
 * Pipeline stages (run sequentially, one agent at a time):
 *   INDEX  → Scan file structure, classify files, build file index
 *   PARSE  → Parse economy XML files, populate items array
 *   AUDIT  → Validate items, assign tiers, resolve duplicates, fill gaps
 *   STITCH → Build merge groups, merge territories & spawnables
 *   EXPORT → Write merged types.xml, CSV, trader config, audit reports
 *
 * The number of primary agents is determined by file count and estimated
 * token volume. Each primary agent can delegate sub-tasks.
 */

#include "auditor.h"
#include "web_lookup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

// ============================================================================
// INIT & TASK MANAGEMENT
// ============================================================================

void swarm_init(SwarmState *swarm) {
    if (!swarm) return;
    memset(swarm, 0, sizeof(SwarmState));
    swarm->current_task = -1;
    swarm->complete = false;
}

void swarm_add_task(SwarmState *swarm, AgentType agent, const char *description) {
    if (!swarm || swarm->task_count >= MAX_SWARM_TASKS) return;
    SwarmTask *t = &swarm->tasks[swarm->task_count];
    memset(t, 0, sizeof(SwarmTask));
    t->id = swarm->task_count;
    t->agent = agent;
    t->status = TASK_PENDING;
    strncpy(t->description, description ? description : "", 255);
    swarm->task_count++;
}

static const char* agent_name(AgentType type) {
    switch (type) {
        case AGENT_INDEX:  return "INDEX";
        case AGENT_SORT:   return "SORT";
        case AGENT_PARSE:  return "PARSE";
        case AGENT_AUDIT:  return "AUDIT";
        case AGENT_STITCH: return "STITCH";
        case AGENT_EXPORT: return "EXPORT";
        case AGENT_RELAY:  return "RELAY";
        default: return "UNKNOWN";
    }
}

static const char* status_name(TaskStatus s) {
    switch (s) {
        case TASK_PENDING:     return "PENDING";
        case TASK_IN_PROGRESS: return "IN_PROGRESS";
        case TASK_COMPLETE:    return "COMPLETE";
        case TASK_FAILED:      return "FAILED";
        default: return "?";
    }
}

// ============================================================================
// PLAN — Determine how many agents and what tasks are needed
// ============================================================================

void swarm_plan(AuditorContext *ctx) {
    if (!ctx) return;
    SwarmState *swarm = &ctx->swarm;
    swarm_init(swarm);
    
    // Estimate token count and determine primary agent count
    swarm->total_files = ctx->file_index.count;
    swarm->total_tokens = ctx->item_count * 800; // ~800 tokens per item (classname + fields)
    
    // Primary agent count: at least 5 (one per stage), scale up for volume
    swarm->primary_agent_count = 5;
    if (swarm->total_files > 200) swarm->primary_agent_count = 7;
    if (swarm->total_files > 500) swarm->primary_agent_count = 10;
    if (swarm->total_tokens > 100000) swarm->primary_agent_count += 2;
    
    util_log(SEVERITY_INFO, "Swarm: Planning pipeline — %d files, ~%d tokens → %d primary agents",
             swarm->total_files, swarm->total_tokens, swarm->primary_agent_count);
    
    // --- Build task queue ---
    
    // Stage 0: SORT (runs first, right after download)
    swarm_add_task(swarm, AGENT_SORT, "Sort all downloaded files by content into organized directories");
    swarm_add_task(swarm, AGENT_SORT, "Write sort manifest and print summary");

    // Stage 1: INDEX
    swarm_add_task(swarm, AGENT_INDEX, "Scan downloaded file structure");
    swarm_add_task(swarm, AGENT_INDEX, "Classify files by type (economy/territory/spawnable/config)");
    swarm_add_task(swarm, AGENT_INDEX, "Detect map associations (Chernarus/Enoch/Sakhal)");
    swarm_add_task(swarm, AGENT_INDEX, "Identify mod sources for each file");
    
    // Stage 2: PARSE
    swarm_add_task(swarm, AGENT_PARSE, "Parse all economy types.xml files");
    swarm_add_task(swarm, AGENT_PARSE, "Extract spawnable blocks from cfgspawnabletypes.xml");
    swarm_add_task(swarm, AGENT_PARSE, "Parse territory file structure");
    
    // Stage 3: AUDIT
    swarm_add_task(swarm, AGENT_AUDIT, "Pre-audit integrity check — verify duplicates vs variations");
    swarm_add_task(swarm, AGENT_AUDIT, "Workshop intelligence: fetch mod stats and discussion types");
    swarm_add_task(swarm, AGENT_AUDIT, "Sort items alphabetically");
    swarm_add_task(swarm, AGENT_AUDIT, "Filter debug/test/broken items");
    swarm_add_task(swarm, AGENT_AUDIT, "Resolve duplicate items across mods");
    swarm_add_task(swarm, AGENT_AUDIT, "Rename colliding mod items");
    swarm_add_task(swarm, AGENT_AUDIT, "Fill gaps from spawnable data");
    swarm_add_task(swarm, AGENT_AUDIT, "Phoenix: Research item capacities via AI");
    swarm_add_task(swarm, AGENT_AUDIT, "Generate spawnable entries for items with storage");
    swarm_add_task(swarm, AGENT_AUDIT, "Assign tiers and fill missing fields");
    swarm_add_task(swarm, AGENT_AUDIT, "Run full validation audit");
    swarm_add_task(swarm, AGENT_AUDIT, "Generate store pricing and economy balancing");
    
    // Stage 4: STITCH
    swarm_add_task(swarm, AGENT_STITCH, "Build stitch groups from file index");
    swarm_add_task(swarm, AGENT_STITCH, "Merge territory files (vanilla + mod overlays)");
    swarm_add_task(swarm, AGENT_STITCH, "Merge spawnable type files");
    
    // Stage 5: EXPORT
    // NOTE: zombie/wildlife types MUST be exported before cfgeconomycore,
    // because cfgeconomycore checks for their existence on disk to register them.
    swarm_add_task(swarm, AGENT_EXPORT, "Export types.xml (merged + upload-ready)");
    swarm_add_task(swarm, AGENT_EXPORT, "Export merged cfgspawnabletypes.xml");
    swarm_add_task(swarm, AGENT_EXPORT, "Export tiered zombie configuration");
    swarm_add_task(swarm, AGENT_EXPORT, "Export cfgeconomycore.xml manifest");
    swarm_add_task(swarm, AGENT_EXPORT, "Export cfglimitsdefinitionuser.xml");
    swarm_add_task(swarm, AGENT_EXPORT, "Merge and export cfgrandompresets.xml");
    swarm_add_task(swarm, AGENT_EXPORT, "Export audit CSV report");
    swarm_add_task(swarm, AGENT_EXPORT, "Export items CSV spreadsheet");
    swarm_add_task(swarm, AGENT_EXPORT, "Export raw audit report");
    swarm_add_task(swarm, AGENT_EXPORT, "Fill trader gaps and export trader configuration");
    swarm_add_task(swarm, AGENT_EXPORT, "Export building spawn location templates");
    swarm_add_task(swarm, AGENT_EXPORT, "Export SearchForLoot configuration");
    swarm_add_task(swarm, AGENT_EXPORT, "Generate mod manifest for LLM context");
    swarm_add_task(swarm, AGENT_EXPORT, "Sew integrity verification — validate output files");
    
    // Relay task (summary)
    swarm_add_task(swarm, AGENT_RELAY, "Summarize pipeline results");
    
    util_log(SEVERITY_INFO, "Swarm: Planned %d tasks across pipeline.", swarm->task_count);
}

// ============================================================================
// AGENT EXECUTORS — Each agent type runs its tasks
// ============================================================================

static bool execute_sort_task(AuditorContext *ctx, SwarmTask *task) {
    if (!ctx || !task) return false;
    
    char local_root[256] = "downloaded_mods";
    util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
    
    char sorted_root[256];
    snprintf(sorted_root, sizeof(sorted_root), "%s/sorted", local_root);
    
    if (strstr(task->description, "Sort all")) {
        file_sorter_run(ctx, local_root, sorted_root);
        snprintf(task->result, 511, "Sorted %d files: Types:%d Spawnable:%d Trader:%d Territory:%d Events:%d Globals:%d Presets:%d Config:%d Unknown:%d",
                 ctx->sorted_total_count,
                 ctx->sorted_types_count, ctx->sorted_spawnable_count, ctx->sorted_trader_count,
                 ctx->sorted_territory_count, ctx->sorted_events_count, ctx->sorted_globals_count,
                 ctx->sorted_randompresets_count, ctx->sorted_config_count, ctx->sorted_unknown_count);
        task->items_processed = ctx->sorted_total_count;
        return true;
    }
    if (strstr(task->description, "manifest") || strstr(task->description, "summary")) {
        file_sorter_print_summary(ctx, sorted_root);
        snprintf(task->result, 511, "Sort manifest written (%d files catalogued)", ctx->sorted_total_count);
        return true;
    }
    
    return false;
}

static bool execute_index_task(AuditorContext *ctx, SwarmTask *task) {
    if (!ctx || !task) return false;
    
    char local_root[256] = "downloaded_mods";
    util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
    
    if (strstr(task->description, "Scan")) {
        file_index_scan(&ctx->file_index, local_root);
        task->items_processed = ctx->file_index.count;
        snprintf(task->result, 511, "Scanned %d files", ctx->file_index.count);
        return true;
    }
    if (strstr(task->description, "Classify")) {
        file_index_classify(&ctx->file_index);
        int economy = 0, territory = 0, spawnable = 0, config = 0;
        for (int i = 0; i < ctx->file_index.count; i++) {
            switch (ctx->file_index.entries[i].file_type) {
                case FILE_TYPE_ECONOMY:   economy++;   break;
                case FILE_TYPE_TERRITORY: territory++; break;
                case FILE_TYPE_SPAWNABLE: spawnable++; break;
                case FILE_TYPE_CONFIG:    config++;    break;
                default: break;
            }
        }
        snprintf(task->result, 511, "Economy:%d Territory:%d Spawnable:%d Config:%d", 
                 economy, territory, spawnable, config);
        task->items_processed = ctx->file_index.count;
        return true;
    }
    if (strstr(task->description, "map")) {
        // Map detection is done during classify
        int chernarusplus = 0, enoch = 0, sakhal = 0;
        for (int i = 0; i < ctx->file_index.count; i++) {
            switch (ctx->file_index.entries[i].map_id) {
                case MAP_CHERNARUSPLUS: chernarusplus++; break;
                case MAP_ENOCH:     enoch++;     break;
                case MAP_SAKHAL:    sakhal++;    break;
                default: break;
            }
        }
        snprintf(task->result, 511, "ChernarusPlus:%d Enoch:%d Sakhal:%d", chernarusplus, enoch, sakhal);
        return true;
    }
    if (strstr(task->description, "mod")) {
        // Mod detection is done during scan
        int mod_files = 0;
        for (int i = 0; i < ctx->file_index.count; i++) {
            if (ctx->file_index.entries[i].is_mod) mod_files++;
        }
        snprintf(task->result, 511, "%d mod files identified", mod_files);
        task->items_processed = mod_files;
        return true;
    }
    
    return false;
}

static bool execute_parse_task(AuditorContext *ctx, SwarmTask *task) {
    if (!ctx || !task) return false;
    
    // If data is already loaded in memory, skip re-parsing from disk
    // This prevents the duplication bug where items are added on top of existing ones
    if (ctx->data_loaded && ctx->item_count > 0) {
        snprintf(task->result, 511, "Using %d pre-loaded items (skipped disk re-read)", ctx->item_count);
        task->items_processed = ctx->item_count;
        return true;
    }

    // Ensure inline dedup hash map is ready (may not be if running headless
    // without prior do_full_load)
    if (!ctx->classname_map) {
        classname_map_init(ctx);
    }

    char local_root[256] = "downloaded_mods";
    util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
    char sorted_root[256];
    snprintf(sorted_root, sizeof(sorted_root), "%s/sorted", local_root);
    
    if (strstr(task->description, "economy")) {
        int before = ctx->item_count;

        // Prefer sorted/types/ directory (catches SNAFU_types.xml, mmg_types_*.xml, etc.)
        if (ctx->files_sorted && ctx->sorted_types_count > 0) {
            char sorted_types_dir[MAX_PATH_LEN];
            snprintf(sorted_types_dir, sizeof(sorted_types_dir), "%s/types", sorted_root);
#ifdef _WIN32
            char search_path[MAX_PATH_LEN];
            snprintf(search_path, sizeof(search_path), "%s\\*", sorted_types_dir);
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(search_path, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.cFileName[0] == '.') continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    char full_path[MAX_PATH_LEN];
                    snprintf(full_path, sizeof(full_path), "%s\\%s", sorted_types_dir, fd.cFileName);
                    parser_load_types_xml(ctx, full_path);
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
#endif
        } else {
            // Fallback to file_index
            for (int i = 0; i < ctx->file_index.count; i++) {
                FileIndexEntry *e = &ctx->file_index.entries[i];
                if (e->file_type == FILE_TYPE_ECONOMY) {
                    parser_load_types_xml(ctx, e->filepath);
                }
            }
        }

        int loaded = ctx->item_count - before;
        snprintf(task->result, 511, "Loaded %d items from %s", loaded,
                 (ctx->files_sorted && ctx->sorted_types_count > 0) ? "sorted/types/" : "file_index");
        task->items_processed = loaded;
        return true;
    }
    if (strstr(task->description, "spawnable")) {
        int before = ctx->spawn_block_count;

        // Prefer sorted/spawnabletypes/ directory
        if (ctx->files_sorted && ctx->sorted_spawnable_count > 0) {
            char sorted_spawn_dir[MAX_PATH_LEN];
            snprintf(sorted_spawn_dir, sizeof(sorted_spawn_dir), "%s/spawnabletypes", sorted_root);
#ifdef _WIN32
            char search_path[MAX_PATH_LEN];
            snprintf(search_path, sizeof(search_path), "%s\\*", sorted_spawn_dir);
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA(search_path, &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.cFileName[0] == '.') continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    char full_path[MAX_PATH_LEN];
                    snprintf(full_path, sizeof(full_path), "%s\\%s", sorted_spawn_dir, fd.cFileName);
                    parser_load_spawnable_xml(ctx, full_path);
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
#endif
        } else {
            for (int i = 0; i < ctx->file_index.count; i++) {
                FileIndexEntry *e = &ctx->file_index.entries[i];
                if (e->file_type == FILE_TYPE_SPAWNABLE) {
                    parser_load_spawnable_xml(ctx, e->filepath);
                }
            }
        }

        int loaded = ctx->spawn_block_count - before;
        snprintf(task->result, 511, "Extracted %d spawnable blocks from %s", loaded,
                 (ctx->files_sorted && ctx->sorted_spawnable_count > 0) ? "sorted/spawnabletypes/" : "file_index");
        task->items_processed = loaded;
        return true;
    }
    if (strstr(task->description, "territory")) {
        int count = 0;
        for (int i = 0; i < ctx->file_index.count; i++) {
            if (ctx->file_index.entries[i].file_type == FILE_TYPE_TERRITORY) count++;
        }
        snprintf(task->result, 511, "Found %d territory files for stitching", count);
        task->items_processed = count;
        return true;
    }
    
    return false;
}

static bool execute_audit_task(AuditorContext *ctx, SwarmTask *task) {
    if (!ctx || !task) return false;
    
    if (strstr(task->description, "Pre-audit integrity")) {
        IntegrityReport report = auditor_pre_audit_integrity(ctx);
        ctx->pre_audit_integrity = report;
        snprintf(task->result, 511, "Pre-audit integrity: %s — %d findings, %d duplicates",
                 report.passed ? "PASSED" : "WARNINGS", report.finding_count, report.duplicate_groups);
        task->items_processed = report.total_items;
        task->errors = report.finding_count;
        return true;
    }
    if (strstr(task->description, "Workshop intelligence")) {
        web_lookup_ensure_ready(ctx);
        if (ctx->web) {
            int stats_count = 0;
            int link_count = 0;
            for (int m = 0; m < ctx->web->mod_count; m++) {
                if (ctx->web->mods[m].has_stats) stats_count++;
                link_count += ctx->web->mods[m].discussion_link_count;
            }
            snprintf(task->result, 511,
                     "Workshop: %d mods loaded, %d with stats, %d discussion links, %d fetches, %d cache hits",
                     ctx->web->mod_count, stats_count, link_count,
                     ctx->web->fetch_count, ctx->web->cache_hits);
            task->items_processed = ctx->web->mod_count;
        } else {
            snprintf(task->result, 511, "Workshop intelligence: web lookup not available");
        }
        return true;
    }
    if (strstr(task->description, "Sort")) {
        auditor_sort_items(ctx);
        snprintf(task->result, 511, "Sorted %d items", ctx->item_count);
        task->items_processed = ctx->item_count;
        return true;
    }
    if (strstr(task->description, "Filter debug")) {
        int before = 0;
        for (int i = 0; i < ctx->item_count; i++) if (!ctx->items[i].deleted) before++;
        auditor_filter_debug_items(ctx);
        int after = 0;
        for (int i = 0; i < ctx->item_count; i++) if (!ctx->items[i].deleted) after++;
        snprintf(task->result, 511, "Filtered %d debug/test items (%d -> %d)", before - after, before, after);
        task->items_processed = before - after;
        return true;
    }
    if (strstr(task->description, "duplicate")) {
        int before = 0;
        for (int i = 0; i < ctx->item_count; i++) if (!ctx->items[i].deleted) before++;
        auditor_resolve_duplicates(ctx);
        int after = 0;
        for (int i = 0; i < ctx->item_count; i++) if (!ctx->items[i].deleted) after++;
        snprintf(task->result, 511, "Resolved %d duplicates (%d -> %d)", before - after, before, after);
        task->items_processed = before - after;
        return true;
    }
    if (strstr(task->description, "Rename collid")) {
        auditor_rename_collisions(ctx);
        int renamed = 0;
        for (int i = 0; i < ctx->item_count; i++) if (ctx->items[i].renamed) renamed++;
        snprintf(task->result, 511, "Renamed %d colliding items", renamed);
        task->items_processed = renamed;
        return true;
    }
    if (strstr(task->description, "gap")) {
        gap_fill_missing_data(ctx);
        snprintf(task->result, 511, "Gap fill complete (spawn blocks: %d)", ctx->spawn_block_count);
        return true;
    }
    if (strstr(task->description, "Phoenix")) {
        run_phoenix_scanner(ctx);
        int no_cargo = ctx->phoenix.no_cargo_count;
        int specific = ctx->phoenix.specific_count;
        int has_cargo = ctx->phoenix.has_cargo_count;
        snprintf(task->result, 511, "Phoenix scan complete — no_cargo: %d, specific: %d, has_cargo: %d",
                 no_cargo, specific, has_cargo);
        task->items_processed = no_cargo + specific + has_cargo;
        return true;
    }
    if (strstr(task->description, "spawnable entries")) {
        int before = ctx->spawn_block_count;
        gap_fill_spawnable_types(ctx);
        int generated = ctx->spawn_block_count - before;
        snprintf(task->result, 511, "Generated %d spawnable entries (total: %d)", generated, ctx->spawn_block_count);
        task->items_processed = generated;
        return true;
    }
    if (strstr(task->description, "tier") || strstr(task->description, "Assign")) {
        auditor_fill_missing_data(ctx);
        auditor_sanitize_ce_values(ctx);
        auditor_rebalance_usage_pools(ctx);
        int modified = 0;
        for (int i = 0; i < ctx->item_count; i++) if (ctx->items[i].modified) modified++;
        snprintf(task->result, 511, "Filled missing data for %d items", modified);
        task->items_processed = modified;
        return true;
    }
    if (strstr(task->description, "validation") || strstr(task->description, "audit")) {
        auditor_run_full_audit(ctx);
        snprintf(task->result, 511, "Audit found %d issues", ctx->issue_count);
        task->items_processed = ctx->item_count;
        task->errors = ctx->issue_count;
        return true;
    }
    if (strstr(task->description, "store pricing") || strstr(task->description, "economy balancing")) {
        auditor_generate_store_data(ctx);
        int priced = 0, heirloom = 0, bm = 0;
        for (int i = 0; i < ctx->item_count; i++) {
            if (ctx->items[i].deleted) continue;
            if (ctx->items[i].buy_price > 0 || ctx->items[i].sell_price > 0) priced++;
            if (util_strcasecmp(ctx->items[i].trader_cat, "Heirloom") == 0) heirloom++;
            if (ctx->items[i].black_market) bm++;
        }
        snprintf(task->result, 511, "Priced %d items (Heirloom: %d, Black Market: %d)", priced, heirloom, bm);
        task->items_processed = priced;
        return true;
    }
    
    return false;
}

static bool execute_stitch_task(AuditorContext *ctx, SwarmTask *task) {
    if (!ctx || !task) return false;
    
    if (strstr(task->description, "Build")) {
        stitcher_init(ctx);  // Reset stitch groups before building
        stitcher_build_groups(ctx);
        snprintf(task->result, 511, "Built %d stitch groups", ctx->stitch_group_count);
        task->items_processed = ctx->stitch_group_count;
        return true;
    }
    if (strstr(task->description, "territory")) {
        stitcher_merge_territories(ctx, "output");
        int merged = 0;
        for (int i = 0; i < ctx->stitch_group_count; i++) {
            if (ctx->stitch_groups[i].file_type == FILE_TYPE_TERRITORY && ctx->stitch_groups[i].mod_count > 0)
                merged++;
        }
        snprintf(task->result, 511, "Merged %d territory groups", merged);
        task->items_processed = merged;
        return true;
    }
    if (strstr(task->description, "spawnable")) {
        stitcher_merge_spawnables(ctx, "output");
        snprintf(task->result, 511, "Spawnable merge complete");
        return true;
    }
    
    return false;
}

static bool execute_export_task(AuditorContext *ctx, SwarmTask *task) {
    if (!ctx || !task) return false;
    
    util_ensure_directory("output");
    
    if (strstr(task->description, "types.xml") && !strstr(task->description, "cfgspawnabletypes")) {
        /* Write the canonical types.xml used by both upload and integrity. */
        bool ok = writer_export_merged_xml(ctx, "output/types.xml");
        snprintf(task->result, 511, "Types export: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "items CSV")) {
        bool ok = writer_export_csv(ctx, "output/items.csv");
        snprintf(task->result, 511, "Items CSV: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "raw audit report")) {
        bool ok = writer_export_audit_report(ctx, "output/audit_report_raw.txt", true);
        snprintf(task->result, 511, "Raw audit report: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "cfgspawnabletypes")) {
        bool ok = writer_export_spawnable_types(ctx, "output/cfgspawnabletypes.xml");
        snprintf(task->result, 511, "Spawnable types (%d blocks): %s", ctx->spawn_block_count, ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "cfglimitsdefinitionuser")) {
        bool ok = writer_export_cfglimitsdefinitionuser(ctx, "output/cfglimitsdefinitionuser.xml");
        snprintf(task->result, 511, "CE limits definition user: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "cfgeconomycore")) {
        bool ok = writer_export_cfgeconomycore(ctx, "output/cfgeconomycore.xml");
        snprintf(task->result, 511, "Economy core manifest: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "cfgrandompresets") || strstr(task->description, "randompresets")) {
        char local_root[256] = "downloaded_mods";
        util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
        char sorted_root[256];
        snprintf(sorted_root, sizeof(sorted_root), "%s/sorted", local_root);
        bool ok = writer_merge_random_presets(ctx, sorted_root, "output/cfgrandompresets.xml");
        snprintf(task->result, 511, "Random presets merge: %s", ok ? "SUCCESS" : "SKIPPED (no presets found)");
        // Don't flag as error — presets are optional
        return true;
    }
    if (strstr(task->description, "CSV")) {
        bool ok = writer_export_audit_report(ctx, "output/audit_report.txt", false);
        snprintf(task->result, 511, "Audit report: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "trader") || strstr(task->description, "Trader")) {
        // First, fill trader gaps (creates entries for items missing from trader files)
        char local_root[256] = "downloaded_mods";
        util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
        char sorted_root[256];
        snprintf(sorted_root, sizeof(sorted_root), "%s/sorted", local_root);
        trader_gap_fill(ctx, sorted_root);

        bool ok = writer_export_trader_by_shop_mod(ctx);
        snprintf(task->result, 511, "Trader config (%s): %s (gap-filled)",
                 shop_mod_name(ctx->shop_mod), ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "zombie") || strstr(task->description, "tiered")) {
        bool ok = writer_export_zombie_config(ctx, "output");
        snprintf(task->result, 511, "Zombie tier config: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "manifest")) {
        char local_root[256] = "downloaded_mods";
        util_read_ini_value("config/server_paths.ini", "LOCAL_ROOT", local_root, sizeof(local_root));
        util_generate_mod_manifest(local_root, "output/mod_manifest.md");
        snprintf(task->result, 511, "Manifest generated");
        return true;
    }
    if (strstr(task->description, "spawn location") || strstr(task->description, "building spawn")) {
        bool ok = writer_export_spawn_templates(ctx, "output");
        snprintf(task->result, 511, "Spawn templates: %s", ok ? "SUCCESS" : "FAILED");
        if (!ok) task->errors = 1;
        return ok;
    }
    if (strstr(task->description, "SearchForLoot")) {
        bool ok = sfl_generate_config(ctx, "config/SearchForLoot.json", "output/SearchForLoot.json");
        snprintf(task->result, 511, "SearchForLoot config: %s", ok ? "SUCCESS" : "SKIPPED (no template)");
        // Don't fail pipeline if no template — feature is opt-in
        return true;
    }
    if (strstr(task->description, "Sew integrity") || strstr(task->description, "sew integrity")) {
        IntegrityReport report = auditor_sew_integrity_check(
            ctx, "output/types.xml", "output/cfgspawnabletypes.xml");
        ctx->sew_integrity = report;
        snprintf(task->result, 511, "Sew integrity: %s — %d findings",
                 report.passed ? "PASSED" : "FAILED", report.finding_count);
        task->errors = report.finding_count;
        return true;
    }
    
    return false;
}

static bool execute_relay_task(AuditorContext *ctx, SwarmTask *task) {
    if (!ctx || !task) return false;
    
    // Relay: Summarize all results
    int completed = 0, failed = 0, total_items = 0, total_errors = 0;
    SwarmState *swarm = &ctx->swarm;
    
    for (int i = 0; i < swarm->task_count; i++) {
        if (swarm->tasks[i].status == TASK_COMPLETE) completed++;
        else if (swarm->tasks[i].status == TASK_FAILED) failed++;
        total_items += swarm->tasks[i].items_processed;
        total_errors += swarm->tasks[i].errors;
    }
    
    snprintf(task->result, 511, "Pipeline: %d/%d tasks complete, %d failed, %d items processed, %d errors",
             completed, swarm->task_count - 1, failed, total_items, total_errors);
    
    util_log(SEVERITY_INFO, "Swarm Relay: %s", task->result);
    return true;
}

// ============================================================================
// EXECUTE — Process all tasks in the queue, one at a time
// ============================================================================

void swarm_execute(AuditorContext *ctx) {
    if (!ctx) return;
    SwarmState *swarm = &ctx->swarm;
    
    util_log(SEVERITY_INFO, "Swarm: === EXECUTING PIPELINE (%d tasks) ===", swarm->task_count);
    clock_t start = clock();
    
    for (int i = 0; i < swarm->task_count; i++) {
        SwarmTask *task = &swarm->tasks[i];
        swarm->current_task = i;
        task->status = TASK_IN_PROGRESS;
        
        util_log(SEVERITY_INFO, "Swarm [%d/%d] %s: %s",
                 i + 1, swarm->task_count, agent_name(task->agent), task->description);
        
        bool ok = false;
        switch (task->agent) {
            case AGENT_SORT:   ok = execute_sort_task(ctx, task);   break;
            case AGENT_INDEX:  ok = execute_index_task(ctx, task);  break;
            case AGENT_PARSE:  ok = execute_parse_task(ctx, task);  break;
            case AGENT_AUDIT:  ok = execute_audit_task(ctx, task);  break;
            case AGENT_STITCH: ok = execute_stitch_task(ctx, task); break;
            case AGENT_EXPORT: ok = execute_export_task(ctx, task); break;
            case AGENT_RELAY:  ok = execute_relay_task(ctx, task);  break;
        }
        
        task->status = ok ? TASK_COMPLETE : TASK_FAILED;
        
        // Mark audit as complete when transitioning from AUDIT to STITCH stage.
        // This prevents the sew/stitch step from re-running destructive passes
        // (filter/dedup/rename) that would corrupt economy-balanced values.
        if (task->agent == AGENT_AUDIT) {
            int next = i + 1;
            if (next >= swarm->task_count || swarm->tasks[next].agent != AGENT_AUDIT) {
                ctx->audit_complete = true;
                util_log(SEVERITY_INFO, "Swarm: Audit stage complete — item data locked for sew/export.");
            }
        }

        if (task->result[0]) {
            util_log(SEVERITY_INFO, "  -> %s: %s", status_name(task->status), task->result);
        }
        
        // Update status message for UI feedback
        snprintf(ctx->status_message, 255, "Swarm [%d/%d] %s: %s",
                 i + 1, swarm->task_count, agent_name(task->agent), 
                 ok ? "OK" : "FAILED");
    }
    
    double elapsed = (double)(clock() - start) / CLOCKS_PER_SEC;
    swarm->complete = true;
    
    int completed = 0, failed = 0;
    for (int i = 0; i < swarm->task_count; i++) {
        if (swarm->tasks[i].status == TASK_COMPLETE) completed++;
        else if (swarm->tasks[i].status == TASK_FAILED) failed++;
    }
    
    util_log(SEVERITY_INFO, "Swarm: === PIPELINE COMPLETE in %.2fs === (%d ok, %d failed)",
             elapsed, completed, failed);
    
    snprintf(ctx->status_message, 255, "Swarm Complete: %d/%d tasks (%.1fs). %d issues.",
             completed, swarm->task_count, elapsed, ctx->issue_count);
}
